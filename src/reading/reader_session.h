#pragma once
#include "engine/engine.h"
#include "open_request.h"
#include "reading_progress.h"
#include "highlight_geometry.h"
#include "storage/custom_pins.h"
#include <QElapsedTimer>
#include <QFileInfo>
#include <QObject>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>

namespace kobalt {
struct PageSummary
{
    QString start, excerpt, chapter;
    // Whole-book spread index for a prepared two-column chapter, or -1.
    int bookPage = -1;
};

struct BookmarkLocations
{
    QVector<int> pages, order;
    // Whole-book display indexes, separate from the loaded layout pages.
    QVector<int> bookPages = {};
    QStringList chapters = {};
};

struct ReaderMarkupRange
{
    QString start, end, color;
    QString style = QStringLiteral("shaded");

    bool operator==(const ReaderMarkupRange &other) const
    {
        return start == other.start && end == other.end && color == other.color
                && style == other.style;
    }
};

struct ReaderMarkupRequest
{
    QVector<ReaderMarkupRange> saved;
    QString draftStart, draftEnd;

    bool operator==(const ReaderMarkupRequest &other) const
    {
        return saved == other.saved && draftStart == other.draftStart && draftEnd == other.draftEnd;
    }
};

struct ReaderMarkupFrame
{
    // Geometry belongs to these exact raw pixels and this immutable request.
    // The request echo lets a closed draft discard only its own stale geometry.
    ReaderMarkupRequest request;
    quint64 generation = 0;
    qint64 imageKey = 0;
    QVector<HighlightGeometry> saved;
    AnnotationGeometry draft;
    int resolvedRanges = 0;
};

struct ReaderFrame
{
    quint64 renderId = 0;
    int turnDirection = 0;
    bool formattingWarning = false;
    QImage image;
    ReaderMarkupFrame markup;
    // Changed rectangle against the image with cache key `damageFrom`, if the
    // worker prepared it before this turn. `damageFrom` is 0 otherwise.
    QRect damage;
    qint64 damageFrom = 0;
    QJsonObject state, resources;
    QString title, author, error;
    // Source identity of the painted page, independent of later slice preparation.
    QString pageStart;
    qint64 totalWords = -1, mainWords = -1;
    QString nativePosition, nativePositionError;
    QImage cover;
    QStringList fonts;
    QVector<ReadingFont> fontCatalog;
    QVector<ChapterLink> chapters, chapterGraph, printedPages, landmarks;
    QVector<PageLink> links;
    ChapterSelection contentsSelection, graphSelection;
};

enum class RenderReason { Appearance, EnableBionic, DisableBionic, PageLayout };

struct RenderProgress
{
    bool active = false;
    quint64 revision = 0;
    RenderReason reason = RenderReason::Appearance;
    OpeningStage stage = OpeningStage::Styles;
    int percent = 0;
    qint64 elapsedMilliseconds = 0;

    // Reflow styles and pagination share one bar; a phase change never empties it.
    int overallPercent() const
    {
        const int done = qBound(0, percent, 100);
        if (stage == OpeningStage::Styles)
            return done * 10 / 100;
        if (stage == OpeningStage::Pages)
            return 10 + done * 89 / 100;
        return stage == OpeningStage::Ready ? 99 : 0;
    }
};

// Where a platform position lies relative to the current page.
struct PositionComparison
{
    enum class Status {
        Compared, // direction is valid
        Failed, // the position cannot be placed; error says why
        Stale // the book moved or changed layout while comparing
    };
    Status status = Status::Failed;
    // Compared only: negative before, zero at, positive after the current position.
    int direction = 0;
    QString error;
};

struct SyncedPositionDescription
{
    PositionComparison comparison;
    QString chapter;
    // Compared only: the whole-book page of the synced position, or -1.
    int page = -1;
};

// GUI-side settings and immutable frame copies. The worker owns engine access.
// After application shutdown joins that worker, remaining documents are released
// synchronously by their GUI-side owners.
class ReaderSession : public QObject
{
public:
    ReaderSession(EngineFontInputs fonts, QString placeholderFamily);

