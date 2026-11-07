#include "column_view.h"
#include <cmath>
#include "render/image_damage.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "engine_document.h"
#include "reader_source.h"
#include "publication/image_policy.h"
#include "publication/resource_cache.h"
#include "reader_render_callback.h"
#include "engine_activity.h"
#include "reader_styles.h"
#include "reader_hyphenation.h"
#include "reader_alignment.h"
#include "document_cache.h"
#include "font_weight.h"
#include "render/page_refresh_regions.h"
#include "page_pixels.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QRegion>
#include <QPainter>
#include <QSet>
#include <QtMath>
#include <lvdocview.h>
#include <lvfntman.h>
#include <lvrend.h>
#include <hyphman.h>
#include <textlang.h>
#include <algorithm>
#include <functional>
#include <atomic>

namespace kobalt {
namespace {
std::atomic<unsigned> liveDocuments{ 0 };
bool fontsReady = false;
EngineFontInputs activeFonts;
QVector<ReadingFont> readingFonts;
QStringList readingFontFaces;
QVector<ReadingFont> readingFontCatalog;
bool fontFacesReady = false, fontCatalogReady = false;

QString qstring(const lString32 &s)
{
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}

lString32 cre(const QString &s)
{
    return Utf8ToUnicode(s.toUtf8().constData());
}

bool equationImage(const LVImageSourceRef &image)
{
    for (auto *node = image->GetSourceNode(); node; node = node->getParentNode()) {
        if (!node->isElement())
            continue;
        if (!node->getAttributeValue("data-tex").empty()
            || node->getAttributeValue("role") == U"math")
            return true;
    }
    return false;
}

// Transform neutral ink before alpha compositing, so transparent pixels keep
// the actual page background. Delegate vector sizing to avoid an extra raster scale.
class EquationImage : public LVImageSource, private LVImageDecoderCallback
{
public:
    explicit EquationImage(LVImageSourceRef source) : source(source) { }

    ldomDocument *GetSourceDocument() override { return source->GetSourceDocument(); }

    ldomNode *GetSourceNode() override { return source->GetSourceNode(); }

    LVStream *GetSourceStream() override { return source->GetSourceStream(); }

    void Compact() override { source->Compact(); }

    int GetWidth() const override { return source->GetWidth(); }

    int GetHeight() const override { return source->GetHeight(); }

    bool IsScalable() const override { return source->IsScalable(); }

    bool Decode(LVImageDecoderCallback *target) override
    {
        if (!target)
            return source->Decode(nullptr);
        callback = target;
        int width = GetWidth(), height = GetHeight();
        if (IsScalable())
            target->GetTargetSize(width, height);
        row.resize(width);
        return source->Decode(this);
    }

private:
    bool GetTargetSize(int &width, int &height) const override
    {
        return callback->GetTargetSize(width, height);
    }

    void OnStartDecode(LVImageSource *) override { callback->OnStartDecode(this); }

    bool OnLineDecoded(LVImageSource *, int y, lUInt32 *pixels) override
    {
        for (size_t x = 0; x < row.size(); ++x) {
            const auto pixel = pixels[x];
            const bool neutral = qRed(pixel) == qGreen(pixel) && qRed(pixel) == qBlue(pixel);
            row[x] = neutral ? pixel ^ 0x00ffffffU : pixel;
        }
        return callback->OnLineDecoded(this, y, row.data());
    }

    void OnEndDecode(LVImageSource *, bool errors) override { callback->OnEndDecode(this, errors); }

    LVImageSourceRef source;
    LVImageDecoderCallback *callback = nullptr;
    std::vector<lUInt32> row;
};

// A white border and mostly white paper distinguish illustrations from dark artwork.
// Check every visible source pixel so even a small colour accent prevents inversion.
class IllustrationProbe : public LVImageDecoderCallback
{
public:
    IllustrationProbe(int width, int height) : width(width), height(height) { }

    bool eligible(LVImageSourceRef source)
    {
        return source->Decode(this) && !failed && lines == height && neutral && total
                && white * 2 >= total && border && whiteBorder * 10 >= border * 8;
    }

private:
    bool GetTargetSize(int &w, int &h) const override
    {
        w = width;
        h = height;
        return true;
    }

    void OnStartDecode(LVImageSource *) override { }

    bool OnLineDecoded(LVImageSource *, int y, lUInt32 *pixels) override
    {
        ++lines;
        for (int x = 0; x < width; ++x) {
            const auto pixel = pixels[x];
            // CREngine uses an inverted alpha channel. Invisible RGB is irrelevant.
            const int transparency = pixel >> 24;
            if (transparency != 255
                && (qRed(pixel) != qGreen(pixel) || qRed(pixel) != qBlue(pixel)))
                neutral = false;
            const bool paper = transparency == 255
                    || (qRed(pixel) >= 245 && qGreen(pixel) >= 245 && qBlue(pixel) >= 245);
            ++total;
            white += paper;
            if (x == 0 || y == 0 || x == width - 1 || y == height - 1) {
                ++border;
                whiteBorder += paper;
            }
        }
        return true;
    }

    void OnEndDecode(LVImageSource *, bool errors) override { failed = errors; }

    int width, height, lines = 0;
    quint64 total = 0, white = 0, border = 0, whiteBorder = 0;
    bool neutral = true, failed = false;
};

class PageDrawBuffer : public LVColorDrawBuf
{
public:
    PageDrawBuffer(QImage &image, bool dark, bool smooth,
                   QMap<std::tuple<quintptr, int, int>, bool> *inversion = nullptr)
        : LVColorDrawBuf(image.width(), image.height(), image.bits(), 32),
          pageSize(image.size()),
          dark(dark),
          inversion(inversion)
    {
        setInvertColors(dark);
        setSmoothScalingImages(smooth);
    }

    bool hasImages = false, hasText = false;
    QVector<QRect> imageRegions;
    using LVColorDrawBuf::Draw;

    void Draw(int x, int y, const lUInt8 *bitmap, int width, int height,
              const lUInt32 *palette) override
    {
        lvRect clip;
        GetClipRect(&clip);
        if (width > 0 && height > 0 && qint64(x) + width > clip.left && x < clip.right
            && qint64(y) + height > clip.top && y < clip.bottom)
            hasText = true;
        const auto color = palette ? palette[0] : GetTextColor();
        const auto background = GetBackgroundColor();
        const bool neutral = qRed(color) == qGreen(color) && qRed(color) == qBlue(color);
        const bool coloredPaper =
                qRed(background) != qGreen(background) || qRed(background) != qBlue(background);
        const bool preserveContrast = coloredPaper && (background >> 24) != 255;
        // CREngine pre-inverts every non-gray text color. Undo that for faint
        // tints on neutral paper so near-black text becomes light in dark mode.
        // On preserved colored paper, keep the publisher's original contrast.
        const bool invertInk =
                dark && (preserveContrast ? neutral : !neutral && !hasVisibleColor(color));
        const lUInt32 ink = invertInk ? color ^ 0x00ffffffU : color;
        LVColorDrawBuf::Draw(x, y, bitmap, width, height, &ink);
    }

