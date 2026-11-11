#include <cmath>
#include "support/qt_compat.h"
#include "support/strings.h"
#include "reader_session.h"
#include "opening_job.h"
#include "sync/kosync.h"
#include "engine/crengine/engine_worker.h"
#include "render/highlight_paint.h"
#include "appearance_settings.h"
#include "engine/crengine/engine_activity.h"
#include "support/diagnostics.h"
#include <QElapsedTimer>
#include "publication/publication.h"
#include "publication/layout_compatibility.h"
#include "storage/layout_cache.h"
#include "storage/sleep_snapshot.h"
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <utility>

namespace kobalt {
namespace {
struct DocumentDeleter
{
    void operator()(EngineDocument *value) const
    {
        EngineJob job;
        delete value;
    }
};

} // namespace

void captureChapters(ReaderFrame &result, EngineDocument &engine)
{
    const auto chapter = engine.chapterProgress();
    result.state["chapter_title"] = chapter.title;
    result.state["chapter_page"] = chapter.page;
    result.state["chapter_pages"] = chapter.pages;
    result.chapters = engine.chapters();
    result.chapterGraph = engine.chapterGraph();
    result.totalWords = engine.totalWords();
    result.contentsSelection = engine.chapterSelection(result.chapters);
    result.graphSelection = engine.chapterSelection(result.chapterGraph);
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

// Draws the pages beside the current one, so the next turn finds its image in
// the cache. `direction` is the turn that led here; the page further that way
// is the likelier next one and goes first. Stops when `current` reports a newer
// request, because that request's page matters more than a page nobody asked for.
void prepareNeighbours(EngineDocument &engine, int direction, const std::function<bool()> &current)
{
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
}

ReaderMarkupFrame captureFrameMarkup(EngineDocument &engine, const ReaderMarkupRequest &request,
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

// `neighbours` is false when the caller hands the frame to the screen first
// and prepares the neighbour pages afterwards.
ReaderFrame capture(EngineDocument &engine, const PositionCodec &positions, bool neighbours,
                    const ReaderMarkupRequest &markup, quint64 markupGeneration)
{
    engineActivity().checkpoint();
    ReaderFrame result;
    result.image = engine.image(engine.page());
    result.markup = captureFrameMarkup(engine, markup, markupGeneration, result.image.cacheKey());
    result.links = engine.pageLinks();
    result.state = engine.state();
    result.nativePosition = engine.platformPosition(positions, &result.nativePositionError);
    captureChapters(result, engine);
    result.state["page"] = engine.page();
    result.state["pages"] = engine.pages();
    result.state["next_page"] = engine.turnTarget(engine.page(), 1);
    result.state["text_font_face"] = engine.textFontFace();
    result.state["embedded_font_count"] = engine.embeddedFontCount();
    result.title = engine.title();
    result.fonts = engine.fontFaces();
    result.fontCatalog = engine.fontCatalog();
    engineActivity().checkpoint();
    if (neighbours) {
        prepareNeighbours(engine, 1, {});
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
    engine_ = std::shared_ptr<EngineDocument>(
            new EngineDocument(fonts_), [worker](EngineDocument *value) {
                // Session owners live on the GUI thread; queued jobs own the other
                // references. After the GUI has joined the worker, it can destroy a
                // retained document directly without touching either deleted Qt object.
                // While the worker runs, release stays on that thread. Owning the
                // queued value also releases it if shutdown discards the pending call.
                if (worker->stopped.load() || QThread::currentThread() == worker->thread) {
                    DocumentDeleter{}(value);
                    return;
                }
                auto release =
                        std::make_shared<std::unique_ptr<EngineDocument, DocumentDeleter>>(value);
                kobalt::qt::invoke(
                        worker->object, [release] { release->reset(); }, Qt::QueuedConnection);
            });
}

ReaderSession::ReaderSession(std::shared_ptr<EngineDocument> retainedEngine,
                             QString placeholderFamily)
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
    retained->formattingWarning_ = formattingWarning_;
    retained->formattingDetails_ = formattingDetails_;
    retained->documentAuthor = documentAuthor;
    retained->documentCover = documentCover;
    retained->frame.turnDirection = 0;
    retained->fingerprint = fingerprint;
    retained->syncDigest_ = syncDigest_;
    retained->syncFilenameDigest_ = syncFilenameDigest_;
    retained->syncFilename_ = syncFilename_;
    retained->desired = desired;
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
    syncDigest_.clear();
    syncFilename_ = QFileInfo(request.syncFilename.isNull() ? request.path : request.syncFilename)
                            .fileName();
    syncFilenameDigest_ = kosync::filenameDigest(syncFilename_);
    if (ownsLease_)
        cancelSource();
    ++revision;
    layoutReady_ = false;
    newestLayout->fetch_add(1);
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
    const auto nativeLocation = std::move(request.platformLocation),
               layoutCachePath = std::move(request.layoutCachePath);
    const auto documentCachePath = std::move(request.documentCachePath);
    const auto userStyleSheetPath = std::move(request.userStyleSheetPath);
    const auto coverPath = std::move(request.coverImagePath);
    const auto factory = std::move(request.resources);
    running = true;
    retiring = false;
    loadingImage = false;
    preview_ = false;
    desiredSize = size;
    destination.clear();
    pageDelta = 0;
    pageTarget = -1;
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
    const bool retainedWarning = formattingWarning_;
    const auto retainedDetails = formattingDetails_;
    formattingDetails_ = {};
    formattingWarning_ = false;
    const auto systemFont = placeholderFamily_;
    const auto latest = newest;
    const auto job = revision;
    const auto neighbourCount = neighbourJobs;
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
    opening.layoutCachePath = layoutCachePath;
    opening.documentCachePath = documentCachePath;
    opening.userStyleSheetPath = userStyleSheetPath;
    opening.customPinsPath = request.customPinsPath;
    opening.coverPath = coverPath;
    opening.pinnedAppearance = pinnedAppearance;
    opening.appearanceDefaults = appearanceDefaults;
    opening.restartAtStart = restartAtStart;
    opening.markup = markup;
    opening.markupGeneration = markupGeneration;
    opening.retainedHash = retainedHash;
    opening.retainedSettings = retainedSettings;
    opening.retainedWarning = retainedWarning;
    opening.retainedDetails = retainedDetails;
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
            [guard, opening = std::move(opening), identity, finished, lease, job, current,
             neighbourCount]() mutable {
                EngineJob activity;
                auto output = runOpening(opening);
                const bool opened = output.outcome.ready();
                // The session reports the opening done before this job draws the
                // pages beside the first one. Count that from before the report.
                std::unique_ptr<NeighbourJob> neighbours;
                if (opened)
                    neighbours = std::make_unique<NeighbourJob>(neighbourCount);
                kobalt::qt::invoke(
                        qApp,
                        [guard, result = std::move(output.frame), outcome = output.outcome,
                         digest = output.digest, syncDigest = output.syncDigest, identity,
                         savedMatches = output.savedMatches, finished, lease, job,
                         formattingWarning = output.formattingWarning,
                         formattingDetails = output.formattingDetails,
                         customPins = std::move(output.customPins)]() mutable {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, outcome, digest, syncDigest, identity, savedMatches,
                                     finished, lease, job, formattingWarning, formattingDetails,
                                     customPins](ReaderFrame result) mutable {
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
                                        guard->formattingWarning_ =
                                                outcome.ready() && formattingWarning;
                                        guard->formattingDetails_ = guard->formattingWarning_
                                                ? formattingDetails
                                                : QJsonObject();
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
                                            guard->loadImages();
                                        }
                                    });
                        },
                        Qt::QueuedConnection);
                // Show the first page before drawing the pages beside it, as page turns do.
                if (!opened || !current())
                    return;
                prepareNeighbours(*opening.engine, 1, current);
                // The neighbour pages can find images that the first frame did not count.
                const auto resources = opening.engine->resourceStats();
                const auto cachedPages = opening.engine->cachedPages();
                kobalt::qt::invoke(
                        qApp,
                        [guard, resources, cachedPages, job] {
                            if (!guard || guard->running || guard->revision != job)
                                return;
                            guard->frame.resources = resources;
                            guard->frame.state["cached_pages"] = cachedPages;
                            guard->loadImages();
                            if (guard->idle && !guard->running && !guard->loadingImage)
                                guard->idle();
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
                    return;
                } else
                    result = capture(*engine, positions, false, markup, markupGeneration);
                diagnostic("preview-end",
                           { { "elapsed_ms", double(timer.elapsed()) },
                             { "failed", !result.error.isEmpty() } });
                kobalt::qt::invoke(
                        qApp,
                        [guard, result = std::move(result), finished, lease, job]() mutable {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, finished, lease, job](ReaderFrame result) mutable {
                                        if (!guard || guard->revision != job
                                            || guard->lease_ != lease)
                                            return;
                                        if (!lease || lease->revoked()) {
                                            result = {};
                                            result.error =
                                                    strings::get("error.opening_interrupted");
                                        }
                                        guard->running = false;
                                        guard->frame = result;
                                        guard->layoutReady_ = !result.image.isNull()
                                                && result.state.value("pages").toInt() > 0;
                                        guard->desired = result.state;
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

void ReaderSession::requestAppearance(const appearance::Requested &settings, QSize size)
{
    auto next = desired;
    if (temporaryTypography())
        appearance::copy(next, temporaryAppearance);
    else
        settings.replaceIn(next);
    const bool resized = size.isValid() && size != desiredSize;
    if (resized)
        desiredSize = size;
    if (next != desired || resized) {
        const auto previousBionic = desired.value("bold_reading").toString("off");
        const auto nextBionic = next.value("bold_reading").toString("off");
        const auto reason = nextBionic != previousBionic
                ? (nextBionic == "off" ? RenderReason::DisableBionic : RenderReason::EnableBionic)
                : (next == desired && resized ? RenderReason::PageLayout
                                              : RenderReason::Appearance);
        desired = next;
        request(true, reason);
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

void ReaderSession::cancelPendingTurn()
{
    pendingTurn = 0;
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
    if ((direction < 0 && page() == 0) || (direction > 0 && nextPage() == page()))
        return false;
    pageDelta = direction;
    pageTarget = -1;
    destination.clear();
    cancelSearch();
    ++revision;
    newest->store(revision);
    dispatch(true);
    return true;
}

bool ReaderSession::go(int target)
{
    if (target < 0 || target >= pages())
        return false;
    pageTarget = target;
    pageDelta = 0;
    destination.clear();
    request();
    return true;
}

bool ReaderSession::goTo(const QString &anchor)
{
    if (anchor.isEmpty())
        return false;
    destination = anchor;
    pageDelta = 0;
    pageTarget = -1;
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
    if (running)
        return false;
    cancelSearch();
    running = true;
    const auto job = revision;
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
                                        guard->frame = result;
                                        // The owner records the accepted native baseline before saving the
                                        // newly imported frame. A failed import leaves the anchor unchanged.
                                        finished(error);
                                        if (!guard || guard->lease_ != lease)
                                            return;
                                        if (!guard->running && job != guard->revision)
                                            guard->dispatch();
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
                    result.comparison.direction =
                            engine->comparePosition(position, &result.comparison.error);
                    if (result.comparison.error.isEmpty()) {
                        result.comparison.status = PositionComparison::Status::Compared;
                        result.chapter = engine->chapterTitle(position);
                        result.page = engine->pagesOf({ position }).value(0, -1);
                    }
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
                            if (guard->revision != job || guard->running)
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

void ReaderSession::bookmarkPages(QStringList positions, std::function<void(QVector<int>)> finished,
                                  bool rangeEndpoints)
{
    bookmarkLocations(
            std::move(positions),
            [finished](BookmarkLocations result) { finished(std::move(result.pages)); },
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
                    locations.pages =
                            engine->pagesOf(positions, rangeEndpoints,
                                            sourceOrder ? &locations.order : nullptr, current);
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
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto latest = newestLayout;
    const auto job = latest->load();
    const auto lease = lease_;
    const bool ready = layoutReady_;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, page, finished, job, latest, lease, ready] {
                EngineJob activity;
                QString start, excerpt;
                if (ready && latest->load() == job && lease && !lease->revoked()) {
                    start = engine->pageStart(page);
                    excerpt = engine->pageExcerpt(page);
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, start, excerpt, finished, job, lease] {
                            if (!guard)
                                return;
                            const bool current = guard->layoutRevision() == job
                                    && guard->lease_ == lease && lease && !lease->revoked();
                            finished(current ? start : QString(), current ? excerpt : QString());
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

void ReaderSession::anchoredPage(bool stayInChapter, std::function<void(QImage)> finished)
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
            [engine, guard, stayInChapter, finished, request, token, latest, job, lease] {
                if (token->load() != request || latest->load() != job || lease->revoked())
                    return;
                EngineJob activity;
                DiagnosticOperation operation("appearance-preview-page");
                const auto image = engine->imageFromAnchor(stayInChapter);
                kobalt::qt::invoke(
                        qApp,
                        [guard, image, finished, token, latest, job, request, lease] {
                            if (guard && token->load() == request && latest->load() == job
                                && !lease->revoked())
                                finished(image);
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
                diagnostic("native-export-end", { { "failed", !error.isEmpty() } });
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
                                finished(guard->revision == job && !guard->running
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
                                finished(guard->revision == job && !guard->running
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
                                finished(guard->revision == job ? result : TextRange());
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
                                finished(guard->revision == job ? result : TextRange());
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
                            if (guard && guard->revision == job && !guard->running)
                                finished(result);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::imageAt(QPoint point, std::function<void(QImage)> finished)
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
                const auto image = engine->imageAt(point);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, image, finished] {
                            if (guard)
                                finished(guard->revision == job && !guard->running ? image
                                                                                   : QImage());
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ReaderSession::linkDetails(const QString &source, std::function<void(LinkDetails)> finished)
{
    if (running)
        return;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, source, finished] {
                EngineJob activity;
                const auto result = engine->linkDetails(source);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result, finished] {
                            if (guard && guard->revision == job && !guard->running)
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
    searchNext(searchQuery, {}, 0, revision, searchCancelled, searchFinished, true);
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
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, query, cursor, count, job, cancelled, finished, reset, lease, scope,
             origin, details] {
                if (cancelled->load() || !lease || lease->revoked())
                    return;
                EngineJob activity;
                if (cancelled->load() || lease->revoked())
                    return;
                auto result = engine->searchText(query, cursor, count ? (details ? 32 : 512) : 1,
                                                 scope, origin, details);
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

quint64 ReaderSession::setMarkup(ReaderMarkupRequest request)
{
    if (request.saved.size() > 2000) {
        return markupGeneration_;
    }
    const auto valid = [](const QString &anchor) {
        return !anchor.isEmpty() && anchor.size() <= 4096;
    };
    for (const auto &range : request.saved) {
        if (!valid(range.start) || !valid(range.end)) {
            return markupGeneration_;
        }
    }
    if ((!request.draftStart.isEmpty() || !request.draftEnd.isEmpty())
        && (!valid(request.draftStart) || !valid(request.draftEnd))) {
        return markupGeneration_;
    }
    if (!(request == markupRequest_)) {
        markupRequest_ = std::move(request);
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
                            if (guard && guard->revision == job && !guard->running) {
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
                            if (guard && guard->revision == job && !guard->running)
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
                            if (guard && guard->revision == job && !guard->running) {
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
                            if (guard && guard->revision == job && !guard->running) {
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
    request(true, RenderReason::PageLayout);
}

void ReaderSession::setChapters(QVector<ChapterTarget> contents, QVector<ChapterTarget> graph)
{
    const auto positions = positions_;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto job = revision;
    kobalt::qt::invoke(
            engineWorker(),
            [engine, guard, job, contents, graph, positions] {
                EngineJob activity;
                auto resolved = resolveChapterTargets(*engine, positions, contents, graph);
                engine->setChapters(std::move(resolved.contents), std::move(resolved.graph));
                ReaderFrame result;
                captureChapters(result, *engine);
                kobalt::qt::invoke(
                        qApp,
                        [guard, job, result] {
                            // A later page or layout job captures these chapters itself. This
                            // metadata update must not cancel a search or replace a newer frame.
                            if (!guard || guard->revision != job || guard->running)
                                return;
                            guard->frame.chapters = result.chapters;
                            guard->frame.chapterGraph = result.chapterGraph;
                            guard->frame.totalWords = result.totalWords;
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
    if (retiring || running || pendingTurn || loadingImage || !openingResult_.ready()
        || frame.resources.value("pending_images").toInt() == 0 || !frame.error.isEmpty())
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
                if (latest->load() == job) {
                    const bool timing = diagnosticsActive();
                    QElapsedTimer timer;
                    if (timing)
                        timer.start();
                    loaded = engine->loadNextImage();
                    if (loaded && latest->load() == job)
                        result = capture(*engine, positions, false, markup, markupGeneration);
                    if (loaded && timing)
                        diagnostic("image-load-end",
                                   { { "elapsed_ms", double(timer.elapsed()) },
                                     { "current", latest->load() == job },
                                     { "failed", !engine->resourceError().isEmpty() } },
                                   false);
                }
                // Every loaded image invalidates page pixels. Refill once the batch is
                // complete, after handing over the current frame, rather than redrawing
                // neighbours that the next image will immediately invalidate.
                const bool refill = loaded && !preview && result.error.isEmpty()
                        && result.resources.value("pending_images").toInt() == 0
                        && latest->load() == job;
                kobalt::qt::invoke(
                        qApp,
                        [guard, loaded, result = std::move(result), job, lease] {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, loaded, job, lease](ReaderFrame result) {
                                        if (!guard || guard->lease_ != lease)
                                            return;
                                        guard->loadingImage = false;
                                        if (loaded && !guard->running && job == guard->revision) {
                                            guard->frame = result;
                                            if (guard->updated)
                                                guard->updated();
                                        }
                                        if (guard && (loaded || job != guard->revision))
                                            guard->loadImages();
                                    });
                        },
                        Qt::QueuedConnection);
                if (refill) {
                    prepareNeighbours(*engine, 1, [latest, job] { return latest->load() == job; });
                    const auto resources = engine->resourceStats();
                    const auto cachedPages = engine->cachedPages();
                    kobalt::qt::invoke(
                            qApp,
                            [guard, resources, cachedPages, job, lease] {
                                if (!guard || guard->lease_ != lease || guard->running
                                    || guard->revision != job)
                                    return;
                                guard->frame.resources = resources;
                                guard->frame.state["cached_pages"] = cachedPages;
                                guard->loadImages();
                                if (guard->idle && !guard->running && !guard->loadingImage)
                                    guard->idle();
                            },
                            Qt::QueuedConnection);
                }
            },
            Qt::QueuedConnection);
}

void ReaderSession::resumeIdleWork(int direction, quint64 job)
{
    if (running || pendingTurn || retiring || revision != job || !frame.error.isEmpty())
        return;
    loadImages();
    if (loadingImage || preview_ || !openingResult_.ready() || !lease_ || lease_->revoked())
        return;
    const auto engine = engine_;
    const auto latest = newest;
    const auto lease = lease_;
    const auto neighbours = std::make_shared<NeighbourJob>(neighbourJobs);
    QPointer<ReaderSession> guard(this);
    kobalt::qt::invoke(
            engineWorker(),
            [engine, latest, direction, job, guard, lease, neighbours] {
                EngineJob activity;
                const bool timing = diagnosticsActive();
                QElapsedTimer timer;
                if (timing)
                    timer.start();
                prepareNeighbours(*engine, direction,
                                  [latest, job] { return latest->load() == job; });
                if (timing)
                    diagnostic("neighbours-end",
                               { { "elapsed_ms", double(timer.elapsed()) },
                                 { "completed", latest->load() == job } },
                               false);
                // Drawing a neighbour can discover deferred images. Publish them now
                // so they load before that page is requested.
                const auto resources = engine->resourceStats();
                const auto cachedPages = engine->cachedPages();
                kobalt::qt::invoke(
                        qApp,
                        [guard, resources, cachedPages, job, lease] {
                            if (!guard || guard->lease_ != lease || guard->running
                                || guard->revision != job)
                                return;
                            guard->frame.resources = resources;
                            guard->frame.state["cached_pages"] = cachedPages;
                            guard->loadImages();
                            if (guard->idle && !guard->running && !guard->loadingImage)
                                guard->idle();
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
    cancelPendingTurn();
    turning = false;
    if (appearanceChange) {
        layoutReady_ = false;
        newestLayout->fetch_add(1);
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

void ReaderSession::dispatch(bool pageTurn)
{
    running = true;
    turning = pageTurn;
    const auto positions = positions_;
    const auto markup = markupRequest_;
    const auto markupGeneration = markupGeneration_;
    const auto requested = desired;
    const auto size = desiredSize;
    const auto anchor = std::exchange(destination, {});
    const auto delta = std::exchange(pageDelta, 0);
    const auto target = std::exchange(pageTarget, -1);
    const auto job = revision;
    auto engine = engine_;
    QPointer<ReaderSession> guard(this);
    const auto lease = lease_;
    const auto latest = newest;
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
            [guard, engine, requested, size, anchor, delta, target, job, lease, progress, positions,
             markup, markupGeneration] {
                EngineJob activity;
                const bool timing = diagnosticsActive();
                QElapsedTimer timer;
                if (timing)
                    timer.start();
                if (timing)
                    diagnostic("render-start",
                               { { "render_id", double(job) },
                                 { "page_turn", delta != 0 },
                                 { "jump", !anchor.isEmpty() || target >= 0 } },
                               delta == 0);
                const auto before = engine->state();
                const auto oldSize = engine->viewport();
                const auto oldPage = engine->page();
                const auto oldImage = engine->cachedImageKey(oldPage);
                const int turned = engine->turnTarget(oldPage, delta);
                ReaderFrame result;
                bool appearanceChanged = false;
                // Turns carry the last requested settings. Only a settings or viewport
                // change needs validation and application; rotation can change the latter
                // without opening the menu.
                const bool unchanged = appearance::same(before, requested) && oldSize == size;
                if (!unchanged
                    && !engine->applyAppearance(kobalt::appearance::Patch(requested), size,
                                                progress))
                    result.error = strings::get("error.could_not_update_text");
                else {
                    appearanceChanged = !unchanged
                            && (!appearance::same(before, engine->state())
                                || oldSize != engine->viewport());
                    if (!anchor.isEmpty() && !engine->goTo(anchor))
                        result.error = strings::get("error.could_not_find_that_passage");
                    else if (target >= 0)
                        engine->go(target);
                    else if (delta)
                        engine->go(appearanceChanged ? engine->turnTarget(engine->page(), delta)
                                                     : turned);
                }
                const auto error = result.error;
                // The panel needs far longer to draw a page than the engine does. Hand
                // the frame over first and prepare the neighbour pages after it.
                result = capture(*engine, positions, false, markup, markupGeneration);
                if (!error.isEmpty())
                    result.error = error;
                if (!appearanceChanged && anchor.isEmpty() && result.error.isEmpty()
                    && qAbs(delta) == 1 && engine->page() == turned && turned != oldPage)
                    result.turnDirection = delta;
                if (result.turnDirection && oldImage
                    && engine->preparedDamage(oldImage, result.image.cacheKey(), &result.damage))
                    result.damageFrom = oldImage;
                if (timing)
                    diagnostic("render-end",
                               { { "render_id", double(job) },
                                 { "page", engine->page() },
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
                        [guard, result = std::move(result), job, lease, delta, failed] {
                            if (!guard) {
                                return;
                            }
                            guard->completeFrameMarkup(
                                    std::move(result), job, lease,
                                    [guard, job, lease, delta, failed](ReaderFrame result) {
                                        if (!guard || guard->lease_ != lease)
                                            return;
                                        guard->running = false;
                                        guard->turning = false;
                                        // Do not flash intermediate layouts while newer settings wait.
                                        if (job != guard->revision) {
                                            guard->dispatch();
                                            return;
                                        }
                                        guard->frame = result;
                                        if (diagnosticsActive())
                                            diagnostic("frame-ready",
                                                       { { "render_id", double(job) },
                                                         { "page", guard->page() } },
                                                       false);
                                        // A rejected jump or appearance edit can retain a
                                        // usable layout while reporting the command error.
                                        guard->layoutReady_ = !result.image.isNull()
                                                && result.state.value("pages").toInt() > 0;
                                        if (!result.error.isEmpty())
                                            guard->desired = result.state;
                                        if (failed)
                                            guard->cancelPendingTurn();
                                        guard->clearRenderProgress();
                                        if (!guard)
                                            return;
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
