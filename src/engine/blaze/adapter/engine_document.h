#pragma once
#include "reading/appearance_settings.h"
#include "publication/location.h"
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
#include "engine/engine.h"
#include "engine/links.h"
#include "engine/types.h"
#include "engine/word_counts.h"
#include "reading/reading_density.h"
#include "reading/appearance_settings.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <vector>
#include <functional>
#include <utility>
#include <tuple>

class ldomXPointerEx;
struct ldomNode;

namespace kobalt {
class ColumnView;

namespace nickel {
class NickelPositionCodec;
}

struct ReaderSource;
struct PageEstimateLayout;
class PageMap;

// All engine calls stay on one owning thread. Images own their pixels.
class EngineDocument final : public Engine
{
public:
    explicit EngineDocument(EngineFontInputs fonts);
    ~EngineDocument() override;

    const char *name() const override { return "crengine"; }

    bool initializeFonts() override;
    QStringList fontFaces() override;
    QVector<ReadingFont> fontCatalog() override;

    const EngineFontInputs &fontInputs() const override { return fonts_; }

    static bool supportedArchive(const QByteArray &bytes, QString *error,
                                 QSet<QString> *skippedFonts = nullptr);
    // The format of new page images in this process: Format_RGB32 (the
    // default) or Format_RGBX8888. Choose the one that the screen's backing
    // store can take without converting pixels. Pages drawn earlier keep
    // their format. Qt before 5.9 cannot reinterpret an image and keeps RGB32.
    static void setPageFormat(QImage::Format format);
    bool open(QByteArray bytes, QSize size, const QJsonObject &saved, QString *error,
              OpeningProgress progress = {}) override;
    bool openResources(std::shared_ptr<ResourceCache> resources, const QMap<QString, qint64> &sizes,
                       QSize size, const QJsonObject &saved, QString *error,
                       OpeningProgress progress = {}, OpeningResult *result = nullptr,
                       std::function<void()> coverReady = {},
                       std::function<void()> cacheReady = {}) override;
    bool rebindResources(const std::shared_ptr<ResourceCache> &resources,
                         const QMap<QString, qint64> &sizes) override;
    // Set before opening. CREngine keeps parsed books and their pages in
    // `directory`, keyed by the publication fingerprint and by `policy`, which
    // changes with the code that builds the parsed book. An empty directory
    // or fingerprint opens without the cache.
    void setDocumentCache(QString directory, QByteArray fingerprint, QByteArray policy,
                          qint64 bytes) override;

    // Set before opening. Previews inherit these bytes from their source document.
    void setUserStyleSheet(QByteArray css) override { userStyleSheet_ = std::move(css); }

    const QByteArray &userStyleSheet() const override { return userStyleSheet_; }

    // The last opening restored the parsed book from the cache without
    // reading its text from the publication.
    bool openedFromCache() const override { return openedFromCache_; }

    // Writes the parsed book and its pages to the cache, for the next opening.
    bool saveDocumentCache() override;
    // Shared count, on the owning engine thread. Populates the Stats graph
    // from source anchors. Counts visible source text once, including
    // non-linear spine fragments; excludes SVG and ruby annotation text.
    // Cancellation discards the result. Completed counts use document-cache
    // properties, so reflow and cached reopening do not scan text again.
    DocumentWordCounts wordCounts(std::function<bool()> current = {}) override;

    qint64 totalWords() const override { return totalWords_; }