    void Draw(LVImageSourceRef image, int x, int y, int width, int height, bool dither) override
    {
        hasImages = true;
        lvRect clip;
        GetClipRect(&clip);
        const qint64 left = qMax<qint64>(0, qMax<qint64>(x, clip.left));
        const qint64 top = qMax<qint64>(0, qMax<qint64>(y, clip.top));
        const qint64 right =
                qMin<qint64>(pageSize.width(), qMin<qint64>(qint64(x) + width, clip.right));
        const qint64 bottom =
                qMin<qint64>(pageSize.height(), qMin<qint64>(qint64(y) + height, clip.bottom));
        if (!imageRegionsFull && right > left && bottom > top) {
            if (imageRegions.size() < 128)
                imageRegions.append(
                        QRect(int(left), int(top), int(right - left), int(bottom - top)));
            else {
                imageRegions.clear();
                imageRegions.append(QRect(QPoint(0, 0), pageSize));
                imageRegionsFull = true;
            }
        }
        bool invertIllustration = false;
        if (dark && inversion && !equationImage(image)) {
            const auto node = reinterpret_cast<quintptr>(image->GetSourceNode());
            // Vector details can appear at a larger size. Do not reuse a smaller raster verdict.
            const auto key = std::make_tuple(node, image->IsScalable() ? width : 0,
                                             image->IsScalable() ? height : 0);
            auto found = inversion->constFind(key);
            if (node && found != inversion->constEnd()) {
                invertIllustration = found.value();
            } else {
                IllustrationProbe probe(image->IsScalable() ? width : image->GetWidth(),
                                        image->IsScalable() ? height : image->GetHeight());
                invertIllustration = probe.eligible(image);
                if (inversion->size() >= 1024)
                    inversion->clear();
                if (node)
                    inversion->insert(key, invertIllustration);
            }
        }
        if (dark && (equationImage(image) || invertIllustration))
            image = LVImageSourceRef(new EquationImage(image));
        // Composite images in the final page colors. CREngine's invert-images
        // option instead blends transparency against a white background.
        if (dark)
            InvertRect(x, y, x + width, y + height);
        LVColorDrawBuf::Draw(image, x, y, width, height, dither);
        if (dark)
            InvertRect(x, y, x + width, y + height);
    }

private:
    QSize pageSize;
    bool imageRegionsFull = false;
    bool dark;
    QMap<std::tuple<quintptr, int, int>, bool> *inversion;
};

QString regionMetadata(const QVector<QRect> &regions)
{
    QJsonArray array;
    for (const QRect &rect : regions) {
        QJsonArray values;
        values.append(rect.x());
        values.append(rect.y());
        values.append(rect.width());
        values.append(rect.height());
        array.append(values);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QVector<QRect> colorTileRegions(const QVector<uchar> &tiles, int columns, int rows,
                                const QSize &size)
{
    QRegion region;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns;) {
            if (!tiles[row * columns + column]) {
                ++column;
                continue;
            }
            const int start = column;
            while (column < columns && tiles[row * columns + column])
                ++column;
            region += QRect(start * 32, row * 32, qMin(size.width(), column * 32) - start * 32,
                            qMin(32, size.height() - row * 32));
            if (region.rectCount() > 128)
                return { QRect(QPoint(0, 0), size) };
        }
    }
    QVector<QRect> result;
#if QT_VERSION < QT_VERSION_CHECK(5, 8, 0)
    result = region.rects();
#else
    for (const QRect &rect : region)
        result.append(rect);
#endif
    return result;
}

bool validAlignment(const QString &value)
{
    return value == "publisher" || value == "left" || value == "justify" || value == "center"
            || value == "right";
}
} // namespace

EngineDocument::EngineDocument(EngineFontInputs fonts) : fonts_(std::move(fonts))
{
    ++liveDocuments;
}

EngineDocument::~EngineDocument()
{
    sourceIndex_.reset();
    engine_.reset();
    source_.reset();
    // Document work and destruction run on the engine worker. A preview may
    // still use these fonts, but the last closed document must release their
    // file mappings so Nickel can unmount user storage for USB.
    if (--liveDocuments == 0) {
        ShutdownFontManager();
        HyphMan::uninit();
        fontsReady = false;
        readingFonts.clear();
        readingFontFaces.clear();
        readingFontCatalog.clear();
        fontFacesReady = fontCatalogReady = false;
        activeFonts = {};
    }
}

bool EngineDocument::initializeFonts()
{
    // CREngine has one global font manager. A preview or retained document can
    // still use it, so a different host input cannot replace it until the last
    // document releases its font mappings on this worker.
    if (fontsReady)
        return activeFonts == fonts_;
    if (fonts_.sources.bundled.isEmpty() || fonts_.hyphenationDirectory.isEmpty())
        return false;
    if (!InitFontManager(lString8()))
        return false;
    readingFonts = discoverReadingFonts(fonts_.sources);
    if (readingFonts.isEmpty()) {
        ShutdownFontManager();
        return false;
    }
    for (const auto &font : readingFonts)
        for (const auto &file : font.files)
            if (!fontMan->RegisterFont(lString8(file.toUtf8().constData()))
                && (font.origin == FontOrigin::Recommended
                    || font.origin == FontOrigin::Interface)) {
                readingFonts.clear();
                ShutdownFontManager();
                return false;
            }
    // The generic monospace family for code, also under a chosen reading font,
    // and fallback faces for characters that a reading font lacks. The picker
    // offers script fallbacks when they match the document language.
    for (const auto &file : fonts_.fallbackFiles)
        if (!fontMan->RegisterFont(lString8(file.toUtf8().constData()))) {
            readingFonts.clear();
            ShutdownFontManager();
            return false;
        }
    // Bundled dictionaries hyphenate by language. A language whose dictionary
    // is not bundled keeps the engine's algorithmic hyphenation.
    HyphMan::initDictionaries(cre(fonts_.hyphenationDirectory + '/'));
    for (const auto *name : engineHyphenationPatterns) {
        if (QFileInfo::exists(fonts_.hyphenationDirectory + '/' + name))
            continue;
        auto *algorithmic = new HyphDictionary(HDT_ALGORITHM, cre(name), cre(name), cre(name));
        if (!HyphMan::addDictionaryItem(algorithmic))
            delete algorithmic;
    }
    fontMan->SetKerningMode(KERNING_MODE_HARFBUZZ);
    fontMan->SetHintingMode(HINTING_MODE_DISABLED);
    fontsReady = true;
    activeFonts = fonts_;
    return true;
}

QStringList EngineDocument::fontFaces()
{
    if (!initializeFonts())
        return {};
    // The list leaves out document fonts. Global faces are registered only
    // when the shared font manager starts, so the list holds until it closes.
    if (fontFacesReady)
        return readingFontFaces;
    lString32Collection faces;
    fontMan->getFaceList(faces);
    QStringList result;
    for (int i = 0; i < faces.length(); ++i)
        result.append(qstring(faces[i]));
    result.removeDuplicates();
    result.sort(Qt::CaseInsensitive);
    readingFontFaces = result;
    fontFacesReady = true;
    return result;
}

QVector<ReadingFont> EngineDocument::fontCatalog()
{
    if (!initializeFonts())
        return {};
    if (fontCatalogReady)
        return readingFontCatalog;
    const auto faces = fontFaces();
    QVector<ReadingFont> result;
    for (const auto &font : readingFonts)
        if (faces.contains(font.family))
            result.append(font);
    for (const auto &family : faces) {
        if (family != "Noto Serif" && family != monospaceFontFace && family != mathFontFace
            && !family.startsWith("Noto Serif CJK "))
            continue;
        bool present = false;
        for (const auto &font : result)
            present |= font.family == family;
        if (present)
            continue;
        QStringList files;
        for (const auto &file : fonts_.fallbackFiles) {
            const auto name = QFileInfo(file).fileName();
            if ((family == "Noto Serif" && name.startsWith("NotoSerif-"))
                || (family == monospaceFontFace && name.startsWith("IBMPlexMono-"))
                || (family == mathFontFace && name.startsWith("NotoSansMath-"))
                || (family.startsWith("Noto Serif CJK ") && name == "NotoSerifCJK-Regular.ttc")) {
                files.append(file);
            }
        }
        if (!files.isEmpty()) {
            result.append({ family, files, FontOrigin::Interface, {}, {} });
        }
    }
    readingFontCatalog = result;
    fontCatalogReady = true;
    return result;
}

void EngineDocument::setDocumentCache(QString directory, QByteArray fingerprint, QByteArray policy,
                                      qint64 bytes)
{
    cacheDirectory_ = std::move(directory);
    cacheFingerprint_ = std::move(fingerprint);
    // Parsed pages from older density, image and double-scaled line-height
    // policies must not survive these renderer changes. The density is fixed by the host
    // before any document opens and is shared by the main reader and previews.
    cachePolicy_ = std::move(policy) + ":css-density-leading-v1:page-layout-v2:line-height-v3:"
            + QByteArray::number(readingPpi.load());
    cacheBytes_ = bytes;
}

QByteArray EngineDocument::documentCachePolicy() const
{
    // Detect CSS edits for cache reuse only. User CSS is never an installation asset.
    if (userStyleSheet_.isEmpty()) {
        return cachePolicy_;
    }
    return cachePolicy_ + ":user-css-v1:"
            + QCryptographicHash::hash(userStyleSheet_, QCryptographicHash::Sha256).toHex();
}

bool EngineDocument::saveDocumentCache()
{
    if (!engine_ || cacheDirectory_.isEmpty() || cacheFingerprint_.isEmpty()
        || !resourceError().isEmpty() || !pages())
        return false;
    // Only a document opened under this book's key may write that key's file.
    const auto name = engine_->getDocProps()->getStringDef(DOC_PROP_FILE_NAME, "");
    if (QString::fromUtf8(UnicodeToUtf8(name).c_str()) != documentCacheName(cacheFingerprint_))
        return false;
    auto *document = engine_->getDocument();
    DocumentCacheScope cache(cacheDirectory_);
    if (!cache.enabled())
        return false;
    // The no-argument update serializes even a clean cache. Keep that path for
    // creation and dirty documents; an existing clean cache needs no update.
    if ((!document->hasCacheFile() || document->isCacheFileStale())
        && engine_->updateCache() != CR_DONE)
        return false;
    if (document->hasCacheFile()) {
        if (!document->updateCacheFileName())
            return false;
        ldomDocCache::removeObsolete(name, document->getCacheFilePath());
    }
    return true;
}

FontWeightAxis EngineDocument::weightAxis() const
{
    return fontWeightAxis(readingFonts, face_);
}

FontOpticalSizeAxis EngineDocument::opticalSizeAxis() const
{
    return fontOpticalSizeAxis(readingFonts, face_);
}

void EngineDocument::activateFontParameters() const
{
    const auto axis = weightAxis();
    // CREngine shares font parameters across documents. Restore this document's
    // values before layout or drawing so previews cannot change another book.
    LVRendSetFontOpticalSize(opticalSizeAxis().available() ? opticalSize_ : 0);
    LVRendSetBaseFontWeight(axis.available() ? (bionic_ == "off" ? axisWeight_ : axis.defaultValue)
                                             : 400);
}

void EngineDocument::configure(QSize size, const QJsonObject &settings)
{
    auto saved = settings;
    appearance::migrateLineSpacing(saved);
    // Outline thickness must not affect layout-time italic overhang measurements.
    setExtraFontWeight(0);
    sourceIndex_.reset();
    engine_.reset();
    alternateViewport_ = {};
    layoutPpi_ = readingPpi.load();
    layoutCss_ = userStyleSheet_;
    anchor_.clear();
    contents_.clear();
    graph_.clear();
    totalWords_ = -1;
    contentsIndex_ = {};
    graphIndex_ = {};
    nativeLocation_ = saved.value("imported_native_location").toString();
    cache_.clear();
    spareImage_ = {};
    damage_.clear();
    size_ = size;
    darkMode_ = saved.value("dark_mode").toBool(false);
    imageInversion_.clear();
    hideNonlinearFragments_ = saved.value("hide_nonlinear_fragments").toBool(false);
    const auto normalization = appearance::normalizationSettings(saved);
    normalizePageMargins_ = normalization.value("normalize_page_margins").toBool();
    reduceChapterSpacing_ = normalization.value("reduce_chapter_spacing").toBool();
    verticalSpacing_ = appearance::verticalSpacingMode(saved.value("vertical_justification"));
    verticalSpacingScope_ = appearance::verticalSpacingScope(saved.value("vertical_spacing_scope"));
    wordSpacing_ = saved.value("word_spacing").toInt(100);
    if (!appearance::validWordSpacing(wordSpacing_))
        wordSpacing_ = 100;
    justificationTracking_ = saved.value("justification_tracking").toInt(0);
    if (!appearance::validTracking(justificationTracking_))
        justificationTracking_ = 0;
    widowsOrphans_ = saved.value("widows_orphans").toString("publisher");
    if (!appearance::validWidowsOrphans(widowsOrphans_))
        widowsOrphans_ = "publisher";
    pixels_ = qBound(minimumFontPixels(), saved.value("font_pixels").toInt(defaultFontPixels()),
                     maximumFontPixels());
    spacing_ =
            qBound(100, saved.value("line_spacing").toInt(defaultLineSpacing), maximumLineSpacing);
    paragraphSpacing_ = appearance::paragraphSpacing(saved.value("paragraph_spacing"));
    paragraphIndent_ = appearance::paragraphIndent(saved.value("paragraph_indent"));
    margin_ = qBound(0, saved.value("margin_pixels").toInt(defaultMarginPixels()),
                     maximumMarginPixels());
    topMargin_ =
            qBound(0, saved.value("top_margin_pixels").toInt(defaultVerticalMarginPixels(margin_)),
                   maximumMarginPixels());
    bottomMargin_ = qBound(
            0, saved.value("bottom_margin_pixels").toInt(defaultVerticalMarginPixels(margin_)),
            maximumMarginPixels());
    const auto hyphenation = saved.value("hyphenation");
    hyphenation_ = hyphenation.isBool() ? (hyphenation.toBool() ? "auto" : "off")
                                        : hyphenation.toString("off");
    if (hyphenation_ != "auto" && hyphenation_ != "on" && hyphenation_ != "off")
        hyphenation_ = "off";
    weight_ = qBound(0, saved.value("weight_steps").toInt(0), 12);
    bionic_ = saved.value("bold_reading").toString("off");
    const auto beautifier = appearance::beautifierSettings(saved);
    beautifierInitials_ = beautifier.value("beautifier_initials").toBool();
    beautifierSpacing_ = beautifier.value("beautifier_spacing").toBool();
    beautifierIndent_ = beautifier.value("beautifier_indent").toBool();
    // Preserve enabled Bionic reading from the retired thickness mode.
    if (bionic_ == "synthetic")
        bionic_ = "bold";
    if (bionic_ != "bold")
        bionic_ = "off";
    face_ = saved.value("font_face").toString(defaultFontFace);
    if (face_ == "KF Libron")
        face_ = defaultFontFace;
    alignment_ = saved.value("text_alignment").toString("left");
    if (!validAlignment(alignment_))
        alignment_ = "publisher";
    const auto faces = fontFaces();
    for (size_t index = 0; index < fallbackFaces_.size(); ++index) {
        const auto family = saved.value(appearance::fallbackFontKeys[index]).toString();
        fallbackFaces_[index] = faces.contains(family) ? family : QString();
    }
    defaultFace_ = saved.value("default_font_face").toString(defaultFontFace);
    if (!faces.contains(defaultFace_))
        defaultFace_ = defaultFontFace;
    if (!face_.isEmpty() && !faces.contains(face_))
        face_ = defaultFace_;
    const auto axis = weightAxis();
    axisWeight_ = axis.available()
            ? qBound(axis.minimum, saved.value("font_weight").toInt(defaultVariableFontWeight),
                     axis.maximum)
            : saved.value("font_weight").toInt(defaultVariableFontWeight);
    const auto optical = opticalSizeAxis();
    const double savedOptical = saved.value("font_optical_size").toDouble();
    opticalSize_ = normalizedOpticalSize(optical, savedOptical);
    activateFontParameters();
    twoColumnsLandscape_ = saved.value("two_columns_landscape").toBool(false);
    columns_ = twoColumnsLandscape_ ? (size.width() > size.height() ? 2 : 1)
                                    : qBound(1, saved.value("columns").toInt(1), 2);
    columnSpacing_ = qBound(0, saved.value("column_spacing").toInt(readingPixels(60)),
                            maximumMarginPixels());
    columnSpacingPercent_ = saved.value("column_spacing_percent").toInt(0);
    if (columnSpacingPercent_ < 100 || columnSpacingPercent_ > 300)
        columnSpacingPercent_ = 0;
    if (columnSpacingPercent_)
        columnSpacing_ = qRound(pixels_ * columnSpacingPercent_ / 100.0);
    hangingPunctuation_ = saved.value("hanging_punctuation").toBool(false);
    engine_ = std::make_unique<ColumnView>();
    engine_->setHangingPunctuation(hangingPunctuation_);
    engine_->setColumns(columns_);
    engine_->setColumnSpacing(columnSpacing_);
    // CREngine defaults to a 72-pixel cap. Use the same physical limits as the controls.
    engine_->setMinFontSize(minimumFontPixels());
    engine_->setMaxFontSize(maximumFontPixels());
    // Keep normal word spacing when fitting a line. CREngine otherwise allows
    // spaces to shrink to half width, including in left-aligned paragraphs.
    auto properties = LVCreatePropsContainer();
    properties->setInt(PROP_FORMAT_MIN_SPACE_CONDENSING_PERCENT, 100);
    properties->setInt(PROP_FORMAT_SPACE_WIDTH_SCALE_PERCENT, wordSpacing_);
    properties->setInt(PROP_FORMAT_MAX_ADDED_LETTER_SPACING_PERCENT, justificationTracking_);
    properties->setInt(PROP_FLOATING_PUNCTUATION, hangingPunctuation_);
    properties->setInt(PROP_RENDER_BLOCK_RENDERING_FLAGS,
                       reduceChapterSpacing_ ? DEF_RENDER_BLOCK_RENDERING_FLAGS
                                             : DEF_RENDER_BLOCK_RENDERING_FLAGS
                                       & ~BLOCK_RENDERING_DISCARD_FORCED_BREAK_MARGINS);
    // CSS already sizes images against the reading font. The engine's legacy
    // zoom would enlarge them a second time, including inline equations.
    properties->setInt(PROP_IMG_SCALING_ZOOMIN_INLINE_MODE, 0);
    properties->setInt(PROP_IMG_SCALING_ZOOMIN_BLOCK_MODE, 0);
    // CREngine's CSS pixel is 1/96 inch. The reading font and margins already
    // use physical screen pixels, so only CSS lengths and images scale here.
    properties->setInt(PROP_FONT_BASE_WEIGHT, LVRendGetBaseFontWeight());
    properties->setInt(PROP_RENDER_DPI, readingPpi.load());
    properties->setInt(PROP_RENDER_SCALE_FONT_WITH_DPI, 0);
    properties->setString(PROP_FALLBACK_FONT_FACES, lString8(fallbackFontFaces));
    // A saved position is an XPointer, whose form depends on the DOM version.
    // Keep the version that wrote it, so that it still resolves after an engine
    // update. States without the field were written at 20260812.
    domVersion_ = saved.value("position").toString().isEmpty()
            ? gDOMVersionCurrent
            : saved.value("dom_version").toInt(20260812);
    if (domVersion_ <= 0 || domVersion_ > gDOMVersionCurrent)
        domVersion_ = gDOMVersionCurrent;
    properties->setInt(PROP_REQUESTED_DOM_VERSION, domVersion_);
    // Non-linear content, such as footnote asides and linear="no" spine items,
    // gets its own pages so that page turns can skip it.
    properties->setInt(PROP_NONLINEAR_PAGEBREAK, 1);
    // Loading the dictionaries forces algorithmic hyphenation and ignores lang
    // attributes. Use each language's rules instead. Hyphenation itself stays
    // under the CSS hyphens property.
    properties->setInt(PROP_TEXTLANG_HYPHENATION_ENABLED, 1);
    properties->setInt(PROP_TEXTLANG_HYPH_FORCE_ALGORITHMIC, 0);
    properties->setInt(PROP_TEXTLANG_EMBEDDED_LANGS_ENABLED, 1);
    properties->setString(PROP_TEXTLANG_MAIN_LANG, lString8(TEXTLANG_DEFAULT_MAIN_LANG));
    // The ignore-font-names flag, then the generic families in the order of
    // css_font_family_t: serif, sans-serif, cursive, fantasy, monospace, math,
    // emoji and fangsong. Empty entries keep the engine's choice.
    properties->setString(PROP_FONT_FAMILY_FACES,
                          lString8(QString("|||||%1|%2||")
                                           .arg(monospaceFontFace, mathFontFace)
                                           .toUtf8()
                                           .constData()));
    engine_->propsApply(properties);
    engine_->setViewMode(DVM_PAGES, 1);
    engine_->setVerticalSpacing(verticalSpacing_ == "center"           ? 2
                                        : verticalSpacing_ == "expand" ? 1
                                                                       : 0,
                                verticalSpacingScope_ == "protection");
    engine_->setDefaultFontFace(
            lString8((face_.isEmpty() ? QString(defaultFontFace) : face_).toUtf8().constData()));
    engine_->setFontSize(pixels_);
    // CSS sets the line height; the engine must not multiply it again.
    engine_->setDefaultInterlineSpace(100);
    engine_->setPageMargins(lvRect(margin_, topMargin_, margin_, bottomMargin_));
    engine_->setPageHeaderInfo(0);
    engine_->setShowCover(false);
    engine_->setBackgroundColor(0xffffff);
    engine_->setTextColor(0);
    applyStyleSheet();
    engine_->Resize(size.width(), size.height());
}

int EngineDocument::page() const
{
    return engine_ ? engine_->getCurPage() : 0;
}

int EngineDocument::pages() const
{
    return engine_ ? engine_->getPageCount() : 0;
}

QString EngineDocument::title() const
{
    return engine_ ? qstring(engine_->getTitle()) : QString();
}

QString EngineDocument::author() const
{
    return engine_ ? qstring(engine_->getAuthors()) : QString();
}

QImage EngineDocument::coverImage(QSize maximumSize, const QString &preferredPath,
                                  QString *usedPath) const
{
    if (usedPath)
        usedPath->clear();
    if (maximumSize.isEmpty()
        || qint64(maximumSize.width()) * maximumSize.height() > 4 * 1024 * 1024) {
        return {};
    }
    const auto decode = [maximumSize](LVStreamRef stream) -> QImage {
        if (stream.isNull()) {
            return {};
        }
        const auto length = stream->GetSize();
        if (!length || length > ResourceCache::budgetBytes) {
            return {};
        }
        // A resource stream can decode a placeholder while its image is deferred.
        // Copy the cover bytes before rendering an image that must outlive the book.
        auto bytes = LVCreateMemoryStream(stream);
        if (bytes.isNull()) {
            return {};
        }
        auto cover = LVCreateStreamImageSource(bytes);
        // Unrecognised formats produce a dummy image with no source stream.
        // It must not prevent the next cover source from being tried.
        if (cover.isNull() || !cover->GetSourceStream() || cover->GetWidth() <= 0
            || cover->GetHeight() <= 0) {
            return {};
        }
        if (quint64(cover->GetWidth()) * quint64(cover->GetHeight()) > imagePixelLimit) {
            return {};
        }
        auto size = QSize(cover->GetWidth(), cover->GetHeight());
        if (size.width() > maximumSize.width() || size.height() > maximumSize.height()) {
            size.scale(maximumSize, Qt::KeepAspectRatio);
        }
        if (size.isEmpty()) {
            return {};
        }
        QImage image(size, QImage::Format_RGB32);
        if (image.isNull()) {
            return {};
        }
        image.fill(Qt::white);
        PageDrawBuffer buffer(image, false, true);
        buffer.Draw(cover, 0, 0, size.width(), size.height(), false);
        // CREngine leaves the high byte unused; Qt RGB32 requires opaque pixels.
        const int width = image.width(), height = image.height();
        for (int y = 0; y < height; ++y) {
            auto *pixels = reinterpret_cast<quint32 *>(image.scanLine(y));
            for (int x = 0; x < width; ++x) {
                pixels[x] |= 0xff000000U;
            }
        }
        return image;
    };
    if (!preferredPath.isEmpty()) {
        const auto image = decode(LVOpenFileStream(cre(preferredPath).c_str(), LVOM_READ));
        if (!image.isNull()) {
            if (usedPath)
                *usedPath = preferredPath;
            return image;
        }
    }
    return engine_ ? decode(engine_->getCoverPageImageStream()) : QImage();
}

QString EngineDocument::position() const
{
    return engine_ ? anchor_ : QString();
}

bool EngineDocument::go(int page)
{
    if (!engine_ || page < 0 || page >= pages())
        return false;
    // goToPage updates its bookmark before marking the position initialized.
    // Resolve the lazy initial/restored position first, or it can reset this jump.
    engine_->getCurPage();
    const bool moved = engine_->goToPage(page);
    if (moved)
        anchor_ = qstring(engine_->getBookmark().toString());
    trim();
    return moved;
}

int EngineDocument::turnTarget(int from, int delta) const
{
    if (!engine_ || !delta || from < 0 || from >= pages())
        return from;
    const int flow = engine_->getPageFlow(from), step = delta > 0 ? 1 : -1;
    int page = from;
    for (int turns = qAbs(delta); turns > 0; --turns) {
        int next = page + step;
        // When hiding, stay in the linear text or the non-linear section already open.
        while (hideNonlinearFragments_ && next >= 0 && next < pages()
               && engine_->getPageFlow(next) != 0 && engine_->getPageFlow(next) != flow)
            next += step;
        if (next < 0 || next >= pages())
            break;
        page = next;
    }
    return page;
}

bool EngineDocument::goTo(const QString &position)
{
    if (!engine_)
        return false;
    auto pointer = engine_->getDocument()->createXPointer(cre(position));
    if (pointer.isNull())
        return false;
    engine_->goToBookmark(pointer);
    anchor_ = position;
    trim();
    return true;
}

bool EngineDocument::style(int pixels, int spacing)
{
    return style(pixels, spacing, face_, margin_);
}

bool EngineDocument::style(int pixels, int spacing, const QString &face, int margin)
{
    return style(pixels, spacing, face, margin, alignment_);
}

bool EngineDocument::style(int pixels, int spacing, const QString &face, int margin,
                           const QString &alignment)
{
    return applyAppearance(kobalt::qt::object({ { "font_pixels", pixels },
                                                { "line_spacing", spacing },
                                                { "font_face", face },
                                                { "margin_pixels", margin },
                                                { "text_alignment", alignment } }));
}

bool EngineDocument::applyAppearance(const QJsonObject &settings, QSize size,
                                     OpeningProgress progress)
{
    const int pixels = settings.value("font_pixels").toInt(pixels_);
    const int spacing = settings.value("line_spacing").toInt(spacing_);
    const int paragraphSpacing = settings.contains("paragraph_spacing")
            ? appearance::paragraphSpacing(settings.value("paragraph_spacing"))
            : paragraphSpacing_;
    if (settings.contains("paragraph_spacing")
        && settings.value("paragraph_spacing") != paragraphSpacing)
        return false;
    const int paragraphIndent = settings.contains("paragraph_indent")
            ? appearance::paragraphIndent(settings.value("paragraph_indent"))
            : paragraphIndent_;
    if (settings.contains("paragraph_indent")
        && settings.value("paragraph_indent") != paragraphIndent)
        return false;
    for (const auto *key : appearance::normalizationKeys)
        if (settings.contains(key) && !settings.value(key).isBool())
            return false;
    const bool beautifierInitials =
            settings.value("beautifier_initials").toBool(beautifierInitials_);
    const bool beautifierSpacing = settings.value("beautifier_spacing").toBool(beautifierSpacing_);
    const bool beautifierIndent = settings.value("beautifier_indent").toBool(beautifierIndent_);
    const int margin = settings.value("margin_pixels").toInt(margin_);
    const int topMargin = settings.value("top_margin_pixels").toInt(topMargin_);
    const int bottomMargin = settings.value("bottom_margin_pixels").toInt(bottomMargin_);
    const int weight = settings.value("weight_steps").toInt(weight_);
    const auto bionic = settings.value("bold_reading").toString(bionic_);
    const auto face = settings.value("font_face").toString(face_);
    const auto defaultFace = settings.value("default_font_face").toString(defaultFace_);
    auto fallbackFaces = fallbackFaces_;
    for (size_t index = 0; index < fallbackFaces.size(); ++index) {
        fallbackFaces[index] =
                settings.value(appearance::fallbackFontKeys[index]).toString(fallbackFaces[index]);
        if (!fallbackFaces[index].isEmpty() && !fontFaces().contains(fallbackFaces[index]))
            return false;
    }
    const auto axis = fontWeightAxis(readingFonts, face);
    const int requestedAxisWeight = settings.value("font_weight").toInt(axisWeight_);
    const int axisWeight = axis.available()
            ? qBound(axis.minimum, requestedAxisWeight, axis.maximum)
            : requestedAxisWeight;
    const auto optical = fontOpticalSizeAxis(readingFonts, face);
    const double requestedOptical = settings.value("font_optical_size").toDouble(opticalSize_);
    if (!std::isfinite(requestedOptical) || requestedOptical < 0)
        return false;
    const double opticalSize = normalizedOpticalSize(optical, requestedOptical);
    const bool opticalChanged = opticalSize != opticalSize_;
    const bool axisChanged = axis.available() && axisWeight != axisWeight_;
    const auto alignment = settings.value("text_alignment").toString(alignment_);
    const auto hyphenation = settings.value("hyphenation").toString(hyphenation_);
    const bool dark = settings.value("dark_mode").toBool(darkMode_);
    const bool hideNonlinear =
            settings.value("hide_nonlinear_fragments").toBool(hideNonlinearFragments_);
    const bool normalizePageMargins =
            settings.value("normalize_page_margins").toBool(normalizePageMargins_);
    const bool reduceChapterSpacing =
            settings.value("reduce_chapter_spacing").toBool(reduceChapterSpacing_);
    const auto verticalSpacing = settings.contains("vertical_justification")
            ? appearance::verticalSpacingMode(settings.value("vertical_justification"))
            : verticalSpacing_;
    const auto verticalScope = settings.contains("vertical_spacing_scope")
            ? appearance::verticalSpacingScope(settings.value("vertical_spacing_scope"))
            : verticalSpacingScope_;
    const bool verticalChanged =
            verticalSpacing != verticalSpacing_ || verticalScope != verticalSpacingScope_;
    const int wordSpacing = settings.value("word_spacing").toInt(wordSpacing_);
    const int tracking = settings.value("justification_tracking").toInt(justificationTracking_);
    const bool hanging = settings.value("hanging_punctuation").toBool(hangingPunctuation_);
    if (size.width() < 320 || size.height() < 240)
        size = size_;
    const bool landscapeColumns =
            settings.value("two_columns_landscape").toBool(twoColumnsLandscape_);
    const int columns = landscapeColumns
            ? (size.width() > size.height() ? 2 : 1)
            : settings.value("columns").toInt(twoColumnsLandscape_ ? 1 : columns_);
    const int gapPercent = settings.value("column_spacing_percent").toInt(columnSpacingPercent_);
    const int columnSpacing = gapPercent ? qRound(pixels * gapPercent / 100.0)
                                         : settings.value("column_spacing").toInt(columnSpacing_);
    const auto widowsOrphans = settings.value("widows_orphans").toString(widowsOrphans_);
    if (!engine_ || (gapPercent != 0 && (gapPercent < 100 || gapPercent > 300)) || columnSpacing < 0
        || (!gapPercent && columnSpacing > maximumMarginPixels()) || columns < 1 || columns > 2
        || pixels < minimumFontPixels() || pixels > maximumFontPixels() || spacing < 100
        || spacing > maximumLineSpacing || margin < 0 || margin > maximumMarginPixels()
        || topMargin < 0 || topMargin > maximumMarginPixels() || bottomMargin < 0
        || bottomMargin > maximumMarginPixels() || weight < 0 || weight > 12
        || !validAlignment(alignment) || !appearance::validTracking(tracking)
        || !appearance::validWordSpacing(wordSpacing)
        || !appearance::validWidowsOrphans(widowsOrphans)
        || (hyphenation != "auto" && hyphenation != "on" && hyphenation != "off")
        || (bionic != "off" && bionic != "bold") || (!face.isEmpty() && !fontFaces().contains(face))
        || !fontFaces().contains(defaultFace))
        return false;
    const bool bionicChanged = bionic != bionic_;
    // Outline thickness changes pixels only. Keep the formatted page and its anchors.
    const bool thicknessChanged = weight != weight_;
    const bool styled = beautifierInitials != beautifierInitials_
            || beautifierSpacing != beautifierSpacing_ || beautifierIndent != beautifierIndent_
            || fallbackFaces != fallbackFaces_ || normalizePageMargins != normalizePageMargins_
            || reduceChapterSpacing != reduceChapterSpacing_ || wordSpacing != wordSpacing_
            || tracking != justificationTracking_ || hanging != hangingPunctuation_
            || widowsOrphans != widowsOrphans_ || bionicChanged || axisChanged || opticalChanged
            || pixels != pixels_ || spacing != spacing_ || paragraphSpacing != paragraphSpacing_
            || paragraphIndent != paragraphIndent_ || face != face_ || defaultFace != defaultFace_
            || margin != margin_ || topMargin != topMargin_ || bottomMargin != bottomMargin_
            || alignment != alignment_ || hyphenation != hyphenation_;
    const bool resized = size != size_ || columns != columns_ || columnSpacing != columnSpacing_;
    twoColumnsLandscape_ = landscapeColumns;
    columnSpacingPercent_ = gapPercent;
    if (!verticalChanged && !styled && !resized && weight == weight_ && dark == darkMode_
        && hideNonlinear == hideNonlinearFragments_)
        return true;
    if (styled || resized || verticalChanged)
        sourceIndex_.reset();
    const QString anchor = position();
    auto *document = engine_->getDocument();
    // Snapshots belong to this live publication and parser instance. Invalidate
    // before any setting or stylesheet edit; only geometry can swap layouts.
    const bool layoutInputsChanged = styled || verticalChanged || layoutPpi_ != readingPpi.load()
            || layoutCss_ != userStyleSheet_;
    if (layoutInputsChanged) {
        document->clearAlternateLayout();
        alternateViewport_ = {};
        layoutPpi_ = readingPpi.load();
        layoutCss_ = userStyleSheet_;
    }
    const bool reuseLayout = !layoutInputsChanged && resized && size == alternateViewport_
            && columns == alternateColumns_ && columnSpacing == alternateColumnSpacing_;
    const auto previousSize = size_;
    const int previousColumns = columns_, previousSpacing = columnSpacing_;
    if (resized && !reuseLayout && !layoutInputsChanged) {
        if (document->retainLayout(maxAlternateLayoutBytes)) {
            alternateViewport_ = size_;
            alternateColumns_ = columns_;
            alternateColumnSpacing_ = columnSpacing_;
        } else {
            alternateViewport_ = {};
        }
    }
    OpeningCallback callback(engine_, std::move(progress));
    pixels_ = pixels;
    spacing_ = spacing;
    paragraphSpacing_ = paragraphSpacing;
    paragraphIndent_ = paragraphIndent;
    beautifierInitials_ = beautifierInitials;
    beautifierSpacing_ = beautifierSpacing;
    beautifierIndent_ = beautifierIndent;
    face_ = face;
    defaultFace_ = defaultFace;
    fallbackFaces_ = fallbackFaces;
    margin_ = margin;
    topMargin_ = topMargin;
    bottomMargin_ = bottomMargin;
    alignment_ = alignment;
    hyphenation_ = hyphenation;
    weight_ = weight;
    axisWeight_ = axisWeight;
    opticalSize_ = opticalSize;
    bionic_ = bionic;
    darkMode_ = dark;
    activateFontParameters();
    hideNonlinearFragments_ = hideNonlinear;
    normalizePageMargins_ = normalizePageMargins;
    reduceChapterSpacing_ = reduceChapterSpacing;
    verticalSpacing_ = verticalSpacing;
    verticalSpacingScope_ = verticalScope;
    engine_->setVerticalSpacing(verticalSpacing_ == "center"           ? 2
                                        : verticalSpacing_ == "expand" ? 1
                                                                       : 0,
                                verticalSpacingScope_ == "protection");
    wordSpacing_ = wordSpacing;
    justificationTracking_ = tracking;
    hangingPunctuation_ = hanging;
    engine_->setHangingPunctuation(hangingPunctuation_);
    widowsOrphans_ = widowsOrphans;
    if (styled) {
        setExtraFontWeight(0);
        auto properties = LVCreatePropsContainer();
        properties->setInt(PROP_FONT_BASE_WEIGHT, LVRendGetBaseFontWeight());
        properties->setInt(PROP_FORMAT_SPACE_WIDTH_SCALE_PERCENT, wordSpacing_);
        properties->setInt(PROP_FORMAT_MAX_ADDED_LETTER_SPACING_PERCENT, justificationTracking_);
        properties->setInt(PROP_FLOATING_PUNCTUATION, hangingPunctuation_);
        properties->setInt(PROP_RENDER_BLOCK_RENDERING_FLAGS,
                           reduceChapterSpacing_ ? DEF_RENDER_BLOCK_RENDERING_FLAGS
                                                 : DEF_RENDER_BLOCK_RENDERING_FLAGS
                                           & ~BLOCK_RENDERING_DISCARD_FORCED_BREAK_MARGINS);
        engine_->propsApply(properties);
        engine_->requestRender();
        applyPublisherFontFallbacks();
        engine_->setPageMargins(lvRect(margin, topMargin, margin, bottomMargin));
        engine_->setFontSize(pixels);
        engine_->setDefaultInterlineSpace(100);
        applyStyleSheet();
    }
    if (bionicChanged) {
        if (!cacheFingerprint_.isEmpty())
            engine_->getDocProps()->setHex(
                    DOC_PROP_FILE_CRC32,
                    documentCacheChecksum(documentCachePolicy() + ":bionic-v3:" + bionic_.toUtf8(),
                                          domVersion_));
        engine_->getDocument()->getProps()->setInt("kobalt.bionic.mode", bionic_ == "bold" ? 1 : 0);
        engine_->requestRender();
    }
    if (resized) {
        columns_ = columns;
        columnSpacing_ = columnSpacing;
        engine_->setColumns(columns_);
        engine_->setColumnSpacing(columnSpacing_);
        size_ = size;
        engine_->Resize(size.width(), size.height());
    }
    cache_.clear();
    spareImage_ = {};
    damage_.clear();
    if (thicknessChanged) {
        engine_->clearImageCache();
        // Rebuild drawn paragraphs without repaginating or changing anchors.
        engine_->getDocument()->clearRendBlockCache();
    }
    if (!styled && !resized)
        return true;
    // Applying a panel can change typography, hyphenation and the viewport together.
    // Set all three before paginating so the book is laid out only once.
    if (reuseLayout) {
        if (engine_->swapLayout()) {
            alternateViewport_ = previousSize;
            alternateColumns_ = previousColumns;
            alternateColumnSpacing_ = previousSpacing;
        } else {
            alternateViewport_ = {};
        }
    }
    engine_->Render();
    return goTo(anchor);
}

bool EngineDocument::setThickness(int steps)
{
    if (steps < 0 || steps > 12)
        return false;
    return applyAppearance(kobalt::qt::object({ { "weight_steps", steps } }));
}

bool EngineDocument::setWeight(int steps)
{
    const auto axis = weightAxis();
    if (axis.available() && (steps < axis.minimum || steps > axis.maximum))
        return false;
    return applyAppearance(
            kobalt::qt::object({ { axis.available() ? "font_weight" : "weight_steps", steps } }));
}

bool EngineDocument::setOpticalSize(double points)
{
    const auto axis = opticalSizeAxis();
    if (!axis.available() || !std::isfinite(points)
        || (points < axis.minimum || points > axis.maximum) || points != std::floor(points))
        return false;
    return applyAppearance(kobalt::qt::object({ { "font_optical_size", points } }));
}

bool EngineDocument::setHyphenation(const QString &mode)
{
    return applyAppearance(kobalt::qt::object({ { "hyphenation", mode } }));
}

void EngineDocument::applyPublisherFontFallbacks()
{
    const bool publisher = face_.isEmpty();
    const bool useDefaults = publisher && embeddedFontCount() == 0;
    const auto defaultSans =
            fontFaces().contains(publisherSansFontFace) ? publisherSansFontFace : "IBM Plex Sans";
    // The first field is empty so named publisher and embedded fonts keep precedence.
    // Explicit family choices also resolve generic CSS in books with embedded fonts.
    const auto serif = publisher
            ? (fallbackFaces_[0].isEmpty() ? (useDefaults ? QString(defaultFontFace) : QString())
                                           : fallbackFaces_[0])
            : QString();
    const auto sans = publisher
            ? (fallbackFaces_[1].isEmpty() ? (useDefaults ? QString(defaultSans) : QString())
                                           : fallbackFaces_[1])
            : QString();
    const auto mono = fallbackFaces_[2].isEmpty() ? QString(monospaceFontFace) : fallbackFaces_[2];
    const auto families = QString("|%1|%2|||%3|%4||").arg(serif, sans, mono, mathFontFace);
    auto properties = LVCreatePropsContainer();
    properties->setString(PROP_FONT_FAMILY_FACES, lString8(families.toUtf8().constData()));
    engine_->propsApply(properties);
    const auto fallback =
            fallbackFaces_[0].isEmpty() ? QString(defaultFontFace) : fallbackFaces_[0];
    engine_->setDefaultFontFace(lString8((publisher ? fallback : face_).toUtf8().constData()));
}

void EngineDocument::applyStyleSheet()
{
    QString overrides;
    if (normalizePageMargins_) {
        overrides = "html,body,DocFragment { margin:0 !important; padding:0 !important; }\n";
    }
    overrides +=
            "svg[data-kobalt-fullpage=\"1\"] { width:100% !important; height:100vh !important; }\n";
    // A one-screen-pixel boundary stops a leading illustration's margin from
    // collapsing through the body and disappearing at the page boundary.
    // Round upward in the engine's 1/256 CSS-pixel units so every density keeps it.
    const double boundaryCssPixels = qCeil(256.0 * 96 / readingPpi.load()) / 256.0;
    if (normalizePageMargins_) {
        overrides +=
                QString("body[data-kobalt-leading-image=\"1\"] { padding-top:%1px !important; }\n")
                        .arg(boundaryCssPixels, 0, 'f', 6);
    }
    if (hyphenation_ != "auto")
        overrides += QString("* { hyphens:%1 !important; }\n")
                             .arg(hyphenation_ == "on" ? "auto" : "none");
    // Explicit choices replace publisher declarations on every element, including
    // paragraphs that do not inherit the body's values. Publisher adds no rule.
    if (widowsOrphans_ != "publisher")
        overrides += QString("* { orphans:%1 !important; widows:%1 !important; }\n")
                             .arg(widowsOrphans_ == "avoid" ? 2 : 1);
    if (!face_.isEmpty()) {
        auto escaped = face_;
        escaped.replace('\\', "\\\\");
        escaped.replace('"', "\\\"");
        overrides += "* { font-family: \"" + escaped
                + "\" !important; }\n"
                  // Code keeps its fixed width; MathML keeps the math font and its layout metrics.
                  "pre, code, kbd, samp, tt, pre *, code *, kbd *, samp *, tt * { font-family: monospace !important; }\n"
                  "math, math * { font-family: math !important; }\n";
    }
    // Like Nickel's line-height override, leave inline spans and media alone.
    // Relative font sizes remain publisher CSS. A percentage resolves against the
    // font size of the element that sets it, and children inherit the result, so
    // each block that holds text, table cells and captions included, sets its own.
    overrides +=
            QString("body,p,div,section,article,blockquote,li,dt,dd,h1,h2,h3,h4,h5,h6,td,th,caption,figcaption { line-height:%1% !important; }")
                    .arg(spacing_);
    {
        const QString prose =
                "p[data-kobalt-beautify=\"body\"], p[data-kobalt-beautify=\"opening\"], p[data-kobalt-beautify=\"first-letter\"]";
        if (beautifierSpacing_)
            overrides += prose + " { margin-bottom:0 !important; padding-bottom:0 !important; }\n";
        if (beautifierIndent_)
            overrides += prose
                    + " { text-indent:1.2em !important; }\n"
                      "p[data-kobalt-beautify=\"opening\"], p[data-kobalt-beautify=\"first-letter\"] { text-indent:0 !important; }\n";
        if (beautifierInitials_)
            overrides +=
                    "p[data-kobalt-beautify=\"first-letter\"]::first-letter, [data-kobalt-first-letter=\"1\"]::first-letter, span[data-kobalt-initial=\"1\"] { font-size:1em !important; font-family:inherit !important; font-weight:inherit !important; font-style:inherit !important; float:none !important; line-height:inherit !important; margin:0 !important; padding:0 !important; initial-letter:normal !important; }\n";
    }
    if (paragraphSpacing_ >= 0)
        overrides +=
                QString("p[data-kobalt-paragraph=\"1\"] { margin-top:0 !important; margin-bottom:%1em !important; padding-top:0 !important; padding-bottom:0 !important; }\n")
                        .arg(paragraphSpacing_ / 100.0, 0, 'f', 2);
    if (paragraphIndent_ >= 0)
        overrides += QString("p[data-kobalt-paragraph=\"1\"] { text-indent:%1em !important; }\n")
                             .arg(paragraphIndent_ / 100.0, 0, 'f', 2);
    // Only marked text blocks override publisher alignment; decorative blocks keep it.
    if (alignment_ != "publisher")
        overrides +=
                QString("[data-kobalt-prose=\"1\"], [data-kobalt-prose=\"1\"] span { text-align:%1 !important; text-align-last:auto !important; }")
                        .arg(alignment_);
    engine_->setStyleSheet(lString8(readerHtmlCss) + lString8(readerOverridesCss)
                           + lString8(overrides.toUtf8().constData()) + lString8("\n")
                           + lString8(userStyleSheet_.constData()));
}

void EngineDocument::resize(QSize size)
{
    applyAppearance({}, size);
}

void EngineDocument::prepareDamage(int fromPage, int toPage)
{
    const auto from = cache_.value(fromPage), to = cache_.value(toPage);
    if (from.isNull() || to.isNull())
        return;
    // One rectangle serves the turn in either direction.
    const auto key =
            qMakePair(qMin(from.cacheKey(), to.cacheKey()), qMax(from.cacheKey(), to.cacheKey()));
    if (!damage_.contains(key))
        damage_.insert(key, kobalt::render::imageDamage(from, to));
}

namespace {
std::atomic<int> pageFormat{ QImage::Format_RGB32 };

// CREngine draws into RGB32 buffers. Both page formats store four bytes per
// pixel, so a buffer changes format without copying its pixels.
void usePageFormat(QImage &image, QImage::Format format)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    if (image.format() != format)
        image.reinterpretAsFormat(format);
#else
    Q_UNUSED(image);
    Q_UNUSED(format);
#endif
}
} // namespace

