#include "opening_job.h"
#include "appearance_settings.h"
#include "engine/crengine/engine_activity.h"
#include "publication/layout_compatibility.h"
#include "storage/layout_cache.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "sync/kosync.h"
#include <QCryptographicHash>
#include <QFileInfo>

namespace kobalt {
namespace {
QByteArray readUserStyleSheet(const QString &path)
{
    if (path.isEmpty() || !QFileInfo(path).isFile()) {
        return {};
    }
    // Bound memory on small devices. A missing or unusable stylesheet never
    // prevents reading, and an empty file intentionally disables custom CSS.
    constexpr qint64 maximumBytes = 256 * 1024;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumBytes) {
        return {};
    }
    const auto bytes = file.read(maximumBytes + 1);
    if (file.error() != QFile::NoError || bytes.size() > maximumBytes || bytes.contains('\0')) {
        return {};
    }
    return bytes;
}

// Nickel's reader opens every saved position, falling back to the chapter or
// the book start without a message. Log the fallback, with the chapter file
// inside the book, so a device report can tell which position failed.
bool openAtPlatform(EngineDocument &engine, const PositionCodec &codec, const QString &location,
                    QString *failure)
{
    QString error;
    const auto target = engine.goToPlatformNearest(codec, location, &error);
    if (target == EngineDocument::PositionTarget::Unavailable) {
        if (failure)
            *failure = error;
        return false;
    }
    if (target != EngineDocument::PositionTarget::Exact) {
        diagnostic("native-position-fallback",
                   { { "target",
                       target == EngineDocument::PositionTarget::Chapter ? "chapter" : "book" },
                     { "error_code", diagnosticErrorCode(error) },
                     { "chapter",
                       location.left(location.indexOf('#') < 0 ? location.size()
                                                               : location.indexOf('#')) } });
    }
    return true;
}

void publishCover(const OpeningJob &job, OpeningState &state)
{
    if (state.coverPublished || !job.coverReady || job.cancelled())
        return;
    auto image = job.engine->coverImage({ 2048, 2048 }, job.coverPath);
    state.coverPublished = !image.isNull();
    job.coverReady(std::move(image));
}

void locate(const OpeningJob &job, OpeningState &state, const QString &location)
{
    QString detail;
    if (!openAtPlatform(*job.engine, job.positions, location, &detail))
        state.outcome = OpeningResult::failed(job.positions ? OpeningFailureKind::InvalidContent
                                                            : OpeningFailureKind::HostUnavailable,
                                              "opening.platform-position", detail);
}

// Recheck readable content through the fresh authorized server
// when the text itself is not read again. Never treat a matching
// archive hash as an access decision.
bool readable(OpeningState &state)
{
    if (state.resources->readBounded(state.probe, ResourceCache::budgetBytes).isEmpty()
        || !state.resources->error().isEmpty()) {
        state.outcome = OpeningResult::failed(
                OpeningFailureKind::Io,
                strings::key("error.nickel_did_not_supply_readable_publication_text"));
        return false;
    }
    return true;
}

// Asks the host for the book's resources and decides whether the layout that
// the engine kept from the previous opening still fits them.
void bindResources(OpeningJob &job, OpeningState &state)
{
    const auto &engine = job.engine;
    state.resources = job.factory(state.publication.sizes(), job.systemFont, state.contentRejected);
    if (!state.resources)
        state.outcome = OpeningResult::failed(OpeningFailureKind::HostUnavailable,
                                              strings::key("error.cannot_read_the_publication"));
    const auto userStyleSheet = readUserStyleSheet(job.userStyleSheetPath);
    // A deferred image failure or changed user CSS requires a fresh
    // layout, even when the publication and appearance are unchanged.
    state.retainedHit = state.resources && !job.retainedHash.isEmpty()
            && job.retainedHash == state.digest && engine->resourceError().isEmpty()
            && engine->userStyleSheet() == userStyleSheet;
    engine->setUserStyleSheet(userStyleSheet);
    // Cold opening normalizes missing, old or invalid appearance fields.
    // Reuse only the settings that produced this retained layout.
    if (!appearance::same(job.saved, job.retainedSettings))
        state.retainedHit = false;
    for (const auto &name : state.publication.names()) {
        if (name.endsWith(".xhtml", Qt::CaseInsensitive)
            || name.endsWith(".html", Qt::CaseInsensitive)) {
            state.probe = name;
            break;
        }
    }
    if (state.retainedHit) {
        if (state.probe.isEmpty())
            state.retainedHit = false;
        else
            readable(state);
    }
    if (state.retainedHit && state.outcome.ready()
        && !engine->rebindResources(state.resources, state.publication.sizes()))
        state.retainedHit = false;
}
} // namespace