    QString resourceError() const override;
    QJsonObject resourceStats() const override;
    bool openPreview(Engine &source, QSize size, QString *error) override;
    bool go(int page) override;
    // The page that `delta` page turns from `from` reach. When hiding is enabled,
    // skip other non-linear sections. Returns `from` at either end.
    int turnTarget(int from, int delta) const override;
    bool goTo(const QString &position) override;
    QString platformPosition(const PositionCodec &codec, QString *error) const override;
    bool goToPlatform(const PositionCodec &codec, const QString &location, QString *error) override;
    // With a codec, an unknown anchor opens its chapter and an unknown chapter
    // opens the book. Without one, leave the position unchanged and report
    // Unavailable. *error explains why the exact position was not used.
    PositionTarget goToPlatformNearest(const PositionCodec &codec, const QString &location,
                                       QString *error) override;
    // Negative is before the current source anchor; positive is beyond it.
    int comparePosition(const QString &position, QString *error) const override;
    int comparePlatformPosition(const PositionCodec &codec, const QString &location,
                                QString *error) const override;
    // The shared location of an engine position. False when the position is
    // not in a spine item or is in generated content. *exact is false when
    // the location is the nearest one that the source has, for example for
    // a position inside whitespace.
    bool locationOf(const QString &position, Location *location,
                    bool *exact = nullptr) const override;
    // The engine position of a shared location, or empty when unresolved.
    QString positionOf(const Location &location, Restoration *restoration = nullptr) const override;
    // The page of each position in the current layout, or -1 where a
    // position no longer resolves.
    QVector<int> pagesOf(const QStringList &positions, bool rangeEndpoints = false,
                         QVector<int> *sourceOrder = nullptr,
                         std::function<bool()> current = {}) const override;
    // The start position of a page and its first words.
    QString pageStart(int page) const override;
    QString pageExcerpt(int page) const override;
    WordSelection wordAt(QPoint point) override;
    QImage imageAt(QPoint point) override;
    AnnotationPageEdges annotationPageEdges() override;
    AnnotationToken annotationTokenAt(QPoint point) override;
    AnnotationToken annotationDragTokenAt(QPoint point, const QString &boundary,
                                          bool start) override;
    // Resolves and orders source boundaries without changing pagination or position.
    // `withContext` also reads the surrounding sentence, at most 600 characters.
    TextRange textRange(const QString &start, const QString &end,
                        bool withContext = false) override;
    // Indices of source ranges intersecting [start, end), in input order.
    // Invalid, empty and reversed ranges are ignored. No text or geometry is read.
    QVector<int> intersectingRanges(const QString &start, const QString &end,
                                    const QVector<QPair<QString, QString>> &ranges) const override;
    // Crossed boundary edits retain both complete words using wordAt segmentation.
    TextRange annotationRange(const QString &start, const QString &end) override;
    // Complete token boundaries preserve explicit punctuation choices when reversed.
    // An equal pair resolves the token at a saved start, or before a saved end.
    TextRange annotationRange(const QString &startTokenStart, const QString &startTokenEnd,
                              const QString &endTokenStart, const QString &endTokenEnd,
                              bool endTokenPolicy = false, bool startTokenPolicy = false,
                              std::function<bool()> current = {}) override;
    QVector<PageLink> pageLinks() override;
    LinkDetails linkDetails(const QString &source, bool preview = true) override;
    SearchBatch searchPage(const QString &query, int page) override;
    // Worker-only, incremental preparation. The cache belongs to this layout.
    bool prepareSearch() override;
    SearchBatch searchText(const QString &query, SearchCursor cursor, int maximum,
                           SearchScope scope = SearchScope::Book, const QString &origin = {},
                           bool details = true, std::function<bool()> current = {}) override;
    QVector<SearchHit> searchDetails(QVector<SearchHit> hits, SearchScope scope,
                                     const QString &origin) override;

    // Source-only current-page intersection. Does not resolve remote geometry.
    // Returns input indices in order; at most 2000 saved ranges plus one draft.
    QVector<int> visibleRanges(const QVector<QPair<QString, QString>> &ranges) const override;
    // Check a changing draft without rebuilding the saved-range index.
    bool rangeVisible(const QString &start, const QString &end) const override;
    // Text selection excludes images. Links also need their image hit regions.
    AnnotationGeometry annotationGeometry(const QString &start, const QString &end) override;
    QVector<QRect> rangeRects(const QString &start, const QString &end,
                              bool includeImages = false) override;
    // Bounded saved-range geometry for a preview. Keeps the exact reading bookmark.
    QVector<QVector<QRect>> pageRangeRects(int page,
                                           const QVector<QPair<QString, QString>> &ranges) override;
    bool setChapterMode(bool fast, const appearance::Patch &settings, QSize size,
                        OpeningProgress progress = {},
                        const std::function<bool()> &current = {}) override;
    bool applyAppearance(const appearance::Patch &patch, QSize size = {},
                         OpeningProgress progress = {}) override;
    // With the koblaze fork only, experimental. Loads the book again with
    // the text of the spine items from `first` to `last` only, counted from
    // zero in spine order. A negative `first` loads the whole book. Keeps
    // the settings. The reading position is kept when it is in the window.
    bool showSpineItems(int first, int last, QString *error = nullptr);
    // The paths of the spine items in spine order, and the index of the
    // spine item of a position, or -1.
    QStringList spineItems() const;
    int spineItemOf(const QString &position) const;
    // Turns `delta` pages. At an edge of the window it loads the next or the
    // previous spine item with its neighbors and continues there. False at
    // either end of the book. Without a window this is a plain page turn.
    bool turnInWindow(int delta) override;
    void prepareAhead(const std::function<bool()> &current = {}) override;

