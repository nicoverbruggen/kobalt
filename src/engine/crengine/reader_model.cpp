#include <cmath>
#include "ui/image_damage.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "reader_model.h"
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
#include "model/page_refresh_regions.h"
#include "page_pixels.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QRegion>
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
std::atomic<unsigned> liveModels{0};
bool fontsReady = false;
EngineFontInputs activeFonts;
QVector<ReadingFont> readingFonts;
QStringList readingFontFaces;
QVector<ReadingFont> readingFontCatalog;
bool fontFacesReady = false, fontCatalogReady = false;
QString qstring(const lString32 &s) {
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}
lString32 cre(const QString &s) {
    return Utf8ToUnicode(s.toUtf8().constData());
}
bool equationImage(const LVImageSourceRef &image) {
    for (auto *node = image->GetSourceNode(); node; node = node->getParentNode()) {
        if (!node->isElement())
            continue;
        if (!node->getAttributeValue("data-tex").empty() || node->getAttributeValue("role") == U"math")
            return true;
    }
    return false;
}
// Transform equation ink before alpha compositing, so transparent pixels keep
// the actual page background. Delegate vector sizing to avoid an extra raster scale.
class EquationImage : public LVImageSource, private LVImageDecoderCallback {
public:
    explicit EquationImage(LVImageSourceRef source) : source(source) {}
    ldomDocument *GetSourceDocument() override {
        return source->GetSourceDocument();
    }
    ldomNode *GetSourceNode() override {
        return source->GetSourceNode();
    }
    LVStream *GetSourceStream() override {
        return source->GetSourceStream();
    }
    void Compact() override {
        source->Compact();
    }
    int GetWidth() const override {
        return source->GetWidth();
    }
    int GetHeight() const override {
        return source->GetHeight();
    }
    bool IsScalable() const override {
        return source->IsScalable();
    }
    bool Decode(LVImageDecoderCallback *target) override {
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
    bool GetTargetSize(int &width, int &height) const override {
        return callback->GetTargetSize(width, height);
    }
    void OnStartDecode(LVImageSource *) override {
        callback->OnStartDecode(this);
    }
    bool OnLineDecoded(LVImageSource *, int y, lUInt32 *pixels) override {
        for (size_t x = 0; x < row.size(); ++x) {
            const auto pixel = pixels[x];
            const bool neutral = qRed(pixel) == qGreen(pixel) && qRed(pixel) == qBlue(pixel);
            row[x] = neutral ? pixel ^ 0x00ffffffU : pixel;
        }
        return callback->OnLineDecoded(this, y, row.data());
    }
    void OnEndDecode(LVImageSource *, bool errors) override {
        callback->OnEndDecode(this, errors);
    }
    LVImageSourceRef source;
    LVImageDecoderCallback *callback = nullptr;
    std::vector<lUInt32> row;
};
class PageDrawBuffer : public LVColorDrawBuf {
public:
    PageDrawBuffer(QImage &image, bool dark, bool smooth)
        : LVColorDrawBuf(image.width(), image.height(), image.bits(), 32), pageSize(image.size()), dark(dark) {
        setInvertColors(dark);
        setSmoothScalingImages(smooth);
    }
    bool hasImages = false;
    QVector<QRect> imageRegions;
    using LVColorDrawBuf::Draw;
    void Draw(int x, int y, const lUInt8 *bitmap, int width, int height, const lUInt32 *palette) override {
        const auto color = palette ? palette[0] : GetTextColor();
        const auto background = GetBackgroundColor();
        const bool neutral = qRed(color) == qGreen(color) && qRed(color) == qBlue(color);
        const bool coloredPaper = qRed(background) != qGreen(background) || qRed(background) != qBlue(background);
        // Preserve the publisher's contrast on colored backgrounds. Inverting
        // black text on a preserved yellow highlight would make it white.
        const lUInt32 ink = dark && neutral && coloredPaper && (background >> 24) != 255 ? color ^ 0x00ffffffU : color;
        LVColorDrawBuf::Draw(x, y, bitmap, width, height, &ink);
    }
    void Draw(LVImageSourceRef image, int x, int y, int width, int height, bool dither) override {
        hasImages = true;
        lvRect clip;
        GetClipRect(&clip);
        const qint64 left = qMax<qint64>(0, qMax<qint64>(x, clip.left));
        const qint64 top = qMax<qint64>(0, qMax<qint64>(y, clip.top));
        const qint64 right = qMin<qint64>(pageSize.width(), qMin<qint64>(qint64(x) + width, clip.right));
        const qint64 bottom = qMin<qint64>(pageSize.height(), qMin<qint64>(qint64(y) + height, clip.bottom));
        if (!imageRegionsFull && right > left && bottom > top) {
            if (imageRegions.size() < 128)
                imageRegions.append(QRect(int(left), int(top), int(right - left), int(bottom - top)));
            else {
                imageRegions.clear();
                imageRegions.append(QRect(QPoint(0, 0), pageSize));
                imageRegionsFull = true;
            }
        }
        if (dark && equationImage(image))
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
};
QString regionMetadata(const QVector<QRect> &regions) {
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
QVector<QRect> colourTileRegions(const QVector<uchar> &tiles, int columns, int rows, const QSize &size) {
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
                return {QRect(QPoint(0, 0), size)};
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
bool validAlignment(const QString &value) {
    return value == "publisher" || value == "left" || value == "justify" || value == "center" || value == "right";
}
}
ReaderModel::ReaderModel(EngineFontInputs fonts) : fonts_(std::move(fonts)) {
    ++liveModels;
}
ReaderModel::~ReaderModel() {
    engine_.reset();
    source_.reset();
    // Model work and destruction run on the engine worker. A preview may
    // still use these fonts, but the last closed model must release their
    // file mappings so Nickel can unmount user storage for USB.
    if (--liveModels == 0) {
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

bool ReaderModel::initializeFonts() {
    // CREngine has one global font manager. A preview or retained model can
    // still use it, so a different host input cannot replace it until the last
    // model releases its font mappings on this worker.
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
            if (!fontMan->RegisterFont(lString8(file.toUtf8().constData())) &&
                (font.origin == FontOrigin::Recommended || font.origin == FontOrigin::Interface)) {
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
QStringList ReaderModel::fontFaces() {
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

QVector<ReadingFont> ReaderModel::fontCatalog() {
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
        if (family != "Noto Serif" && !family.startsWith("Noto Serif CJK "))
            continue;
        bool present = false;
        for (const auto &font : result)
            present |= font.family == family;
        if (present)
            continue;
        QStringList files;
        for (const auto &file : fonts_.fallbackFiles) {
            const auto name = QFileInfo(file).fileName();
            if (family == "Noto Serif" ? name.startsWith("NotoSerif-") : name == "NotoSerifCJK-Regular.ttc") {
                files.append(file);
            }
        }
        if (!files.isEmpty()) {
            result.append({family, files, FontOrigin::Interface, false, {}, {}});
        }
    }
    readingFontCatalog = result;
    fontCatalogReady = true;
    return result;
}

void ReaderModel::setDocumentCache(QString directory, QByteArray fingerprint, QByteArray policy, qint64 bytes) {
    cacheDirectory_ = std::move(directory);
    cacheFingerprint_ = std::move(fingerprint);
    // Parsed pages from older density, image and double-scaled line-height
    // policies must not survive these renderer changes. The density is fixed by the host
    // before any model opens and is shared by the main reader and previews.
    cachePolicy_ =
        std::move(policy) + ":css-density-leading-v1:line-height-v2:" + QByteArray::number(readingPpi.load());
    cacheBytes_ = bytes;
}
QByteArray ReaderModel::documentCachePolicy() const {
    // Detect CSS edits for cache reuse only. User CSS is never an installation asset.
    if (userStyleSheet_.isEmpty()) {
        return cachePolicy_;
    }
    return cachePolicy_ +
           ":user-css-v1:" + QCryptographicHash::hash(userStyleSheet_, QCryptographicHash::Sha256).toHex();
}
bool ReaderModel::saveDocumentCache() {
    if (!engine_ || cacheDirectory_.isEmpty() || cacheFingerprint_.isEmpty() || !resourceError().isEmpty() || !pages())
        return false;
    // Only a document opened under this book's key may write that key's file.
    const auto name = engine_->getDocProps()->getStringDef(DOC_PROP_FILE_NAME, "");
    if (QString::fromUtf8(UnicodeToUtf8(name).c_str()) != documentCacheName(cacheFingerprint_))
        return false;
    auto *document = engine_->getDocument();
    DocumentCacheScope cache(cacheDirectory_);
    if (!cache.enabled() || engine_->updateCache() != CR_DONE)
        return false;
    if (document->hasCacheFile()) {
        if (!document->updateCacheFileName())
            return false;
        ldomDocCache::removeObsolete(name, document->getCacheFilePath());
    }
    return true;
}
FontWeightAxis ReaderModel::weightAxis() const {
    return fontWeightAxis(readingFonts, face_);
}
FontOpticalSizeAxis ReaderModel::opticalSizeAxis() const {
    return fontOpticalSizeAxis(readingFonts, face_);
}
void ReaderModel::activateFontParameters() const {
    const auto axis = weightAxis();
    // CREngine shares font parameters across models. Restore this model's
    // values before layout or drawing so previews cannot change another book.
    LVRendSetFontOpticalSize(opticalSizeAxis().available() ? opticalSize_ : 0);
    LVRendSetBaseFontWeight(axis.available() ? (bionic_ == "off" ? axisWeight_ : axis.defaultValue) : 400);
}
void ReaderModel::configure(QSize size, const QJsonObject &settings) {
    auto saved = settings;
    appearance::migrateLineSpacing(saved);
    // Outline thickness must not affect layout-time italic overhang measurements.
    setExtraFontWeight(0);
    engine_.reset();
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
    hideNonlinearFragments_ = saved.value("hide_nonlinear_fragments").toBool(false);
    pixels_ = qBound(minimumFontPixels(), saved.value("font_pixels").toInt(defaultFontPixels()), maximumFontPixels());
    spacing_ = qBound(100, saved.value("line_spacing").toInt(defaultLineSpacing), maximumLineSpacing);
    margin_ = qBound(0, saved.value("margin_pixels").toInt(defaultMarginPixels()), maximumMarginPixels());
    topMargin_ =
        qBound(0, saved.value("top_margin_pixels").toInt(defaultVerticalMarginPixels(margin_)), maximumMarginPixels());
    bottomMargin_ = qBound(0, saved.value("bottom_margin_pixels").toInt(defaultVerticalMarginPixels(margin_)),
                           maximumMarginPixels());
    const auto hyphenation = saved.value("hyphenation");
    hyphenation_ = hyphenation.isBool() ? (hyphenation.toBool() ? "auto" : "off") : hyphenation.toString("off");
    if (hyphenation_ != "auto" && hyphenation_ != "on" && hyphenation_ != "off")
        hyphenation_ = "off";
    weight_ = qBound(0, saved.value("weight_steps").toInt(0), 12);
    bionic_ = saved.value("bold_reading").toString("off");
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
    defaultFace_ = saved.value("default_font_face").toString(defaultFontFace);
    if (!faces.contains(defaultFace_))
        defaultFace_ = defaultFontFace;
    if (!face_.isEmpty() && !faces.contains(face_))
        face_ = defaultFace_;
    const auto axis = weightAxis();
    axisWeight_ = axis.available()
                      ? qBound(axis.minimum, saved.value("font_weight").toInt(defaultVariableFontWeight), axis.maximum)
                      : saved.value("font_weight").toInt(defaultVariableFontWeight);
    const auto optical = opticalSizeAxis();
    const double savedOptical = saved.value("font_optical_size").toDouble();
    opticalSize_ = normalizedOpticalSize(optical, savedOptical);
    activateFontParameters();
    engine_ = std::make_unique<LVDocView>(32, true);
    // CREngine defaults to a 72-pixel cap. Use the same physical limits as the controls.
    engine_->setMinFontSize(minimumFontPixels());
    engine_->setMaxFontSize(maximumFontPixels());
    // Keep normal word spacing when fitting a line. CREngine otherwise allows
    // spaces to shrink to half width, including in left-aligned paragraphs.
    auto properties = LVCreatePropsContainer();
    properties->setInt(PROP_FORMAT_MIN_SPACE_CONDENSING_PERCENT, 100);
    properties->setInt(PROP_FLOATING_PUNCTUATION, 0);
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
    domVersion_ =
        saved.value("position").toString().isEmpty() ? gDOMVersionCurrent : saved.value("dom_version").toInt(20260812);
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
                          lString8(QString("|||||%1|%2||").arg(monospaceFontFace, mathFontFace).toUtf8().constData()));
    engine_->propsApply(properties);
    engine_->setViewMode(DVM_PAGES, 1);
    engine_->setDefaultFontFace(lString8((face_.isEmpty() ? QString(defaultFontFace) : face_).toUtf8().constData()));
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
int ReaderModel::page() const {
    return engine_ ? engine_->getCurPage() : 0;
}
int ReaderModel::pages() const {
    return engine_ ? engine_->getPageCount() : 0;
}
QString ReaderModel::title() const {
    return engine_ ? qstring(engine_->getTitle()) : QString();
}
QString ReaderModel::author() const {
    return engine_ ? qstring(engine_->getAuthors()) : QString();
}
QImage ReaderModel::coverImage(QSize maximumSize, const QString &preferredPath) const {
    if (!engine_ || maximumSize.isEmpty() || qint64(maximumSize.width()) * maximumSize.height() > 4 * 1024 * 1024) {
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
        if (cover.isNull() || !cover->GetSourceStream() || cover->GetWidth() <= 0 || cover->GetHeight() <= 0) {
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
            return image;
        }
    }
    return decode(engine_->getCoverPageImageStream());
}

QString ReaderModel::position() const {
    return engine_ ? anchor_ : QString();
}
bool ReaderModel::go(int page) {
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
int ReaderModel::turnTarget(int from, int delta) const {
    if (!engine_ || !delta || from < 0 || from >= pages())
        return from;
    const int flow = engine_->getPageFlow(from), step = delta > 0 ? 1 : -1;
    int page = from;
    for (int turns = qAbs(delta); turns > 0; --turns) {
        int next = page + step;
        // When hiding, stay in the linear text or the non-linear section already open.
        while (hideNonlinearFragments_ && next >= 0 && next < pages() && engine_->getPageFlow(next) != 0 &&
               engine_->getPageFlow(next) != flow)
            next += step;
        if (next < 0 || next >= pages())
            break;
        page = next;
    }
    return page;
}
bool ReaderModel::goTo(const QString &position) {
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
bool ReaderModel::style(int pixels, int spacing) {
    return style(pixels, spacing, face_, margin_);
}
bool ReaderModel::style(int pixels, int spacing, const QString &face, int margin) {
    return style(pixels, spacing, face, margin, alignment_);
}
bool ReaderModel::style(int pixels, int spacing, const QString &face, int margin, const QString &alignment) {
    return applyAppearance(kobalt::qt::object({{"font_pixels", pixels},
                                               {"line_spacing", spacing},
                                               {"font_face", face},
                                               {"margin_pixels", margin},
                                               {"text_alignment", alignment}}));
}
bool ReaderModel::applyAppearance(const QJsonObject &settings, QSize size, OpeningProgress progress) {
    const int pixels = settings.value("font_pixels").toInt(pixels_);
    const int spacing = settings.value("line_spacing").toInt(spacing_);
    const int margin = settings.value("margin_pixels").toInt(margin_);
    const int topMargin = settings.value("top_margin_pixels").toInt(topMargin_);
    const int bottomMargin = settings.value("bottom_margin_pixels").toInt(bottomMargin_);
    const int weight = settings.value("weight_steps").toInt(weight_);
    const auto bionic = settings.value("bold_reading").toString(bionic_);
    const auto face = settings.value("font_face").toString(face_);
    const auto defaultFace = settings.value("default_font_face").toString(defaultFace_);
    const auto axis = fontWeightAxis(readingFonts, face);
    const int requestedAxisWeight = settings.value("font_weight").toInt(axisWeight_);
    const int axisWeight =
        axis.available() ? qBound(axis.minimum, requestedAxisWeight, axis.maximum) : requestedAxisWeight;
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
    const bool hideNonlinear = settings.value("hide_nonlinear_fragments").toBool(hideNonlinearFragments_);
    if (!engine_ || pixels < minimumFontPixels() || pixels > maximumFontPixels() || spacing < 100 ||
        spacing > maximumLineSpacing || margin < 0 || margin > maximumMarginPixels() || topMargin < 0 ||
        topMargin > maximumMarginPixels() || bottomMargin < 0 || bottomMargin > maximumMarginPixels() || weight < 0 ||
        weight > 12 || !validAlignment(alignment) ||
        (hyphenation != "auto" && hyphenation != "on" && hyphenation != "off") ||
        (bionic != "off" && bionic != "bold") || (!face.isEmpty() && !fontFaces().contains(face)) ||
        !fontFaces().contains(defaultFace))
        return false;
    if (size.width() < 320 || size.height() < 240)
        size = size_;
    const bool bionicChanged = bionic != bionic_;
    // Outline thickness changes pixels only. Keep the formatted page and its anchors.
    const bool thicknessChanged = weight != weight_;
    const bool styled = bionicChanged || axisChanged || opticalChanged || pixels != pixels_ || spacing != spacing_ ||
                        face != face_ || defaultFace != defaultFace_ || margin != margin_ || topMargin != topMargin_ ||
                        bottomMargin != bottomMargin_ || alignment != alignment_ || hyphenation != hyphenation_;
    const bool resized = size != size_;
    if (!styled && !resized && weight == weight_ && dark == darkMode_ && hideNonlinear == hideNonlinearFragments_)
        return true;
    const QString anchor = position();
    OpeningCallback callback(engine_, std::move(progress));
    pixels_ = pixels;
    spacing_ = spacing;
    face_ = face;
    defaultFace_ = defaultFace;
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
    if (styled) {
        setExtraFontWeight(0);
        auto properties = LVCreatePropsContainer();
        properties->setInt(PROP_FONT_BASE_WEIGHT, LVRendGetBaseFontWeight());
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
                documentCacheChecksum(documentCachePolicy() + ":bionic-v3:" + bionic_.toUtf8(), domVersion_));
        engine_->getDocument()->getProps()->setInt("kobalt.bionic.mode", bionic_ == "bold" ? 1 : 0);
        engine_->requestRender();
    }
    if (resized) {
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
    engine_->Render();
    return goTo(anchor);
}
bool ReaderModel::setThickness(int steps) {
    if (steps < 0 || steps > 12)
        return false;
    return applyAppearance(kobalt::qt::object({{"weight_steps", steps}}));
}
bool ReaderModel::setWeight(int steps) {
    const auto axis = weightAxis();
    if (axis.available() && (steps < axis.minimum || steps > axis.maximum))
        return false;
    return applyAppearance(kobalt::qt::object({{axis.available() ? "font_weight" : "weight_steps", steps}}));
}
bool ReaderModel::setOpticalSize(double points) {
    const auto axis = opticalSizeAxis();
    if (!axis.available() || !std::isfinite(points) || (points < axis.minimum || points > axis.maximum) ||
        points != std::floor(points))
        return false;
    return applyAppearance(kobalt::qt::object({{"font_optical_size", points}}));
}
bool ReaderModel::setHyphenation(const QString &mode) {
    return applyAppearance(kobalt::qt::object({{"hyphenation", mode}}));
}
void ReaderModel::applyPublisherFontFallbacks() {
    const bool useFallbacks = face_.isEmpty() && embeddedFontCount() == 0;
    const auto sansFace = fontFaces().contains(publisherSansFontFace) ? publisherSansFontFace : "IBM Plex Sans";
    const auto families =
        useFallbacks ? QString("|%1|%2|||%3|%4||").arg(defaultFontFace, sansFace, monospaceFontFace, mathFontFace)
                     : QString("|||||%1|%2||").arg(monospaceFontFace, mathFontFace);
    auto properties = LVCreatePropsContainer();
    properties->setString(PROP_FONT_FAMILY_FACES, lString8(families.toUtf8().constData()));
    engine_->propsApply(properties);
    const auto fallback = QString::fromLatin1(defaultFontFace);
    engine_->setDefaultFontFace(lString8((face_.isEmpty() ? fallback : face_).toUtf8().constData()));
}
void ReaderModel::applyStyleSheet() {
    QString overrides = "html,body,DocFragment { margin:0 !important; padding:0 !important; }\n";
    // A one-screen-pixel boundary stops a leading illustration's margin from
    // collapsing through the body and disappearing at the page boundary.
    // Round upward in the engine's 1/256 CSS-pixel units so every density keeps it.
    const double boundaryCssPixels = qCeil(256.0 * 96 / readingPpi.load()) / 256.0;
    overrides += QString("body[data-kobalt-leading-image=\"1\"] { padding-top:%1px !important; }\n")
                     .arg(boundaryCssPixels, 0, 'f', 6);
    if (hyphenation_ != "auto")
        overrides += QString("* { hyphens:%1 !important; }\n").arg(hyphenation_ == "on" ? "auto" : "none");
    if (!face_.isEmpty()) {
        auto escaped = face_;
        escaped.replace('\\', "\\\\");
        escaped.replace('"', "\\\"");
        overrides +=
            "* { font-family: \"" + escaped +
            "\" !important; }\n"
            // Code keeps its fixed width; MathML keeps the math font and its layout metrics.
            "pre, code, kbd, samp, tt, pre *, code *, kbd *, samp *, tt * { font-family: monospace !important; }\n"
            "math, math * { font-family: math !important; }\n";
    }
    // Like Nickel's line-height override, leave inline spans and media alone.
    // Relative font sizes, indentation and paragraph margins remain publisher CSS.
    overrides +=
        QString("body,p,div,section,article,blockquote,li,dt,dd,h1,h2,h3,h4,h5,h6 { line-height:%1% !important; }")
            .arg(spacing_);
    // Only marked text blocks override publisher alignment; decorative blocks keep it.
    if (alignment_ != "publisher")
        overrides +=
            QString(
                "[data-kobalt-prose=\"1\"], [data-kobalt-prose=\"1\"] span { text-align:%1 !important; text-align-last:auto !important; }")
                .arg(alignment_);
    engine_->setStyleSheet(lString8(readerHtmlCss) + lString8(readerOverridesCss) +
                           lString8(overrides.toUtf8().constData()) + lString8("\n") +
                           lString8(userStyleSheet_.constData()));
}
void ReaderModel::resize(QSize size) {
    applyAppearance({}, size);
}
void ReaderModel::prepareDamage(int fromPage, int toPage) {
    const auto from = cache_.value(fromPage), to = cache_.value(toPage);
    if (from.isNull() || to.isNull())
        return;
    // One rectangle serves the turn in either direction.
    const auto key = qMakePair(qMin(from.cacheKey(), to.cacheKey()), qMax(from.cacheKey(), to.cacheKey()));
    if (!damage_.contains(key))
        damage_.insert(key, kobalt::ui::imageDamage(from, to));
}
bool ReaderModel::preparedDamage(qint64 fromKey, qint64 toKey, QRect *damage) const {
    const auto found = damage_.constFind(qMakePair(qMin(fromKey, toKey), qMax(fromKey, toKey)));
    if (found == damage_.cend())
        return false;
    *damage = found.value();
    return true;
}
void ReaderModel::trim() {
    const int current = page(), previous = turnTarget(current, -1), next = turnTarget(current, 1);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it.key() != current && it.key() != previous && it.key() != next) {
            // Session and GUI images share these pixels. Only the model's last
            // reference can become a draw buffer for a different page.
            QImage retired = std::move(it.value());
            it = cache_.erase(it);
            if (spareImage_.isNull() && retired.isDetached() && retired.size() == size_ &&
                retired.format() == QImage::Format_RGB32)
                spareImage_ = std::move(retired);
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
void ReaderModel::setDarkMode(bool enabled) {
    applyAppearance(kobalt::qt::object({{"dark_mode", enabled}}));
}
bool ReaderModel::loadNextImage() {
    if (!source_ || !source_->resources)
        return false;
    source_->resources->loadNextImage();
    return refreshImages();
}
void ReaderModel::releaseImages() {
    cache_.clear();
    spareImage_ = {};
    damage_.clear();
    if (engine_)
        engine_->clearImageCache();
    if (source_ && source_->resources) {
        source_->resources->releaseBytes();
        imageRevision_ = source_->resources->imageRevision();
    }
}
bool ReaderModel::refreshImages() {
    if (!engine_ || !source_ || !source_->resources || imageRevision_ == source_->resources->imageRevision())
        return false;
    // Appearance previews share resources but keep their own rendered pages.
    // A load in either model must invalidate placeholders in the other one.
    imageRevision_ = source_->resources->imageRevision();
    cache_.clear();
    spareImage_ = {};
    damage_.clear();
    engine_->clearImageCache();
    return true;
}
QImage ReaderModel::image(int pageNumber) {
    activateFontParameters();
    setExtraFontWeight(bionic_ == "off" ? weight_ : 0);
    if (!engine_ || pageNumber < 0 || pageNumber >= pages())
        return {};
    refreshImages();
    if (cache_.contains(pageNumber))
        return cache_.value(pageNumber);
    QImage result;
    if (spareImage_.isDetached() && spareImage_.size() == size_ && spareImage_.format() == QImage::Format_RGB32)
        result = std::move(spareImage_);
    else
        result = QImage(size_, QImage::Format_RGB32);
    if (result.isNull())
        return {};
    PageDrawBuffer buffer(result, darkMode_, true);
    engine_->Draw(buffer, -1, pageNumber, false);
    // CREngine composites into 0x00RRGGBB. Qt's opaque RGB32 contract requires
    // 0xffRRGGBB, including when a painter uses this image as a source.
    bool hasColor = false;
    constexpr int tileSize = 32;
    const int tileColumns = result.width() / tileSize + (result.width() % tileSize != 0);
    const int tileRows = result.height() / tileSize + (result.height() % tileSize != 0);
    const bool boundedTiles = qint64(tileColumns) * tileRows <= 4096;
    QVector<uchar> colourTiles(boundedTiles ? tileColumns * tileRows : 0, 0);
    hasColor = normalizePagePixels(result, darkMode_, boundedTiles ? &colourTiles : nullptr, tileColumns);
    result.setText("kobalt.image_regions", regionMetadata(buffer.imageRegions));
    result.setText(
        "kobalt.colour_regions",
        regionMetadata(hasColor ? (boundedTiles ? colourTileRegions(colourTiles, tileColumns, tileRows, result.size())
                                                : QVector<QRect>{QRect(QPoint(0, 0), result.size())})
                                : QVector<QRect>{}));
    result.setText("kobalt.animation_text_only", !buffer.hasImages && !hasColor ? "true" : "false");
    // A turn can skip several physical pages of non-linear text.
    const int current = page();
    if (pageNumber == current || pageNumber == turnTarget(current, -1) || pageNumber == turnTarget(current, 1))
        cache_.insert(pageNumber, result);
    trim();
    return result;
}
QVector<ChapterLink> ReaderModel::chapters() const {
    if (!engine_)
        return {};
    return chapterIndex(contentsIndex_, contents_, contents_.isEmpty()).links;
}
quint64 ReaderModel::layoutSignature() const {
    if (!engine_)
        return 0;
    // CREngine's extended rendering hash tracks pagination settings, page
    // count and document height. It also survives document-cache restoration.
    // Mix the document identity so reopening cannot reuse another book's index.
    quint64 hash = 1469598103934665603ULL;
    const auto mix = [&hash](quint64 value) {
        hash = (hash ^ value) * 1099511628211ULL;
    };
    mix(quint64(quintptr(engine_->getDocument())));
    mix(engine_->getDocumentRenderingHash(true));
    return hash ? hash : 1;
}
const ReaderModel::ChapterIndex &ReaderModel::chapterIndex(ChapterIndex &index, const QVector<ChapterLink> &source,
                                                           bool toc) const {
    const auto layout = layoutSignature();
    // `source` shares its data with the cached copy until it changes.
    if (index.layout == layout && index.toc == toc && index.source.constData() == source.constData())
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
QVector<ChapterLink> ReaderModel::tocChapters() const {
    QVector<ChapterLink> result;
    if (!engine_)
        return result;
    std::function<void(LVTocItem *, int)> visit = [&](LVTocItem *item, int depth) {
        if (!item || depth > 32 || result.size() >= 1024)
            return;
        for (int i = 0; i < item->getChildCount() && result.size() < 1024; ++i) {
            auto *child = item->getChild(i);
            auto pointer = child->getXPointer();
            const int start = pointer.isNull() || pointer.toPoint().y < 0 ? -1 : engine_->getBookmarkPage(pointer);
            result.append({qstring(child->getName()), qstring(pointer.toString()), start, depth});
            visit(child, depth + 1);
        }
    };
    visit(engine_->getToc(), 0);
    return result;
}
void ReaderModel::setChapters(QVector<ChapterLink> contents, QVector<ChapterLink> graph) {
    if (!engine_)
        return;
    // An unresolved native list must not hide the publication's usable TOC.
    // Keep partially resolved lists and the native word-count graph intact.
    if (std::none_of(contents.cbegin(), contents.cend(), [](const ChapterLink &chapter) {
            return !chapter.position.isEmpty();
        })) {
        contents.clear();
    }
    contents_ = std::move(contents);
    graph_ = std::move(graph);
}
QVector<ChapterLink> ReaderModel::chapterPages(QVector<ChapterLink> links) const {
    if (!engine_)
        return {};
    for (auto &link : links) {
        auto pointer = engine_->getDocument()->createXPointer(cre(link.position));
        link.page = pointer.isNull() || pointer.toPoint().y < 0 ? -1 : engine_->getBookmarkPage(pointer);
    }
    return links;
}
QVector<ChapterLink> ReaderModel::chapterGraph() const {
    if (!engine_)
        return {};
    return chapterIndex(graphIndex_, graph_, false).links;
}
ChapterSelection ReaderModel::chapterSelection(const QVector<ChapterLink> &links) const {
    auto result = chapterAround(links, anchor_);
    if (result.current >= 0) {
        const int first = links[result.current].page;
        const int last = result.next < 0 ? pages() - 1 : qMax(first, links[result.next].page - 1);
        result.page = qBound(1, page() - first + 1, last - first + 1);
        result.pages = last - first + 1;
    }
    return result;
}
QString ReaderModel::chapterTitle(const QString &position) const {
    const auto links = chapters();
    const auto selected = chapterAround(links, position);
    if (selected.current >= 0) {
        const auto title = links[selected.current].title.trimmed();
        return title.isEmpty() ? strings::get("reader.untitled_chapter") : title;
    }
    return selected.next >= 0 ? strings::get("reader.front_matter") : QString();
}
ChapterSelection ReaderModel::chapterAround(const QVector<ChapterLink> &links, const QString &position) const {
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
    for (const auto *index : {&contentsIndex_, &graphIndex_}) {
        if (!index->sorted || index->layout != layout || index->links.constData() != links.constData() ||
            index->links.size() != links.size())
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
ChapterProgress ReaderModel::chapterProgress() const {
    const auto links = chapters();
    if (!contents_.isEmpty()) {
        const auto selected = chapterSelection(links);
        if (selected.current >= 0) {
            const auto title = links[selected.current].title.trimmed();
            return {title.isEmpty() ? strings::get("reader.untitled_chapter") : title, selected.page, selected.pages};
        }
        if (selected.next >= 0)
            return {strings::get("reader.front_matter"), page() + 1, qMax(1, links[selected.next].page)};
        return {};
    }
    const int current = page();
    int start = -1, end = pages();
    ChapterProgress progress;
    bool found = false;
    for (const auto &chapter : links) {
        if (chapter.page < 0 || chapter.page >= pages())
            continue;
        found = true;
        if (chapter.page <= current && chapter.page >= start) {
            start = chapter.page;
            progress.title = chapter.title.trimmed();
        } else if (chapter.page > current)
            end = qMin(end, chapter.page);
    }
    if (!found)
        return progress;
    if (progress.title.isEmpty())
        progress.title = start < 0 ? strings::get("reader.front_matter") : strings::get("reader.untitled_chapter");
    const int first = qMax(0, start);
    progress.page = current - first + 1;
    progress.pages = end - first;
    return progress;
}
QString ReaderModel::textFontFace() const {
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
int ReaderModel::embeddedFontCount() const {
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
QJsonObject ReaderModel::state() const {
    return kobalt::qt::object({{"language", engine_ ? qstring(engine_->getLanguage()) : QString()},
                               {"position", position()},
                               {"font_pixels", pixels_},
                               {"line_spacing", spacing_},
                               {"line_spacing_version", 1},
                               {"font_face", face_},
                               {"default_font_face", defaultFace_},
                               {"margin_pixels", margin_},
                               {"top_margin_pixels", topMargin_},
                               {"bottom_margin_pixels", bottomMargin_},
                               {"text_alignment", alignment_},
                               {"weight_steps", weight_},
                               {"font_weight", axisWeight_},
                               {"font_optical_size", opticalSize_},
                               {"bold_reading", bionic_},
                               {"hyphenation", hyphenation_},
                               {"dark_mode", darkMode_},
                               {"hide_nonlinear_fragments", hideNonlinearFragments_},
                               {"dom_version", domVersion_},
                               {"page_progression_rtl", engine_ && engine_->getDocument()->getProps()->getStringDef(
                                                                       "doc.page-progression-direction") == U"rtl"},
                               {"imported_native_location", nativeLocation_}});
}
}