    const EngineFontInputs &fontInputs() const { return fonts_; }

    const QString &placeholderFamily() const { return placeholderFamily_; }

    ~ReaderSession() override;
    // Call after deleting sessions and cancelling their resources.
    static void drain(std::function<void()> finished);
    void open(OpenRequest request, std::function<void(OpeningResult)> finished,
              OpeningProgress progress = {}, std::function<void(QImage)> coverReady = {},
              std::function<void()> reusing = {});
    // A fresh facade prevents callbacks from the closed reader reaching a reopen.
    std::unique_ptr<ReaderSession> retained();
    // Debug mode, in place of retained() when the reader closes: drops the
    // book and removes its parsed-book cache. With `allBooks` it removes the
    // whole cache, also `documentCache` when the book opened without it, and
    // the preview tiles in `previews`. Runs on the engine worker, which the
    // close barrier drains.
    void evictDocumentCache(bool allBooks, const QString &documentCache, const QString &previews);
    void cancelSource();
    void revokeSource();

    QByteArray sourceHash() const { return fingerprint; }

    QByteArray syncDigest() const { return syncDigest_; }

    QByteArray syncFilenameDigest() const { return syncFilenameDigest_; }

    // The file name that filename matching hashes, for KOSync document metadata.
    QString syncFilename() const { return syncFilename_; }

    // Whether the saved state passed to open belongs to this version of the book.
    bool savedStateMatched() const { return savedMatched; }

    bool recoveredOpeningPosition() const { return openingResult_.recoveredPosition; }

    QJsonObject resourceStats() const { return frame.resources; }

    void openPreview(const ReaderSession &source, QSize size,
                     std::function<void(QString)> finished);
    // `chapterMode`: 1 shows the chapter only, 0 the whole book, -1 keeps
    // what is shown. hybridChapterMode goes to the chapter when the
    // appearance changes and keeps what is shown otherwise.
    static constexpr int hybridChapterMode = 2;
    void requestAppearance(const appearance::Requested &settings, QSize size = {},
                           int chapterMode = -1);
    void editAppearance(const appearance::Patch &patch, QSize size = {});
    std::function<void()> updated;
    RenderProgress renderProgress() const;
    // Appearance and viewport work reports progress separately from frames.
    // Each revision starts a new elapsed time, including its wait for the worker.
    std::function<void()> renderProgressChanged;
    // A buffered request reached the first or final page without moving.
    std::function<void(int direction)> turnBoundary;
    // The engine drew the pages beside the current one and has nothing else
    // queued for this session. Work that may wait, such as preview tiles,
    // starts here instead of on a timer.
    std::function<void()> idle;
    // The book's background work is done for now: the whole book is shown
    // and its parsed-book cache is written, or no whole-book layout will
    // follow, as in Fast or after a build that gave up. Work that would
    // compete with that work, such as building hidden panels, starts here.
    std::function<void()> settled;
    bool style(int pixels, int spacing);
    bool style(int pixels, int spacing, const QString &face, int margin);
    bool style(int pixels, int spacing, const QString &face, int margin, const QString &alignment);
    bool setWeight(int steps);
    bool setThickness(int steps);
    void setDarkMode(bool enabled);
    bool setHyphenation(const QString &mode);
    bool setBookBeautifier(bool enabled);
    // Keep at most one turn behind the current turn. The latest direction wins.
    bool turn(int direction);
    bool canTurn() const;
    void cancelPendingTurn();
    // Suspend optional work for the next Hybrid turn while the reader is closing.
    void pauseHybridTurnPreparation(bool paused);
    void requestMenuPagination();
    bool paginationPending() const;
    bool go(int page);
    bool goTo(const QString &anchor, bool keepLayout = false);
    // Goes to the entry `index` of chapters(). See Engine::goToChapter().
    bool goToChapter(int index);
    // Goes to about `fraction` of the whole book, from 0 to 1. With only a
    // part of the book laid out, the engine lays out the text there first.
    bool goToFraction(double fraction);
    bool goToNative(const QString &location, std::function<void(QString)> finished);
    bool goToSyncedPosition(const QString &position, std::function<void(QString)> finished);
    // Resolves chapter, page and source order without changing the displayed page.
    void describeSyncedPosition(const QString &position,
                                std::function<void(SyncedPositionDescription)> finished);
    void nativePosition(std::function<void(QString, QString)> finished);
    void compareNativePosition(const QString &location,
                               std::function<void(PositionComparison)> finished);