    bool continuesBefore() const override { return windowFirst_ > 0 || sliceFirst_ > 0; }

    double bookFraction() const override;
    RoughPages roughPages() const override;
    QVector<int> bookPages(const QStringList &positions, const std::function<bool()> &current = {},
                           bool rangeEndpoints = false) const override;
    bool goToFraction(double fraction) override;
    qint64 chapterLoadMilliseconds() const override;
    bool refinePageEstimate() override;
    bool countPages(const std::function<bool()> &current) override;
    FullDocument prepareFullDocument(const std::function<bool()> &current,
                                     bool cachedOnly = false) override;
    // Both are for the settings and the page size in force. A document or
    // a build for other settings is dropped by the next prepareFullDocument()
    // and counts as nothing here.
    bool fullDocumentReady() const override;
    bool fullDocumentStarted() const override;
    void setBackgroundCount(bool enabled) override;
    bool backgroundCount() const override;

    bool continuesAfter() const override
    {
        return windowFirst_ >= 0
                && (windowLast_ + 1 < spineItemCount_ || (slices() && sliceEnd_ < spineItemText()));
    }

    // With the koblaze fork only, experimental. A spine item that is shown
    // alone is laid out in slices of about `pages` pages of text, so that no
    // layout is longer than that. Each slice starts on a new page. Zero lays
    // out a spine item as a whole. Takes effect when a spine item is shown.
    void setChapterSlices(int pages) { slicePages_ = qMax(0, pages); }

    // With the koblaze fork only. `interrupt` is asked after each block of
    // text while applyAppearance() lays out the book. When it returns true,
    // the layout stops, applyAppearance() returns false and
    // layoutInterrupted() is true. The book then has no valid layout until
    // resumeLayout() succeeds. With stock CREngine the layout always
    // finishes.
    void setLayoutInterrupt(std::function<bool()> interrupt) { interrupt_ = std::move(interrupt); }

    bool layoutInterrupted() const { return layoutInterrupted_; }

    // Lays out the book again after an interrupt and returns to the
    // reading position.
    bool resumeLayout();
    bool style(int pixels, int spacing) override;
    bool style(int pixels, int spacing, const QString &face, int margin) override;
    bool style(int pixels, int spacing, const QString &face, int margin,
               const QString &alignment) override;
    bool setWeight(int steps) override;
    bool setThickness(int steps) override;
    bool setHyphenation(const QString &mode) override;
    void setDarkMode(bool enabled) override;
    void resize(QSize size) override;
    QImage image(int page) override;
    PagePreview previewTurn(int direction, const QVector<QPair<QString, QString>> &ranges = {},
                            const std::function<bool()> &current = {},
                            const QStringList &bookmarkPositions = {}) override;
    // One screen that starts at the reading position instead of a page break. With
    // stayInChapter, it starts earlier near a chapter's end, so that a full screen stays in
    // the chapter. Without it, the text continues past the chapter to fill the screen.
    QImage imageFromAnchor(bool stayInChapter = true) override;
    bool loadNextImage() override;
    // The idle book keeps its active layout, not images or an alternate layout.
    void releaseImages() override;

    // Compares the cached images of two pages ahead of a turn between them.
    // Cache key of a page's cached image, or 0. Never draws.
    qint64 cachedImageKey(int page) const override { return cache_.value(page).cacheKey(); }

    void prepareDamage(int fromPage, int toPage) override;
    // The prepared rectangle for these two images, or false if there is none.
    bool preparedDamage(qint64 fromKey, qint64 toKey, QRect *damage) const override;
    int page() const override;
    int pages() const override;
    PageInfo pageInfo() const override;

    int fontSize() const override { return pixels_; }

    int spacing() const override { return spacing_; }

    int margin() const override { return margin_; }

