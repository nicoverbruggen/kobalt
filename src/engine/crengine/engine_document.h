#pragma once
#include "publication/opening_result.h"
#include "reading/chapter.h"
#include "assets/font_catalog.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QRect>
#include <QSet>
#include "publication/resource_cache.h"
#include "platform/api/position_codec.h"
#include "reader_links.h"
#include "reader_word_counts.h"
#include "reading/reading_density.h"
#include "reading/appearance_settings.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <functional>
#include <utility>
#include <tuple>

namespace kobalt {
class ColumnView;

namespace nickel {
class NickelPositionCodec;
}

// Bundled defaults for generic families.
inline constexpr auto publisherSansFontFace = "NV Legible Next";
// Bundled face for the generic monospace family.
inline constexpr auto monospaceFontFace = "IBM Plex Mono";
// Bundled face for MathML and the generic math family.
inline constexpr auto mathFontFace = "Noto Sans Math";
// Bundled faces, in order, for characters that the reading font lacks.
// Text faces precede Math so its letter glyphs cannot interrupt script shaping.
// Math precedes CJK so shared mathematical symbols keep their math metrics.
// The CJK face has full coverage and localized glyphs selected by HarfBuzz
// from the book or element language. Untagged text uses its Japanese forms.
inline constexpr auto fallbackFontFaces =
        "Noto Serif|Noto Naskh Arabic|Noto Serif Hebrew|Noto Serif Armenian|Noto Serif Georgian|Noto Serif Devanagari|Noto Serif Bengali|Noto Serif Tamil|Noto Serif Thai|Noto Sans Math|Noto Serif CJK JP";
struct ReaderSource;
enum class OpeningStage { Content, Text, Styles, Pages, Ready };
using OpeningProgress = std::function<void(OpeningStage, int)>;

struct ChapterProgress
{
    QString title;
    int page = 0, pages = 0;
};

struct SearchHit
{
    QString start, end, before, text, after;
    int page = -1;
};

struct SearchBatch
{
    QVector<SearchHit> hits;
    int nextPage = 0;
    bool done = false, limited = false;
    // A restarted scan replaces results from the previous layout.
    bool reset = false;
};

struct TextRange
{
    QString start, end, text;
    QString chosenEnd = {};
    QString chosenStart = {};
    bool sentenceStartPolicy = false;
    bool sentenceEndPolicy = false;
};

struct WordSelection
{
    QString text, start, end;
    QVector<QRect> rectangles;
};

struct SelectionEndpoint
{
    // True terminal edge span in viewport pixels, never a clipped range edge.
    QPoint first, last;
    // Axis unit normal pointing away from selected text. Caps point inward.
    QPoint outward;
    bool visible = false; // False for endpoints outside the current page/viewport.
};

struct AnnotationGeometry
{
    QVector<QRect> rectangles;
    SelectionEndpoint start, end;
    bool vertical = false; // Current page's writing axis, independent of range shape.
};

struct AnnotationToken : WordSelection
{
    // Source boundaries for automatic paired punctuation. No whitespace is included.
    QString startBeforePunctuation, endAfterPunctuation;
    bool punctuation = false;
};

struct AnnotationPageEdges
{
    AnnotationToken first, last;
    SelectionEndpoint start, end;
    int columns = 1;
    bool vertical = false;
    bool continuesBefore = false, continuesAfter = false;
};

// All engine calls stay on one owning thread. Images own their pixels.
class EngineDocument
{
public:
    explicit EngineDocument(EngineFontInputs fonts);
    ~EngineDocument();
    bool initializeFonts();
    QStringList fontFaces();
    QVector<ReadingFont> fontCatalog();

    const EngineFontInputs &fontInputs() const { return fonts_; }

    static bool supportedArchive(const QByteArray &bytes, QString *error,
                                 QSet<QString> *skippedFonts = nullptr);
    // The format of new page images in this process: Format_RGB32 (the
    // default) or Format_RGBX8888. Choose the one that the screen's backing
    // store can take without converting pixels. Pages drawn earlier keep
    // their format. Qt before 5.9 cannot reinterpret an image and keeps RGB32.
    static void setPageFormat(QImage::Format format);
    bool open(QByteArray bytes, QSize size, const QJsonObject &saved, QString *error,
              OpeningProgress progress = {});
    bool openResources(std::shared_ptr<ResourceCache> resources, const QMap<QString, qint64> &sizes,
                       QSize size, const QJsonObject &saved, QString *error,
                       OpeningProgress progress = {}, OpeningResult *result = nullptr,
                       std::function<void()> coverReady = {});
    bool rebindResources(const std::shared_ptr<ResourceCache> &resources,
                         const QMap<QString, qint64> &sizes);
    // Set before opening. CREngine keeps parsed books and their pages in
    // `directory`, keyed by the publication fingerprint and by `policy`, which
    // changes with the code that builds the parsed book. An empty directory
    // or fingerprint opens without the cache.
    void setDocumentCache(QString directory, QByteArray fingerprint, QByteArray policy,
                          qint64 bytes);