    // Source and pagination generation. Navigation within the same layout keeps it.
    int layoutProgressPercent() const { return engine_->progress->percent(); }

    quint64 layoutRevision() const { return newestLayout->load(); }

    // Milliseconds since the opening or the last new layout, which is a new
    // appearance or page size; -1 before the opening. The Book status field
    // shows how long the whole book took from there.
    qint64 layoutMilliseconds() const
    {
        if (layoutReadyMilliseconds_ >= 0)
            return layoutReadyMilliseconds_;
        return layoutClock_.isValid() ? layoutClock_.elapsed() : -1;
    }

    // Counts the openings and new layouts that restart that clock.
    quint64 layoutStarts() const { return layoutStarts_; }

    // Bookmark lookups on the engine thread. They report nothing when the
    // layout changed before they finished; the caller asks again.
    void bookmarkLocations(QStringList positions, std::function<void(BookmarkLocations)> finished,
                           bool rangeEndpoints = false, bool sourceOrder = true,
                           std::shared_ptr<std::atomic_bool> cancelled = {});
    // My Words needs the book page even if Hybrid adopts a prepared layout
    // during the lookup. Retry on the same open book when its pages are ready.
    void bookmarkPages(QStringList positions, std::function<void(QVector<int>)> finished,
                       bool rangeEndpoints = false);
    void pageSummary(int page, std::function<void(QString position, QString excerpt)> finished);
    void pageSummary(int page, std::function<void(PageSummary)> finished);
    // The start of the text on `page` in the layout shown now. A layout that
    // is requested after this call does not discard the answer, because the
    // engine thread reads the text before it starts that layout. Call it only
    // while the session is not busy.
    void displayedText(int page, std::function<void(QString text)> finished);
    // Renders a bounded thumbnail with saved highlights without moving the reading position. Only
    // the latest request can finish; navigation, reflow and cancellation drop it.
    void previewPage(int page, QSize bounds, std::function<void(QImage)> finished);
    void previewAdjacentPage(int direction, QSize bounds, QStringList bookmarkPositions,
                             std::function<void(QImage, bool)> finished);
    void cancelPagePreview();
    // Layout and Text start at the reading position. Sample opens a separate in-memory passage.
    enum class PreviewPage { Layout, Text, Sample };
    void appearancePage(PreviewPage kind, std::function<void(QImage)> finished);
    // One worker job resolves all eligible passages. Stale replies contain no indices.
    void intersectingRanges(const QString &start, const QString &end,
                            QVector<QPair<QString, QString>> ranges,
                            std::function<void(QVector<int>)> finished);
    void annotationPageEdges(std::function<void(AnnotationPageEdges)> finished);
    void annotationTokenAt(QPoint point, std::function<void(AnnotationToken)> finished,
                           const QString &boundary = {}, bool start = false);
    void annotationRange(const QString &startTokenStart, const QString &startTokenEnd,
                         const QString &endTokenStart, const QString &endTokenEnd,
                         std::function<void(TextRange)> finished, bool endTokenPolicy = false,
                         bool startTokenPolicy = false);
    void annotationRange(const QString &start, const QString &end,
                         std::function<void(TextRange)> finished);
    // `withContext` also reads the sentence around the range.
    void textRange(const QString &start, const QString &end,
                   std::function<void(TextRange)> finished, bool withContext = false);
    void wordAt(QPoint point, std::function<void(WordSelection)> finished);
    void imageAt(QPoint point, std::function<void(QImage)> finished, const QString &resource = {});
    // The engine worker polls current while opening a remote target.
    void linkDetails(const QString &source, std::function<void(LinkDetails)> finished,
                     std::function<bool()> current = {});