void EngineDocument::setPageFormat(QImage::Format format)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    pageFormat = format == QImage::Format_RGBX8888 ? QImage::Format_RGBX8888 : QImage::Format_RGB32;
#else
    Q_UNUSED(format);
#endif
}

bool EngineDocument::preparedDamage(qint64 fromKey, qint64 toKey, QRect *damage) const
{
    const auto found = damage_.constFind(qMakePair(qMin(fromKey, toKey), qMax(fromKey, toKey)));
    if (found == damage_.cend())
        return false;
    *damage = found.value();
    return true;
}

void EngineDocument::trim()
{
    const int current = page(), previous = turnTarget(current, -1), next = turnTarget(current, 1);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it.key() != current && it.key() != previous && it.key() != next) {
            // Session and GUI images share these pixels. Only the document's last
            // reference can become a draw buffer for a different page.
            QImage retired = std::move(it.value());
            it = cache_.erase(it);
            if (spareImage_.isNull() && retired.isDetached() && retired.size() == size_
                && (retired.format() == QImage::Format_RGB32
                    || retired.format() == QImage::Format_RGBX8888)) {
                usePageFormat(retired, QImage::Format_RGB32);
                spareImage_ = std::move(retired);
            }
        } else
            ++it;
    }
    // Keep only rectangles whose two images are both still cached.
    QList<qint64> keys;
    for (const auto &image : cache_)
        keys.append(image.cacheKey());
    for (auto it = damage_.begin(); it != damage_.end();)
        if (!keys.contains(it.key().first) || !keys.contains(it.key().second))
            it = damage_.erase(it);
        else
            ++it;
}

