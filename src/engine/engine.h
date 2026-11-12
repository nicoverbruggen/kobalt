#pragma once
#include "assets/font_catalog.h"
#include "engine/font_defaults.h"
#include "engine/links.h"
#include "engine/query.h"
#include "engine/types.h"
#include "engine/word_counts.h"
#include "platform/api/position_codec.h"
#include "publication/location.h"
#include "publication/opening_result.h"
#include "publication/resource_cache.h"
#include "reading/appearance_settings.h"
#include "reading/chapter.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

namespace kobalt {
// One open book in one rendering engine. The application reaches an engine
// only through this interface. All calls stay on the engine worker thread.
// A position is a string that only the engine that made it can read; a
// Location is the form that every engine and saved data share.
class Engine
{
public:
    virtual ~Engine() = default;
    // The name of the engine, for cache identity and for code that still
    // needs one specific engine.
    virtual const char *name() const = 0;

    virtual bool initializeFonts() = 0;
    virtual QStringList fontFaces() = 0;
    virtual QVector<ReadingFont> fontCatalog() = 0;
    virtual const EngineFontInputs &fontInputs() const = 0;
    virtual bool open(QByteArray bytes, QSize size, const QJsonObject &saved, QString *error,
                      OpeningProgress progress = {}) = 0;
    virtual bool openResources(std::shared_ptr<ResourceCache> resources,
                               const QMap<QString, qint64> &sizes, QSize size,
                               const QJsonObject &saved, QString *error,
                               OpeningProgress progress = {}, OpeningResult *result = nullptr,
                               std::function<void()> coverReady = {},
                               std::function<void()> cacheReady = {}) = 0;
    virtual bool rebindResources(const std::shared_ptr<ResourceCache> &resources,
                                 const QMap<QString, qint64> &sizes) = 0;
    // Set before opening. CREngine keeps parsed books and their pages in
    // `directory`, keyed by the publication fingerprint and by `policy`, which
    // changes with the code that builds the parsed book. An empty directory
    // or fingerprint opens without the cache.
    virtual void setDocumentCache(QString directory, QByteArray fingerprint, QByteArray policy,
                                  qint64 bytes) = 0;
    // Set before opening. Previews inherit these bytes from their source document.
    virtual void setUserStyleSheet(QByteArray css) = 0;
    virtual const QByteArray &userStyleSheet() const = 0;
    // The last opening restored the parsed book from the cache without
    // reading its text from the publication.
    virtual bool openedFromCache() const = 0;
    // Writes the parsed book and its pages to the cache, for the next opening.
    virtual bool saveDocumentCache() = 0;
    // Shared count, on the owning engine thread. Populates the Stats graph
    // from source anchors. Counts visible source text once, including
    // non-linear spine fragments; excludes SVG and ruby annotation text.
    // Cancellation discards the result. Completed counts use document-cache
    // properties, so reflow and cached reopening do not scan text again.
    virtual DocumentWordCounts wordCounts(std::function<bool()> current = {}) = 0;
    virtual qint64 totalWords() const = 0;
    virtual QString resourceError() const = 0;
    virtual QJsonObject resourceStats() const = 0;
    virtual bool openPreview(Engine &source, QSize size, QString *error) = 0;
    virtual bool go(int page) = 0;
    // The page that `delta` page turns from `from` reach. When hiding is enabled,
    // skip other non-linear sections. Returns `from` at either end.
    virtual int turnTarget(int from, int delta) const = 0;
    virtual bool goTo(const QString &position) = 0;
    virtual QString platformPosition(const PositionCodec &codec, QString *error) const = 0;
    virtual bool goToPlatform(const PositionCodec &codec, const QString &location,
                              QString *error) = 0;
    // Where goToPlatformNearest opened: the saved position, the start of its
    // chapter or the start of the book.
    enum class PositionTarget { Exact, Chapter, Book, Unavailable };
    // With a codec, an unknown anchor opens its chapter and an unknown chapter
    // opens the book. Without one, leave the position unchanged and report
    // Unavailable. *error explains why the exact position was not used.
    virtual PositionTarget goToPlatformNearest(const PositionCodec &codec, const QString &location,
                                               QString *error) = 0;
    // Negative is before the current source anchor; positive is beyond it.
    virtual int comparePosition(const QString &position, QString *error) const = 0;
    virtual int comparePlatformPosition(const PositionCodec &codec, const QString &location,
                                        QString *error) const = 0;
    // The shared location of an engine position. False when the position is
    // not in a spine item or is in generated content. *exact is false when
    // the location is the nearest one that the source has, for example for
    // a position inside whitespace.
    virtual bool locationOf(const QString &position, Location *location,
                            bool *exact = nullptr) const = 0;
    // The engine position of a shared location, or empty when unresolved.
    virtual QString positionOf(const Location &location,
                               Restoration *restoration = nullptr) const = 0;
    // The page of each position in the current layout, or -1 where a
    // position no longer resolves.
    virtual QVector<int> pagesOf(const QStringList &positions, bool rangeEndpoints = false,
                                 QVector<int> *sourceOrder = nullptr,
                                 std::function<bool()> current = {}) const = 0;
    // The start position of a page and its first words.
    virtual QString pageStart(int page) const = 0;
    virtual QString pageExcerpt(int page) const = 0;
    virtual WordSelection wordAt(QPoint point) = 0;
    virtual QImage imageAt(QPoint point) = 0;
    virtual AnnotationPageEdges annotationPageEdges() = 0;
    virtual AnnotationToken annotationTokenAt(QPoint point) = 0;
    virtual AnnotationToken annotationDragTokenAt(QPoint point, const QString &boundary,
                                                  bool start) = 0;
    // Resolves and orders source boundaries without changing pagination or position.
    // `withContext` also reads the surrounding sentence, at most 600 characters.
    virtual TextRange textRange(const QString &start, const QString &end,
                                bool withContext = false) = 0;
    // Indices of source ranges intersecting [start, end), in input order.
    // Invalid, empty and reversed ranges are ignored. No text or geometry is read.
    virtual QVector<int>
    intersectingRanges(const QString &start, const QString &end,
                       const QVector<QPair<QString, QString>> &ranges) const = 0;
    // Crossed boundary edits retain both complete words using wordAt segmentation.
    virtual TextRange annotationRange(const QString &start, const QString &end) = 0;
    // Complete token boundaries preserve explicit punctuation choices when reversed.
    // An equal pair resolves the token at a saved start, or before a saved end.
    virtual TextRange annotationRange(const QString &startTokenStart, const QString &startTokenEnd,
                                      const QString &endTokenStart, const QString &endTokenEnd,
                                      bool endTokenPolicy = false, bool startTokenPolicy = false,
                                      std::function<bool()> current = {}) = 0;
    virtual QVector<PageLink> pageLinks() = 0;
    virtual LinkDetails linkDetails(const QString &source, bool preview = true) = 0;
    virtual SearchBatch searchPage(const QString &query, int page) = 0;
    // Worker-only, incremental preparation. The cache belongs to this layout.
    virtual bool prepareSearch() = 0;
    virtual SearchBatch searchText(const QString &query, SearchCursor cursor, int maximum,
                                   SearchScope scope = SearchScope::Book,
                                   const QString &origin = {}, bool details = true) = 0;
    virtual QVector<SearchHit> searchDetails(QVector<SearchHit> hits, SearchScope scope,
                                             const QString &origin) = 0;
    // Source-only current-page intersection. Does not resolve remote geometry.
    // Returns input indices in order; at most 2000 saved ranges plus one draft.
    virtual QVector<int> visibleRanges(const QVector<QPair<QString, QString>> &ranges) const = 0;
    // Check a changing draft without rebuilding the saved-range index.
    virtual bool rangeVisible(const QString &start, const QString &end) const = 0;
    // Text selection excludes images. Links also need their image hit regions.
    virtual AnnotationGeometry annotationGeometry(const QString &start, const QString &end) = 0;
    virtual QVector<QRect> rangeRects(const QString &start, const QString &end,
                                      bool includeImages = false) = 0;
    // Bounded saved-range geometry for a preview. Keeps the exact reading bookmark.
    virtual QVector<QVector<QRect>>
    pageRangeRects(int page, const QVector<QPair<QString, QString>> &ranges) = 0;
    virtual bool applyAppearance(const appearance::Patch &patch, QSize size = {},
                                 OpeningProgress progress = {}) = 0;
    virtual bool style(int pixels, int spacing) = 0;
    virtual bool style(int pixels, int spacing, const QString &face, int margin) = 0;
    virtual bool style(int pixels, int spacing, const QString &face, int margin,
                       const QString &alignment) = 0;
    virtual bool setWeight(int steps) = 0;
    virtual bool setThickness(int steps) = 0;
    virtual bool setHyphenation(const QString &mode) = 0;
    virtual void setDarkMode(bool enabled) = 0;
    virtual void resize(QSize size) = 0;
    virtual QImage image(int page) = 0;
    // One screen that starts at the reading position instead of a page break. With
    // stayInChapter, it starts earlier near a chapter's end, so that a full screen stays in
    // the chapter. Without it, the text continues past the chapter to fill the screen.
    virtual QImage imageFromAnchor(bool stayInChapter = true) = 0;
    virtual bool loadNextImage() = 0;
    // The idle book keeps its active layout, not images or an alternate layout.
    virtual void releaseImages() = 0;
    // Compares the cached images of two pages ahead of a turn between them.
    // Cache key of a page's cached image, or 0. Never draws.
    virtual qint64 cachedImageKey(int page) const = 0;
    virtual void prepareDamage(int fromPage, int toPage) = 0;
    // The prepared rectangle for these two images, or false if there is none.
    virtual bool preparedDamage(qint64 fromKey, qint64 toKey, QRect *damage) const = 0;
    virtual int page() const = 0;
    virtual int pages() const = 0;
    // The page numbers of the reading position, each with its state. An
    // engine without a layout of the whole book reports the book values as
    // pending or estimated. Never starts a layout.
    virtual PageInfo pageInfo() const = 0;
    // An engine that has the text of only some spine items says whether
    // the book continues before its first page and after its last page.
    virtual bool continuesBefore() const = 0;
    virtual bool continuesAfter() const = 0;
    // The part of the book before the end of the page, from 0 to 1, as a
    // rough estimate, when the engine has the text of only a part of the
    // book. Negative when the page numbers cover the whole book.
    virtual double bookFraction() const = 0;