    FontWeightAxis weightAxis() const override;
    FontOpticalSizeAxis opticalSizeAxis() const override;

    double opticalSize() const override { return opticalSize_; }

    bool setOpticalSize(double points) override;

    int thickness() const override { return weight_; }

    int weight() const override { return weightAxis().available() ? axisWeight_ : weight_; }

    QString fontFace() const override { return face_; }

    int embeddedFontCount() const override;
    // The face CREngine uses for the text at the reading position. With publisher
    // fonts this is the book's own face, which may be embedded in the book.
    QString textFontFace() const override;

    QString alignment() const override { return alignment_; }

    QSize viewport() const override { return size_; }

    // One alternate viewport, released with an idle book or changed typography.
    static constexpr size_t maxAlternateLayoutBytes = 4 * 1024 * 1024;
    size_t alternateLayoutBytes() const override;

    int cachedPages() const override { return cache_.size(); }

    QString position() const override;
    QString title() const override;
    QString author() const override;
    // Decodes the host cover first, then the publication cover, within the
    // requested size and 4 million pixels. Empty means neither is usable.
    QImage coverImage(QSize maximumSize, const QString &preferredPath = {},
                      QString *usedPath = nullptr) const override;
    QVector<ChapterLink> chapters() const override;
    bool goToChapter(int index, const QString &position) override;
    void setChapters(QVector<ChapterLink> contents, QVector<ChapterLink> graph) override;
    QVector<ChapterLink> chapterGraph() const override;
    ChapterSelection chapterSelection(const QVector<ChapterLink> &links) const override;
    ChapterProgress chapterProgress() const override;
    // The title of the chapter that contains `position`, chosen like the
    // header's chapter. Empty when the book has no usable contents.
    QString chapterTitle(const QString &position) const override;
    QJsonObject state() const override;

private:
    // Sets the current and next entries of `links` around `position`.
    ChapterSelection chapterAround(const QVector<ChapterLink> &links,
                                   const QString &position) const;
    int chapterEndPage(const ChapterLink &next) const;
    void markKoboLinkDecorations();
    void markPageNotes();
    EngineFontInputs fonts_;
    friend class nickel::NickelPositionCodec;
    std::shared_ptr<ReaderSource> source_;
    // A replacement reads fresh resources but belongs to the same book.
    // Consumed by openSource(), before it touches the embedded font cache.
    quint64 openingFontSource_ = 0;
    QJsonObject openingTimes_;
    std::unique_ptr<ColumnView> engine_;
    struct SearchText;
    std::shared_ptr<SearchText> searchText_;
    // The search in the text that this document has.
    SearchBatch searchLoaded(const QString &query, SearchCursor cursor, int maximum,
                             SearchScope scope, const QString &origin, bool details,
                             const std::function<bool()> &current = {});
    QVector<SearchHit> searchDetailsLoaded(QVector<SearchHit> hits, SearchScope scope,
                                           const QString &origin);
    // In chapter mode: the document that has the text of spine item `item`,
    // this one, the one opened ahead, or a new one opened ahead. Null when
    // it cannot open.
    EngineDocument *documentFor(int item, const std::function<bool()> &current = {});
    // False when the plain text of spine item `item` cannot contain `query`.
    // It can say true where the engine finds nothing.
    bool mayContain(int item, const QString &query) const;
    void labelChapterHit(SearchHit &hit) const;
    // The estimated page of the book that shows `position`, counted from
    // zero, from the page map. -1 until the map covers every spine item, or
    // when this document does not have the text of `position`.
    int estimatedBookPage(const QString &position) const;
    void ensureSearchText();
    bool ensureSearchScope(SearchScope scope, const QString &origin);
    struct SourceIndex;
    friend struct ReaderSourceIndexProbe;
    friend struct ReaderFootnoteProbe;
    friend struct ReaderFigureProbe;
    friend struct ReaderLocationProbe;
    mutable std::shared_ptr<SourceIndex> sourceIndex_;
    std::shared_ptr<SourceIndex> sourceIndex(const QStringList &positions,
                                             const std::function<bool()> &current = {}) const;
    std::shared_ptr<SourceIndex> sourceRanges(const QVector<QPair<QString, QString>> &ranges) const;
    QPair<int, qint64> sourceIndexUsage() const;
    int columns_ = 1, columnSpacing_ = readingPixels(60), columnSpacingPercent_ = 0;
    bool twoColumnsLandscape_ = false;
    QSize size_;
    QString anchor_;
    std::function<bool()> interrupt_;
    bool layoutInterrupted_ = false;
    // The spine items that have their text, or -1 for the whole book.
    int windowFirst_ = -1, windowLast_ = -1, spineItemCount_ = 0;
    // The pages of text in one slice, and the characters of the spine item
    // before the slice that has a layout.
    int slicePages_ = 0, sliceFirst_ = 0;
    // The characters of the spine item before the end of the slice. A slice
    // ends where a page starts with a new block, so that the next slice
    // continues on a full page. Extra text is laid out to find that page;
    // visiblePages_ is the number of pages before it, or 0 for all.
    int sliceEnd_ = 0, visiblePages_ = 0;
    // The end of a slice that must stop at the start of the slice after
    // it, when the reader goes backward without a recorded start.
    int sliceForcedEnd_ = 0;
    // How many times more extra text the layout takes, after a search that
    // found no clean end, and whether the last search found one.
    int sliceExtra_ = 0;
    bool sliceCut_ = true;
    // The characters that are laid out from the start of the slice.
    int sliceReach() const;
    // The starts of the slices that the reader passed going forward, so
    // that going backward shows the same pages.
    // Each entry is the start of a slice and its forced end, or 0.
    QVector<QPair<int, int>> sliceHistory_;