void EngineDocument::setDarkMode(bool enabled)
{
    applyAppearance(kobalt::qt::object({ { "dark_mode", enabled } }));
}

bool EngineDocument::loadNextImage()
{
    if (!source_ || !source_->resources)
        return false;
    source_->resources->loadNextImage();
    return refreshImages();
}

size_t EngineDocument::alternateLayoutBytes() const
{
    return engine_ ? engine_->getDocument()->alternateLayoutBytes() : 0;
}

void EngineDocument::releaseImages()
{
    if (engine_)
        engine_->getDocument()->clearAlternateLayout();
    alternateViewport_ = {};
    cache_.clear();
    spareImage_ = {};
    damage_.clear();
    if (engine_)
        engine_->clearImageCache();
    if (source_ && source_->resources) {
        source_->resources->releaseBytes();
        imageRevision_ = source_->resources->imageRevision();
        imageInversion_.clear();
    }
}

bool EngineDocument::refreshImages()
{
    if (!engine_ || !source_ || !source_->resources
        || imageRevision_ == source_->resources->imageRevision())
        return false;
    // Appearance previews share resources but keep their own rendered pages.
    // A load in either document must invalidate placeholders in the other one.
    imageRevision_ = source_->resources->imageRevision();
    imageInversion_.clear();
    cache_.clear();
    spareImage_ = {};
    damage_.clear();
    engine_->clearImageCache();
    return true;
}