OpeningResult OpeningJob::interrupted() const
{
    return OpeningResult::failed(lease && lease->end() == SourceLease::End::Revoked
                                         ? OpeningFailureKind::Revoked
                                         : OpeningFailureKind::Cancelled,
                                 strings::key("error.opening_interrupted"));
}

void inspectSource(OpeningJob &job, OpeningState &state)
{
    auto &file = state.file;
    file.setFileName(job.path);
    if (job.cancelled())
        state.outcome = job.interrupted();
    else if (!job.factory)
        state.outcome = OpeningResult::failed(OpeningFailureKind::HostUnavailable,
                                              strings::key("error.opening_interrupted"));
    else if (!file.open(QIODevice::ReadOnly))
        state.outcome = OpeningResult::failed(OpeningFailureKind::Io,
                                              strings::key("error.cannot_read_the_publication"));
    else if (!state.publication.inspect(
                     file, nullptr,
                     &state.outcome)) { /* The inspector supplies the typed failure. */
    } else if (!file.seek(0))
        state.outcome = OpeningResult::failed(OpeningFailureKind::Io,
                                              strings::key("error.cannot_read_the_publication"));
    else {
        state.directoryMs = state.timer.elapsed();
        state.digest = state.publication.fingerprint();
        state.syncDigest = kosync::documentDigest(file);
        state.savedMatches = job.saved.value("fingerprint").toString().toLatin1() == state.digest;
        // States saved before the fingerprint hold the SHA-256 of the whole
        // file. Hash it once so that they keep their position and typography.
        const auto legacy = job.saved.contains("fingerprint")
                ? QByteArray()
                : job.saved.value("sha256").toString().toLatin1();
        if (!legacy.isEmpty()) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            const auto length = file.size();
            int previousPercent = -1;
            while (!file.atEnd() && !job.cancelled()) {
                const auto chunk = file.read(ResourceCache::blockBytes);
                if (chunk.isEmpty()) {
                    state.outcome = OpeningResult::failed(
                            OpeningFailureKind::Io,
                            strings::key("error.cannot_read_the_publication"));
                    break;
                }
                hash.addData(chunk);
                const int percent = int(file.pos() * 100 / length);
                if (percent != previousPercent) {
                    job.report(OpeningStage::Content, percent);
                    previousPercent = percent;
                }
            }
            if (job.cancelled())
                state.outcome = job.interrupted();
            else if (state.outcome.ready())
                state.savedMatches = hash.result().toHex() == legacy;
        }
        job.report(OpeningStage::Content, 100);
        state.hashMs = state.timer.elapsed() - state.directoryMs;
    }
}

void mergeSavedState(OpeningJob &job, OpeningState &state)
{
    auto &saved = job.saved;
    const auto bionic = saved.value("bold_reading");
    const auto dark = saved.value("dark_mode");
    const auto hideNonlinear = saved.value("hide_nonlinear_fragments");
    const auto normalization = appearance::normalizationSettings(saved);
    const auto landscapeColumns = saved.value("two_columns_landscape");
    const auto columnSpacing = saved.value("column_spacing_percent");
    const auto wordSpacing = saved.value("word_spacing");
    const auto paragraphSpacing = saved.value("paragraph_spacing");
    const auto paragraphIndent = saved.value("paragraph_indent");
    const auto tracking = saved.value("justification_tracking");
    const auto hanging = saved.value("hanging_punctuation");
    const auto widowsOrphans = saved.value("widows_orphans");
    if (!state.savedMatches)
        saved = {};
    saved["bold_reading"] = bionic;
    saved["dark_mode"] = dark;
    saved["hide_nonlinear_fragments"] = hideNonlinear;
    for (const auto *key : appearance::normalizationKeys)
        saved[key] = normalization.value(key);
    saved["two_columns_landscape"] = landscapeColumns;
    saved["column_spacing_percent"] = columnSpacing;
    saved["word_spacing"] = wordSpacing;
    saved["paragraph_spacing"] = paragraphSpacing;
    saved["paragraph_indent"] = paragraphIndent;
    saved["justification_tracking"] = tracking;
    saved["hanging_punctuation"] = hanging;
    saved["widows_orphans"] = widowsOrphans;
    appearance::migrateLineSpacing(saved);
    if (!job.appearanceDefaults.isEmpty()) {
        appearance::applyBookDefaults(saved, job.appearanceDefaults);
    }
    if (!job.pinnedAppearance.isEmpty())
        appearance::copy(saved, job.pinnedAppearance);
    state.restart = job.restartAtStart || !saved.value("finished_position").toString().isEmpty();
    // Keep the saved position until the document loads: it selects
    // the DOM version used by existing bookmarks and annotations.
    if (state.restart)
        saved.remove("imported_native_location");
    state.initial = !state.restart && saved.value("position").toString().isEmpty()
            ? job.nativeLocation
            : QString();
}