    const QVector<PageLink> &pageLinks() const { return frame.links; }

    // Layout changes restart unfinished queries and completed deferred-detail queries.
    // Their first batch replaces prior results.
    // Explicit cancellation or a new source discards both running and queued searches.
    void search(const QString &query, std::function<void(SearchBatch)> finished,
                SearchScope scope = SearchScope::Book, const QString &origin = {},
                bool details = true);
    void searchDetails(QVector<SearchHit> hits, SearchScope scope, const QString &origin,
                       std::function<void(QVector<SearchHit>)> finished);
    void prepareSearch();
    void cancelSearch();
    void annotationGeometry(const QString &start, const QString &end,
                            std::function<void(AnnotationGeometry)> finished);
    void rangeRects(const QString &start, const QString &end,
                    std::function<void(QVector<QRect>)> finished);
    void rangeRects(QVector<QPair<QString, QString>> ranges,
                    std::function<void(QVector<QRect>)> finished);
    // Stable groups for independently colored ranges, resolved in one worker job.
    void rangeRectGroups(QVector<QPair<QString, QString>> ranges,
                         std::function<void(QVector<QVector<QRect>>)> finished);
    void resize(QSize size);

    quint64 renderId() const { return frame.renderId; }

    bool busy() const { return running; }

    // Whether the pages beside the current one are queued or being drawn,
    // after the session itself has reported its work done.
    bool preparingNeighbours() const { return *neighbourJobs > 0; }

    // Preview and later reading-command errors. Initial opening failures are
    // delivered only through the typed completion result.
    QString error() const { return frame.error; }

    bool formattingWarning() const { return frame.formattingWarning; }

    const storage::CustomPins &customPins() const { return customPins_; }

    QString imageWarning() const { return frame.resources.value("image_error").toString(); }

    int turnDirection() const { return frame.turnDirection; }

    QRect damage() const { return frame.damage; }

    qint64 damageFrom() const { return frame.damageFrom; }

    QImage image() const { return frame.image; }

    // Stores an immutable source snapshot without rendering or queuing work.
    // At most 2000 saved ranges and 4096 characters per anchor are accepted.
    // Invalid requests leave the prior snapshot and generation unchanged.
    quint64 setMarkup(const ReaderMarkupRequest &request);

    quint64 markupGeneration() const { return markupGeneration_; }

    const ReaderMarkupFrame &markup() const { return frame.markup; }

    // Prepare markup for an already captured image without rendering it again.
    // Completion runs while this session lives, false on busy or stale work.
    // Empty requests complete immediately. No updated notification is emitted.
    void captureMarkup(std::function<void(bool)> finished);

    QJsonObject state() const { return frame.state; }

    // The newest requested settings, including any whose frame is still rendering.
    appearance::Requested requestedAppearance() const
    {
        return appearance::Requested::fromResolvedState(desired);
    }

    appearance::Displayed displayedAppearance() const
    {
        return { appearance::Requested::fromResolvedState(frame.state), frame.renderId };
    }

    bool temporaryTypography() const { return !temporaryAppearance.isEmpty(); }

    appearance::SavedInput persistentAppearance() const;

    // Captured with the displayed frame, so sleep never waits for the engine worker.
    QString savedNativePosition(QString *error) const
    {
        *error = frame.nativePositionError;
        return frame.nativePosition;
    }

    int page() const { return frame.state.value("page").toInt(); }