    // Rough page numbers for an engine with the text of only a part of the
    // book: the page in the book and in the spine item, counted from one,
    // and the pages of each. They come from an estimate of each spine item,
    // corrected by the pages of the text that has a layout. While the user
    // reads forward, the page does not move backward. All zero when the page
    // numbers of the engine cover the whole book.
    struct RoughPages
    {
        int page = 0, pages = 0, itemPage = 0, itemPages = 0;
        // False when the total is counted: every spine item of the book, or
        // every part of the spine item, had a layout.
        bool pagesEstimated = true, itemPagesEstimated = true;
    };

    virtual RoughPages roughPages() const = 0;

    // The page of the book, counted from one, of each position anywhere in
    // the book: the real page for an engine with a layout of the whole
    // book, and the page from the estimate otherwise, the same number that
    // roughPages() gives for the page on screen. Zero where the page is not
    // known yet, such as before the estimate has a total, and -1 where a
    // position does not resolve. Never lays out text.
    virtual QVector<int> bookPages(const QStringList &positions,
                                   const std::function<bool()> &current = {}) const = 0;
    // Goes to about `fraction` of the whole book, from 0 to 1. An engine
    // with the text of only a part of the book lays out the text there
    // first.
    virtual bool goToFraction(double fraction) = 0;