QImage EngineDocument::imageFromAnchor(bool stayInChapter)
{
    if (!engine_ || pages() <= 0)
        return {};
    const int current = page();
    auto *document = engine_->getDocument();
    auto *list = engine_->getPageList();
    const auto anchor = document->createXPointer(cre(anchor_));
    if (columns_ != 1 || anchor.isNull() || engine_->isVerticalText())
        return image(current);
    const int pageIndex = engine_->getBookmarkPage(anchor);
    if (pageIndex < 0 || pageIndex >= list->length()
        || (list->get(pageIndex)->flags & RN_PAGE_TYPE_COVER))
        return image(current);
    const int screen = size_.height() - topMargin_ - bottomMargin_;
    auto chapterLimits = [&] {
        int first = 0;
        int last = list->get(list->length() - 1)->start + list->get(list->length() - 1)->height;
        if (!stayInChapter)
            return qMakePair(first, last);
        const auto links = chapters();
        const auto around = chapterAround(links, anchor_);
        if (around.current >= 0 && links[around.current].page >= 0
            && links[around.current].page < list->length())
            first = list->get(links[around.current].page)->start;
        if (around.next >= 0 && links[around.next].page >= 0
            && links[around.next].page < list->length())
            last = list->get(links[around.next].page)->start;
        return qMakePair(first, last);
    }();
    int start = anchor.toPoint().y;
    if (start < 0 || screen <= 0)
        return image(current);
    if (chapterLimits.second - start < screen && chapterLimits.second > chapterLimits.first) {
        const int wanted = qMax(chapterLimits.first, chapterLimits.second - screen);
        const auto line =
                document->createXPointer(lvPoint(0, wanted), PT_DIR_SCAN_FORWARD_LOGICAL_FIRST);
        const int y = line.isNull() ? -1 : line.toPoint().y;
        if (y >= 0 && y < start)
            start = y;
    }
    // Stop after the last whole line, the same way that a page break does.
    int height = qMin(screen, chapterLimits.second - start);
    const auto last = document->createXPointer(lvPoint(0, start + height - 1),
                                               PT_DIR_SCAN_BACKWARD_LOGICAL_FIRST);
    lvRect box;
    if (!last.isNull() && last.getRect(box) && box.bottom > start) {
        const int whole = box.bottom <= start + height ? box.bottom - start : box.top - start;
        if (whole > 0)
            height = whole;
    }
    // Page drawing reads the start and the height from the page list. Lend this
    // entry for one draw, on the worker that owns the layout.
    auto *entry = list->get(pageIndex);
    const int savedStart = entry->start, savedHeight = entry->height;
    const auto kept = cache_.value(pageIndex);
    const auto keptDamage = damage_;
    cache_.remove(pageIndex);
    entry->start = start;
    entry->height = height;
    auto result = image(pageIndex);
    entry->start = savedStart;
    entry->height = savedHeight;
    cache_.remove(pageIndex);
    if (!kept.isNull())
        cache_.insert(pageIndex, kept);
    damage_ = keptDamage;
    return result;
}