    // True on the extra pages after the end of the slice. They are the
    // first pages of the next slice.
    bool pastSliceEnd() const { return slices() && visiblePages_ > 0 && page() >= visiblePages_; }

    // Lays out the next slice and stays on the same page of text. With
    // `current`, the layout stops when that says other work waits: the
    // slice that was shown comes back, with its pages.
    bool enterNextSlice(const std::function<bool()> &current = {});
    // Set while a slice is laid out ahead: true stops that layout.
    std::function<bool()> sliceInterrupt_;
    // Finds the end of the slice after a layout.
    void cutSlice();
    // True when the page starts with the first text of a block.
    bool pageStartsBlock(int page, int *before) const;
    // True when something before `block` shows on the page: the last line
    // of the text before it, or a block without text.
    bool earlierContentOnPage(ldomNode *block, int page) const;

    // True when the one spine item of the window is laid out in slices.
    bool slices() const
    {
        return slicePages_ > 0 && windowFirst_ >= 0 && windowFirst_ == windowLast_;
    }

    // The characters of text in one slice, for the current font and page.
    int sliceSize() const;
    // Asks for a new layout of the document for another slice.
    void relayout();
    // One neighbouring unit, prepared in the background or retained after
    // a crossing. Swapping it with the current unit keeps at most one spare
    // document. `aheadSettings_` and `aheadSize_` are what it
    // was opened with: after a change of settings it is not used.
    std::unique_ptr<EngineDocument> ahead_;
    // The build of the document of the whole book. It runs on its own
    // thread, but never while this thread uses the engine: the two hand the
    // engine to each other. A page turn parks the build at its next stop
    // check, and the next quiet moment lets it go on from there, so that no
    // work is lost.
    struct FullBuild;
    std::shared_ptr<FullBuild> building_;
    // The document of the whole book that prepareFullDocument() built, with
    // the page it was built for. `fullFailed_` holds the page for which the
    // whole book did not open, so that it is not tried again.
    std::unique_ptr<EngineDocument> full_;
    QString fullKey_, fullFailed_;
    // The page for which the parsed-book cache had no usable document.
    QString fullUncached_;
    QSize fullSize_;
    // The document whose pages countPages() counts, with the page it was
    // opened for, and the spine items that gave no count.
    std::unique_ptr<EngineDocument> counting_;
    QString countingKey_;
    QSize countingSize_;
    int countingItem_ = -1;
    // The first character of a slice of `counting_` whose layout was
    // stopped, or -1. The next step lays out that slice again; the slices
    // before it keep their count.
    int countingNext_ = -1;
    QSet<int> uncounted_;
    // Shows the real pages of the counted spine items up to the page.
    void adoptCounted();
    // Writes the `page-count-end` diagnostic event, once for a count.
    void reportCountEnd();
    bool countReported_ = false;
    QJsonObject countingSettings_;