    // How long the engine took to load and lay out the spine items or the
    // part of a spine item that it has now. Negative when the engine has the
    // whole book.
    virtual qint64 chapterLoadMilliseconds() const { return -1; }

    // Does a small step of the background work that makes the rough page
    // numbers more precise. Never writes storage. True while work remains.
    virtual bool refinePageEstimate() { return false; }

    // Turns `delta` pages. Past the first or the last page that the engine
    // has, it loads the spine item there and continues. False at an end of
    // the book.
    virtual bool turnInWindow(int delta) = 0;
    // Called after a page is handed over, while the user reads it. An
    // engine that lays out a part of the text at a time prepares the next
    // part here, so that the turn into it does not wait. The page on screen
    // stays the same page.
    // `current` is false once other work waits, such as a tap. Work that
    // the reader does not need yet then stops at once and is dropped.
    virtual void prepareAhead(const std::function<bool()> &current = {}) = 0;
    virtual int fontSize() const = 0;
    virtual int spacing() const = 0;
    virtual int margin() const = 0;
    virtual FontWeightAxis weightAxis() const = 0;
    virtual FontOpticalSizeAxis opticalSizeAxis() const = 0;
    virtual double opticalSize() const = 0;
    virtual bool setOpticalSize(double points) = 0;
    virtual int thickness() const = 0;
    virtual int weight() const = 0;
    virtual QString fontFace() const = 0;
    virtual int embeddedFontCount() const = 0;
    // The face CREngine uses for the text at the reading position. With publisher
    // fonts this is the book's own face, which may be embedded in the book.
    virtual QString textFontFace() const = 0;
    virtual QString alignment() const = 0;
    virtual QSize viewport() const = 0;
    virtual size_t alternateLayoutBytes() const = 0;
    virtual int cachedPages() const = 0;
    virtual QString position() const = 0;
    virtual QString title() const = 0;
    virtual QString author() const = 0;
    // Decodes the host cover first, then the publication cover, within the
    // requested size and 4 million pixels. Empty means neither is usable.
    virtual QImage coverImage(QSize maximumSize, const QString &preferredPath = {},
                              QString *usedPath = nullptr) const = 0;
    virtual QVector<ChapterLink> chapters() const = 0;
    virtual void setChapters(QVector<ChapterLink> contents, QVector<ChapterLink> graph) = 0;
    virtual QVector<ChapterLink> chapterGraph() const = 0;
    virtual ChapterSelection chapterSelection(const QVector<ChapterLink> &links) const = 0;
    virtual ChapterProgress chapterProgress() const = 0;
    // The title of the chapter that contains `position`, chosen like the
    // header's chapter. Empty when the book has no usable contents.
    virtual QString chapterTitle(const QString &position) const = 0;
    virtual QJsonObject state() const = 0;
};

// The engine that reads books: CREngine.
std::unique_ptr<Engine> createEngine(EngineFontInputs fonts);

// 1 opens the books that follow in chapter mode and 0 opens them as a whole.
// The application sets it through renderingMode in reading/rendering_mode.h.
// -1, the default, leaves the choice to the environment.
void setChapterModeOverride(int mode);
} // namespace kobalt