    int pages() const { return frame.state.value("pages").toInt(); }

    // Display pages cover the whole book. Local pages still drive navigation and geometry.
    int bookPage() const
    {
        return frame.state.contains("book_fraction") ? frame.state.value("rough_page").toInt() - 1
                                                     : page();
    }

    int bookPages() const
    {
        return frame.state.contains("book_fraction") ? frame.state.value("rough_pages").toInt()
                                                     : pages();
    }

    // True while the pages of the book are an estimate.
    bool bookPagesEstimated() const
    {
        return frame.state.contains("book_fraction")
                && frame.state.value("rough_pages_estimated").toBool();
    }

    // The engine has more of the book after its last page: that page is not
    // the end of the book.
    bool continuesAfter() const { return frame.state.value("continues_after").toBool(); }

    // The percent of the book that is read. An engine with the text of only
    // a part of the book gives a rough estimate; otherwise it is the page
    // count of pageProgressPercent().
    // The same as a fraction from 0 to 1, for the progress bar.
    double progressFraction() const
    {
        const auto fraction = frame.state.value("book_fraction");
        return fraction.isDouble() ? qBound(0.0, fraction.toDouble(), 1.0)
                                   : pageProgressFraction(page(), pages());
    }

    int progressPercent() const
    {
        const auto fraction = frame.state.value("book_fraction");
        if (fraction.isDouble()) {
            // When the estimate has page counts, keep exact percentages such as 29/50.
            if (bookPages() > 0 && bookPage() >= 0)
                return pageProgressPercent(bookPage(), bookPages());
            return qBound(0, int(fraction.toDouble() * 100), 100);
        }
        return pageProgressPercent(page(), pages());
    }

    // The physical page numbers above also drive sync, completion and navigation.
    int firstMainPage() const;
    int lastMainPage() const;
    QString displayPageLabel(int absolute, const QString &style) const;
    QString displayBookPosition(const QString &style) const;

    int mainPages() const { return qMax(0, lastMainPage() - firstMainPage() + 1); }

    int mainPage() const { return bookPage() - firstMainPage(); }

    double mainProgress() const;
    int mainPercent() const;

    int statsPercent(bool wholeVolume) const
    {
        if (!wholeVolume && mainPages() != bookPages())
            return mainPercent();
        return bookPages() > 0 ? pageProgressPercent(bookPage(), bookPages()) : progressPercent();
    }

    bool hasMainTextBoundary() const { return frame.state.value("main_text_bounded").toBool(); }
    QJsonObject mainTextMarks() const { return frame.state.value("main_text_marks").toObject(); }

    struct ChapterBoundaryAction
    {
        bool start, reset;
        std::function<bool()> apply;
    };

    void chapterBoundaryActions(int index,
                                std::function<void(QVector<ChapterBoundaryAction>)> finished);

    // The page a forward turn reaches, or page() on the last page of the text.
    int nextPage() const { return frame.state.value("next_page").toInt(page()); }

    int cachedPages() const { return frame.state.value("cached_pages").toInt(); }

    int fontSize() const { return desired.value("font_pixels").toInt(); }

    int spacing() const { return desired.value("line_spacing").toInt(); }

    int margin() const { return desired.value("margin_pixels").toInt(); }

    int topMargin() const
    {
        return desired.value("top_margin_pixels").toInt(defaultVerticalMarginPixels(margin()));
    }

    int bottomMargin() const
    {
        return desired.value("bottom_margin_pixels").toInt(defaultVerticalMarginPixels(margin()));
    }

    bool setVerticalMargins(int margin);

    FontOpticalSizeAxis opticalSizeAxis() const
    {
        return fontOpticalSizeAxis(frame.fontCatalog, fontFace());
    }

    double opticalSize() const
    {
        return normalizedOpticalSize(opticalSizeAxis(),
                                     desired.value("font_optical_size").toDouble());
    }