    // Documents of other units that stay in memory after the count or after
    // the reader left them, so that a jump there needs no new load. Their
    // spine items hold at most `keptUnitBytes` bytes together. Over that,
    // the unit farthest from the page goes.
    struct KeptUnit
    {
        int first = -1;
        QJsonObject settings;
        QSize size;
        qint64 bytes = 0;
        std::unique_ptr<EngineDocument> document;
    };

    std::vector<KeptUnit> kept_;
    void keepUnit(std::unique_ptr<EngineDocument> document, const QJsonObject &settings,
                  QSize size);
    // The kept document of the unit that starts at `first`, when it was
    // opened with `settings` for the page of this document.
    std::unique_ptr<EngineDocument> takeKept(int first, const QJsonObject &settings);
    // Drops every other document of the book: the one opened ahead, the
    // one of the count and the kept ones.
    void dropSideDocuments();
    // Opens another document of the same book with `settings`. Null when it
    // cannot open or `current` stops it. `packageCache` reads the package
    // files of the book from ReaderSource::epubCache.
    std::unique_ptr<EngineDocument> openSide(const QJsonObject &settings,
                                             const std::function<bool()> &current, QString *error,
                                             bool packageCache = false);
    // Set in a document of the background count; see openSide().
    bool packageCache_ = false;
    int aheadItem_ = -1;
    QJsonObject aheadSettings_;
    QSize aheadSize_;
    // The settings that open the window of spine items `first` to `last`.
    QJsonObject windowSettings(int first, int last) const;
    // Opens the next spine item in `ahead_` when the reader is in the last
    // slice.
    void prepareNextItem(const std::function<bool()> &current);
    void preparePreviousItem(const std::function<bool()> &current);
    QPair<int, int> neighbouringUnit(int direction) const;
    // Opens the unit that starts at spine item `item` in `ahead_`, or keeps
    // it when it is open there. The reader can then show it without a new
    // load. Also used to read a spine item that is not shown, such as the
    // target of a link. Null when it cannot open or `current` stops it.
    EngineDocument *openAhead(int item, const std::function<bool()> &current = {}, int last = -1,
                              QString *error = nullptr);
    // Set in a document that is opened ahead: true stops its layout.
    std::function<bool()> openInterrupt_;
    // Makes `ahead_` the document that is shown. False when it is not the
    // spine item `item` or its settings differ.
    bool adoptAhead(int first, int last);
    // A unit of work: the spine items that are loaded together. A long
    // spine item is a unit by itself and is laid out in slices. Short
    // ones that follow each other are one unit, up to about the text of
    // one slice, so that a book of many short chapters does not load a
    // document for each of them. unitFrom() starts a unit at `item`,
    // unitUntil() ends one there. Each spine item starts on a new page, so
    // the units do not change the pages.
    QPair<int, int> unitFrom(int item) const;
    QPair<int, int> unitUntil(int item) const;
    // Where a unit without pages gets a spine item added: after it, or
    // before it when the reader goes backward.
    int emptyUnitDirection_ = 1;
    // Loads the unit after or before the one that is shown.
    bool showNextUnit();
    bool showPreviousUnit();
    // The size in bytes of each spine item, from the directory of the
    // archive: nothing is read for it.
    const QVector<qint64> &spineBytes() const;
    // The bytes of spine items that make one unit.
    qint64 unitBudget() const;
    // For adoptAhead() only.
    EngineDocument &operator=(EngineDocument &&) = default;
    // The characters of the spine item, as the engine counts them.
    int spineItemText() const;
    // The characters of the spine item before a position, or -1.
    int textBefore(const QString &position) const;
    // Gives the slice to the engine before a layout.
    void applySlice();
    // Lays out more of the slice until it ends at a page that starts a
    // block. False when the layout was stopped.
    bool extendSlice();
    // The page for the page estimate, and the map with the spine items and
    // this page.
    PageEstimateLayout estimateLayout() const;
    PageMap &pageMap() const;
    // Gives the pages of the text that has a layout to the page estimate.
    // Only after a layout that was not stopped.
    void observeLayout();

    // The part of a spine item that the page on the screen belongs to: its
    // item, its first and last engine character of `characters`, and the
    // page in its pages. A part is a slice, or a whole spine item.
    struct RoughPlace
    {
        int item = 0, first = 0, last = 1, characters = 1, page = 0, pages = 1;
    };

