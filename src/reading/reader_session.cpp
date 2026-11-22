#include "reading/header_footer.h"
#include <cmath>
#include "support/qt_compat.h"
#include "support/percentage_text.h"
#include "reader_session.h"
#include "opening_job.h"
#include "sync/kosync.h"
#include "engine/engine_worker.h"
#include "render/highlight_paint.h"
#include "render/page_content.h"
#include "appearance_settings.h"
#include "style_sample.h"
#include "engine/engine_activity.h"
#include "support/diagnostics.h"
#include <QElapsedTimer>
#include "publication/publication.h"
#include "storage/sleep_snapshot.h"
#include "storage/preview_store.h"
#include "support/main_thread.h"
#include <QDir>
#include <QFile>
#include "reading/rendering_mode.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QFontInfo>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QPointer>
#include <QRegion>
#include <QThread>
#include <QTimer>
#include <utility>

namespace kobalt {
namespace {
// Hybrid rendering starts its count after this many page turns without a
// change of the appearance.
constexpr int backgroundCountTurns = 3;

struct DocumentDeleter
{
    void operator()(Engine *value) const
    {
        EngineJob job;
        delete value;
    }
};

} // namespace

namespace {
// Ticks on the same scale as ReaderSession::mainProgress(), measured in a
// layout of the whole book so they match the page that the reader adopts.
// An estimate would put several chapters of one spine item on its first
// page and move them later.
QVector<double> wholeBookTicks(Engine &engine, const QString &first, const QString &after,
                               bool deferred, const std::function<bool()> &current)
{
    // The main text marks, not their resolved positions: those differ
    // between a chapter and the whole book.
    const auto range =
            QString::fromUtf8(QJsonDocument(engine.mainTextMarks).toJson(QJsonDocument::Compact));
    const int total = engine.wholeBookPageCount();
    // Each tick cuts a 4-pixel gap in the bar, so the bar's width sets how
    // many it can hold: one every 12 design pixels on average keeps two
    // thirds of the bar.
    const int maxTicks = engine.viewport().width() / qMax(1, readingPixels(12));
    // Ticks measured for this layout and these marks do not change, unless
    // an older, larger limit allowed them. While the marks wait for their
    // source, `first` and `after` are not theirs.
    QVector<double> saved;
    const bool known = engine.savedWholeBookTicks(range, &saved) && saved.size() <= maxTicks;
    if (known || total <= 0 || deferred)
        return known ? saved : QVector<double>{};
    int firstPage = 0, lastPage = total - 1;
    if (!first.isEmpty() || !after.isEmpty()) {
        const auto bounds = engine.wholeBookPages({ first, after }, current, true);
        if (!first.isEmpty() && bounds.value(0) > 0)
            firstPage = qBound(0, bounds.value(0) - 1, total - 1);
        if (!after.isEmpty() && bounds.value(1) > 0)
            lastPage = qBound(firstPage, qMax(bounds.value(0), bounds.value(1)) - 1, total - 1);
    }
    const int mainPages = lastPage - firstPage + 1;
    // The shallowest level of the contents with three chapter starts inside
    // the main text, between its first and last page, or else the level with
    // the most that the bar can hold. Some books put their chapters
    // under a part or a half-title, beside front and back matter.
    const auto chapters = engine.wholeBookChapters();
    int deepest = 0;
    for (const auto &chapter : chapters)
        deepest = qMax(deepest, chapter.depth);
    QVector<double> ticks;
    for (int depth = 0; depth <= deepest && ticks.size() < 3; ++depth) {
        QStringList positions;
        for (const auto &chapter : chapters)
            if (chapter.depth == depth && !chapter.position.isEmpty())
                positions.append(chapter.position);
        const auto pages = engine.wholeBookPages(positions, current);
        if (pages.size() != positions.size())
            return {};
        QVector<double> level;
        for (const int page : pages) {
            const int main = page - 1 - firstPage;
            // The bar's ends need no tick. Rounded as the saved record keeps
            // them, so a measured tick equals the saved one and nothing
            // repaints.
            if (page > 0 && main > 0 && main < mainPages - 1)
                level.append(std::round(pageProgressFraction(main, mainPages) * 1000000) / 1000000);
        }
        // Deeper levels than one with too many ticks would only show part of
        // the book, so the search ends there.
        if (level.size() > maxTicks)
            break;
        if (level.size() > ticks.size())
            ticks = level;
    }
    engine.rememberWholeBookTicks(ticks, range);
    return ticks;
}
} // namespace

void captureChapters(ReaderFrame &result, Engine &engine, const std::function<bool()> &current)
{
    const auto chapter = engine.chapterProgress();
    result.state["chapter_title"] = chapter.title;
    result.state["chapter_page"] = chapter.page;
    result.state["chapter_pages"] = chapter.pages;
    result.printedPages = engine.printedPages();
    result.landmarks = engine.landmarks();
    result.chapters = engine.chapters();
    result.chapterGraph = engine.chapterGraph();
    result.totalWords = engine.totalWords();
    result.contentsSelection = engine.chapterSelection(result.chapters);
    result.graphSelection = engine.chapterSelection(result.chapterGraph);
    if (engine.partOfBook()) {
        const auto bookPages = [&engine, &current](QVector<ChapterLink> &links) {
            QStringList positions;
            for (const auto &link : links)
                positions.append(link.position);
            const auto pages = engine.bookPages(positions, current);
            for (int i = 0; i < links.size(); ++i)
                links[i].page = pages.value(i, 0) > 0 ? pages[i] - 1 : -1;
        };
        bookPages(result.chapters);
        bookPages(result.chapterGraph);
    }
    auto hints = engine.mainTextHints();
    bool deferred = false;
    if (!engine.validMainTextRange(hints.first, hints.second, &deferred))
        hints.second.clear();
    QString first = hints.first, after = hints.second;
    auto marks = engine.mainTextMarks;
    for (bool start : { true, false }) {
        const auto key = start ? "start" : "end";
        auto mark = marks.value(key).toObject();
        if (mark.isEmpty())
            continue;
        if (!mark.contains("anchor")) {
            const int i = mark.value("index").toInt(-1);
            if (i < 0 || i >= result.chapters.size())
                continue;
            const auto &link = result.chapters[i];
            if (link.title != mark.value("title").toString() || link.position.isEmpty())
                continue;
            // An unloaded spine item can have one placeholder for several TOC entries.
            // Resolve the selected entry against the source before retaining its anchor.
            const auto anchor = start
                    ? engine.chapterBoundaryPosition(result.chapters, i, &deferred)
                    : engine.afterChapter(result.chapters, i, &deferred);
            if (anchor.isNull() || (start && anchor.isEmpty())) {
                marks.remove(key);
                continue;
            }
            mark["anchor"] = anchor;
            marks[key] = mark;
        }
        // An empty exclusive end explicitly includes the last spine item.
        (start ? first : after) = mark.value("anchor").toString();
    }
    // Older versions allowed crossed marks. Restore the offending side to its
    // publication default before computing words or pages, including on reopen.
    if (!engine.validMainTextRange(first, after, &deferred)) {
        if (marks.contains("end")) {
            marks.remove("end");
            after = hints.second;
        }
        if (!engine.validMainTextRange(first, after, &deferred) && marks.contains("start")) {
            marks.remove("start");
            first = hints.first;
        }
        if (!engine.validMainTextRange(first, after, &deferred))
            after.clear();
    }
    if (deferred) {
        // Keep the original marks until the necessary source fragments can be read.
        first = hints.first;
        after = hints.second;
    } else
        engine.mainTextMarks = marks;
    result.state["main_text_marks"] = engine.mainTextMarks;
    QVector<int> bounds;
    if (!first.isEmpty() || !after.isEmpty())
        bounds = engine.bookPages({ first, after }, current, true);
    const int firstPage = first.isEmpty() ? -1 : bounds.value(0, -1);
    int lastPage = after.isEmpty() ? -1 : bounds.value(1, -1);
    // A short interval can round to a single estimated page. Keep its source anchors.
    if (lastPage > 0)
        lastPage = qMax(firstPage, lastPage);
    result.state["main_text_bounded"] =
            !first.isEmpty() || !after.isEmpty() || !engine.mainTextMarks.isEmpty();
    result.mainWords = engine.wordsInMainText(first, after, current);
    // The whole book is shown at a later turn. It does this work now, on the
    // background capture that follows its preparation. Only a capture that
    // can stop for the reader does it: a turn must not walk the whole book.
    if (engine.partOfBook() && current)
        engine.prepareWholeBookText(first, after, current);
    result.state["main_text_first"] = firstPage;
    result.state["main_text_last"] = lastPage;
    result.chapterTicks = wholeBookTicks(engine, first, after, deferred, current);
    if (const double fraction = engine.bookFraction(); fraction >= 0) {
        result.state["book_fraction"] = fraction;
        result.state["whole_book_failed"] = engine.fullDocumentFailed();
        result.state["whole_book_ready"] = engine.fullDocumentReady();
        result.state["slice_start"] = engine.pageStart(0);
        const auto rough = engine.roughPages();
        result.state["rough_page"] = rough.page;
        result.state["rough_pages"] = rough.pages;
        result.state["rough_item_page"] = rough.itemPage;
        result.state["rough_item_pages"] = rough.itemPages;
        result.state["rough_pages_estimated"] = rough.pagesEstimated;
        result.state["rough_item_pages_estimated"] = rough.itemPagesEstimated;
    }
}

namespace {
// Counts one neighbour job from the moment it is queued until it ends.
class NeighbourJob
{
public:
    explicit NeighbourJob(std::shared_ptr<std::atomic_int> count) : count(std::move(count))
    {
        ++*this->count;
    }

    ~NeighbourJob() { --*count; }

    NeighbourJob(const NeighbourJob &) = delete;
    NeighbourJob &operator=(const NeighbourJob &) = delete;

private:
    std::shared_ptr<std::atomic_int> count;
};

// One span for cancellable idle work, including early returns. Only diagnostic
// counters are sampled here; no storage or GUI access occurs on the engine worker.
class BackgroundDiagnostic
{
public:
    BackgroundDiagnostic(const char *work, Engine &engine, const std::function<bool()> &current)
        : work(work), engine(engine), current(current)
    {
        if (!diagnosticsActive())
            return;
        static std::atomic<quint64> sequence{ 0 };
        id = ++sequence;
        timer.start();
        diagnostic("engine-work-start",
                   { { "work", QString::fromLatin1(work) }, { "work_id", double(id) } }, false);
    }