QImage EngineDocument::image(int pageNumber)
{
    activateFontParameters();
    setExtraFontWeight(bionic_ == "off" ? weight_ : 0);
    if (!engine_ || pageNumber < 0 || pageNumber >= pages())
        return {};
    refreshImages();
    if (cache_.contains(pageNumber))
        return cache_.value(pageNumber);
    QImage result;
    if (spareImage_.isDetached() && spareImage_.size() == size_
        && spareImage_.format() == QImage::Format_RGB32)
        result = std::move(spareImage_);
    else
        result = QImage(size_, QImage::Format_RGB32);
    if (result.isNull())
        return {};
    PageDrawBuffer buffer(result, darkMode_, true, &imageInversion_);
    if (columns_ == 1) {
        engine_->drawColumn(buffer, pageNumber, 0);
    } else {
        result.fill(Qt::white);
        QImage column(engine_->columnWidth(), size_.height(), QImage::Format_RGB32);
        if (column.isNull())
            return {};
        QPainter painter(&result);
        for (int i = 0; i < columns_; ++i) {
            PageDrawBuffer part(column, darkMode_, true, &imageInversion_);
            engine_->drawColumn(part, pageNumber, i);
            buffer.hasImages |= part.hasImages;
            buffer.hasText |= part.hasText;
            const int x = engine_->columnOffset(i);
            painter.drawImage(x, 0, column);
            for (const auto &region : part.imageRegions)
                buffer.imageRegions.append(region.translated(x, 0));
        }
    }
    // CREngine composites into 0x00RRGGBB. Qt's opaque RGB32 contract requires
    // 0xffRRGGBB, including when a painter uses this image as a source.
    bool hasColor = false;
    constexpr int tileSize = 32;
    const int tileColumns = result.width() / tileSize + (result.width() % tileSize != 0);
    const int tileRows = result.height() / tileSize + (result.height() % tileSize != 0);
    const bool boundedTiles = qint64(tileColumns) * tileRows <= 4096;
    QVector<uchar> colorTiles(boundedTiles ? tileColumns * tileRows : 0, 0);
    const auto format = QImage::Format(pageFormat.load());
    hasColor = normalizePagePixels(result, darkMode_, boundedTiles ? &colorTiles : nullptr,
                                   tileColumns, format == QImage::Format_RGBX8888);
    usePageFormat(result, format);
    result.setText("kobalt.image_regions", regionMetadata(buffer.imageRegions));
    result.setText("kobalt.image_only",
                   buffer.imageRegions.size() == 1 && !buffer.hasText ? "true" : "false");
    result.setText(
            "kobalt.colour_regions",
            regionMetadata(
                    hasColor ? (boundedTiles ? colorTileRegions(colorTiles, tileColumns, tileRows,
                                                                result.size())
                                             : QVector<QRect>{ QRect(QPoint(0, 0), result.size()) })
                             : QVector<QRect>{}));
    result.setText("kobalt.animation_text_only", !buffer.hasImages && !hasColor ? "true" : "false");
    // A turn can skip several physical pages of non-linear text.
    const int current = page();
    if (pageNumber == current || pageNumber == turnTarget(current, -1)
        || pageNumber == turnTarget(current, 1))
        cache_.insert(pageNumber, result);
    trim();
    return result;
}