    // Set before opening. Previews inherit these bytes from their source document.
    void setUserStyleSheet(QByteArray css) { userStyleSheet_ = std::move(css); }

    const QByteArray &userStyleSheet() const { return userStyleSheet_; }

    // The last opening restored the parsed book from the cache without
    // reading its text from the publication.
    bool openedFromCache() const { return openedFromCache_; }

    // Writes the parsed book and its pages to the cache, for the next opening.
    bool saveDocumentCache();
    // Shared count, on the owning engine thread. Populates the Stats graph
    // from source anchors. Counts visible source text once, including
    // non-linear spine fragments; excludes SVG and ruby annotation text.
    // Cancellation discards the result. Completed counts use document-cache
    // properties, so reflow and cached reopening do not scan text again.
    DocumentWordCounts wordCounts(std::function<bool()> current = {});

    qint64 totalWords() const { return totalWords_; }

    QString resourceError() const;
    QJsonObject resourceStats() const;
    bool openPreview(EngineDocument &source, QSize size, QString *error);
    bool go(int page);
    // The page that `delta` page turns from `from` reach. When hiding is enabled,
    // skip other non-linear sections. Returns `from` at either end.
    int turnTarget(int from, int delta) const;
    bool goTo(const QString &position);
    QString platformPosition(const PositionCodec &codec, QString *error) const;
    bool goToPlatform(const PositionCodec &codec, const QString &location, QString *error);
    // Where goToPlatformNearest opened: the saved position, the start of its
    // chapter or the start of the book.
    enum class PositionTarget { Exact, Chapter, Book, Unavailable };
    // With a codec, an unknown anchor opens its chapter and an unknown chapter
    // opens the book. Without one, leave the position unchanged and report
    // Unavailable. *error explains why the exact position was not used.
    PositionTarget goToPlatformNearest(const PositionCodec &codec, const QString &location,
                                       QString *error);
    // Negative is before the current source anchor; positive is beyond it.
    int comparePosition(const QString &position, QString *error) const;
    int comparePlatformPosition(const PositionCodec &codec, const QString &location,
                                QString *error) const;
    // The page of each position in the current layout, or -1 where a
    // position no longer resolves.
    QVector<int> pagesOf(const QStringList &positions, bool rangeEndpoints = false,
                         QVector<int> *sourceOrder = nullptr,
                         std::function<bool()> current = {}) const;
    // The start position of a page and its first words.
    QString pageStart(int page) const;
    QString pageExcerpt(int page) const;
    WordSelection wordAt(QPoint point);
    QImage imageAt(QPoint point);
    AnnotationPageEdges annotationPageEdges();
    AnnotationToken annotationTokenAt(QPoint point);
    AnnotationToken annotationDragTokenAt(QPoint point, const QString &boundary, bool start);
    // Resolves and orders source boundaries without changing pagination or position.
    TextRange textRange(const QString &start, const QString &end);
    // Indices of source ranges intersecting [start, end), in input order.
    // Invalid, empty and reversed ranges are ignored. No text or geometry is read.
    QVector<int> intersectingRanges(const QString &start, const QString &end,
                                    const QVector<QPair<QString, QString>> &ranges) const;
    // Crossed boundary edits retain both complete words using wordAt segmentation.
    TextRange annotationRange(const QString &start, const QString &end);
    // Complete token boundaries preserve explicit punctuation choices when reversed.
    // An equal pair resolves the token at a saved start, or before a saved end.
    TextRange annotationRange(const QString &startTokenStart, const QString &startTokenEnd,
                              const QString &endTokenStart, const QString &endTokenEnd,
                              bool endTokenPolicy = false, bool startTokenPolicy = false,
                              std::function<bool()> current = {});
    QVector<PageLink> pageLinks();
    LinkDetails linkDetails(const QString &source, bool preview = true);
    SearchBatch searchPage(const QString &query, int page);
    // Source-only current-page intersection. Does not resolve remote geometry.
    // Returns input indices in order; at most 2000 saved ranges plus one draft.
    QVector<int> visibleRanges(const QVector<QPair<QString, QString>> &ranges) const;
    // Check a changing draft without rebuilding the saved-range index.
    bool rangeVisible(const QString &start, const QString &end) const;
    // Text selection excludes images. Links also need their image hit regions.
    AnnotationGeometry annotationGeometry(const QString &start, const QString &end);
    QVector<QRect> rangeRects(const QString &start, const QString &end, bool includeImages = false);
    // Bounded saved-range geometry for a preview. Keeps the exact reading bookmark.
    QVector<QVector<QRect>> pageRangeRects(int page,
                                           const QVector<QPair<QString, QString>> &ranges);
    bool applyAppearance(const QJsonObject &settings, QSize size = {},
                         OpeningProgress progress = {});
    bool style(int pixels, int spacing);
    bool style(int pixels, int spacing, const QString &face, int margin);
    bool style(int pixels, int spacing, const QString &face, int margin, const QString &alignment);
    bool setWeight(int steps);
    bool setThickness(int steps);
    bool setHyphenation(const QString &mode);
    void setDarkMode(bool enabled);
    void resize(QSize size);
    QImage image(int page);
    // One screen that starts at the reading position instead of a page break. Near the
    // end of a chapter it starts earlier, so that a full screen stays in the chapter.
    QImage imageFromAnchor();
    bool loadNextImage();
    // The idle book keeps its active layout, not images or an alternate layout.
    void releaseImages();