    ~BackgroundDiagnostic()
    {
        if (!id)
            return;
        diagnostic("engine-work-end",
                   { { "work", QString::fromLatin1(work) },
                     { "work_id", double(id) },
                     { "elapsed_ms", timer.nsecsElapsed() / 1e6 },
                     { "completed", !current || current() },
                     { "cached_pages", engine.cachedPages() },
                     { "book_fraction", engine.bookFraction() },
                     { "loaded_pages", engine.pages() },
                     { "resources", engine.resourceStats() } },
                   false);
    }

private:
    const char *work;
    Engine &engine;
    const std::function<bool()> current;
    quint64 id = 0;
    QElapsedTimer timer;
};

// Draws the pages beside the current one, so the next turn finds its image in
// the cache. `direction` is the turn that led here; the page further that way
// is the likelier next one and goes first. Stops when `current` reports a newer
// request, because that request's page matters more than a page nobody asked for.
void prepareNeighbours(Engine &engine, int direction, const std::function<bool()> &current)
{
    BackgroundDiagnostic diagnostic("Neighbour pages and chapters", engine, current);
    EngineSlowdownScope slowdown(true, [&current] { return current && !current(); });
    const auto pages = [&] {
        for (const int step : { direction < 0 ? -1 : 1, direction < 0 ? 1 : -1 }) {
            if (current && !current())
                return;
            engineActivity().checkpoint();
            const int target = engine.turnTarget(engine.page(), step);
            if (target == engine.page())
                continue;
            engine.image(target);
            engine.prepareDamage(engine.page(), target);
        }
    };
    // The next tap most likely turns one page: that page comes first. The
    // work ahead takes longer, and it can replace the layout, which drops
    // the prepared pages. They are then prepared again.
    pages();
    if (current && !current())
        return;
    engine.prepareAhead(current);
    pages();
}

void capturePageCoordinates(QJsonObject &state, Engine &engine)
{
    state["page"] = engine.page();
    state["pages"] = engine.pages();
    state["next_page"] = engine.turnTarget(engine.page(), 1);
    state["continues_before"] = engine.continuesBefore();
    state["continues_after"] = engine.continuesAfter();
    if (engine.partOfBook())
        state["slice_start"] = engine.pageStart(0);
}

ReaderMarkupFrame captureFrameMarkup(Engine &engine, const ReaderMarkupRequest &request,
                                     quint64 generation, qint64 imageKey)
{
    ReaderMarkupFrame result;
    result.request = request;
    result.generation = generation;
    result.imageKey = imageKey;
    if (request.saved.isEmpty() && request.draftStart.isEmpty()) {
        return result;
    }
    QVector<QPair<QString, QString>> ranges;
    ranges.reserve(request.saved.size());
    for (const auto &range : request.saved) {
        ranges.append(qMakePair(range.start, range.end));
    }
    // Only intersecting source ranges reach segment or caret geometry. A book
    // with marks elsewhere keeps the unmarked destination's geometry fast path.
    const auto visible = engine.visibleRanges(ranges);
    int remaining = 65536;
    // A moving draft does not change the saved-range index. Give its geometry
    // the budget first so overlapping saved marks cannot hide the selection.
    if (!request.draftStart.isEmpty()
        && engine.rangeVisible(request.draftStart, request.draftEnd)) {
        engineActivity().checkpoint();
        ++result.resolvedRanges;
        result.draft = engine.annotationGeometry(request.draftStart, request.draftEnd);
        if (result.draft.rectangles.size() > remaining) {
            result.draft.rectangles.resize(remaining);
        }
        remaining -= result.draft.rectangles.size();
    }
    for (const int index : visible) {
        engineActivity().checkpoint();
        if (remaining <= 0) {
            break;
        }
        ++result.resolvedRanges;
        const auto &range = request.saved[index];
        auto rectangles = engine.rangeRects(range.start, range.end);
        if (rectangles.size() > remaining) {
            rectangles.resize(remaining);
        }
        remaining -= rectangles.size();
        result.saved.append({ std::move(rectangles), range.color, range.style });
    }
    return result;
}

} // namespace

void ReaderSession::notifySettled(QPointer<ReaderSession> guard,
                                  const std::shared_ptr<SourceLease> &lease)
{
    kobalt::qt::invoke(
            qApp,
            [guard, lease] {
                if (guard && guard->lease_ == lease && lease && !lease->revoked() && guard->settled)
                    guard->settled();
            },
            Qt::QueuedConnection);
}

// Capture the chapter and book labels on the engine worker. The GUI can apply
// them without changing the displayed page.
bool ReaderSession::publishPageEstimate(Engine &engine, const std::function<bool()> &wanted,
                                        QPointer<ReaderSession> guard, quint64 job,
                                        const std::shared_ptr<SourceLease> &lease)
{
    {
        DiagnosticOperation words("page-estimate-words", [&wanted] { return !wanted(); });
        engine.wordCounts(wanted);
    }
    if (!wanted())
        return false;
    ReaderFrame estimate;
    {
        DiagnosticOperation chapters("page-estimate-chapters", [&wanted] { return !wanted(); });
        captureChapters(estimate, engine, wanted);
    }
    if (!wanted())
        return false;
    estimate.state["page_estimate"] = engine.state().value("page_estimate");
    if (engine.partOfBook())
        estimate.state["whole_book_failed"] = engine.fullDocumentFailed();
    kobalt::qt::invoke(
            qApp,
            [guard, job, lease, estimate] {
                if (!guard || guard->running || guard->revision != job || guard->lease_ != lease
                    || !lease || lease->revoked())
                    return;
                auto &frame = guard->frame;
                const bool wasCounting = guard->paginationPending();
                const bool newBookPages = frame.state.value("columns").toInt() == 2
                        && estimate.state.value("whole_book_ready").toBool()
                        && !frame.state.value("whole_book_ready").toBool();
                const int shownCounts = [&frame] {
                    if (frame.state.value("rough_pages").toInt() <= 0
                        || frame.state.value("rough_item_pages").toInt() <= 0)
                        return 0;
                    return frame.state.value("rough_pages_estimated").toBool()
                                    || frame.state.value("rough_item_pages_estimated").toBool()
                            ? 1
                            : 2;
                }();
                // Most rounds of a long measurement change nothing that the
                // reader shows. Those do not refresh it. The record of the
                // estimate goes with the next save.
                // Ticks come once, from the prepared whole book: show them then.
                const bool newTicks = frame.chapterTicks != estimate.chapterTicks;
                bool changed = frame.chapters != estimate.chapters || newTicks
                        || frame.chapterGraph != estimate.chapterGraph
                        || frame.totalWords != estimate.totalWords
                        || frame.mainWords != estimate.mainWords
                        || frame.contentsSelection != estimate.contentsSelection
                        || frame.graphSelection != estimate.graphSelection;
                for (auto it = estimate.state.begin(); it != estimate.state.end(); ++it) {
                    changed = changed
                            || (it.key() != QLatin1String("page_estimate")
                                && frame.state.value(it.key()) != it.value());
                    frame.state[it.key()] = it.value();
                }
                frame.chapters = estimate.chapters;
                frame.chapterTicks = estimate.chapterTicks;
                frame.chapterGraph = estimate.chapterGraph;
                frame.totalWords = estimate.totalWords;
                frame.mainWords = estimate.mainWords;
                frame.contentsSelection = estimate.contentsSelection;
                frame.graphSelection = estimate.graphSelection;
                // Refresh for the first count, the exact count, or new spread labels.
                // Intermediate estimates wait for the next page turn.
                const auto counts = [](const QJsonObject &state) {
                    if (state.value("rough_pages").toInt() <= 0
                        || state.value("rough_item_pages").toInt() <= 0)
                        return 0;
                    return state.value("rough_pages_estimated").toBool()
                                    || state.value("rough_item_pages_estimated").toBool()
                            ? 1
                            : 2;
                };
                const int after = counts(frame.state);
                const bool step = after > shownCounts;
                if (estimate.state.value("whole_book_ready").toBool())
                    guard->showFullDocument(job);
                // Saved locations can move to another spread even if both totals are the same.
                if (changed
                    && (shownCounts == 0 || step || newBookPages || newTicks
                        || wasCounting != guard->paginationPending())
                    && guard->updated)
                    guard->updated();
            },
            Qt::QueuedConnection);
    return true;
}

// Publish background estimates without drawing the page again.
void ReaderSession::refinePageEstimate(std::shared_ptr<Engine> engine,
                                       const std::function<bool()> &current,
                                       QPointer<ReaderSession> guard, quint64 job,
                                       const std::shared_ptr<SourceLease> &lease, quint64 requests,
                                       const std::shared_ptr<BackgroundWork> &background)
{
    const std::function<bool()> wanted = [current, requests] {
        return current() && engineWorkRequests() == requests;
    };
    // A new engine request can be a preview of this same book, so it does
    // not necessarily change the session revision. Resume behind that work.
    const auto resumeAfterWork = [background, engine, current, guard, job, lease] {
        kobalt::qt::invoke(
                qApp,
                [background, engine, current, guard, job, lease] {
                    if (!guard || !current() || !lease || lease->revoked())
                        return;
                    const quint64 queued = engineWorkRequests();
                    kobalt::qt::invoke(
                            engineWorkerForIdle(),
                            [background, engine, current, guard, job, lease, queued] {
                                if (!current() || !lease || lease->revoked())
                                    return;
                                EngineJob activity;
                                refinePageEstimate(engine, current, guard, job, lease, queued,
                                                   background);
                            },
                            Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
    };
    enum class Publish { Done, Gone, Interrupted };
    const auto publish = [&]() -> Publish {
        if (publishPageEstimate(*engine, wanted, guard, job, lease))
            return Publish::Done;
        return current() ? Publish::Interrupted : Publish::Gone;
    };
    if (engine->bookFraction() < 0) {
        // The whole book is shown. Its parsed-book cache is written in short
        // steps while nothing else needs the engine, so that closing the book
        // has little left to write and a crash keeps the parse.
        static constexpr int cacheStepMilliseconds = 20;
        while (wanted()) {
            engineActivity().checkpoint();
            if (engine->saveDocumentCacheStep(cacheStepMilliseconds)) {
                notifySettled(guard, lease);
                return;
            }
        }
        if (current())
            resumeAfterWork();
        return;
    }
    BackgroundDiagnostic diagnostic("Page estimates", *engine, wanted);
    const auto measure = [&] {
        bool left = false;
        for (int step = 0; step < 32 && wanted(); ++step) {
            engineActivity().checkpoint();
            left = engine->refinePageEstimate(wanted);
            if (!left)
                break;
        }
        return left;
    };
    // Hybrid rendering: the whole book is built beside the chapter. A page
    // turn stops the try, and the next quiet moment starts again. Before
    // the user reads, only a book that is in the parsed-book cache is
    // worth the work: it needs no parsing. It comes before the measuring:
    // it has the real pages. Without a cache file the try ends at once.
    const bool reading = background->count.load();
    // The page estimate does not wait for the whole book: a build from the
    // cache goes first only once the estimate has every spine item.
    const bool cacheFirst =
            !reading && renderingMode::countsInBackground() && engine->progress->estimateComplete();
    bool more = cacheFirst ? false : measure();
    if (!current())
        return;
    if (!wanted()) {
        resumeAfterWork();
        return;
    }
    // The start of a build reads and parses the package of the book before
    // it reaches its first chapter. It waits for a quiet moment, so that it
    // does not meet a page turn. A build that has started goes on in every
    // idle moment: it pauses within milliseconds.
    const qint64 quiet = renderingMode::buildQuietMilliseconds() - engineWorkQuietMilliseconds();
    const bool wait = reading && quiet > 0 && !engine->fullDocumentStarted();
    // The round that finishes the count is shown before the build starts.
    // So are the counts that an earlier session saved, before a build from
    // the parsed-book cache. A started build returns before the publication
    // below, and can take minutes.
    bool published = false;
    if (!more && !engine->fullDocumentStarted()) {
        switch (publish()) {
        case Publish::Gone:
            return;
        case Publish::Interrupted:
            resumeAfterWork();
            return;
        case Publish::Done:
            published = true;
            break;
        }
    }
    if (!more && !wait && (reading || renderingMode::countsInBackground())) {
        BackgroundDiagnostic diagnostic("Full document", *engine, current);
        // The build can run for minutes. It parks as soon as anyone queues
        // work for the engine, not only for a page turn, and goes on after
        // that work.
        const auto full = engine->prepareFullDocument(wanted, !reading);
        if (!current())
            return;
        if (full == Engine::FullDocument::Stopped) {
            // Go on after the work that parked the build. That work is
            // announced a moment before it is queued, so queueing the next
            // step from here could put it in front. The main thread queues
            // it instead: it gets to this only after the code that
            // announced the work has queued it.
            resumeAfterWork();
            return;
        }
        if (full == Engine::FullDocument::Ready) {
            // The whole book has its pages: the page fields can show the
            // exact count before the next turn adopts the whole book.
            const auto publication = publish();
            if (publication == Publish::Interrupted)
                resumeAfterWork();
            if (publication != Publish::Done)
                return;
            // The turn that shows the whole book would draw its page and
            // find its place in the contents. Idle time does that now, in
            // steps that stop when other work waits. The turn does the rest.
            // Only restart if the engine actually saw the pause. Keep this attempt
            // stopped if the GUI has already resumed and queued its replacement.
            bool paused = false;
            const auto prepareWanted = [&] {
                if (!paused) {
                    auto state = background->hybridTurn.load(std::memory_order_relaxed);
                    if (state == HybridTurnPreparation::Paused)
                        paused = background->hybridTurn.compare_exchange_strong(
                                state, HybridTurnPreparation::Deferred, std::memory_order_relaxed);
                    paused = paused || state == HybridTurnPreparation::Deferred;
                }
                return !paused && wanted();
            };
            if (prepareWanted()) {
                BackgroundDiagnostic diagnostic("Whole-book turn", *engine, prepareWanted);
                EngineSlowdownScope slowdown(true, [&] { return !prepareWanted(); });
                engineActivity().checkpoint();
                engine->prepareFullDocumentTurn(prepareWanted);
            }
            if (!paused && current() && !wanted())
                resumeAfterWork();
            return;
        }
    }
    if (cacheFirst) {
        more = measure();
        // The counts may have changed since the publication before the build.
        published = false;
        if (!current())
            return;
        if (!wanted()) {
            resumeAfterWork();
            return;
        }
    }
    if (!published) {
        switch (publish()) {
        case Publish::Gone:
            return;
        case Publish::Interrupted:
            resumeAfterWork();
            return;
        case Publish::Done:
            break;
        }
    }
    if (!more && wait) {
        kobalt::qt::invoke(
                qApp,
                [background, engine, current, guard, job, lease, quiet] {
                    if (!guard || !current() || !lease || lease->revoked())
                        return;
                    kobalt::qt::later(
                            int(quiet) + 1, guard.data(),
                            [background, engine, current, guard, job, lease] {
                                if (!guard || !current() || !lease || lease->revoked())
                                    return;
                                const quint64 queued = engineWorkRequests();
                                kobalt::qt::invoke(
                                        engineWorkerForIdle(),
                                        [background, engine, current, guard, job, lease, queued] {
                                            if (!current() || !lease || lease->revoked())
                                                return;
                                            EngineJob activity;
                                            refinePageEstimate(engine, current, guard, job, lease,
                                                               queued, background);
                                        },
                                        Qt::QueuedConnection);
                            });
                },
                Qt::QueuedConnection);
    }
    if (!more && !wait)
        notifySettled(guard, lease);
    if (more) {
        const quint64 queued = engineWorkRequests();
        kobalt::qt::invoke(
                engineWorker(),
                [background, engine, current, guard, job, lease, queued] {
                    if (!current() || !lease || lease->revoked())
                        return;
                    EngineJob activity;
                    refinePageEstimate(engine, current, guard, job, lease, queued, background);
                },
                Qt::QueuedConnection);
    }
}

// `neighbours` is false when the caller hands the frame to the screen first
// and prepares the neighbour pages afterwards.
ReaderFrame capture(Engine &engine, const PositionCodec &positions, bool neighbours,
                    const ReaderMarkupRequest &markup, quint64 markupGeneration, QJsonObject *times)
{
    engineActivity().checkpoint();
    QElapsedTimer step;
    step.start();
    const auto took = [&](const char *name) {
        if (times)
            (*times)[QLatin1String(name)] = step.nsecsElapsed() / 1e6;
        step.start();
    };
    ReaderFrame result;
    result.image = engine.image(engine.page());
    result.formattingWarning = pageFormattingWarning(result.image);
    took("image_ms");
    result.markup = captureFrameMarkup(engine, markup, markupGeneration, result.image.cacheKey());
    took("markup_ms");
    result.links = engine.pageLinks();
    took("links_ms");
    result.state = engine.state();
    took("state_ms");
    result.nativePosition = engine.platformPosition(positions, &result.nativePositionError);
    took("position_ms");
    captureChapters(result, engine);
    took("chapters_ms");
    result.pageStart = engine.pageStart(engine.page());
    capturePageCoordinates(result.state, engine);
    if (result.formattingWarning)
        diagnostic("page-formatting-warning",
                   { { "page", engine.page() }, { "position", result.state.value("position") } });

    // The header can show it. The engine measured it while it loaded.
    if (const qint64 loadMs = engine.chapterLoadMilliseconds(); loadMs >= 0)
        result.state["chapter_load_ms"] = double(loadMs);
    result.state["text_font_face"] = engine.textFontFace();
    result.state["embedded_font_count"] = engine.embeddedFontCount();
    result.title = engine.title();
    result.fonts = engine.fontFaces();
    result.fontCatalog = engine.fontCatalog();
    engineActivity().checkpoint();
    if (neighbours) {
        prepareNeighbours(engine, 1, {});
        capturePageCoordinates(result.state, engine);
        engineActivity().checkpoint();
    }
    result.state["cached_pages"] = engine.cachedPages();
    result.resources = engine.resourceStats();
    result.error = engine.resourceError();
    return result;
}

ReaderSession::ReaderSession(EngineFontInputs fonts, QString placeholderFamily)
    : fonts_(std::move(fonts)), placeholderFamily_(std::move(placeholderFamily))
{
    const auto worker = engineWorkerState();
    engine_ = std::shared_ptr<Engine>(createEngine(fonts_).release(), [worker](Engine *value) {
        // Session owners live on the GUI thread; queued jobs own the other
        // references. After the GUI has joined the worker, it can destroy a
        // retained document directly without touching either deleted Qt object.
        // While the worker runs, release stays on that thread. Owning the
        // queued value also releases it if shutdown discards the pending call.
        if (worker->stopped.load() || QThread::currentThread() == worker->thread) {
            DocumentDeleter{}(value);
            return;
        }
        auto release = std::make_shared<std::unique_ptr<Engine, DocumentDeleter>>(value);
        kobalt::qt::invoke(worker->object, [release] { release->reset(); }, Qt::QueuedConnection);
    });
}

ReaderSession::ReaderSession(std::shared_ptr<Engine> retainedEngine, QString placeholderFamily)
    : fonts_(retainedEngine->fontInputs()),
      placeholderFamily_(std::move(placeholderFamily)),
      engine_(std::move(retainedEngine))
{
}

std::unique_ptr<ReaderSession> ReaderSession::retained()
{
    if (running || !openingResult_.ready() || !frame.error.isEmpty() || fingerprint.isEmpty())
        return {};
    // Stop background image work before queuing eviction. The existing close
    // barrier drains this job before it releases Nickel's publication service.
    retiring = true;
    ++revision;
    newest->store(revision);
    cancelSearch();
    frame.image = {};
    frame.markup = {};
    frame.damage = {};
    frame.damageFrom = 0;
    frame.state["cached_pages"] = 0;
    frame.resources["allocated_bytes"] = 0;
    frame.resources["pending_images"] = 0;
    frame.resources["ready_images"] = 0;
    auto retained = std::unique_ptr<ReaderSession>(new ReaderSession(engine_, placeholderFamily_));
    retained->frame = frame;
    retained->documentAuthor = documentAuthor;
    retained->documentCover = documentCover;
    retained->frame.turnDirection = 0;
    retained->fingerprint = fingerprint;
    retained->syncDigest_ = syncDigest_;
    retained->syncFilenameDigest_ = syncFilenameDigest_;
    retained->syncFilename_ = syncFilename_;
    retained->desired = desired;
    retained->desiredChapterMode = desiredChapterMode;
    retained->temporaryAppearance = temporaryAppearance;
    retained->originalAppearance = originalAppearance;
    retained->desiredSize = desiredSize;
    retained->lease_ = lease_;
    retained->identity_ = identity_;
    retained->positions_ = positions_;
    cancelSource();
    const auto engine = engine_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine] {
                EngineJob activity;
                engine->releaseRetired();
                engine->releaseImages();
                diagnostic("retained-images-released",
                           { { "cached_pages", engine->cachedPages() },
                             { "resource_cache", engine->resourceStats() } });
                // Keep the parsed book and its pages for the next opening. The reader
                // has closed, so this write delays no page.
                QElapsedTimer timer;
                timer.start();
                const bool saved = engine->saveDocumentCache();
                diagnostic("document-cache-save",
                           { { "saved", saved }, { "elapsed_ms", double(timer.elapsed()) } });
            },
            Qt::QueuedConnection);
    return retained;
}

void ReaderSession::evictDocumentCache(bool allBooks, const QString &documentCache,
                                       const QString &previews)
{
    retiring = true;
    ++revision;
    newest->store(revision);
    cancelSearch();
    cancelSource();
    const auto engine = engine_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, allBooks, documentCache, previews] {
                EngineJob activity;
                mainthread::blocking("document-cache-evict");
                bool removed = engine->removeDocumentCache(allBooks);
                // A book that opened without the cache has no folder of its own.
                if (allBooks && !removed && !documentCache.isEmpty()) {
                    const QFileInfo folder(documentCache);
                    removed = folder.isSymLink() ? QFile::remove(documentCache)
                                                 : QDir(documentCache).removeRecursively();
                }
                const bool tiles =
                        !allBooks || previews.isEmpty() || storage::removePreviewFiles(previews);
                diagnostic("document-cache-evicted",
                           { { "all_books", allBooks },
                             { "removed", removed },
                             { "previews_removed", tiles } },
                           DiagnosticSeverity::Warning);
            },
            Qt::QueuedConnection);
}