    bool setOpticalSize(double points);

    FontWeightAxis weightAxis() const { return fontWeightAxis(frame.fontCatalog, fontFace()); }

    int thickness() const { return desired.value("weight_steps").toInt(); }

    int weight() const
    {
        const auto axis = weightAxis();
        return axis.available()
                ? qBound(axis.minimum,
                         desired.value("font_weight").toInt(defaultVariableFontWeight),
                         axis.maximum)
                : desired.value("weight_steps").toInt();
    }

    QString fontFace() const { return desired.value("font_face").toString(); }

    QString hyphenation() const { return desired.value("hyphenation").toString("off"); }

    QString alignment() const { return desired.value("text_alignment").toString(); }

    QString title() const { return frame.title; }

    QString author() const { return documentAuthor; }

    QImage coverThumbnail() const { return documentCover; }

    // Decodes on the engine worker. A cancelled or replaced source returns no image.
    void coverImage(QSize maximumSize, std::function<void(QImage)> finished,
                    QString preferredPath = {});
    void coverImage(QSize maximumSize, std::function<void(QImage, QFileInfo)> finished,
                    QString preferredPath);

    void sleepOverlay(const QStringList &directories, std::function<void(QImage)> finished);
    // Lists the wallpapers and decodes one image for each entry of `wanted`, a
    // file name or empty for a random one, on the engine worker.
    void sleepWallpapers(const QStringList &directories, const QStringList &wanted, QSize bound,
                         std::function<void(QStringList, QVector<QImage>)> finished);

    QString chapterTitle() const { return frame.state.value("chapter_title").toString(); }

    int chapterPage() const { return frame.state.value("chapter_page").toInt(); }

    int chapterPages() const { return frame.state.value("chapter_pages").toInt(); }

    QStringList fontFaces() const { return frame.fonts; }

    QVector<ReadingFont> fontCatalog() const { return frame.fontCatalog; }

    const QVector<ChapterLink> &landmarks() const { return frame.landmarks; }

    const QVector<ChapterLink> &printedPages() const { return frame.printedPages; }
    QVector<ChapterLink> chapters() const { return frame.chapters; }

    QVector<ChapterLink> chapterGraph() const { return frame.chapterGraph; }

    ChapterSelection contentsSelection() const { return frame.contentsSelection; }

    ChapterSelection graphSelection() const { return frame.graphSelection; }

    qint64 mainWords() const { return frame.mainWords; }
    qint64 totalWords() const { return frame.totalWords; }