void openRetained(OpeningJob &job, OpeningState &state)
{
    const auto &engine = job.engine;
    job.notifyReuse();
    publishCover(job, state);
    const auto modelStart = state.timer.elapsed();
    if (!engine->applyAppearance(kobalt::appearance::Patch(job.saved), job.size))
        state.outcome =
                OpeningResult::failed(OpeningFailureKind::InvalidContent,
                                      strings::key("error.cannot_restore_the_retained_appearance"));
    else if (state.restart)
        engine->go(0);
    else if (!state.initial.isEmpty())
        locate(job, state, state.initial);
    else if (!job.saved.value("position").toString().isEmpty()
             && !engine->goTo(job.saved.value("position").toString()))
        state.outcome =
                OpeningResult::failed(OpeningFailureKind::InvalidContent,
                                      strings::key("error.cannot_restore_the_retained_position"));
    if (state.outcome.ready() && !job.cancelled()) {
        state.frame = capture(*engine, job.positions, false, job.markup, job.markupGeneration);
        state.frame.author = engine->author();
        state.frame.cover = engine->coverImage({ 120, 180 });
        job.report(OpeningStage::Ready, 100);
    }
    state.modelMs = state.timer.elapsed() - modelStart;
}

void openCold(OpeningJob &job, OpeningState &state)
{
    const auto &engine = job.engine;
    auto &outcome = state.outcome;
    // A cached book reads no text from Nickel either. A parsed book
    // reads the probed resource from memory afterwards.
    const bool cached =
            !job.documentCachePath.isEmpty() && !state.probe.isEmpty() && readable(state);
    engine->setDocumentCache(cached ? job.documentCachePath : QString(),
                             cached ? state.digest : QByteArray(), KOBALT_LAYOUT_POLICY,
                             state.file.size());
    const auto inspectionStart = state.timer.elapsed();
    LayoutCache cache(job.cancelled() ? QString() : job.layoutCachePath, KOBALT_LAYOUT_POLICY);
    LayoutCompatibility layout;
    state.cacheHit = !job.cancelled() && outcome.ready() && cache.lookup(state.digest, &layout);
    // Older warning cache entries have no issue location or excerpt.
    // Reinspect warnings so diagnostics always contain the cause.
    state.cacheHit = state.cacheHit && layout.verdict == LayoutCompatibility::Supported;
    if (!state.cacheHit && !job.cancelled() && outcome.ready()) {
        layout = inspectLayout(*state.resources, state.publication.sizes());
        if (!job.cancelled())
            state.cacheStored = cache.store(state.digest, layout);
    }
    state.inspectionMs = state.timer.elapsed() - inspectionStart;
    state.formattingDetails = kobalt::qt::object({ { "verdict", int(layout.verdict) },
                                                   { "resources", layout.resources },
                                                   { "bytes", double(layout.bytes) },
                                                   { "issue", layout.issue },
                                                   { "resource", layout.resource },
                                                   { "line", double(layout.line) },
                                                   { "column", double(layout.column) },
                                                   { "excerpt", layout.excerpt } });
    diagnostic("layout-compatibility", state.formattingDetails);
    if (job.cancelled())
        outcome = job.interrupted();
    else if (outcome.ready() && !state.resources->resumeAfterInspection()) {
        outcome = OpeningResult::failed(OpeningFailureKind::Io, layout.reasonKey());
    } else if (outcome.ready()) {
        // Incomplete inspection does not establish an unsupported
        // feature. Keep its details in diagnostics without a warning.
        state.formattingWarning = layout.verdict == LayoutCompatibility::VerticalWriting
                || layout.verdict == LayoutCompatibility::RightToLeftProgression;
    }
    const auto modelStart = state.timer.elapsed();
    job.report(OpeningStage::Text, 0);
    if (outcome.ready()
        && engine->openResources(
                state.resources, state.publication.sizes(), job.size, job.saved, nullptr,
                job.progress, &outcome, [&job, &state] { publishCover(job, state); },
                job.notifyReuse)) {
        if (state.restart)
            engine->go(0);
        else if (!state.initial.isEmpty())
            locate(job, state, state.initial);
        if (outcome.ready()) {
            job.report(OpeningStage::Ready, 0);
            state.frame = capture(*engine, job.positions, false, job.markup, job.markupGeneration);
            state.frame.author = engine->author();
            state.frame.cover = engine->coverImage({ 120, 180 });
            job.report(OpeningStage::Ready, 100);
        }
    }
    state.modelMs = state.timer.elapsed() - modelStart;
}