void ReaderSession::cancelSource()
{
    layoutReady_ = false;
    newestLayout->fetch_add(1);
    cancelSearch();
    cancelPagePreview();
    cancelPendingTurn();
    if (lease_)
        lease_->cancel();
    clearRenderProgress();
}

void ReaderSession::revokeSource()
{
    layoutReady_ = false;
    newestLayout->fetch_add(1);
    cancelSearch();
    cancelPagePreview();
    cancelPendingTurn();
    if (lease_)
        lease_->revoke();
    clearRenderProgress();
}

void ReaderSession::drain(std::function<void()> finished)
{
    kobalt::qt::invoke(
            engineWorker(),
            [finished] { kobalt::qt::invoke(qApp, finished, Qt::QueuedConnection); },
            Qt::QueuedConnection);
}

void ReaderSession::open(OpenRequest request, std::function<void(OpeningResult)> finished,
                         OpeningProgress progress, std::function<void(QImage)> coverReady,
                         std::function<void()> reusing)
{
    quietTurns_ = 0;
    background_->count.store(false);
    fullDocumentPending_ = false;
    syncDigest_.clear();
    syncFilename_ = QFileInfo(request.syncFilename.isNull() ? request.path : request.syncFilename)
                            .fileName();
    syncFilenameDigest_ = kosync::filenameDigest(syncFilename_);
    if (ownsLease_)
        cancelSource();
    ++revision;
    layoutReady_ = false;
    newestLayout->fetch_add(1);
    layoutClock_.start();
    layoutReadyMilliseconds_ = -1;
    ++layoutStarts_;
    // A preview session shares the engine's progress with the reader.
    if (!preview_)
        engine_->progress->reset();
    newest->store(revision);
    cancelSearch();
    lease_ = request.lease;
    ownsLease_ = true;
    positions_ = request.positions;
    const auto positions = positions_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    const auto lease = lease_;
    const auto path = std::move(request.path), identity = std::move(request.identity);
    const auto size = request.size;
    const bool restartAtStart = request.restartAtStart;
    auto saved = std::move(request.saved);
    temporaryAppearance = request.temporaryAppearance;
    auto original = request.originalAppearance;
    appearance::migrateLineSpacing(original);
    originalAppearance = appearance::SavedInput(original);
    const auto pinnedAppearance = temporaryAppearance;
    const auto appearanceDefaults = std::move(request.appearanceDefaults);
    const auto nativeLocation = std::move(request.platformLocation);
    const auto documentCachePath = std::move(request.documentCachePath);
    const auto userStyleSheetPath = std::move(request.userStyleSheetPath);
    const auto coverPath = std::move(request.coverImagePath);
    const auto factory = std::move(request.resources);
    running = true;
    retiring = false;
    loadingImage = false;
    preview_ = false;
    openingPreview_ = false;
    desiredChapterMode = -1;
    desiredSize = size;
    destination.clear();
    pageDelta = 0;
    pageTarget = -1;
    fractionTarget = -1;
    turning = false;
    pendingTurn = 0;
    QPointer<ReaderSession> guard(this);
    auto engine = engine_;
    // A failed open may still have inspected this archive and recorded its
    // fingerprint. It is not a retained readable document, even on this facade.
    const auto retainedHash =
            openingResult_.ready() && frame.error.isEmpty() && identity_ == identity ? fingerprint
                                                                                     : QByteArray();
    const auto retainedSettings = frame.state;
    frame.formattingWarning = false;
    const auto systemFont = placeholderFamily_;
    const auto latest = newest;
    const auto job = revision;
    const auto notifyReuse = [guard, lease, job, reusing] {
        if (reusing)
            kobalt::qt::invoke(
                    qApp,
                    [guard, lease, job, reusing] {
                        if (guard && guard->revision == job && lease && !lease->revoked())
                            reusing();
                    },
                    Qt::QueuedConnection);
    };
    auto report = [guard, progress, lease, job](OpeningStage stage, int percent) {
        if (!progress)
            return;
        kobalt::qt::invoke(
                qApp,
                [guard, progress, stage, percent, lease, job] {
                    if (guard && guard->revision == job && lease && !lease->revoked())
                        progress(stage, percent);
                },
                Qt::QueuedConnection);
    };
    const auto current = [latest, job, lease] {
        return latest->load() == job && lease && !lease->revoked();
    };
    OpeningJob opening;
    opening.engine = engine;
    opening.path = path;
    opening.size = size;
    opening.saved = std::move(saved);
    opening.nativeLocation = nativeLocation;
    opening.lease = lease;
    opening.factory = factory;
    opening.positions = positions;
    opening.systemFont = systemFont;
    opening.documentCachePath = documentCachePath;
    opening.migrateKoboDocumentCache = request.migrateKoboDocumentCache;
    opening.userStyleSheetPath = userStyleSheetPath;
    opening.customPinsPath = request.customPinsPath;
    opening.dictionariesPath = request.dictionariesPath;
    opening.coverPath = coverPath;
    opening.pinnedAppearance = pinnedAppearance;
    opening.appearanceDefaults = appearanceDefaults;
    opening.restartAtStart = restartAtStart;
    opening.markup = markup;
    opening.markupGeneration = markupGeneration;
    opening.retainedHash = retainedHash;
    opening.retainedSettings = retainedSettings;
    opening.progress = report;
    opening.notifyReuse = notifyReuse;
    if (coverReady)
        opening.coverReady = [guard, lease, job, coverReady](QImage image) {
            kobalt::qt::invoke(
                    qApp,
                    [guard, lease, job, coverReady, image = std::move(image)]() mutable {
                        if (guard && guard->revision == job && !lease->revoked())
                            coverReady(std::move(image));
                    },
                    Qt::QueuedConnection);
        };
    opening.current = current;
    kobalt::qt::invoke(
            engineWorker(),
            [guard, opening = std::move(opening), identity, finished, lease, job]() mutable {
                EngineJob activity;
                auto output = runOpening(opening);
                kobalt::qt::invoke(
                        qApp,
                        [guard, result = std::move(output.frame), outcome = output.outcome,
                         digest = output.digest, syncDigest = output.syncDigest, identity,
                         savedMatches = output.savedMatches, finished, lease, job,
                         customPins = std::move(output.customPins)]() mutable {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, outcome, digest, syncDigest, identity, savedMatches,
                                     finished, lease, job, customPins](ReaderFrame result) mutable {
                                        if (!guard || guard->revision != job
                                            || guard->lease_ != lease)
                                            return;
                                        if (!lease || lease->revoked()) {
                                            result = {};
                                            outcome = OpeningResult::failed(
                                                    lease
                                                                    && lease->end()
                                                                            == SourceLease::End::
                                                                                    Revoked
                                                            ? OpeningFailureKind::Revoked
                                                            : OpeningFailureKind::Cancelled,
                                                    strings::key("error.opening_interrupted"));
                                        }
                                        guard->documentAuthor = result.author;
                                        guard->documentCover = result.cover;
                                        guard->running = false;
                                        guard->frame = result;
                                        guard->openingResult_ = outcome;
                                        guard->layoutReady_ = outcome.ready();
                                        guard->customPins_ = outcome.ready()
                                                ? customPins
                                                : storage::CustomPins();
                                        guard->desired = result.state;
                                        guard->fingerprint = digest;
                                        guard->syncDigest_ =
                                                outcome.ready() ? syncDigest : QByteArray();
                                        if (!outcome.ready()) {
                                            guard->syncFilenameDigest_.clear();
                                            guard->syncFilename_.clear();
                                        }
                                        guard->savedMatched = savedMatches;
                                        guard->identity_ = identity;
                                        finished(outcome);
                                        if (guard) {
                                            guard->startPendingSearch();
                                            // Let opening callbacks build the window and queue
                                            // its presentation work before idle rendering.
                                            guard->resumeIdleWork(1, job);
                                        }
                                    });
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::openPreview(const ReaderSession &source, QSize size,
                                std::function<void(QString)> finished)
{
    if (ownsLease_)
        cancelSource();
    ownsLease_ = false;
    lease_ = source.lease_;
    positions_ = source.positions_;
    markupRequest_ = source.markupRequest_;
    markupGeneration_ = source.markupGeneration_;
    openingResult_ = {};
    layoutReady_ = false;
    ++revision;
    newestLayout->fetch_add(1);
    newest->store(revision);
    const auto job = revision;
    const auto lease = lease_;
    running = true;
    turning = false;
    pendingTurn = 0;
    preview_ = true;
    openingPreview_ = true;
    // A preview starts with the visible book's settings and font catalog.
    // Edits can use them while the worker opens the isolated document.
    frame.state = source.frame.state;
    frame.fonts = source.frame.fonts;
    frame.fontCatalog = source.frame.fontCatalog;
    desired = frame.state;
    desiredChapterMode = -1;
    desiredSize = size;
    QPointer<ReaderSession> guard(this);
    const auto positions = positions_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    auto engine = engine_, book = source.engine_;
    const auto latest = newest;
    kobalt::qt::invoke(
            engineWorker(),
            [guard, engine, book, size, finished, lease, job, positions, latest, markup,
             markupGeneration] {
                EngineJob activity;
                if (latest->load() != job) {
                    return;
                }
                QElapsedTimer timer;
                timer.start();
                diagnostic("preview-start");
                QString error;
                ReaderFrame result;
                if (!lease || lease->revoked())
                    result.error = strings::get("error.opening_interrupted");
                else if (!engine->openPreview(*book, size, &error))
                    result.error = error;
                else if (latest->load() != job) {
                    diagnostic(
                            "preview-end",
                            { { "elapsed_ms", double(timer.elapsed()) }, { "completed", false } },
                            false);
                    return;
                } else
                    result = capture(*engine, positions, false, markup, markupGeneration);
                diagnostic("preview-end",
                           { { "elapsed_ms", double(timer.elapsed()) },
                             { "failed", !result.error.isEmpty() } });
                kobalt::qt::invoke(
                        qApp,
                        [guard, result = std::move(result), finished, lease, job, size]() mutable {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, finished, lease, job,
                                     size](ReaderFrame result) mutable {
                                        if (!guard || guard->revision != job
                                            || guard->lease_ != lease)
                                            return;
                                        if (!lease || lease->revoked()) {
                                            result = {};
                                            result.error =
                                                    strings::get("error.opening_interrupted");
                                        }
                                        const auto requested = guard->requestedAppearance();
                                        const bool edited =
                                                requested != guard->displayedAppearance().settings
                                                || guard->desiredSize != size;
                                        guard->openingPreview_ = false;
                                        guard->running = false;
                                        guard->frame = result;
                                        guard->layoutReady_ = !result.image.isNull()
                                                && result.state.value("pages").toInt() > 0;
                                        guard->desired = result.state;
                                        if (edited && guard->layoutReady_) {
                                            requested.replaceIn(guard->desired);
                                            guard->request(true);
                                        }
                                        finished(result.error);
                                        if (guard) {
                                            guard->startPendingSearch();
                                            guard->loadImages();
                                        }
                                    });
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

bool ReaderSession::setVerticalMargins(int margin)
{
    if (temporaryTypography())
        return false;
    if (margin < 0 || margin > maximumMarginPixels())
        return false;
    if (margin != topMargin() || margin != bottomMargin()) {
        desired["top_margin_pixels"] = margin;
        desired["bottom_margin_pixels"] = margin;
        request(true);
    }
    return true;
}

appearance::SavedInput ReaderSession::persistentAppearance() const
{
    return temporaryTypography() ? originalAppearance : appearance::SavedInput(frame.state);
}

void ReaderSession::editAppearance(const appearance::Patch &patch, QSize size)
{
    requestAppearance(requestedAppearance().edited(patch), size);
}

void ReaderSession::showFullDocument(quint64 job)
{
    if (!currentSourceFrame(job))
        return;
    fullDocumentPending_ = true;
    // The layout took this long. The next turn shows it, maybe much later.
    if (layoutReadyMilliseconds_ < 0 && layoutClock_.isValid())
        layoutReadyMilliseconds_ = layoutClock_.elapsed();
}

void ReaderSession::requestAppearance(const appearance::Requested &settings, QSize size,
                                      int chapterMode)
{
    // While the appearance changes, a count would be thrown away.
    quietTurns_ = 0;
    background_->count.store(false);
    auto next = desired;
    if (temporaryTypography())
        appearance::copy(next, temporaryAppearance);
    else
        settings.replaceIn(next);
    const bool resized = size.isValid() && size != desiredSize;
    if (resized)
        desiredSize = size;
    const int previousMode = desiredChapterMode >= 0
            ? desiredChapterMode
            : (frame.state.contains("book_fraction") ? 1 : 0);
    // Hybrid rendering: another appearance or page size goes back to the
    // chapter, which is quick, and the whole book follows in the background.
    // The same appearance keeps what is shown. An automatic Hybrid edit
    // requests chapter mode even when the last frame is still a chapter:
    // the worker may have adopted the whole book before that frame arrives.
    // A Text preview owns a single section. Reflow it in place; it has no
    // whole-book layout to switch to or rebuild in the background.
    if (preview_)
        chapterMode = -1;
    else if (chapterMode == hybridChapterMode
             || (chapterMode < 0 && renderingMode::countsInBackground()))
        chapterMode = next != desired || resized ? 1 : -1;
    if (chapterMode >= 0)
        fullDocumentPending_ = false;
    const bool modeChanged = chapterMode >= 0 && chapterMode != previousMode;
    if (next != desired || resized || modeChanged) {
        if (chapterMode >= 0)
            desiredChapterMode = chapterMode;
        const auto previousBionic = desired.value("bold_reading").toString("off");
        const auto nextBionic = next.value("bold_reading").toString("off");
        const auto reason = nextBionic != previousBionic
                ? (nextBionic == "off" ? RenderReason::DisableBionic : RenderReason::EnableBionic)
                : (next == desired && resized ? RenderReason::PageLayout
                                              : RenderReason::Appearance);
        // Only the chapter mode changes: Hybrid shows the whole book that its
        // build laid out.
        switchingMode_ = next == desired && !resized;
        desired = next;
        request(true, reason);
        switchingMode_ = false;
    }
}

bool ReaderSession::style(int pixels, int spacing)
{
    return style(pixels, spacing, fontFace(), margin(), alignment());
}

bool ReaderSession::style(int pixels, int spacing, const QString &face, int margin)
{
    return style(pixels, spacing, face, margin, alignment());
}

bool ReaderSession::style(int pixels, int spacing, const QString &face, int margin,
                          const QString &alignment)
{
    if (temporaryTypography())
        return false;
    if (pixels < minimumFontPixels() || pixels > maximumFontPixels() || spacing < 100
        || spacing > maximumLineSpacing || margin < 0 || margin > maximumMarginPixels()
        || (!face.isEmpty() && !frame.fonts.contains(face))
        || (alignment != "publisher" && alignment != "left" && alignment != "justify"
            && alignment != "center" && alignment != "right"))
        return false;
    auto next = desired;
    next["font_pixels"] = pixels;
    next["line_spacing"] = spacing;
    next["font_face"] = face;
    next["margin_pixels"] = margin;
    next["text_alignment"] = alignment;
    if (next != desired) {
        desired = next;
        request(true);
    }
    return true;
}

void ReaderSession::setDarkMode(bool enabled)
{
    if (temporaryTypography())
        return;
    if (desired.value("dark_mode").toBool(false) == enabled)
        return;
    desired["dark_mode"] = enabled;
    request(true);
}

bool ReaderSession::setThickness(int steps)
{
    if (temporaryTypography() || steps < 0 || steps > 12
        || desired.value("bold_reading").toString("off") != "off")
        return false;
    if (steps != thickness()) {
        desired["weight_steps"] = steps;
        request(true);
    }
    return true;
}

bool ReaderSession::setWeight(int steps)
{
    if (temporaryTypography())
        return false;
    const auto axis = weightAxis();
    if (axis.available() ? (steps < axis.minimum || steps > axis.maximum)
                         : (steps < 0 || steps > 12))
        return false;
    if (desired.value("bold_reading").toString("off") != "off")
        return false;
    if (steps != weight()) {
        desired[axis.available() ? "font_weight" : "weight_steps"] = steps;
        request(true);
    }
    return true;
}

bool ReaderSession::setOpticalSize(double points)
{
    const auto axis = opticalSizeAxis();
    if (temporaryTypography() || !axis.available() || !std::isfinite(points)
        || (points < axis.minimum || points > axis.maximum) || points != std::floor(points))
        return false;
    if (points != opticalSize()) {
        desired["font_optical_size"] = points;
        request(true);
    }
    return true;
}

bool ReaderSession::setBookBeautifier(bool enabled)
{
    if (temporaryTypography())
        return false;
    for (const auto *key : appearance::beautifierKeys)
        desired[key] = enabled;
    request(true);
    return true;
}

bool ReaderSession::setHyphenation(const QString &mode)
{
    if (temporaryTypography())
        return false;
    if (mode != "auto" && mode != "on" && mode != "off")
        return false;
    if (mode != hyphenation()) {
        desired["hyphenation"] = mode;
        request(true);
    }
    return true;
}

bool ReaderSession::canTurn() const
{
    return !retiring && !preview_ && openingResult_.ready() && lease_ && !lease_->revoked()
            && pages() > 0 && (!running || turning);
}

bool ReaderSession::paginationPending() const
{
    return renderingMode::countsInBackground() && !preview_ && !retiring && openingResult_.ready()
            && frame.state.contains("book_fraction")
            && !frame.state.value("whole_book_ready").toBool()
            && !frame.state.value("whole_book_failed").toBool();
}

void ReaderSession::requestMenuPagination()
{
    if (!paginationPending() || background_->count.exchange(true))
        return;
    // The initial estimate may already have finished and left the worker idle.
    resumeIdleWork(0, revision);
}

void ReaderSession::cancelPendingTurn()
{
    pendingTurn = 0;
}

void ReaderSession::pauseHybridTurnPreparation(bool paused)
{
    if (paused) {
        auto allowed = HybridTurnPreparation::Allowed;
        background_->hybridTurn.compare_exchange_strong(allowed, HybridTurnPreparation::Paused,
                                                        std::memory_order_relaxed);
    } else if (background_->hybridTurn.exchange(HybridTurnPreparation::Allowed,
                                                std::memory_order_relaxed)
               == HybridTurnPreparation::Deferred) {
        resumeIdleWork(0, revision);
    }
}

bool ReaderSession::turn(int direction)
{
    if ((direction != -1 && direction != 1) || !canTurn())
        return false;
    if (running || pendingTurn) {
        if (pendingTurn != direction)
            diagnostic("page-turn-buffered",
                       { { "direction", direction }, { "replaced", pendingTurn != 0 } }, false);
        pendingTurn = direction;
        return true;
    }
    // An engine with the text of only some spine items continues past its
    // first and last page.
    if ((direction < 0 && page() == 0 && !frame.state.value("continues_before").toBool())
        || (direction > 0 && nextPage() == page()
            && !frame.state.value("continues_after").toBool()))
        return false;
    pageDelta = direction;
    pageTarget = -1;
    fractionTarget = -1;
    destination.clear();
    cancelSearch();
    // A reader who turns pages has settled on the appearance: the count of
    // the whole book is worth its work from here on.
    if (++quietTurns_ == backgroundCountTurns && renderingMode::countsInBackground())
        background_->count.store(true);
    ++revision;
    newest->store(revision);
    dispatch(true);
    return true;
}

bool ReaderSession::go(int target)
{
    // The worker may already have left the slice whose numbers are on screen.
    if (running && frame.state.contains("book_fraction"))
        return false;
    if (target < 0 || target >= pages())
        return false;
    pageTarget = target;
    pageDelta = 0;
    fractionTarget = -1;
    destination.clear();
    request();
    return true;
}

bool ReaderSession::goToFraction(double fraction)
{
    if (!(fraction >= 0 && fraction <= 1))
        return false;
    fractionTarget = fraction;
    pageTarget = -1;
    pageDelta = 0;
    destination.clear();
    request();
    return true;
}

bool ReaderSession::goTo(const QString &anchor, bool keepLayout)
{
    if (anchor.isEmpty())
        return false;
    keepDestinationLayout = keepLayout;
    destination = anchor;
    chapterTarget = -1;
    pageDelta = 0;
    pageTarget = -1;
    fractionTarget = -1;
    request();
    return true;
}

bool ReaderSession::goToChapter(int index)
{
    const auto list = chapters();
    if (index < 0 || index >= list.size() || list[index].position.isEmpty())
        return false;
    keepDestinationLayout = false;
    destination = list[index].position;
    chapterTarget = index;
    pageDelta = 0;
    pageTarget = -1;
    fractionTarget = -1;
    request();
    return true;
}

bool ReaderSession::goToNative(const QString &location, std::function<void(QString)> finished)
{
    return importPosition(location, true, std::move(finished));
}

bool ReaderSession::goToSyncedPosition(const QString &position,
                                       std::function<void(QString)> finished)
{
    return importPosition(position, false, std::move(finished));
}

bool ReaderSession::importPosition(const QString &location, bool native,
                                   std::function<void(QString)> finished)
{
    if (running || retiring || !lease_ || lease_->revoked() || !layoutReady_)
        return false;
    cancelPendingTurn();
    cancelSearch();
    running = true;
    const auto job = ++revision;
    newest->store(job);
    auto engine = engine_;
    const auto positions = positions_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    QPointer<ReaderSession> guard(this);
    const auto lease = lease_;
    kobalt::qt::invoke(
            engineWorker(),
            [guard, engine, location, native, finished, job, lease, positions, markup,
             markupGeneration] {
                EngineJob activity;
                diagnostic("native-import-start");
                QString error;
                if (native)
                    engine->goToPlatform(positions, location, &error);
                else if (!engine->goTo(location))
                    error = strings::get("kosync.position_error");
                auto result = capture(*engine, positions, true, markup, markupGeneration);
                diagnostic("native-import-end", { { "failed", !error.isEmpty() } });
                kobalt::qt::invoke(
                        qApp,
                        [guard, result, error, finished, job, lease] {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, error, finished, job, lease](ReaderFrame result) {
                                        if (!guard || guard->lease_ != lease)
                                            return;
                                        guard->running = false;
                                        if (lease->revoked())
                                            return;
                                        if (job != guard->revision) {
                                            // The owner saves the displayed position in finished().
                                            // Wait for the requested layout before acknowledging it.
                                            const auto position = result.state.value("position");
                                            guard->dispatch(false, [guard, position, error, finished] {
                                                if (error.isEmpty()
                                                    && guard->frame.state.value("position")
                                                            != position)
                                                    finished(strings::get(
                                                            "error.could_not_find_that_passage"));
                                                else
                                                    finished(error);
                                            });
                                            return;
                                        }
                                        guard->frame = result;
                                        finished(error);
                                        if (!guard || guard->lease_ != lease || lease->revoked()
                                            || guard->running || guard->revision != job)
                                            return;
                                        if (guard->updated)
                                            guard->updated();
                                        if (guard) {
                                            guard->startPendingSearch();
                                            guard->loadImages();
                                        }
                                    });
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    return true;
}

void ReaderSession::describeSyncedPosition(const QString &position,
                                           std::function<void(SyncedPositionDescription)> finished)
{
    const auto engine = engine_;
    const auto lease = lease_;
    const auto job = revision;
    const bool ready = layoutReady_;
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, position, lease, job, ready, finished] {
                EngineJob activity;
                SyncedPositionDescription result;
                if (ready && lease && !lease->revoked()) {
                    result.comparison.direction = engine->describePosition(
                            position, &result.chapter, &result.page, &result.comparison.error,
                            [lease] { return !lease->revoked(); });
                    if (result.comparison.error.isEmpty())
                        result.comparison.status = PositionComparison::Status::Compared;
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, lease, job, result, finished]() mutable {
                            if (!guard)
                                return;
                            if (guard->revision != job || guard->running || guard->lease_ != lease
                                || !lease || lease->revoked()) {
                                result = {};
                                result.comparison.status = PositionComparison::Status::Stale;
                            }
                            finished(std::move(result));
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::compareNativePosition(const QString &location,
                                          std::function<void(PositionComparison)> finished)
{
    auto engine = engine_;
    const auto positions = positions_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, location, finished, job, positions] {
                EngineJob activity;
                QString error;
                const int direction = engine->comparePlatformPosition(positions, location, &error);
                kobalt::qt::invoke(
                        qApp,
                        [guard, direction, error, finished, job] {
                            if (!guard)
                                return;
                            PositionComparison result;
                            if (!guard->currentSourceFrame(job) || guard->running)
                                result.status = PositionComparison::Status::Stale;
                            else if (!error.isEmpty())
                                result.error = error;
                            else {
                                result.status = PositionComparison::Status::Compared;
                                result.direction = direction;
                            }
                            finished(result);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::coverImage(QSize maximumSize, std::function<void(QImage)> finished,
                               QString preferredPath)
{
    coverImage(
            maximumSize, [finished](QImage image, QFileInfo) { finished(std::move(image)); },
            preferredPath);
}

void ReaderSession::coverImage(QSize maximumSize, std::function<void(QImage, QFileInfo)> finished,
                               QString preferredPath)
{
    auto engine = engine_;
    const auto lease = lease_;
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, lease, maximumSize, preferredPath, finished] {
                EngineJob activity;
                QFileInfo before(preferredPath);
                const auto bytes = before.size();
                const auto modified = before.lastModified();
                QString usedPath;
                auto image = lease && !lease->revoked()
                        ? engine->coverImage(maximumSize, preferredPath, &usedPath)
                        : QImage();
                const QFileInfo after(usedPath);
                const auto source = !usedPath.isEmpty() && before.isAbsolute() && before.isFile()
                                && bytes == after.size() && modified == after.lastModified()
                        ? before
                        : QFileInfo();
                kobalt::qt::invoke(
                        qApp,
                        [guard, lease, image = std::move(image), source, finished]() mutable {
                            if (!guard)
                                return;
                            const bool valid = guard->lease_ == lease && lease && !lease->revoked();
                            finished(valid ? std::move(image) : QImage(),
                                     valid ? source : QFileInfo());
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::sleepOverlay(const QStringList &directories,
                                 std::function<void(QImage)> finished)
{
    const auto lease = lease_;
    const QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [guard, lease, directories, finished = std::move(finished)] {
                auto image = lease && !lease->revoked() ? storage::loadSleepOverlay(directories)
                                                        : QImage();
                kobalt::qt::invoke(
                        qApp,
                        [guard, lease, image = std::move(image), finished]() mutable {
                            if (guard && guard->lease_ == lease && lease && !lease->revoked())
                                finished(std::move(image));
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

BookmarkLocations captureBookmarkLocations(Engine &engine, const QStringList &positions,
                                           bool rangeEndpoints, bool sourceOrder,
                                           const std::function<bool()> &current)
{
    BookmarkLocations result;
    result.pages = engine.pagesOf(positions, rangeEndpoints, sourceOrder ? &result.order : nullptr,
                                  current);
    result.bookPages = result.pages;
    if (engine.partOfBook()) {
        result.bookPages = engine.bookPages(positions, current, rangeEndpoints);
        for (int &page : result.bookPages)
            page = page > 0 ? page - 1 : -1;
    }
    for (const auto &position : positions) {
        if (current && !current())
            return {};
        result.chapters.append(engine.chapterTitle(position));
    }
    return result;
}

void ReaderSession::sleepWallpapers(const QStringList &directories, const QStringList &wanted,
                                    QSize bound,
                                    std::function<void(QStringList, QVector<QImage>)> finished)
{
    const auto lease = lease_;
    const QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [guard, lease, directories, wanted, bound, finished = std::move(finished)] {
                QStringList names;
                QVector<QImage> images;
                if (lease && !lease->revoked()) {
                    names = storage::sleepWallpapers(directories);
                    for (const auto &name : wanted)
                        images.append(storage::loadSleepWallpaper(directories, name, bound));
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, lease, names = std::move(names), images = std::move(images),
                         finished]() mutable {
                            if (guard && guard->lease_ == lease && lease && !lease->revoked())
                                finished(std::move(names), std::move(images));
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::bookmarkPages(QStringList positions, std::function<void(QVector<int>)> finished,
                                  bool rangeEndpoints)
{
    const auto layout = layoutRevision();
    const auto lease = lease_;
    bookmarkLocations(
            positions,
            [this, positions, layout, lease, rangeEndpoints, finished](BookmarkLocations result) {
                // My Words needs a label for this position even when the chapter gives
                // way to the whole book while its first lookup is queued.
                if (layout != layoutRevision() && layoutReady_ && lease_ == lease && lease
                    && !lease->revoked()) {
                    bookmarkPages(positions, finished, rangeEndpoints);
                    return;
                }
                finished(std::move(result.bookPages));
            },
            rangeEndpoints, false);
}

void ReaderSession::bookmarkLocations(QStringList positions,
                                      std::function<void(BookmarkLocations)> finished,
                                      bool rangeEndpoints, bool sourceOrder,
                                      std::shared_ptr<std::atomic_bool> cancelled)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto latest = newestLayout;
    const auto job = latest->load();
    const auto lease = lease_;
    const bool ready = layoutReady_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, positions, finished, job, latest, lease, ready, rangeEndpoints,
             sourceOrder, cancelled] {
                EngineJob activity;
                BookmarkLocations locations;
                const auto current = [latest, job, lease, ready, cancelled] {
                    return ready && latest->load() == job && lease && !lease->revoked()
                            && (!cancelled || !cancelled->load());
                };
                if (current())
                    locations = captureBookmarkLocations(*engine, positions, rangeEndpoints,
                                                         sourceOrder, current);
                kobalt::qt::invoke(
                        qApp,
                        [guard, locations, finished, job, lease, cancelled] {
                            if (!guard)
                                return;
                            finished(guard->layoutRevision() == job && guard->lease_ == lease
                                                     && lease && !lease->revoked()
                                                     && (!cancelled || !cancelled->load())
                                             ? locations
                                             : BookmarkLocations{});
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::displayedText(int page, std::function<void(QString)> finished)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto lease = lease_;
    const bool ready = layoutReady_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, page, finished, lease, ready] {
                EngineJob activity;
                QString text;
                if (ready && lease && !lease->revoked())
                    text = engine->pageExcerpt(page);
                kobalt::qt::invoke(
                        qApp,
                        [guard, text, finished, lease] {
                            if (guard && guard->lease_ == lease && lease && !lease->revoked())
                                finished(text);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::pageSummary(int page, std::function<void(QString, QString)> finished)
{
    pageSummary(page, [finished = std::move(finished)](const PageSummary &summary) {
        finished(summary.start, summary.excerpt);
    });
}

void ReaderSession::pageSummary(int page, std::function<void(PageSummary)> finished)
{
    const bool pairedWindow =
            frame.state.contains("book_fraction") && frame.state.value("columns").toInt() == 2;
    const auto shown = page == this->page() ? frame.pageStart : QString();
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto latest = newestLayout;
    const auto job = latest->load();
    const auto lease = lease_;
    const bool ready = layoutReady_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, page, finished, job, latest, lease, ready, shown, pairedWindow] {
                EngineJob activity;
                PageSummary summary;
                if (ready && latest->load() == job && lease && !lease->revoked()) {
                    const int local =
                            shown.isEmpty() ? page : engine->pagesOf({ shown }).value(0, -1);
                    if (local >= 0) {
                        summary.start = shown.isEmpty() ? engine->pageStart(local) : shown;
                        summary.excerpt = engine->pageExcerpt(local);
                        summary.chapter = engine->chapterTitle(summary.start);
                        if (pairedWindow && engine->fullDocumentReady() && !summary.start.isEmpty())
                            summary.bookPage =
                                    qMax(-1, engine->bookPages({ summary.start }).value(0) - 1);
                    }
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, summary, finished, job, lease] {
                            if (!guard)
                                return;
                            const bool current = guard->layoutRevision() == job
                                    && guard->lease_ == lease && lease && !lease->revoked();
                            finished(current ? summary : PageSummary{});
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::cancelPagePreview()
{
    newestPreview->fetch_add(1);
}

void ReaderSession::previewPage(int page, QSize bounds, std::function<void(QImage)> finished)
{
    cancelPagePreview();
    if (running || retiring || !lease_ || lease_->revoked() || page < 0 || page >= pages()
        || bounds.isEmpty() || qint64(bounds.width()) * bounds.height() > 1024 * 1024)
        return;
    const auto request = newestPreview->load();
    const auto token = newestPreview;
    const auto latest = newest;
    const auto job = revision;
    const auto lease = lease_;
    const auto engine = engine_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    const bool dark = frame.state.value("dark_mode").toBool(false);
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, page, bounds, finished, request, token, latest, job, lease, markup,
             markupGeneration, dark] {
                if (token->load() != request || latest->load() != job || lease->revoked())
                    return;
                EngineJob activity;
                auto full = engine->image(page);
                if (!full.isNull() && !markup.saved.isEmpty()) {
                    QVector<QPair<QString, QString>> ranges;
                    for (const auto &range : markup.saved)
                        ranges.append(qMakePair(range.start, range.end));
                    const auto geometry = engine->pageRangeRects(page, ranges);
                    bool marked = false;
                    for (const auto &rectangles : geometry)
                        marked = marked || !rectangles.isEmpty();
                    if (marked) {
                        QPainter painter(&full);
                        QRegion covered;
                        for (int index = geometry.size() - 1; index >= 0; --index) {
                            QRegion region;
                            for (const auto &rect : geometry[index])
                                region += rect;
                            render::paintHighlightMark(
                                    painter, geometry[index], region - covered,
                                    render::highlightTint(markup.saved[index].color),
                                    markup.saved[index].style, dark);
                            covered += region;
                        }
                    }
                }
                const auto image =
                        full.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                if (token->load() != request || latest->load() != job || lease->revoked())
                    return;
                kobalt::qt::invoke(
                        qApp,
                        [guard, image, finished, token, latest, job, request, lease,
                         markupGeneration] {
                            if (guard && token->load() == request && latest->load() == job
                                && !lease->revoked()
                                && guard->markupGeneration_ == markupGeneration)
                                finished(image);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::previewAdjacentPage(int direction, QSize bounds, QStringList bookmarkPositions,
                                        std::function<void(QImage, bool)> finished)
{
    cancelPagePreview();
    if (running || retiring || !lease_ || lease_->revoked() || (direction != -1 && direction != 1)
        || bounds.isEmpty() || qint64(bounds.width()) * bounds.height() > 1024 * 1024)
        return;
    const auto request = newestPreview->load();
    const auto token = newestPreview;
    const auto latest = newest;
    const auto job = revision;
    const auto lease = lease_;
    const auto engine = engine_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    const bool dark = frame.state.value("dark_mode").toBool(false);
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, direction, bounds, finished, request, token, latest, job, lease, markup,
             markupGeneration, dark, bookmarkPositions,
             adoptingFullDocument = fullDocumentPending_] {
                const auto current = [token, request, latest, job, lease] {
                    return token->load() == request && latest->load() == job && !lease->revoked();
                };
                if (!current())
                    return;
                EngineJob activity;
                BackgroundDiagnostic diagnostic("Adjacent page preview", *engine, current);
                EngineSlowdownScope slowdown(true, [&current] { return !current(); });
                QVector<QPair<QString, QString>> ranges;
                for (const auto &range : markup.saved)
                    ranges.append(qMakePair(range.start, range.end));
                auto preview = engine->previewTurn(direction, ranges, current, bookmarkPositions,
                                                   adoptingFullDocument);
                if (!current() || preview.image.isNull())
                    return;
                if (!preview.image.isNull() && !markup.saved.isEmpty()) {
                    QPainter painter(&preview.image);
                    QRegion covered;
                    for (int index = preview.ranges.size() - 1; index >= 0; --index) {
                        QRegion region;
                        for (const auto &rect : preview.ranges[index])
                            region += rect;
                        render::paintHighlightMark(painter, preview.ranges[index], region - covered,
                                                   render::highlightTint(markup.saved[index].color),
                                                   markup.saved[index].style, dark);
                        covered += region;
                    }
                }
                const auto image =
                        preview.image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                slowdown.checkpoint();
                if (!current())
                    return;
                kobalt::qt::invoke(
                        qApp,
                        [guard, image, bookmarked = preview.bookmarked, finished, token, latest,
                         job, request, lease, markupGeneration] {
                            if (guard && token->load() == request && latest->load() == job
                                && !lease->revoked()
                                && guard->markupGeneration_ == markupGeneration)
                                finished(image, bookmarked);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

namespace {
ReaderSession::AppearanceImage textPreview(const QImage &image, int cropTop, int inset)
{
    if (image.isNull() || pageNeedsTextSample(image))
        return {};
    cropTop = qBound(0, cropTop, image.height() - 1);
    if (pageTextOnly(image))
        return { image, cropTop };
    const auto illustrations = pageContent(image).images.rectangles;
    if (illustrations.isEmpty())
        return { image, cropTop };

    // Use complete rows outside illustrations, so a tall image cannot be
    // mistaken for a line of text that extends past the preview.
    QRegion rows(QRect(0, cropTop, 1, image.height() - cropTop));
    for (const auto &illustration : illustrations)
        rows -= QRect(0, illustration.top(), 1, illustration.height());
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const auto bands = rows.rects();
#else
    const auto &bands = rows;
#endif
    const auto hasInk = [&image](int y) {
        const auto paper = image.pixel(0, y);
        for (int x = 1; x < image.width(); ++x) {
            if (image.pixel(x, y) != paper)
                return true;
        }
        return false;
    };
    for (const auto &band : bands) {
        for (int y = band.top(); y <= band.bottom(); ++y) {
            if (!hasInk(y))
                continue;
            if (band.top() == cropTop)
                return { image, cropTop };
            int end = y + 1;
            while (end <= band.bottom() && hasInk(end))
                ++end;
            return { image, qMax(band.top(), y - inset), false, end - cropTop };
        }
    }
    return {};
}
} // namespace

void ReaderSession::appearancePage(PreviewPage kind, int cropTop, int inset,
                                   std::function<void(AppearanceImage)> finished)
{
    cancelPagePreview();
    if (running || retiring || !lease_ || lease_->revoked())
        return;
    const auto request = newestPreview->load();
    const auto token = newestPreview;
    const auto latest = newest;
    const auto job = revision;
    const auto lease = lease_;
    const auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, kind, fonts = fonts_, source = image(), settings = state(), cropTop,
             inset, finished, request, token, latest, job, lease] {
                if (token->load() != request || latest->load() != job || lease->revoked())
                    return;
                EngineJob activity;
                DiagnosticOperation operation("appearance-preview-page");
                const auto render = [&]() -> AppearanceImage {
                    if (kind == PreviewPage::Layout)
                        return { engine->imageFromAnchor(true), cropTop };
                    if (!pageNeedsTextSample(source)) {
                        const bool illustrated = !pageTextOnly(source)
                                && !pageContent(source).images.rectangles.isEmpty();
                        const auto body = illustrated ? source : engine->imageFromAnchor(false);
                        const auto preview = textPreview(body, cropTop, inset);
                        if (!preview.image.isNull())
                            return preview;
                    }
                    return { drawReadingTextSample(fonts, source.size(), settings), cropTop, true };
                };
                const auto result = render();
                kobalt::qt::invoke(
                        qApp,
                        [guard, result, finished, token, latest, job, request, lease] {
                            if (guard && token->load() == request && latest->load() == job
                                && !lease->revoked())
                                finished(result);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::nativePosition(std::function<void(QString, QString)> finished)
{
    auto engine = engine_;
    const auto positions = positions_;
    QPointer<ReaderSession> guard(this);
    const auto lease = lease_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, finished, lease, positions] {
                EngineJob activity;
                diagnostic("native-export-start");
                QString error;
                const auto location = engine->platformPosition(positions, &error);
                diagnostic("native-export-end",
                           { { "failed", !error.isEmpty() },
                             { "error_code", diagnosticErrorCode(error) } });
                kobalt::qt::invoke(
                        qApp,
                        [guard, location, error, finished, lease] {
                            if (guard && guard->lease_ == lease)
                                finished(location, error);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::intersectingRanges(const QString &start, const QString &end,
                                       QVector<QPair<QString, QString>> ranges,
                                       std::function<void(QVector<int>)> finished)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto latest = newestLayout;
    const auto job = latest->load();
    const auto lease = lease_;
    const bool ready = layoutReady_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, latest, lease, ready, start, end, ranges = std::move(ranges),
             finished] {
                EngineJob activity;
                const auto result = ready && latest->load() == job && lease && !lease->revoked()
                        ? engine->intersectingRanges(start, end, ranges)
                        : QVector<int>();
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, lease, result, finished] {
                            if (guard) {
                                finished(guard->layoutRevision() == job && guard->lease_ == lease
                                                         && lease && !lease->revoked()
                                                 ? result
                                                 : QVector<int>());
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

bool ReaderSession::currentSourceFrame(quint64 job) const
{
    return revision == job && lease_ && !lease_->revoked();
}

void ReaderSession::annotationPageEdges(std::function<void(AnnotationPageEdges)> finished)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, finished] {
                EngineJob activity;
                const auto result = engine->annotationPageEdges();
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished] {
                            if (guard)
                                finished(guard->currentSourceFrame(job) && !guard->running
                                                 ? result
                                                 : AnnotationPageEdges{});
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::annotationTokenAt(QPoint point, std::function<void(AnnotationToken)> finished,
                                      const QString &boundary, bool start)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, point, finished, boundary, start] {
                EngineJob activity;
                const auto result = boundary.isEmpty()
                        ? engine->annotationTokenAt(point)
                        : engine->annotationDragTokenAt(point, boundary, start);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished] {
                            if (guard) {
                                finished(guard->currentSourceFrame(job) && !guard->running
                                                 ? result
                                                 : AnnotationToken());
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::annotationRange(const QString &startTokenStart, const QString &startTokenEnd,
                                    const QString &endTokenStart, const QString &endTokenEnd,
                                    std::function<void(TextRange)> finished, bool endTokenPolicy,
                                    bool startTokenPolicy)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    const auto latest = newest;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, startTokenStart, startTokenEnd, endTokenStart, endTokenEnd,
             finished, endTokenPolicy, startTokenPolicy, latest] {
                EngineJob activity;
                const auto result = engine->annotationRange(
                        startTokenStart, startTokenEnd, endTokenStart, endTokenEnd, endTokenPolicy,
                        startTokenPolicy, [latest, job] { return latest->load() == job; });
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished] {
                            if (guard) {
                                finished(guard->currentSourceFrame(job) ? result : TextRange());
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::annotationRange(const QString &start, const QString &end,
                                    std::function<void(TextRange)> finished)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, start, end, finished] {
                EngineJob activity;
                const auto result = engine->annotationRange(start, end);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished] {
                            if (guard) {
                                finished(guard->currentSourceFrame(job) ? result : TextRange());
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::textRange(const QString &start, const QString &end,
                              std::function<void(TextRange)> finished, bool withContext)
{
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto latest = newestLayout;
    const auto job = latest->load();
    const auto lease = lease_;
    const bool ready = layoutReady_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, latest, lease, ready, start, end, finished, withContext] {
                EngineJob activity;
                const auto result = ready && latest->load() == job && lease && !lease->revoked()
                        ? engine->textRange(start, end, withContext)
                        : TextRange();
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, lease, result, finished] {
                            if (guard) {
                                finished(guard->layoutRevision() == job && guard->lease_ == lease
                                                         && lease && !lease->revoked()
                                                 ? result
                                                 : TextRange());
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::wordAt(QPoint point, std::function<void(WordSelection)> finished)
{
    if (running)
        return;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, point, finished] {
                EngineJob activity;
                const auto result = engine->wordAt(point);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished] {
                            if (guard && guard->currentSourceFrame(job) && !guard->running)
                                finished(result);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::imageAt(QPoint point, std::function<void(QImage)> finished,
                            const QString &resource)
{
    if (running)
        return;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, point, finished, resource] {
                EngineJob activity;
                const auto image = engine->imageAt(point, resource);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, image, finished] {
                            if (guard)
                                finished(guard->currentSourceFrame(job) && !guard->running
                                                 ? image
                                                 : QImage());
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::linkDetails(const QString &source, std::function<void(LinkDetails)> finished,
                                std::function<bool()> current)
{
    if (running || !lease_ || lease_->revoked())
        return;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    const auto stillCurrent = [latest = newest, job, lease = lease_, current] {
        return latest->load() == job && !lease->revoked() && (!current || current());
    };
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, source, finished, stillCurrent] {
                EngineJob activity;
                if (!stillCurrent())
                    return;
                const auto result = engine->linkDetails(source, true, stillCurrent);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished, stillCurrent] {
                            if (guard && guard->currentSourceFrame(job) && !guard->running
                                && stillCurrent())
                                finished(result);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

ReaderSession::~ReaderSession()
{
    newestLayout->fetch_add(1);
    cancelPagePreview();
    renderProgressChanged = {};
    if (ownsLease_)
        cancelSource();
    newest->fetch_add(1);
    cancelSearch();
}

void ReaderSession::interruptSearch()
{
    if (searchCancelled) {
        searchCancelled->store(true);
    }
    searchCancelled.reset();
}

void ReaderSession::cancelSearch()
{
    searchQuery.clear();
    searchFinished = {};
    interruptSearch();
}

void ReaderSession::search(const QString &query, std::function<void(SearchBatch)> finished,
                           SearchScope scope, const QString &origin, bool details)
{
    cancelSearch();
    searchWithDetails = details;
    if (query.trimmed().isEmpty() || query.size() > 256 || !finished) {
        return;
    }
    searchScope = scope;
    searchOrigin = origin;
    searchQuery = query.trimmed();
    searchFinished = std::move(finished);
    startPendingSearch();
}

void ReaderSession::startPendingSearch()
{
    if (running || searchCancelled || searchQuery.isEmpty() || !searchFinished) {
        return;
    }
    if (retiring || !lease_ || lease_->revoked() || !frame.error.isEmpty()) {
        cancelSearch();
        return;
    }
    searchCancelled = std::make_shared<std::atomic_bool>(false);
    SearchCursor cursor;
    cursor.prepareWholeBook = renderingMode::countsInBackground();
    searchNext(searchQuery, cursor, 0, revision, searchCancelled, searchFinished, true);
}

void ReaderSession::searchDetails(QVector<SearchHit> hits, SearchScope scope, const QString &origin,
                                  std::function<void(QVector<SearchHit>)> finished)
{
    if (running || retiring || !lease_ || lease_->revoked())
        return;
    const auto engine = engine_;
    const auto lease = lease_;
    const auto job = revision;
    const auto latest = newest;
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, lease, job, latest, guard, hits, scope, origin, finished] {
                if (lease->revoked() || latest->load() != job)
                    return;
                EngineJob activity;
                if (lease->revoked() || latest->load() != job)
                    return;
                const auto result = engine->searchDetails(hits, scope, origin);
                kobalt::qt::invoke(
                        qApp,
                        [guard, lease, job, result, finished] {
                            if (guard && guard->lease_ == lease && !lease->revoked()
                                && guard->revision == job && !guard->running)
                                finished(result);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::prepareSearch()
{
    if (running || searchCancelled || !searchQuery.isEmpty() || retiring || !lease_
        || lease_->revoked())
        return;
    searchCancelled = std::make_shared<std::atomic_bool>(false);
    prepareSearchNext(revision, searchCancelled);
}

void ReaderSession::prepareSearchNext(quint64 job, std::shared_ptr<std::atomic_bool> cancelled)
{
    const auto engine = engine_;
    const auto lease = lease_;
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, lease, guard, job, cancelled] {
                if (cancelled->load() || !lease || lease->revoked())
                    return;
                EngineJob activity;
                if (cancelled->load() || lease->revoked())
                    return;
                const bool done = engine->prepareSearch();
                kobalt::qt::invoke(
                        qApp,
                        [guard, lease, job, cancelled, done] {
                            if (!guard || cancelled->load() || lease->revoked()
                                || guard->lease_ != lease || guard->revision != job
                                || guard->running)
                                return;
                            if (done)
                                guard->searchCancelled.reset();
                            else
                                guard->prepareSearchNext(job, cancelled);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::searchNext(QString query, SearchCursor cursor, int count, quint64 job,
                               std::shared_ptr<std::atomic_bool> cancelled,
                               std::function<void(SearchBatch)> finished, bool reset)
{
    const auto engine = engine_;
    const auto lease = lease_;
    const auto scope = searchScope;
    const bool details = searchWithDetails;
    const auto origin = searchOrigin;
    const auto latest = newest;
    const bool needsBookPages = cursor.prepareWholeBook && frame.state.contains("book_fraction")
            && !frame.state.value("whole_book_ready").toBool();
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, query, cursor, count, job, cancelled, finished, reset, lease, scope,
             origin, details, latest, needsBookPages] {
                if (cancelled->load() || !lease || lease->revoked())
                    return;
                EngineJob activity;
                if (cancelled->load() || lease->revoked())
                    return;
                auto result = engine->searchText(
                        query, cursor, count ? (details ? 32 : 512) : 1, scope, origin, details,
                        [cancelled, lease] { return !cancelled->load() && !lease->revoked(); });
                // Dismissing the search discards hits, but the prepared pagination
                // still belongs to this book and layout until its revision changes.
                const auto currentFrame = [lease, latest, job] {
                    return !lease->revoked() && latest->load() == job;
                };
                if (needsBookPages && currentFrame() && engine->fullDocumentReady())
                    publishPageEstimate(*engine, currentFrame, guard, job, lease);
                result.reset = reset;
                kobalt::qt::invoke(
                        qApp,
                        [guard, query, count, job, cancelled, finished, result, lease] {
                            if (!guard || cancelled->load() || lease->revoked()
                                || guard->lease_ != lease || guard->revision != job
                                || guard->running)
                                return;
                            // Keep the UI subscription after completion so reflow can replace
                            // its anchors and details. Eager callers remain one-shot searches.
                            if (result.done && guard->searchWithDetails) {
                                guard->searchQuery.clear();
                                guard->searchFinished = {};
                                guard->searchCancelled.reset();
                            }
                            finished(result);
                            if (guard && !cancelled->load() && !lease->revoked() && !result.done
                                && guard->searchCancelled == cancelled && guard->revision == job
                                && !guard->running)
                                guard->searchNext(query, result.cursor, count + result.hits.size(),
                                                  job, cancelled, finished, false);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::completeFrameMarkup(ReaderFrame result, quint64 job,
                                        const std::shared_ptr<SourceLease> &lease,
                                        std::function<void(ReaderFrame)> finished)
{
    if (revision != job || lease_ != lease || !lease || lease->revoked() || result.image.isNull()
        || !result.error.isEmpty()) {
        finished(std::move(result));
        return;
    }
    if (result.markup.request == markupRequest_) {
        result.markup.generation = markupGeneration_;
        finished(std::move(result));
        return;
    }
    if (markupRequest_.draftStart.isEmpty()
        && result.markup.request.saved == markupRequest_.saved) {
        result.markup.request = markupRequest_;
        result.markup.generation = markupGeneration_;
        result.markup.draft = {};
        finished(std::move(result));
        return;
    }
    // A close, edit or recolor can change markup while a page is rendering.
    // Correct only its geometry before publishing or advancing a buffered turn.
    // One pending frame and one bounded geometry job remain owned by this chain.
    const auto request = markupRequest_;
    const auto generation = markupGeneration_;
    const auto engine = engine_;
    const auto latest = newest;
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, latest, result = std::move(result), request, generation, job, lease,
             finished]() mutable {
                EngineJob activity;
                if (latest->load() == job && lease && !lease->revoked()) {
                    result.markup = captureFrameMarkup(*engine, request, generation,
                                                       result.image.cacheKey());
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, result = std::move(result), job, lease, finished]() mutable {
                            if (guard) {
                                guard->completeFrameMarkup(std::move(result), job, lease,
                                                           std::move(finished));
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

quint64 ReaderSession::setMarkup(const ReaderMarkupRequest &request)
{
    if (request.saved.size() > 2000) {
        return markupGeneration_;
    }
    const auto valid = [](const QString &anchor) {
        return !anchor.isEmpty() && anchor.size() <= 4096;
    };
    // Draft moves share the already validated saved ranges.
    const bool savedChanged = request.saved != markupRequest_.saved;
    if (savedChanged) {
        for (const auto &range : request.saved) {
            if (!valid(range.start) || !valid(range.end))
                return markupGeneration_;
        }
    }
    if ((!request.draftStart.isEmpty() || !request.draftEnd.isEmpty())
        && (!valid(request.draftStart) || !valid(request.draftEnd))) {
        return markupGeneration_;
    }
    if (savedChanged || request.draftStart != markupRequest_.draftStart
        || request.draftEnd != markupRequest_.draftEnd) {
        markupRequest_ = request;
        ++markupGeneration_;
    }
    return markupGeneration_;
}

void ReaderSession::captureMarkup(std::function<void(bool)> finished)
{
    if (running || frame.image.isNull() || !lease_ || lease_->revoked()) {
        finished(false);
        return;
    }
    const auto request = markupRequest_;
    const auto generation = markupGeneration_;
    const auto imageKey = frame.image.cacheKey();
    if (request.saved.isEmpty() && request.draftStart.isEmpty()) {
        frame.markup = {};
        frame.markup.request = request;
        frame.markup.generation = generation;
        frame.markup.imageKey = imageKey;
        finished(true);
        return;
    }
    const auto engine = engine_;
    const auto job = revision;
    const auto lease = lease_;
    const auto latest = newest;
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, lease, latest, request, generation, imageKey, finished] {
                EngineJob activity;
                const bool prepared = latest->load() == job && lease && !lease->revoked();
                auto result = prepared ? captureFrameMarkup(*engine, request, generation, imageKey)
                                       : ReaderMarkupFrame();
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, lease, result = std::move(result), generation, imageKey,
                         finished, prepared]() mutable {
                            if (!guard) {
                                return;
                            }
                            const bool current = prepared && guard->revision == job
                                    && !guard->running && guard->lease_ == lease && lease
                                    && !lease->revoked() && guard->markupGeneration_ == generation
                                    && guard->frame.image.cacheKey() == imageKey;
                            if (current) {
                                guard->frame.markup = std::move(result);
                            }
                            finished(current);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::annotationGeometry(const QString &start, const QString &end,
                                       std::function<void(AnnotationGeometry)> finished)
{
    if (running) {
        return;
    }
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, start, end, finished] {
                EngineJob activity;
                const auto geometry = engine->annotationGeometry(start, end);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, geometry, finished] {
                            if (guard && guard->currentSourceFrame(job) && !guard->running) {
                                finished(geometry);
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::rangeRects(const QString &start, const QString &end,
                               std::function<void(QVector<QRect>)> finished)
{
    if (running)
        return;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, start, end, finished] {
                EngineJob activity;
                const auto result = engine->rangeRects(start, end);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished] {
                            if (guard && guard->currentSourceFrame(job) && !guard->running)
                                finished(result);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::rangeRects(QVector<QPair<QString, QString>> ranges,
                               std::function<void(QVector<QRect>)> finished)
{
    if (running) {
        return;
    }
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, ranges = std::move(ranges), finished] {
                EngineJob activity;
                QVector<QRect> rectangles;
                for (const auto &range : ranges) {
                    rectangles += engine->rangeRects(range.first, range.second);
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, rectangles, finished] {
                            if (guard && guard->currentSourceFrame(job) && !guard->running) {
                                finished(rectangles);
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::rangeRectGroups(QVector<QPair<QString, QString>> ranges,
                                    std::function<void(QVector<QVector<QRect>>)> finished)
{
    if (running) {
        return;
    }
    if (ranges.size() > 2000) {
        finished({});
        return;
    }
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, ranges = std::move(ranges), finished] {
                EngineJob activity;
                QVector<QVector<QRect>> groups;
                groups.reserve(ranges.size());
                int remaining = 65536;
                for (const auto &range : ranges) {
                    auto rectangles = remaining > 0 ? engine->rangeRects(range.first, range.second)
                                                    : QVector<QRect>();
                    if (rectangles.size() > remaining) {
                        rectangles.resize(remaining);
                    }
                    remaining -= rectangles.size();
                    groups.append(std::move(rectangles));
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, groups, finished] {
                            if (guard && guard->currentSourceFrame(job) && !guard->running) {
                                finished(groups);
                            }
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::resize(QSize size)
{
    if (size == desiredSize || size.width() < 320 || size.height() < 240)
        return;
    desiredSize = size;
    // Hybrid rendering: another page size goes back to the chapter, as
    // another appearance does in requestAppearance(). Laying out the whole
    // book again would hold up the reader; it follows in the background.
    if (!preview_ && renderingMode::countsInBackground()) {
        quietTurns_ = 0;
        background_->count.store(false);
        fullDocumentPending_ = false;
        desiredChapterMode = 1;
    }
    request(true, RenderReason::PageLayout);
}

int ReaderSession::firstMainPage() const
{
    return qBound(0, frame.state.value("main_text_first").toInt() - 1, qMax(0, bookPages() - 1));
}

int ReaderSession::lastMainPage() const
{
    const int last = frame.state.value("main_text_last").toInt();
    return last > 0 ? qBound(firstMainPage(), last - 1, qMax(0, bookPages() - 1)) : bookPages() - 1;
}

double ReaderSession::mainProgress() const
{
    if (firstMainPage() == 0 && lastMainPage() == bookPages() - 1)
        return progressFraction();
    return mainPage() < 0 ? 0 : pageProgressFraction(mainPage(), mainPages());
}

QVector<double> ReaderSession::chapterTicks() const
{
    return frame.chapterTicks;
}

int ReaderSession::mainPercent() const
{
    if (firstMainPage() == 0 && lastMainPage() == bookPages() - 1)
        return progressPercent();
    return mainPage() < 0 ? 0 : pageProgressPercent(mainPage(), mainPages());
}

QString ReaderSession::displayPageLabel(int absolute, const QString &style) const
{
    return bookPageLabel(absolute, firstMainPage(), lastMainPage(), style);
}

QString ReaderSession::displayBookPosition(const QString &style) const
{
    if (bookPages() <= 0)
        return strings::percentage(mainPercent());
    const auto label = displayPageLabel(bookPage(), style);
    if (mainPage() < 0 || mainPage() >= mainPages())
        return label;
    return strings::get("stats.page_of")
            .arg(label)
            .arg(headerfooter::pageTotal(mainPages(), bookPagesEstimated()));
}

void ReaderSession::chapterBoundaryActions(
        int index, std::function<void(QVector<ChapterBoundaryAction>)> finished)
{
    const auto links = frame.chapters;
    if (index < 0 || index >= links.size() || links[index].position.isEmpty() || !lease_)
        return;
    const auto engine = engine_;
    const auto lease = lease_;
    const auto job = revision;
    const QPointer<ReaderSession> guard(this);
    qt::invoke(
            engineWorker(),
            [engine, lease, job, guard, links, index, finished] {
                if (lease->revoked())
                    return;
                EngineJob activity;
                const auto original = engine->mainTextMarks;
                auto hints = engine->mainTextHints();
                bool hintsDeferred = false;
                if (!engine->validMainTextRange(hints.first, hints.second, &hintsDeferred))
                    hints.second.clear();
                QVector<ChapterBoundaryAction> actions;
                for (bool start : { true, false }) {
                    if (hintsDeferred)
                        break;
                    const auto key = start ? "start" : "end";
                    for (bool reset : { false, true }) {
                        bool deferred = false;
                        auto marks = original;
                        if (reset) {
                            if (!marks.contains(key))
                                continue;
                            marks.remove(key);
                        } else {
                            const auto &link = links[index];
                            const auto entryStart =
                                    engine->chapterBoundaryPosition(links, index, &deferred);
                            if (entryStart.isEmpty() || deferred)
                                continue;
                            const auto anchor = start
                                    ? entryStart
                                    : engine->afterChapter(links, index, &deferred);
                            if (anchor.isNull() || deferred
                                || (!start
                                    && !engine->validMainTextRange(entryStart, anchor, &deferred)))
                                continue;
                            marks[key] = qt::object({ { "index", index },
                                                      { "title", link.title },
                                                      { "position", link.position },
                                                      { "anchor", anchor } });
                        }
                        const auto anchor = [&marks](const char *side, const QString &fallback) {
                            const auto mark = marks.value(side).toObject();
                            return mark.contains("anchor") ? mark.value("anchor").toString()
                                                           : fallback;
                        };
                        const auto first = anchor("start", hints.first);
                        if (start && !reset && marks.contains("end")) {
                            const auto oldStart =
                                    original.value("start").toObject().value("anchor").toString();
                            // A later start begins a new range, even inside the previous range.
                            const bool startsLater = !oldStart.isEmpty()
                                    && engine->validMainTextRange(oldStart, first, &deferred);
                            const bool fits = startsLater || deferred
                                    || engine->validMainTextRange(
                                            first, anchor("end", hints.second), &deferred);
                            if (deferred)
                                continue;
                            if (startsLater || !fits)
                                marks.remove("end");
                        }
                        if (!engine->validMainTextRange(first, anchor("end", hints.second),
                                                        &deferred))
                            continue;
                        actions.append({ start, reset, [guard, job, original, marks] {
                                            if (!guard || !guard->currentSourceFrame(job)
                                                || guard->mainTextMarks() != original)
                                                return false;
                                            guard->applyChapterMarks(marks);
                                            return true;
                                        } });
                    }
                }
                qt::invoke(
                        qApp,
                        [guard, job, actions, finished] {
                            if (guard && guard->currentSourceFrame(job))
                                finished(actions);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::applyChapterMarks(const QJsonObject &marks)
{
    frame.state["main_text_marks"] = marks;
    const auto engine = engine_;
    const auto lease = lease_;
    qt::invoke(
            engineWorker(),
            [engine, lease, marks] {
                if (!lease->revoked())
                    engine->mainTextMarks = marks;
            },
            Qt::QueuedConnection);
    request();
}

void ReaderSession::setChapters(QVector<ChapterTarget> contents, QVector<ChapterTarget> graph)
{
    const auto positions = positions_;
    const auto lease = lease_;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, contents, graph, positions, lease] {
                if (!lease || lease->revoked())
                    return;
                EngineJob activity;
                auto resolved = resolveChapterTargets(*engine, positions, contents, graph);
                engine->setChapters(std::move(resolved.contents), std::move(resolved.graph));
                ReaderFrame result;
                captureChapters(result, *engine);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, lease] {
                            // A later page or layout job captures these chapters itself. This
                            // metadata update must not cancel a search or replace a newer frame.
                            if (!guard || guard->lease_ != lease || lease->revoked()
                                || guard->revision != job || guard->running)
                                return;
                            guard->frame.chapters = result.chapters;
                            guard->frame.chapterGraph = result.chapterGraph;
                            guard->frame.totalWords = result.totalWords;
                            guard->frame.mainWords = result.mainWords;
                            guard->frame.contentsSelection = result.contentsSelection;
                            guard->frame.graphSelection = result.graphSelection;
                            for (auto it = result.state.begin(); it != result.state.end(); ++it)
                                guard->frame.state[it.key()] = it.value();
                            if (guard->updated)
                                guard->updated();
                            if (guard)
                                guard->loadImages();
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::loadImages()
{
    if (retiring || running || pendingTurn || loadingImage || !lease_ || lease_->revoked()
        || !openingResult_.ready() || frame.resources.value("pending_images").toInt() == 0
        || !frame.error.isEmpty())
        return;
    loadingImage = true;
    const auto engine = engine_;
    const auto latest = newest;
    const auto job = revision;
    QPointer<ReaderSession> guard(this);
    const auto lease = lease_;
    const auto positions = positions_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    const bool preview = preview_;
    kobalt::qt::invoke(
            engineWorker(),
            [guard, engine, latest, job, lease, preview, positions, markup, markupGeneration] {
                EngineJob activity;
                ReaderFrame result;
                bool loaded = false;
                const auto current = [latest, job, lease] {
                    return latest->load() == job && !lease->revoked();
                };
                if (current()) {
                    const bool timing = diagnosticsActive();
                    QElapsedTimer timer;
                    if (timing)
                        timer.start();
                    loaded = engine->loadNextImage();
                    if (loaded && current())
                        result = capture(*engine, positions, false, markup, markupGeneration);
                    if (loaded && timing)
                        diagnostic("image-load-end",
                                   { { "elapsed_ms", double(timer.elapsed()) },
                                     { "current", current() },
                                     { "failed", !engine->resourceError().isEmpty() } },
                                   false);
                }
                // Every loaded image invalidates page pixels. Refill once the batch is
                // complete, after handing over the current frame, rather than redrawing
                // neighbours that the next image will immediately invalidate.
                const bool refill = loaded && !preview && result.error.isEmpty()
                        && result.resources.value("pending_images").toInt() == 0 && current();
                kobalt::qt::invoke(
                        qApp,
                        [guard, loaded, refill, result = std::move(result), job, lease] {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, loaded, refill, job, lease](ReaderFrame result) {
                                        if (!guard || guard->lease_ != lease)
                                            return;
                                        guard->loadingImage = false;
                                        if (lease->revoked())
                                            return;
                                        if (loaded && !guard->running && job == guard->revision) {
                                            guard->frame = result;
                                            if (guard->updated)
                                                guard->updated();
                                        }
                                        if (guard) {
                                            if (refill && job == guard->revision)
                                                guard->resumeIdleWork(1, job);
                                            else if (loaded || job != guard->revision)
                                                guard->loadImages();
                                        }
                                    });
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::resumeIdleWork(int direction, quint64 job)
{
    if (running || pendingTurn || retiring || !currentSourceFrame(job) || !frame.error.isEmpty())
        return;
    loadImages();
    if (loadingImage || preview_ || !openingResult_.ready())
        return;
    const auto engine = engine_;
    const auto latest = newest;
    const auto lease = lease_;
    const auto neighbours = std::make_shared<NeighbourJob>(neighbourJobs);
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, latest, direction, job, guard, lease, neighbours, background = background_] {
                EngineJob activity;
                const bool timing = diagnosticsActive();
                QElapsedTimer timer;
                if (timing)
                    timer.start();
                const std::function<bool()> current = [latest, job, lease] {
                    return latest->load() == job && !lease->revoked();
                };
                prepareNeighbours(*engine, direction, current);
                if (timing)
                    diagnostic("neighbours-end",
                               { { "elapsed_ms", double(timer.elapsed()) },
                                 { "completed", current() } },
                               false);
                // Drawing a neighbour can discover deferred images. Publish them now
                // so they load before that page is requested.
                const auto resources = engine->resourceStats();
                const auto cachedPages = engine->cachedPages();
                // Keep the job active until the GUI receives the new slice coordinates.
                QJsonObject coordinates;
                capturePageCoordinates(coordinates, *engine);
                kobalt::qt::invoke(
                        qApp,
                        [guard, resources, cachedPages, coordinates, job, lease, neighbours] {
                            if (!guard || guard->lease_ != lease || guard->running
                                || !guard->currentSourceFrame(job))
                                return;
                            for (auto it = coordinates.begin(); it != coordinates.end(); ++it)
                                guard->frame.state[it.key()] = it.value();
                            guard->frame.resources = resources;
                            guard->frame.state["cached_pages"] = cachedPages;
                            guard->loadImages();
                            if (guard->idle && !guard->running && !guard->loadingImage)
                                guard->idle();
                        },
                        Qt::QueuedConnection);
                // The page estimate and the build of the whole book are not
                // neighbour work: they run after this job has ended, so
                // that the reader does not count as preparing pages for as
                // long as they take.
                const quint64 queued = engineWorkRequests();
                kobalt::qt::invoke(
                        engineWorker(),
                        [background, engine, current, guard, job, lease, queued] {
                            if (!current())
                                return;
                            EngineJob activity;
                            // After the neighbour pages: the documents that
                            // the last turn replaced.
                            engine->releaseRetired();
                            refinePageEstimate(engine, current, guard, job, lease, queued,
                                               background);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

RenderProgress ReaderSession::renderProgress() const
{
    auto result = renderProgress_;
    if (result.active)
        result.elapsedMilliseconds = renderTimer.elapsed();
    return result;
}

void ReaderSession::beginRenderProgress(RenderReason reason)
{
    renderProgress_ = {};
    renderProgress_.active = true;
    renderProgress_.reason = reason;
    renderProgress_.revision = revision;
    renderTimer.start();
    if (renderProgressChanged)
        renderProgressChanged();
}

void ReaderSession::clearRenderProgress()
{
    if (!renderProgress_.active)
        return;
    renderProgress_ = {};
    renderTimer.invalidate();
    if (renderProgressChanged)
        renderProgressChanged();
}

void ReaderSession::request(bool appearanceChange, RenderReason reason)
{
    // Keep the opening job current. It renders the latest draft after the
    // preview document is ready.
    if (openingPreview_ && appearanceChange)
        return;
    cancelPendingTurn();
    turning = false;
    if (appearanceChange) {
        fullDocumentPending_ = false;
        layoutReady_ = false;
        newestLayout->fetch_add(1);
        // Hybrid's switch to the whole book keeps the layout it has timed.
        if (!switchingMode_) {
            layoutClock_.start();
            layoutReadyMilliseconds_ = -1;
            ++layoutStarts_;
            if (!preview_)
                engine_->progress->reset();
        }
        interruptSearch();
    } else {
        cancelSearch();
    }
    ++revision;
    newest->store(revision);
    QPointer<ReaderSession> guard(this);
    if (appearanceChange || renderProgress_.active)
        beginRenderProgress(appearanceChange ? reason : renderProgress_.reason);
    if (!guard)
        return;
    if (!running)
        dispatch();
}

void ReaderSession::dispatch(bool pageTurn, std::function<void()> afterFrame)
{
    running = true;
    turning = pageTurn;
    const auto positions = positions_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    const auto requested = desired;
    // The prepared document of the whole book is shown with this render.
    const bool keepLayout = std::exchange(keepDestinationLayout, false) && !destination.isEmpty();
    const bool swapToFull =
            !keepLayout && std::exchange(fullDocumentPending_, false) && desiredChapterMode < 0;
    // A ready notification is only a try. Keep it local to this job until
    // adoption succeeds, so a superseded try cannot request a full load.
    const auto chapterMode = desiredChapterMode;
    const auto size = desiredSize;
    const auto anchor = std::exchange(destination, {});
    const int chapter = std::exchange(chapterTarget, -1);
    const auto delta = std::exchange(pageDelta, 0);
    const auto target = std::exchange(pageTarget, -1);
    const double fraction = std::exchange(fractionTarget, -1.0);
    const auto job = revision;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto lease = lease_;
    const auto latest = newest;
    const auto layouts = newestLayout;
    const auto progress = [guard, job, lease, latest, painted = QElapsedTimer(),
                           previousStage = OpeningStage::Styles,
                           previousPercent = -1](OpeningStage stage, int percent) mutable {
        if (latest->load() != job || !lease || lease->revoked())
            return;
        percent = qBound(0, percent, 100);
        const bool changedStage = stage != previousStage;
        if (int(stage) < int(previousStage) || (!changedStage && percent <= previousPercent)) {
            return;
        }
        // Throttle on the worker before queuing GUI work. Phase boundaries and
        // completion stay immediate, as they do while opening a book.
        if (!changedStage && percent != 100 && painted.isValid() && painted.elapsed() < 200) {
            return;
        }
        previousStage = stage;
        previousPercent = percent;
        painted.start();
        kobalt::qt::invoke(
                qApp,
                [guard, job, lease, stage, percent] {
                    if (!guard || guard->revision != job || guard->lease_ != lease || !lease
                        || lease->revoked() || !guard->renderProgress_.active
                        || guard->renderProgress_.revision != job)
                        return;
                    auto &current = guard->renderProgress_;
                    if (current.stage == stage && current.percent == percent)
                        return;
                    current.stage = stage;
                    current.percent = percent;
                    if (guard->renderProgressChanged)
                        guard->renderProgressChanged();
                },
                Qt::QueuedConnection);
    };
    if (diagnosticsActive())
        diagnostic("render-queued", { { "render_id", double(job) }, { "delta", delta } }, false);
    kobalt::qt::invoke(
            engineWorker(),
            [guard, engine, requested, chapterMode, swapToFull, size, anchor, chapter, delta,
             target, fraction, job, lease, latest, layouts, progress, positions, markup,
             markupGeneration, afterFrame] {
                EngineJob activity;
                const bool timing = diagnosticsActive();
                QElapsedTimer timer;
                if (timing)
                    timer.start();
                if (timing)
                    diagnostic("render-start",
                               { { "render_id", double(job) },
                                 { "page_turn", delta != 0 },
                                 { "jump", !anchor.isEmpty() || target >= 0 || fraction >= 0 } },
                               delta == 0);
                EngineSlowdownScope slowdown(true, [latest, job, lease] {
                    return latest->load() != job || !lease || lease->revoked();
                });
                const auto before = engine->settingsState();
                const auto oldSize = engine->viewport();
                auto oldPage = engine->page();
                const auto oldImage = engine->cachedImageKey(oldPage);
                int turned = engine->turnTarget(oldPage, delta);
                bool windowTurned = false;
                bool adopted = false;
                ReaderFrame result;
                bool appearanceChanged = false;
                // Turns carry the last requested settings. Only a settings or viewport
                // change needs validation and application; rotation can change the latter
                // without opening the menu.
                // The prepared document can be gone, after a change that
                // this render does not know. Loading the whole book here
                // would make the page turn wait: keep the chapter then.
                // In two columns the whole book can pair the columns otherwise
                // than the chapter. A turn there stays in the chapter; idle
                // time offers the switch again after it.
                const bool readyToSwap = swapToFull && oldSize == size
                        && appearance::same(before, requested) && engine->fullDocumentReady();
                bool continues = true;
                const int spreadTurn =
                        readyToSwap && delta ? engine->preparedSpreadTurn(delta, &continues) : -1;
                // Page numbers address the displayed chapter. Check the requested
                // spread after moving there, while its column pair is still known.
                const bool numberedHandoff = readyToSwap && target >= 0;
                if (numberedHandoff)
                    continues = engine->go(target) && engine->prepareMatchingSpread();
                const bool swap = readyToSwap && continues;
                const int mode = swap ? 0 : chapterMode;
                const bool modeChanged = mode >= 0 && engine->partOfBook() != (mode > 0);
                const bool unchanged =
                        !modeChanged && appearance::same(before, requested) && oldSize == size;
                bool applied = unchanged
                        || (modeChanged
                                    ? engine->setChapterMode(mode > 0, appearance::Patch(requested),
                                                             size, progress,
                                                             [latest, job, lease] {
                                                                 return latest->load() == job
                                                                         && lease
                                                                         && !lease->revoked();
                                                             })
                                    : engine->applyAppearance(appearance::Patch(requested), size,
                                                              progress));
                // The prepared document refused the place on screen. Nothing
                // else was asked: the reader stays with the chapter, and the
                // turn or jump of this render goes on there.
                const bool refused = !applied && swap && modeChanged
                        && appearance::same(before, requested) && oldSize == size;
                // The engine now pages the whole book, unless it refused.
                adopted = swap && modeChanged && !refused;
                if (refused) {
                    applied = true;
                    diagnostic("full-document-refused", { { "render_id", double(job) } },
                               DiagnosticSeverity::Warning);
                }
                if (!applied)
                    result.error = strings::get("error.could_not_update_text");
                else {
                    appearanceChanged = !unchanged && !refused
                            && ((modeChanged && !swap)
                                || !appearance::same(before, engine->settingsState())
                                || oldSize != engine->viewport());
                    if (swap && modeChanged && !refused) {
                        // The same page in the document of the whole book:
                        // a page turn goes on from there as a page turn.
                        oldPage = engine->page();
                        turned = engine->turnTarget(oldPage, delta);
                    }
                    if (!anchor.isEmpty()
                        && !(chapter >= 0 ? engine->goToChapter(chapter, anchor)
                                          : engine->goTo(anchor)))
                        result.error = strings::get("error.could_not_find_that_passage");
                    else if (target >= 0 && !numberedHandoff)
                        engine->go(target);
                    else if (fraction >= 0)
                        engine->goToFraction(fraction);
                    else if (swap && !refused && spreadTurn >= 0)
                        windowTurned = engine->go(spreadTurn);
                    else if (delta) {
                        const int to = appearanceChanged ? engine->turnTarget(engine->page(), delta)
                                                         : turned;
                        // An engine with the text of only a part of the
                        // book decides each turn itself: the next page can
                        // need text that has no layout yet.
                        if (to == engine->page() || engine->partOfBook())
                            windowTurned = engine->turnInWindow(delta);
                        else
                            engine->go(to);
                    }
                }
                const auto error = result.error;
                // What the render spent before the frame: the appearance, the
                // turn or the jump. The rest is the capture of the frame.
                const double movedMs = timing ? timer.nsecsElapsed() / 1e6 : 0;
                // The panel needs far longer to draw a page than the engine does. Hand
                // the frame over first and prepare the neighbour pages after it.
                QJsonObject captureTimes;
                // Whether idle time drew the page that this render shows.
                const bool imageCached = timing && engine->cachedImageKey(engine->page()) != 0;
                result = capture(*engine, positions, false, markup, markupGeneration,
                                 timing ? &captureTimes : nullptr);
                const double capturedMs = timing ? timer.nsecsElapsed() / 1e6 - movedMs : 0;
                if (!error.isEmpty())
                    result.error = error;
                // A turn into another part of the book is a page turn too.
                // Its page number starts again, so it does not match `turned`.
                if (!appearanceChanged && anchor.isEmpty() && result.error.isEmpty()
                    && qAbs(delta) == 1
                    && ((engine->page() == turned && turned != oldPage) || windowTurned))
                    result.turnDirection = delta;
                if (result.turnDirection && oldImage
                    && engine->preparedDamage(oldImage, result.image.cacheKey(), &result.damage))
                    result.damageFrom = oldImage;
                slowdown.checkpoint();
                if (timing)
                    diagnostic("render-end",
                               { { "render_id", double(job) },
                                 { "emulated_wait_ms", slowdown.addedMilliseconds() },
                                 { "page", engine->page() },
                                 { "cached_pages", engine->cachedPages() },
                                 { "loaded_pages", engine->pages() },
                                 { "resources", result.resources },
                                 { "book_fraction", engine->bookFraction() },
                                 { "move_ms", movedMs },
                                 { "capture_ms", capturedMs },
                                 { "capture", captureTimes },
                                 { "image_cached", imageCached },
                                 { "damage_prepared", result.damageFrom != 0 },
                                 { "elapsed_ms", double(timer.elapsed()) },
                                 { "failed", !result.error.isEmpty() } },
                               result.error.isEmpty() ? DiagnosticSeverity::Activity
                                                      : DiagnosticSeverity::Error);
                else if (!result.error.isEmpty())
                    // Release logging records failed renders only, not every page.
                    diagnostic("render-end", { { "failed", true } }, DiagnosticSeverity::Error);
                result.renderId = job;
                const bool failed = !result.error.isEmpty();
                kobalt::qt::invoke(
                        qApp,
                        [guard, result = std::move(result), job, lease, delta, failed, afterFrame,
                         layouts, adopted] {
                            // Results of queries that ran before this render
                            // reached the GUI before this frame and describe
                            // the chapter that is still shown. Once the whole
                            // book is adopted, every later result must come
                            // from it. A refused switch keeps the chapter
                            // layout and its results.
                            if (adopted)
                                layouts->fetch_add(1);
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, job, lease, delta, failed,
                                     afterFrame](ReaderFrame result) {
                                        if (!guard || guard->lease_ != lease)
                                            return;
                                        guard->running = false;
                                        guard->turning = false;
                                        if (!lease || lease->revoked()) {
                                            guard->clearRenderProgress();
                                            return;
                                        }
                                        // Do not flash intermediate layouts while newer settings wait.
                                        if (job != guard->revision) {
                                            guard->dispatch(false, afterFrame);
                                            return;
                                        }
                                        guard->frame = result;
                                        // Explicit mode is pending intent, not the mode of
                                        // completed frames. Later requests use that frame.
                                        guard->desiredChapterMode = -1;
                                        if (diagnosticsActive())
                                            diagnostic("frame-ready",
                                                       { { "render_id", double(job) },
                                                         { "page", guard->page() } },
                                                       false);
                                        // A rejected jump or appearance edit can retain a
                                        // usable layout while reporting the command error.
                                        guard->layoutReady_ = !result.image.isNull()
                                                && result.state.value("pages").toInt() > 0;
                                        // This is the latest request. Keep the values the engine
                                        // resolved, including the pixel gap for the new font size.
                                        guard->desired = result.state;
                                        if (failed)
                                            guard->cancelPendingTurn();
                                        guard->clearRenderProgress();
                                        if (!guard || guard->lease_ != lease)
                                            return;
                                        if (afterFrame && !lease->revoked()) {
                                            afterFrame();
                                            if (!guard || guard->lease_ != lease || guard->running
                                                || guard->revision != job)
                                                return;
                                        }
                                        if (guard->updated)
                                            guard->updated();
                                        if (guard) {
                                            guard->startPendingSearch();
                                        }
                                        // Let the event loop compose this frame before starting the buffered
                                        // turn. The callback can open controls, change layout or close first.
                                        if (guard && guard->pendingTurn && guard->revision == job) {
                                            QTimer::singleShot(0, guard, [guard, job, lease] {
                                                if (!guard || guard->lease_ != lease
                                                    || guard->revision != job)
                                                    return;
                                                const int direction =
                                                        std::exchange(guard->pendingTurn, 0);
                                                if (direction && guard->canTurn()
                                                    && !guard->turn(direction)
                                                    && guard->turnBoundary)
                                                    guard->turnBoundary(direction);
                                                if (guard && guard->revision == job)
                                                    guard->resumeIdleWork(0, job);
                                            });
                                        }
                                        // Start refill after the GUI has painted this frame. Drawing and
                                        // comparing future pages must not compete with the requested paint.
                                        if (guard && !guard->pendingTurn && !failed)
                                            guard->resumeIdleWork(delta, job);
                                    });
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}
} // namespace kobalt