    // Compares the cached images of two pages ahead of a turn between them.
    // Cache key of a page's cached image, or 0. Never draws.
    qint64 cachedImageKey(int page) const { return cache_.value(page).cacheKey(); }

    void prepareDamage(int fromPage, int toPage);
    // The prepared rectangle for these two images, or false if there is none.
    bool preparedDamage(qint64 fromKey, qint64 toKey, QRect *damage) const;
    int page() const;
    int pages() const;

    int fontSize() const { return pixels_; }

    int spacing() const { return spacing_; }

    int margin() const { return margin_; }

    FontWeightAxis weightAxis() const;
    FontOpticalSizeAxis opticalSizeAxis() const;

    double opticalSize() const { return opticalSize_; }

    bool setOpticalSize(double points);

    int thickness() const { return weight_; }

    int weight() const { return weightAxis().available() ? axisWeight_ : weight_; }

    QString fontFace() const { return face_; }

    int embeddedFontCount() const;
    // The face CREngine uses for the text at the reading position. With publisher
    // fonts this is the book's own face, which may be embedded in the book.
    QString textFontFace() const;

    QString alignment() const { return alignment_; }

    QSize viewport() const { return size_; }

    // One alternate viewport, released with an idle book or changed typography.
    static constexpr size_t maxAlternateLayoutBytes = 4 * 1024 * 1024;
    size_t alternateLayoutBytes() const;

    int cachedPages() const { return cache_.size(); }

    QString position() const;
    QString title() const;
    QString author() const;
    // Decodes the host cover first, then the publication cover, within the
    // requested size and 4 million pixels. Empty means neither is usable.
    QImage coverImage(QSize maximumSize, const QString &preferredPath = {},
                      QString *usedPath = nullptr) const;
    QVector<ChapterLink> chapters() const;
    void setChapters(QVector<ChapterLink> contents, QVector<ChapterLink> graph);
    QVector<ChapterLink> chapterGraph() const;
    ChapterSelection chapterSelection(const QVector<ChapterLink> &links) const;
    ChapterProgress chapterProgress() const;
    // The title of the chapter that contains `position`, chosen like the
    // header's chapter. Empty when the book has no usable contents.
    QString chapterTitle(const QString &position) const;
    QJsonObject state() const;

private:
    // Sets the current and next entries of `links` around `position`.
    ChapterSelection chapterAround(const QVector<ChapterLink> &links,
                                   const QString &position) const;
    int chapterEndPage(const ChapterLink &next) const;
    void markKoboLinkDecorations();
    EngineFontInputs fonts_;
    friend class nickel::NickelPositionCodec;
    std::shared_ptr<ReaderSource> source_;
    QJsonObject openingTimes_;
    std::unique_ptr<ColumnView> engine_;
    struct SourceIndex;
    friend struct ReaderSourceIndexProbe;
    mutable std::shared_ptr<SourceIndex> sourceIndex_;
    std::shared_ptr<SourceIndex> sourceIndex(const QStringList &positions,
                                             const std::function<bool()> &current = {}) const;
    std::shared_ptr<SourceIndex> sourceRanges(const QVector<QPair<QString, QString>> &ranges) const;
    QPair<int, qint64> sourceIndexUsage() const;
    int columns_ = 1, columnSpacing_ = readingPixels(60), columnSpacingPercent_ = 0;
    bool twoColumnsLandscape_ = false;
    QSize size_;
    QString anchor_;
    QString nativeLocation_;
    QString cacheDirectory_;
    QByteArray cacheFingerprint_, cachePolicy_;
    QByteArray userStyleSheet_;
    QByteArray documentCachePolicy() const;
    qint64 cacheBytes_ = 0;
    bool openedFromCache_ = false;
    QVector<ChapterLink> contents_, graph_;
    qint64 totalWords_ = -1;
    QVector<ChapterLink> chapterPages(QVector<ChapterLink> links) const;
    QVector<ChapterLink> tocChapters() const;