void logOpeningPhases(const OpeningJob &job, const OpeningState &state)
{
    const auto &outcome = state.outcome;
    diagnostic("opening-phases",
               { { "directory_ms", double(state.directoryMs) },
                 { "hash_ms", double(state.hashMs) },
                 { "layout_check_ms", double(state.inspectionMs) },
                 { "model_load_ms", double(state.modelMs) },
                 { "retained_book_hit", state.retainedHit },
                 { "layout_cache_hit", state.cacheHit },
                 { "layout_cache_stored", state.cacheStored },
                 { "document_cache_hit",
                   !state.retainedHit && outcome.ready() && job.engine->openedFromCache() } });
    diagnostic("open-end",
               { { "elapsed_ms", state.timer.nsecsElapsed() / 1e6 },
                 { "failed", !outcome.ready() },
                 { "error_code",
                   outcome.failure() ? QString::fromLatin1(outcome.failure()->reason.constData())
                                     : QString() } });
}

OpeningOutput runOpening(OpeningJob &job)
{
    OpeningState state;
    auto &outcome = state.outcome;
    if (!job.coverPath.isEmpty())
        publishCover(job, state);
    state.timer.start();
    diagnostic("open-start");
    inspectSource(job, state);
    if (outcome.ready()) {
        mergeSavedState(job, state);
        bindResources(job, state);
        if (state.retainedHit && outcome.ready())
            openRetained(job, state);
        else if (outcome.ready())
            openCold(job, state);
        // From here only the engine keeps the resources, if it opened them.
        state.resources.reset();
    }
    if (outcome.ready() && !job.cancelled()) {
        QElapsedTimer counting;
        counting.start();
        const auto counts = job.engine->wordCounts(job.current);
        captureChapters(state.frame, *job.engine);
        diagnostic("chapter-count",
                   { { "ready", counts.total >= 0 },
                     { "chapters", counts.chapters.size() },
                     { "cache_hit", counts.cached },
                     { "milliseconds", double(counting.nsecsElapsed()) / 1000000 } });
    }
    if (outcome.ready() && !state.frame.error.isEmpty())
        outcome = OpeningResult::failed(OpeningFailureKind::Io, "opening.resource-read",
                                        state.frame.error);
    if (!outcome.ready() && state.contentRejected->load())
        outcome.rememberContentRejection();
    if (job.cancelled())
        outcome = job.interrupted();
    if (state.retainedHit) {
        state.formattingWarning = job.retainedWarning;
        state.formattingDetails = job.retainedDetails;
    }
    if (outcome.ready() && state.formattingWarning) {
        state.formattingDetails.insert("retained_book_hit", state.retainedHit);
        diagnostic("opening-formatting-warning", state.formattingDetails);
    }
    logOpeningPhases(job, state);
    OpeningOutput output;
    output.frame = std::move(state.frame);
    output.outcome = outcome;
    output.digest = state.digest;
    output.syncDigest = state.syncDigest;
    output.savedMatches = state.savedMatches;
    output.formattingWarning = state.formattingWarning;
    output.formattingDetails = state.formattingDetails;
    return output;
}
} // namespace kobalt