    void setChapters(QVector<ChapterTarget> contents, QVector<ChapterTarget> graph);

private:
    friend struct ReaderLocationProbe;
    // Background pagination only offers a document. A foreground render
    // can adopt it after checking the requested layout and reading position.
    void showFullDocument(quint64 job);
    bool currentSourceFrame(quint64 job) const;
    static bool publishPageEstimate(Engine &engine, const std::function<bool()> &wanted,
                                    QPointer<ReaderSession> guard, quint64 job,
                                    const std::shared_ptr<SourceLease> &lease);
    // `requests` is engineWorkRequests() from when this step was queued:
    // work that was announced after that is in the queue behind this step,
    // or will be, and goes first.
    enum class HybridTurnPreparation { Allowed, Paused, Deferred };
    struct BackgroundWork
    {
        std::atomic_bool count{ false };
        std::atomic<HybridTurnPreparation> hybridTurn{ HybridTurnPreparation::Allowed };
    };
    static void refinePageEstimate(std::shared_ptr<Engine> engine,
                                   const std::function<bool()> &current,
                                   QPointer<ReaderSession> guard, quint64 job,
                                   const std::shared_ptr<SourceLease> &lease, quint64 requests,
                                   const std::shared_ptr<BackgroundWork> &background);
    // Calls `settled` on the GUI thread while the lease is current.
    static void notifySettled(QPointer<ReaderSession> guard,
                              const std::shared_ptr<SourceLease> &lease);
    storage::CustomPins customPins_;
    ReaderSession(std::shared_ptr<Engine> retainedEngine, QString placeholderFamily);
    EngineFontInputs fonts_;
    QString placeholderFamily_;
    std::shared_ptr<Engine> engine_;
    ReaderFrame frame;
    void completeFrameMarkup(ReaderFrame result, quint64 job,
                             const std::shared_ptr<SourceLease> &lease,
                             std::function<void(ReaderFrame)> finished);
    ReaderMarkupRequest markupRequest_;
    quint64 markupGeneration_ = 0;
    QString documentAuthor;
    QImage documentCover;
    OpeningResult openingResult_;
    QByteArray fingerprint;
    QByteArray syncDigest_;
    QByteArray syncFilenameDigest_;
    QString syncFilename_;
    bool importPosition(const QString &location, bool native,
                        std::function<void(QString)> finished);
    bool savedMatched = false;
    std::shared_ptr<SourceLease> lease_;
    bool ownsLease_ = false;
    QString identity_;
    PositionCodec positions_;
    QJsonObject desired;
    int desiredChapterMode = -1;
    bool fullDocumentPending_ = false;
    QJsonObject temporaryAppearance;
    appearance::SavedInput originalAppearance;
    QSize desiredSize;
    QString destination;
    bool keepDestinationLayout = false;
    // Page turns since the book opened or the appearance changed. Hybrid
    // rendering counts the pages of the book after a few of them.
    int quietTurns_ = 0;
    // These flags are shared with idle work that can outlive this session.
    const std::shared_ptr<BackgroundWork> background_ = std::make_shared<BackgroundWork>();
    // The entry of chapters() that `destination` is, or -1.
    int chapterTarget = -1;
    int pageDelta = 0;
    int pageTarget = -1;
    // A part of the whole book to go to, from 0 to 1, or negative.
    double fractionTarget = -1;
    bool running = false;
    bool turning = false;
    int pendingTurn = 0;
    bool loadingImage = false;
    bool preview_ = false;
    bool openingPreview_ = false;
    bool retiring = false;
    bool layoutReady_ = false;
    RenderProgress renderProgress_;
    QElapsedTimer renderTimer;
    QElapsedTimer layoutClock_;
    // When a background build of the whole book was ready, or -1. Hybrid
    // shows that layout from the next turn; its time ends at the build.
    qint64 layoutReadyMilliseconds_ = -1;
    quint64 layoutStarts_ = 0;
    bool switchingMode_ = false;
    void beginRenderProgress(RenderReason reason);
    void clearRenderProgress();
    void loadImages();
    void resumeIdleWork(int direction, quint64 job);
    quint64 revision = 0;
    // The newest request number, readable on the engine worker. Work that only
    // prepares later page turns stops when it no longer matches its own job.
    std::shared_ptr<std::atomic<quint64>> newest = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> newestLayout = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> newestPreview = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic_int> neighbourJobs = std::make_shared<std::atomic_int>(0);
    std::shared_ptr<std::atomic_bool> searchCancelled;
    QString searchQuery, searchOrigin;
    SearchScope searchScope = SearchScope::Book;
    bool searchWithDetails = true;
    void prepareSearchNext(quint64 job, std::shared_ptr<std::atomic_bool> cancelled);
    std::function<void(SearchBatch)> searchFinished;
    void interruptSearch();
    void startPendingSearch();
    void searchNext(QString query, SearchCursor cursor, int count, quint64 job,
                    std::shared_ptr<std::atomic_bool> cancelled,
                    std::function<void(SearchBatch)> finished, bool reset);
    void applyChapterMarks(const QJsonObject &marks);
    void request(bool appearanceChange = false, RenderReason reason = RenderReason::Appearance);
    void dispatch(bool pageTurn = false, std::function<void()> afterFrame = {});
};
} // namespace kobalt