    RoughPlace roughPlace() const;
    // Forgets the pages that the layout gave before a change of settings.
    void forgetObservedPages();
    // The values of state() without the page estimate.
    QJsonObject settingsState() const;
    // A digest of everything that decides the pages, from settingsState().
    QString estimateKey(const QJsonObject &settings) const;
    // The key of a prepared document of the whole book. Unlike estimateKey()
    // it also holds the settings that change pixels only: a document made
    // before dark mode changed must not be shown after it.
    QString fullDocumentKey(const QJsonObject &settings) const;
    // The saved page estimate, or empty outside chapter mode.
    QJsonObject pageEstimateRecord(const QJsonObject &settings) const;
    // Continues from a saved page estimate when the book opens.
    void restorePageEstimate(const QJsonValue &saved);
    // Lays out the slice that starts at `first` characters.
    bool layoutSlice(int first, int forcedEnd = 0);
    // Lays out the slice that holds `position` when it is another one.
    bool showSliceOf(const QString &position);
    // Lays out the slice that holds the character `before` of the spine item,
    // after the slices before it, as reading forward does.
    bool showSliceAt(int before);
    // The part of the text of `item` before `position`, from its XHTML.
    // Measures where the next few chapters start in the text of their spine
    // items. A frame asks for the book page of every chapter once the book
    // is measured, and reads one XHTML file for each. Idle time does that
    // ahead of the frame. True while chapters are left.
    bool measureChapterPlaces() const;
    double textFraction(int item, const QString &position, bool previous = false,
                        const std::function<bool()> &current = {}) const;
    double readTextFraction(int item, const QString &position, bool previous = false,
                            const std::function<bool()> &current = {}) const;
    // The page in its chapter and the pages of the chapter from the
    // estimate of the pages, when only a part of the book has a layout.
    void estimateChapterPages(const QVector<ChapterLink> &links, ChapterSelection &result) const;
    // True when the document was loaded without the fonts that the book
    // embeds: a reading font replaces them on every element, so they are
    // never drawn. Only for a part of a book, which loads again quickly
    // when the reader chooses the publisher's fonts.
    bool fontsSkipped_ = false;
    // True when the document was loaded without the marks of the
    // beautifier: no setting used them.
    bool beautifierSkipped_ = false;
    // How long the last window took to load and lay out.
    qint64 windowLoadMs_ = -1;
    // Loads the window that holds `position` when it is outside the window.
    bool showSpineItemOf(const QString &position);
    // Lays out the book. False when the layout was interrupted.
    bool renderLayout();
    QString nativeLocation_;
    QString cacheDirectory_;
    QByteArray cacheFingerprint_, cachePolicy_;
    QByteArray userStyleSheet_;
    QByteArray documentCachePolicy() const;
    qint64 cacheBytes_ = 0;
    bool openedFromCache_ = false;
    bool bypassDocumentCache_ = false;
    QVector<ChapterLink> contents_, graph_;
    // Native chapter metadata is separate from a graph derived from the DOM.
    bool externalChapters_ = false;
    qint64 totalWords_ = -1;
    using ChapterPositions = QVector<ldomXPointerEx>;
    QVector<ChapterLink> chapterPages(QVector<ChapterLink> links,
                                      ChapterPositions *positions) const;
    QVector<ChapterLink> tocChapters(ChapterPositions *positions = nullptr) const;

    // Chapter entries resolved for one pagination: their pages, and the
    // entries with a page sorted by source position. Resolving every entry
    // again on each page turn delays the page by far more than drawing it.
    struct ChapterIndex
    {
        quint64 layout = 0;
        bool toc = false;
        QVector<ChapterLink> source, links;
        // Keep resolved DOM positions for this layout. Parsing indexed XPath
        // strings again walks sibling nodes in large single-file songbooks.
        std::shared_ptr<ChapterPositions> positions;
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
    bool inpageFootnotes_ = false;
    bool normalizePageMargins_ = true, reduceChapterSpacing_ = true;
    bool normalizeTextColors_ = false;
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
                    std::function<void()> coverReady = {}, QSet<QString> skippedFonts = {},
                    std::function<void()> cacheReady = {});
    void trim();
    void applyPublisherFontFallbacks();
    void applyStyleSheet();
    void configure(QSize size, const QJsonObject &saved);
};
} // namespace kobalt