QVector<ChapterLink> EngineDocument::chapters() const
{
    if (!engine_)
        return {};
    return chapterIndex(contentsIndex_, contents_, contents_.isEmpty()).links;
}

quint64 EngineDocument::layoutSignature() const
{
    if (!engine_)
        return 0;
    // CREngine's extended rendering hash tracks pagination settings, page
    // count and document height. It also survives document-cache restoration.
    // Mix the document identity so reopening cannot reuse another book's index.
    quint64 hash = 1469598103934665603ULL;
    const auto mix = [&hash](quint64 value) { hash = (hash ^ value) * 1099511628211ULL; };
    mix(quint64(quintptr(engine_->getDocument())));
    mix(engine_->getDocumentRenderingHash(true));
    mix(columns_);
    mix(verticalSpacing_ == "center" ? 2 : verticalSpacing_ == "expand" ? 1 : 0);
    mix(verticalSpacingScope_ == "protection");
    return hash ? hash : 1;
}

const EngineDocument::ChapterIndex &EngineDocument::chapterIndex(ChapterIndex &index,
                                                                 const QVector<ChapterLink> &source,
                                                                 bool toc) const
{
    const auto layout = layoutSignature();
    // `source` shares its data with the cached copy until it changes.
    if (index.layout == layout && index.toc == toc
        && index.source.constData() == source.constData())
        return index;
    index = {};
    index.layout = layout;
    index.toc = toc;
    index.source = source;
    index.links = toc ? tocChapters() : chapterPages(source);
    auto *doc = engine_->getDocument();
    QVector<ldomXPointerEx> pointers(index.links.size());
    index.sorted = true;
    for (int i = 0; i < index.links.size(); ++i) {
        if (index.links[i].page < 0 || index.links[i].page >= pages())
            continue;
        pointers[i] = ldomXPointerEx(doc->createXPointer(cre(index.links[i].position)));
        if (pointers[i].isNull())
            continue;
        if (pointers[i].isElement() && pointers[i].getOffset() > 0)
            index.sorted = false;
        index.order.append(i);
    }
    if (index.sorted)
        std::stable_sort(index.order.begin(), index.order.end(), [&pointers](int a, int b) {
            return pointers[a].compare(pointers[b]) < 0;
        });
    else
        index.order.clear();
    return index;
}

QVector<ChapterLink> EngineDocument::tocChapters() const
{
    QVector<ChapterLink> result;
    if (!engine_)
        return result;
    std::function<void(LVTocItem *, int)> visit = [&](LVTocItem *item, int depth) {
        if (!item || depth > 32 || result.size() >= 1024)
            return;
        for (int i = 0; i < item->getChildCount() && result.size() < 1024; ++i) {
            auto *child = item->getChild(i);
            auto pointer = child->getXPointer();
            const int start = pointer.isNull() || pointer.toPoint().y < 0
                    ? -1
                    : engine_->getBookmarkPage(pointer);
            result.append({ qstring(child->getName()), qstring(pointer.toString()), start, depth });
            visit(child, depth + 1);
        }
    };
    visit(engine_->getToc(), 0);
    return result;
}