    // Chapter entries resolved for one pagination: their pages, and the
    // entries with a page sorted by source position. Resolving every entry
    // again on each page turn delays the page by far more than drawing it.
    struct ChapterIndex
    {
        quint64 layout = 0;
        bool toc = false;
        QVector<ChapterLink> source, links;
        // Indexes into `links`, in source order. Equal positions keep list order.
        QVector<int> order;
        // CREngine places an element position with a child offset before every
        // position inside that element, and an earlier child can also compare
        // before it. Such a list has no consistent order, so it is scanned.
        bool sorted = false;
    };

    mutable ChapterIndex contentsIndex_, graphIndex_;
    // Changes whenever the pages change, including a layout that CREngine
    // redoes by itself.
    quint64 layoutSignature() const;
    const ChapterIndex &chapterIndex(ChapterIndex &index, const QVector<ChapterLink> &source,
                                     bool toc) const;
    QString face_ = defaultFontFace;
    QString defaultFace_ = defaultFontFace;
    std::array<QString, 3> fallbackFaces_;
    QString alignment_ = "left";
    int pixels_ = defaultFontPixels(), spacing_ = defaultLineSpacing, paragraphSpacing_ = -1,
        paragraphIndent_ = -1;
    int margin_ = defaultMarginPixels(), topMargin_ = defaultVerticalMarginPixels(margin_),
        bottomMargin_ = defaultVerticalMarginPixels(margin_);
    void activateFontParameters() const;
    int axisWeight_ = defaultVariableFontWeight;
    double opticalSize_ = 0;
    int weight_ = 0;
    QString bionic_ = "off";
    bool beautifierInitials_ = false, beautifierSpacing_ = false, beautifierIndent_ = false;
    // The engine DOM version that parses this book and writes its positions.
    int domVersion_ = 0;
    QString hyphenation_ = "off";
    bool darkMode_ = false;
    QMap<std::tuple<quintptr, int, int>, bool> imageInversion_;
    bool hideNonlinearFragments_ = false;
    bool normalizePageMargins_ = true, reduceChapterSpacing_ = true;
    QString verticalSpacing_ = "off", verticalSpacingScope_ = "all";
    int wordSpacing_ = 100;
    int justificationTracking_ = 0;
    bool hangingPunctuation_ = false;
    QString widowsOrphans_ = "publisher";
    QSize alternateViewport_;
    int alternateColumns_ = 1, alternateColumnSpacing_ = 0, layoutPpi_ = 0;
    QByteArray layoutCss_;
    QMap<int, QImage> cache_;
    // One retired page, owned only by this document. Never retain client images.
    QImage spareImage_;
    quint64 imageRevision_ = 0;
    bool refreshImages();
    // Changed rectangle between two cached page images, by sorted cache keys.
    QMap<QPair<qint64, qint64>, QRect> damage_;
    bool openSource(std::shared_ptr<ReaderSource> source, QSize size, const QJsonObject &saved,
                    QString *error, OpeningProgress progress = {}, OpeningResult *result = nullptr,
                    std::function<void()> coverReady = {}, QSet<QString> skippedFonts = {});
    void trim();
    void applyPublisherFontFallbacks();
    void applyStyleSheet();
    void configure(QSize size, const QJsonObject &saved);
};
} // namespace kobalt