void EngineDocument::setChapters(QVector<ChapterLink> contents, QVector<ChapterLink> graph)
{
    if (!engine_)
        return;
    // An unresolved native list must not hide the publication's usable TOC.
    // Keep partially resolved lists and the native word-count graph intact.
    if (std::none_of(contents.cbegin(), contents.cend(),
                     [](const ChapterLink &chapter) { return !chapter.position.isEmpty(); })) {
        contents.clear();
    }
    contents_ = std::move(contents);
    graph_ = std::move(graph);
}

QVector<ChapterLink> EngineDocument::chapterPages(QVector<ChapterLink> links) const
{
    if (!engine_)
        return {};
    for (auto &link : links) {
        auto pointer = engine_->getDocument()->createXPointer(cre(link.position));
        link.page = pointer.isNull() || pointer.toPoint().y < 0 ? -1
                                                                : engine_->getBookmarkPage(pointer);
    }
    return links;
}

QVector<ChapterLink> EngineDocument::chapterGraph() const
{
    if (!engine_)
        return {};
    return chapterIndex(graphIndex_, graph_, false).links;
}

int EngineDocument::chapterEndPage(const ChapterLink &next) const
{
    if (columns_ == 1)
        return next.page;
    const auto pointer = engine_->getDocument()->createXPointer(cre(next.position));
    const int column = pointer.isNull() ? -1 : engine_->getBookmarkPage(pointer, true);
    // A chapter starting in the second column shares this screen with the
    // preceding chapter. Round its exclusive end up before counting screens.
    return column < 0 ? next.page : (column + columns_ - 1) / columns_;
}

ChapterSelection EngineDocument::chapterSelection(const QVector<ChapterLink> &links) const
{
    auto result = chapterAround(links, anchor_);
    if (result.current >= 0) {
        const int first = links[result.current].page;
        const int last =
                result.next < 0 ? pages() - 1 : qMax(first, chapterEndPage(links[result.next]) - 1);
        result.page = qBound(1, page() - first + 1, last - first + 1);
        result.pages = last - first + 1;
    }
    return result;
}

QString EngineDocument::chapterTitle(const QString &position) const
{
    const auto links = chapters();
    const auto selected = chapterAround(links, position);
    if (selected.current >= 0) {
        const auto title = links[selected.current].title.trimmed();
        return title.isEmpty() ? strings::get("reader.untitled_chapter") : title;
    }
    return selected.next >= 0 ? strings::get("reader.front_matter") : QString();
}

ChapterSelection EngineDocument::chapterAround(const QVector<ChapterLink> &links,
                                               const QString &position) const
{
    ChapterSelection result;
    if (!engine_)
        return result;
    auto *doc = engine_->getDocument();
    const ldomXPointerEx current(doc->createXPointer(cre(position)));
    if (current.isNull())
        return result;
    // Lists from chapters() and chapterGraph() for this layout are sorted
    // already, so a binary search resolves only a few entries.
    const auto layout = layoutSignature();
    for (const auto *index : { &contentsIndex_, &graphIndex_ }) {
        if (!index->sorted || index->layout != layout
            || index->links.constData() != links.constData() || index->links.size() != links.size())
            continue;
        const auto &order = index->order;
        int low = 0, high = order.size();
        bool resolved = true;
        while (low < high) {
            const int middle = low + (high - low) / 2;
            const ldomXPointerEx pointer(doc->createXPointer(cre(links[order[middle]].position)));
            if (pointer.isNull()) {
                resolved = false;
                break;
            }
            if (pointer.compare(current) <= 0)
                low = middle + 1;
            else
                high = middle;
        }
        if (!resolved)
            break;
        // The last entry at or before `position` is current; with equal
        // positions the later list entry wins, as in the scan below.
        result.current = low > 0 ? order[low - 1] : -1;
        result.next = low < order.size() ? order[low] : -1;
        return result;
    }
    ldomXPointerEx start, end;
    for (int i = 0; i < links.size(); ++i) {
        if (links[i].page < 0 || links[i].page >= pages())
            continue;
        const ldomXPointerEx pointer(doc->createXPointer(cre(links[i].position)));
        if (pointer.isNull())
            continue;
        // Page numbers cannot distinguish two chapter starts on the same page.
        // Equal source positions keep the last entry in the supplied list.
        if (pointer.compare(current) <= 0) {
            if (start.isNull() || pointer.compare(start) >= 0) {
                start = pointer;
                result.current = i;
            }
        } else if (end.isNull() || pointer.compare(end) < 0) {
            end = pointer;
            result.next = i;
        }
    }
    return result;
}

ChapterProgress EngineDocument::chapterProgress() const
{
    const auto links = chapters();
    if (!contents_.isEmpty()) {
        const auto selected = chapterSelection(links);
        if (selected.current >= 0) {
            const auto title = links[selected.current].title.trimmed();
            return { title.isEmpty() ? strings::get("reader.untitled_chapter") : title,
                     selected.page, selected.pages };
        }
        if (selected.next >= 0)
            return { strings::get("reader.front_matter"), page() + 1,
                     qMax(1, chapterEndPage(links[selected.next])) };
        return {};
    }
    const int current = page();
    int start = -1;
    const ChapterLink *next = nullptr;
    ChapterProgress progress;
    bool found = false;
    for (const auto &chapter : links) {
        if (chapter.page < 0 || chapter.page >= pages())
            continue;
        found = true;
        if (chapter.page <= current && chapter.page >= start) {
            start = chapter.page;
            progress.title = chapter.title.trimmed();
        } else if (chapter.page > current && (!next || chapter.page < next->page))
            next = &chapter;
    }
    if (!found)
        return progress;
    if (progress.title.isEmpty())
        progress.title = start < 0 ? strings::get("reader.front_matter")
                                   : strings::get("reader.untitled_chapter");
    const int first = qMax(0, start);
    progress.page = current - first + 1;
    progress.pages = (next ? chapterEndPage(*next) : pages()) - first;
    return progress;
}

QString EngineDocument::textFontFace() const
{
    if (!face_.isEmpty() || !engine_)
        return face_;
    ldomXPointer pointer = engine_->getBookmark(false);
    ldomNode *node = pointer.isNull() ? nullptr : pointer.getNode();
    if (node && node->isText())
        node = node->getParentNode();
    if (!node || !node->isElement())
        return {};
    const auto font = node->getFont();
    return font.isNull() ? QString() : QString::fromUtf8(font->getTypeFace().c_str());
}

int EngineDocument::embeddedFontCount() const
{
    if (source_ && source_->embeddedFonts >= 0) {
        return source_->embeddedFonts;
    }
    if (!engine_ || !engine_->getDocument())
        return 0;
    auto *document = engine_->getDocument();
    lString32Collection registered;
    fontMan->getRegisteredDocumentFontList(document->getDocIndex(), registered);
    QSet<QString> families;
    auto &fonts = document->getEmbeddedFontList();
    for (int i = 0; i < fonts.length(); ++i) {
        const auto *font = fonts[i];
        if (!font->getIsLocal() && registered.contains(Utf8ToUnicode(font->getFace())))
            families.insert(QString::fromUtf8(font->getFace().c_str()));
    }
    return families.size();
}

QJsonObject EngineDocument::state() const
{
    return kobalt::qt::object(
            { { "language", engine_ ? qstring(engine_->getLanguage()) : QString() },
              { "position", position() },
              { "font_pixels", pixels_ },
              { "line_spacing", spacing_ },
              { "paragraph_spacing", paragraphSpacing_ },
              { "paragraph_indent", paragraphIndent_ },
              { "beautifier_initials", beautifierInitials_ },
              { "beautifier_spacing", beautifierSpacing_ },
              { "beautifier_indent", beautifierIndent_ },
              { "line_spacing_version", 1 },
              { "font_face", face_ },
              { "default_font_face", defaultFace_ },
              { "serif_font_face", fallbackFaces_[0] },
              { "sans_font_face", fallbackFaces_[1] },
              { "monospace_font_face", fallbackFaces_[2] },
              { "margin_pixels", margin_ },
              { "top_margin_pixels", topMargin_ },
              { "bottom_margin_pixels", bottomMargin_ },
              { "text_alignment", alignment_ },
              { "weight_steps", weight_ },
              { "font_weight", axisWeight_ },
              { "font_optical_size", opticalSize_ },
              { "bold_reading", bionic_ },
              { "hyphenation", hyphenation_ },
              { "dark_mode", darkMode_ },
              { "hide_nonlinear_fragments", hideNonlinearFragments_ },
              { "normalize_page_margins", normalizePageMargins_ },
              { "reduce_chapter_spacing", reduceChapterSpacing_ },
              { "vertical_justification", verticalSpacing_ },
              { "vertical_spacing_scope", verticalSpacingScope_ },
              { "columns", columns_ },
              { "two_columns_landscape", twoColumnsLandscape_ },
              { "column_spacing_percent", columnSpacingPercent_ },
              { "column_spacing", columnSpacing_ },
              { "word_spacing", wordSpacing_ },
              { "justification_tracking", justificationTracking_ },
              { "hanging_punctuation", hangingPunctuation_ },
              { "widows_orphans", widowsOrphans_ },
              { "dom_version", domVersion_ },
              { "page_progression_rtl",
                engine_
                        && engine_->getDocument()->getProps()->getStringDef(
                                   "doc.page-progression-direction")
                                == U"rtl" },
              { "imported_native_location", nativeLocation_ } });
}
} // namespace kobalt
