#include "support/strings.h"
#include "reading_book.h"
#include "appearance_settings.h"
#include "session_timeout.h"
#include "reader_session.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include "storage/annotation_database.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>
#include <QJsonDocument>
#include <cmath>

namespace kobalt {
namespace {
constexpr int maxImageViews = 64;

bool validImageId(const QString &id)
{
    if (id.size() != 64)
        return false;
    for (const auto c : id)
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
            return false;
    return true;
}

bool validImageView(const QJsonObject &view)
{
    for (const auto *key : { "zoom", "x", "y" }) {
        const auto value = view.value(key);
        if (!value.isDouble() || !std::isfinite(value.toDouble()))
            return false;
    }
    return view["zoom"].toDouble() >= 1 && view["zoom"].toDouble() <= 8 && view["x"].toDouble() >= 0
            && view["x"].toDouble() <= 1 && view["y"].toDouble() >= 0 && view["y"].toDouble() <= 1;
}

// The layout memory is a convenience for the next book. Profile preparation
// moves unusable settings aside. If the file is damaged or replaced by a newer
// version while a book is open, a close or checkpoint skips the layout memory
// and still saves the reading position. A read error still fails the save.
bool skipsLayoutMemory(storage::ReadStatus status)
{
    if (status != storage::ReadStatus::Invalid && status != storage::ReadStatus::Unsupported
        && status != storage::ReadStatus::TooLarge)
        return false;
    diagnostic("appearance-settings-skipped", { { "status", storage::readStatusName(status) } },
               DiagnosticSeverity::Warning);
    return true;
}
} // namespace

ReadingBook::ReadingBook(std::unique_ptr<ReaderSession> session, QString statePath,
                         storage::Paths paths, QString identity, QByteArray digest,
                         std::unique_ptr<ReadingBridge> bridge, ReadingProgress baseline,
                         ReadingProgress diskBaseline, QString sourceKey,
                         ReadingHistory::UtcClock utc, bool koboCloud)
    : session_(std::move(session)),
      statePath(std::move(statePath)),
      identity_(std::move(identity)),
      paths_(std::move(paths)),
      digest(std::move(digest)),
      bridge(std::move(bridge)),
      nativeCloudSync(this->bridge && (koboCloud || !identity_.startsWith("file:///"))),
      koboCloudBook(this->bridge && koboCloud),
      baseline(std::move(baseline)),
      reconciliation(std::move(diskBaseline))
{
    this->utc = utc ? std::move(utc)
                    : ReadingHistory::UtcClock([] { return QDateTime::currentMSecsSinceEpoch(); });
    auto saved = storage::readBookState(paths_, identity_, this->statePath).object;
    // The native opener resets its snapshot before the database record is
    // replaced. Apply the same reset before retaining fields or preferences.
    if (this->bridge)
        resetNativeUnread(saved, reconciliation.baseline());
    preservedState = saved;
    if (saved.value("book").toString() == identity_) {
        for (const auto &value : saved.value("image_views").toArray()) {
            const auto view = value.toObject();
            rememberImageView(view.value("id").toString(), view);
        }
    }
    if (saved.value("book").toString() == identity_)
        finishPromptSuppressed = saved.value("suppress_finish_prompt").toBool(false);
    const auto preferences = appearance::readerPreferences(
            storage::readPreferences(paths_.preferences()).object,
            saved.value("book").toString() == identity_ ? saved : QJsonObject());
    for (const auto *key : appearance::layoutKeys)
        if (preferences.contains(key))
            layoutPreferences_[key] = preferences.value(key);
    if (saved.value("book").toString() == identity_)
        dictionaryChoice_ = saved.value("dictionary_id").toString();
    // Qt 5.2 leaves a default-constructed QElapsedTimer uninitialized, so
    // isValid() is not reliable until the timer is invalidated or started.
    progressPoll.invalidate();
    activeReading.invalidate();
    speedSaved.invalidate();
    activitySaved.start();
    activityFailed.invalidate();
    appearanceFailed.invalidate();
    if (annotationDatabaseEnabled())
        history = std::make_unique<ReadingHistory>(paths_.directory, identity_, this->digest,
                                                   std::move(sourceKey), session_->title(),
                                                   session_->author(), this->utc);
    if (!this->bridge) {
        speed = std::make_unique<ReadingSpeed>();
        speed->restore(storage::readRecord(paths_.directory, "reading-speed", paths_.readingSpeed())
                               .object);
    }
}

ReadingBook::~ReadingBook()
{
    sleepingReading = false;
    if (!discardSaves) {
        if (history)
            stopReading();
        else
            saveSpeed(true);
    }
    ++activityGeneration;
    ++appearanceGeneration;
    activityWriter.reset();
    appearanceWriter.reset();
    writer.reset();
    speedWriter.reset();
}

bool ReadingBook::savesPending() const
{
    return activityPending || appearancePending || checkpointAfterActivity
            || !closesAfterActivity.isEmpty() || (activityWriter && !activityWriter->idle())
            || (appearanceWriter && !appearanceWriter->idle()) || closeCheckpointPending
            || (writer && !writer->idle()) || (history && history->saving())
            || (speedWriter && !speedWriter->idle());
}

bool ReadingBook::deviceTransition() const
{
    return bridge && bridge->deviceTransition();
}

bool ReadingBook::canWriteNative() const
{
    return bridge && bridge->writable();
}

void ReadingBook::releaseNative(bool save)
{
    sleepingReading = false;
    stopReading(true, save);
    ++nativeFinishGeneration;
    const auto callbacks = std::exchange(nativeFinishCallbacks, {});
    bridge.reset();
    released = true;
    // Destroying a native session cancels its callbacks. Release callers that
    // were waiting so their local disposal barrier can retry without it.
    for (const auto &callback : callbacks)
        callback(false);
}

void ReadingBook::finishReading(std::function<void(bool)> done)
{
    sleepingReading = false;
    stopReading(true);
    drainReadingSave(std::move(done));
}

void ReadingBook::drainReadingSave(std::function<void(bool)> done)
{
    if (!bridge) {
        done(true);
        return;
    }
    nativeFinishCallbacks.append(std::move(done));
    if (nativeFinishCallbacks.size() > 1)
        return;
    const auto generation = ++nativeFinishGeneration;
    const std::weak_ptr<ReadingBook *> guard = alive;
    bridge->finishReading([guard, generation](bool saved) {
        if (const auto book = guard.lock())
            (*book)->finishNativeReading(generation, saved);
    });
}

void ReadingBook::finishNativeReading(quint64 generation, bool saved)
{
    if (generation != nativeFinishGeneration)
        return;
    const auto callbacks = std::exchange(nativeFinishCallbacks, {});
    for (const auto &callback : callbacks)
        callback(saved);
}

bool ReadingBook::readingSavePending() const
{
    return bridge && bridge->readingSavePending();
}

void ReadingBook::retryFailedReadingSave()
{
    if (bridge)
        bridge->retryFailedReadingSave();
}

void ReadingBook::observeReading(bool learnPace)
{
    // Reading continues around these stops, so none of them waits for storage.
    if (released || (bridge && !bridge->writable())) {
        stopReading(true);
        return;
    }
    if (session_->renderProgress().active) {
        pauseLocalPace();
    }
    if (session_->busy())
        return;
    if (session_->image().isNull() || !session_->error().isEmpty()) {
        stopReading(true);
        return;
    }
    if (!activeReading.isValid()) {
        if (sleepingReading) {
            const auto now = utc();
            if (sleepingTimeout > 0 && now >= sleepingAt && now - sleepingAt >= sleepingTimeout) {
                if (history)
                    history->stop(completedReadingMilliseconds,
                                  session_->state().value("position").toString());
                sessionBaseline = completedReadingMilliseconds;
            }
        }
        sleepingReading = false;
        activeReading.start();
        if (history) {
            history->start(completedReadingMilliseconds,
                           session_->state().value("position").toString());
        }
    }
    if (speed) {
        if (learnPace) {
            speed->observe(readingActivity(), readingMilliseconds());
        } else {
            speed->pause();
        }
    }
    if (bridge)
        bridge->observeReading(readingActivity());
}

ReadingActivity ReadingBook::readingActivity() const
{
    QString error;
    const auto nativePosition = session_->savedNativePosition(&error);
    return { session_->state().value("position").toString(),
             // Kobalt's page progress, kept below 100 like every native write of
             // an unfinished book, so the last page never reports a finished read.
             qMin(99.0, double(pageProgressPercent(session_->page(), session_->pages()))),
             session_->page(), session_->pages(), session_->turnDirection(), session_->totalWords(),
             session_->chapterGraph(), session_->graphSelection(),
             error.isEmpty() ? nativePosition : QString() };
}

ReadingEstimates ReadingBook::estimates() const
{
    if (speed) {
        return speed->estimates(readingActivity());
    }
    if (bridge && !released && bridge->writable()) {
        ReadingEstimates result;
        if (bridge->estimates(readingActivity(), &result))
            return result;
    }
    const auto reading = readingMilliseconds();
    const int chapterLeft = session_->chapterPages() > 0
            ? session_->chapterPages() - session_->chapterPage() + 1
            : -1;
    return { pace_.estimate(chapterLeft, reading),
             pace_.estimate(session_->pages() - session_->page(), reading),
             pace_.estimate(0, reading) < 0 };
}

void ReadingBook::sleepReading(const QJsonObject &preferences)
{
    if (!sleepingReading) {
        sleepingReading = true;
        sleepingAt = utc();
        sleepingTimeout = sessionTimeout::milliseconds(preferences);
    }
    stopReading();
}

void ReadingBook::stopReading(bool background, bool nativeAllowed)
{
    if (activeReading.isValid()) {
        const auto elapsed = activeReading.elapsed();
        completedReadingMilliseconds += elapsed;
        activeReading.invalidate();
    }
    // Freeze the duration before history work so both use the same boundary.
    if (history) {
        const auto position = session_->state().value("position").toString();
        if (sleepingReading)
            history->pause(completedReadingMilliseconds, position);
        else
            history->stop(completedReadingMilliseconds, position);
    }
    // End Nickel's interval before local storage work, so its clock does not
    // include that I/O and its asynchronous flush can run alongside our save.
    if (bridge && nativeAllowed)
        bridge->stopReading(readingActivity());
    if (!background)
        checkpointReadingHistory();
    if (speed) {
        speed->pause();
        if (!background)
            saveSpeed(true);
    }
}

bool ReadingBook::checkpointReadingHistory(bool background)
{
    if (!history)
        return true;
    if (activeReading.isValid())
        history->update(readingMilliseconds(), session_->state().value("position").toString());
    return history->save(background);
}

struct ReadingBook::ActivitySave
{
    QJsonObject checkpoint, persisted, speed;
    ReadingHistory::Snapshot history;
    bool durable = false;
    quint64 generation = 0;
};

bool ReadingBook::activitySaveCoolingDown() const
{
    return activityFailed.isValid() && activityFailed.elapsed() < 300000;
}

bool ReadingBook::activitySaveDue(int intervalMilliseconds) const
{
    return !activityPending && !activitySaveCoolingDown()
            && activitySaved.elapsed() >= intervalMilliseconds;
}

void ReadingBook::saveReadingActivity(bool durable)
{
    if (activityPending || checkpointAfterActivity || !closesAfterActivity.isEmpty()
        || closeCheckpointPending || activitySaveCoolingDown())
        return;
    retryAppearanceSave();
    auto save = std::make_shared<ActivitySave>();
    save->checkpoint = persistedCheckpoint(checkpointState());
    save->persisted = save->checkpoint;
    for (const auto *key : appearance::layoutKeys)
        save->persisted.remove(key);
    for (const auto *key : appearance::marginKeys)
        save->persisted.remove(key);
    if (speed && speed->dirty())
        save->speed = speed->state();
    if (history) {
        if (activeReading.isValid())
            history->update(readingMilliseconds(), session_->state().value("position").toString());
        save->history = history->snapshot();
    }
    save->durable = durable;
    save->generation = ++activityGeneration;
    activityPending = true;
    if (!activityWriter)
        activityWriter = std::make_unique<storage::RecordWriter>();
    writeActivity(std::move(save), 0);
}

void ReadingBook::writeActivity(std::shared_ptr<ActivitySave> save, int attempt)
{
    const std::weak_ptr<ReadingBook *> guard = alive;
    activityWriter->post(
            [paths = paths_, identity = identity_, path = statePath, save] {
                bool ok;
                {
                    storage::RoutineWrites routine;
                    ok = storage::writeReadingActivity(paths, identity, save->persisted,
                                                       save->speed, save->history.write, path);
                }
                if (ok && save->durable) {
                    storage::checkpointProfile();
                    ok = !storage::profileHasUnsyncedWrites();
                }
                if (save->history.complete)
                    save->history.complete(ok);
                return ok;
            },
            [guard, save, attempt](bool ok) {
                qt::invoke(
                        QCoreApplication::instance(),
                        [guard, save, attempt, ok] {
                            const auto owner = guard.lock();
                            if (!owner || (*owner)->activityGeneration != save->generation)
                                return;
                            auto *book = *owner;
                            if (!ok && attempt < 2) {
                                QTimer::singleShot(
                                        100, QCoreApplication::instance(), [guard, save, attempt] {
                                            if (const auto owner = guard.lock())
                                                if ((*owner)->activityGeneration
                                                    == save->generation)
                                                    (*owner)->writeActivity(save, attempt + 1);
                                        });
                                return;
                            }
                            book->activityPending = false;
                            if (ok) {
                                book->activitySaved.restart();
                                book->activityFailed.invalidate();
                                book->savedCheckpoint = save->checkpoint;
                                if (book->speed && book->speed->state() == save->speed)
                                    book->speed->saved();
                            } else {
                                book->activityFailed.start();
                                diagnostic("reading-activity-unsaved",
                                           { { "attempts", attempt + 1 } },
                                           DiagnosticSeverity::Error);
                            }
                            book->resumeActivityCheckpoints();
                            if (!ok && book->checkpointFailed)
                                book->checkpointFailed();
                        },
                        Qt::QueuedConnection);
            });
}

void ReadingBook::resumeActivityCheckpoints()
{
    if (!checkpointAfterActivity && closesAfterActivity.isEmpty())
        return;
    const std::weak_ptr<ReadingBook *> guard = alive;
    const auto generation = activityGeneration;
    qt::invoke(
            QCoreApplication::instance(),
            [guard, generation] {
                const auto owner = guard.lock();
                if (!owner || (*owner)->activityGeneration != generation
                    || (*owner)->activityPending || (*owner)->appearancePending)
                    return;
                auto *book = *owner;
                const bool checkpoint = std::exchange(book->checkpointAfterActivity, false);
                auto closes = std::exchange(book->closesAfterActivity, {});
                if (!closes.isEmpty()) {
                    // Closing captures the latest state and supersedes a waiting
                    // background checkpoint. Keep every close completion.
                    book->checkpointForClose([guard, closes](bool saved) {
                        // The close completion holds a locked lifetime guard. Let it
                        // unwind before a callback can destroy the book, so the next
                        // callback sees an expired guard.
                        qt::invoke(
                                QCoreApplication::instance(),
                                [guard, closes, saved] {
                                    for (const auto &done : closes) {
                                        if (guard.expired())
                                            break;
                                        done(saved);
                                    }
                                },
                                Qt::QueuedConnection);
                    });
                } else if (checkpoint) {
                    QJsonObject ignored;
                    book->checkpoint(&ignored, true);
                }
            },
            Qt::QueuedConnection);
}

void ReadingBook::rememberAppearance()
{
    if (session_->temporaryTypography())
        return;
    const auto state = checkpointState();
    QJsonObject appearance;
    appearance::rememberTextLayout(appearance, state);
    appearance::rememberPageMargins(appearance, state);
    if (appearance == rememberedAppearance && !appearanceDirty)
        return;
    rememberedAppearance = appearance;
    appearanceDirty = true;
    retryAppearanceSave();
}

void ReadingBook::retryAppearanceSave()
{
    if (!appearanceDirty || appearancePending
        || (appearanceFailed.isValid() && appearanceFailed.elapsed() < 300000))
        return;
    appearancePending = true;
    ++appearanceGeneration;
    writeAppearance(0);
}

void ReadingBook::writeAppearance(int attempt)
{
    const auto appearance = rememberedAppearance;
    const auto generation = appearanceGeneration;
    if (!appearanceWriter)
        appearanceWriter = std::make_unique<storage::RecordWriter>();
    const std::weak_ptr<ReadingBook *> guard = alive;
    appearanceWriter->post(
            [path = paths_.preferences(), appearance] {
                const auto previous = storage::readPreferences(path);
                if (previous.status != storage::ReadStatus::Ok
                    && previous.status != storage::ReadStatus::Missing)
                    return false;
                auto next = previous.object;
                for (auto it = appearance.begin(); it != appearance.end(); ++it)
                    next[it.key()] = it.value();
                return next == previous.object || storage::writePreferences(path, next);
            },
            [guard, appearance, generation, attempt](bool ok) {
                qt::invoke(
                        QCoreApplication::instance(),
                        [guard, appearance, generation, attempt, ok] {
                            const auto owner = guard.lock();
                            if (!owner || (*owner)->appearanceGeneration != generation)
                                return;
                            auto *book = *owner;
                            if (ok && appearance != book->rememberedAppearance) {
                                book->writeAppearance(0);
                                return;
                            }
                            if (!ok && attempt < 2) {
                                QTimer::singleShot(
                                        100, QCoreApplication::instance(),
                                        [guard, generation, attempt] {
                                            if (const auto owner = guard.lock())
                                                if ((*owner)->appearanceGeneration == generation)
                                                    (*owner)->writeAppearance(attempt + 1);
                                        });
                                return;
                            }
                            book->appearancePending = false;
                            book->appearanceDirty = !ok;
                            if (ok)
                                book->appearanceFailed.invalidate();
                            else {
                                book->appearanceFailed.start();
                                diagnostic("appearance-settings-unsaved",
                                           { { "attempts", attempt + 1 } },
                                           DiagnosticSeverity::Error);
                            }
                            book->resumeActivityCheckpoints();
                        },
                        Qt::QueuedConnection);
            });
}

void ReadingBook::appearanceCheckpointSaved(const QJsonObject &state)
{
    if (session_->temporaryTypography())
        return;
    rememberedAppearance = {};
    appearance::rememberTextLayout(rememberedAppearance, state);
    appearance::rememberPageMargins(rememberedAppearance, state);
    appearanceDirty = false;
    appearanceFailed.invalidate();
}

void ReadingBook::saveSpeed(bool force, bool background)
{
    if (!speed || !speed->dirty()
        || (!force && speedSaved.isValid() && speedSaved.elapsed() < 60000)) {
        return;
    }
    speedSaved.start();
    if (!speedWriter)
        speedWriter = std::make_unique<storage::RecordWriter>();
    const auto state = speed->state();
    const auto write = [directory = paths_.directory, path = paths_.readingSpeed(), state] {
        // Rewritten every minute while reading; a checkpoint syncs it.
        storage::RoutineWrites routine;
        return storage::writeRecord(directory, "reading-speed", path, state);
    };
    const auto finished = [this, state](bool saved) {
        // Reading can change the pace while the write runs. Only the written
        // state is saved.
        if (saved && speed->state() == state)
            speed->saved();
        else if (!saved)
            diagnostic("reading-speed-unsaved", {}, DiagnosticSeverity::Error);
    };
    if (!background) {
        finished(speedWriter->writeNow(write));
        return;
    }
    const std::weak_ptr<ReadingBook *> guard = alive;
    speedWriter->post(write, [guard, finished](bool saved) {
        kobalt::qt::invoke(
                QCoreApplication::instance(),
                [guard, finished, saved] {
                    if (guard.lock())
                        finished(saved);
                },
                Qt::QueuedConnection);
    });
}

qint64 ReadingBook::readingMilliseconds() const
{
    return completedReadingMilliseconds + (activeReading.isValid() ? activeReading.elapsed() : 0);
}

bool ReadingBook::offerFinish() const
{
    return !finishPromptSuppressed && canFinish();
}

bool ReadingBook::canFinish() const
{
    if (released || (bridge && !bridge->writable()) || session_->busy() || session_->pages() <= 0
        || !session_->error().isEmpty())
        return false;
    const auto anchor = session_->state().value("position").toString();
    if (anchor.isEmpty() || anchor == acceptedFinishedAnchor)
        return false;
    // Nickel asks on exit at 90%. Include the final page of very short books,
    // and the last page of the text before trailing non-linear sections.
    return session_->page() * 10 >= session_->pages() * 9
            || session_->nextPage() == session_->page();
}

void ReadingBook::markFinished()
{
    if (canFinish())
        acceptedFinishedAnchor = session_->state().value("position").toString();
}

bool ReadingBook::finishedHere() const
{
    return !acceptedFinishedAnchor.isEmpty()
            && acceptedFinishedAnchor == session_->state().value("position").toString();
}

ReadingProgress ReadingBook::readProgress() const
{
    return bridge ? bridge->read() : ReadingProgress{};
}

void ReadingBook::observeProgress()
{
    if (bridge)
        reconciliation.observe(bridge->read());
}

void ReadingBook::pollProgress()
{
    if (!bridge)
        return;
    // Nickel's saved position changes after sync, account updates and USB, and
    // its signals report each one. Read only then. A bridge without those
    // signals is also read once a minute. The minute tick can come a little
    // early, so accept it after 55 seconds.
    bool changed = bridge->takeChange();
    if (changed)
        progressRetries = 3;
    else
        changed = std::exchange(progressRetry, false);
    if (!changed
        && (bridge->signalsChanges() || (progressPoll.isValid() && progressPoll.elapsed() < 55000)))
        return;
    const auto progress = bridge->read();
    reconciliation.observe(progress);
    // Nickel can still hold its database right after a sync. A failed read
    // tries again on the next checks, up to three times, so the synced
    // position is not lost and a position that never decodes stops costing reads.
    progressRetry = !progress.valid && bridge->signalsChanges() && progressRetries > 0;
    if (progressRetry)
        --progressRetries;
    progressPoll.restart();
    if (!reconciliation.pending() && reconciliation.baseline().valid)
        baseline = reconciliation.baseline();
}

void ReadingBook::acknowledge(const ReadingProgress &shown, bool moved)
{
    reconciliation.acknowledge(shown);
    baseline = reconciliation.baseline();
    acceptedFinishedAnchor =
            moved && shown.status == 2 ? session_->state().value("position").toString() : QString();
}

void ReadingBook::markNativeReading()
{
    if (std::exchange(openingStatusAttempted, true))
        return;
    if (!bridge || !canWriteNative() || reconciliation.pending())
        return;
    mainthread::AllowBlocking allow("opening status before reader input");
    if (!bridge->markReading()) {
        diagnostic("native-opening-status-failed", {}, DiagnosticSeverity::Warning);
        return;
    }
    baseline.status = 1;
    auto acknowledged = reconciliation.baseline();
    acknowledged.status = 1;
    reconciliation = ProgressReconciler(acknowledged);
}

bool ReadingBook::writeNative(const QString &location, const QString &mappingError)
{
    if (!bridge) {
        nativeWriteback = "local-only";
        nativeWritebackError.clear();
        return true;
    }
    // Some plain EPUB files have positions Nickel cannot store. Record the
    // time, status and percentage; Nickel keeps its own position. Kobo's cloud
    // does not sync sideloaded EPUB files, so no other device sees the old
    // position with a new time. Other books save only locally when their
    // position cannot be mapped.
    const bool timeOnly = !mappingError.isEmpty();
    QString error = timeOnly && !plainEpub() ? mappingError : QString();
    // Unsupported mappings intentionally stay local. Storage failures do not.
    nativeWriteback = error.isEmpty() ? "failed" : "local-only";
    if (error.isEmpty() && canWriteNative()) {
        const auto current = bridge->read(&error);
        reconciliation.observe(current);
        if (reconciliation.pending())
            return false;
        if (reconciliation.mayWrite(current)) {
            ReadingProgress verified;
            // The same whole percentage as Kobalt's own views. The firmware 4
            // bridge keeps an unfinished book below 100, as Nickel's reader does.
            const double percent = pageProgressPercent(session_->page(), session_->pages());
            const bool preserveFinished = finishedHere();
            diagnostic("native-save-start");
            if (bridge->write(current, timeOnly ? QString() : location, percent, preserveFinished,
                              readingActivity(), &verified, &error)) {
                baseline = verified;
                reconciliation = ProgressReconciler(verified);
                nativeWriteback = timeOnly ? "time-only" : "verified";
            } else {
                // A commit can lose a race after the earlier read. Ask about
                // that update before letting the window finish its exit.
                observeProgress();
                if (reconciliation.pending() && canWriteNative())
                    return false;
            }
        } else if (error.isEmpty())
            error = strings::get("error.nickel_progress_changed_before_exit");
    } else if (error.isEmpty())
        error = strings::get(
                "error.nickel_save_deferred_while_progress_or_device_state_is_unresolved");
    nativeWritebackError = error;
    diagnostic("native-save",
               { { "result", nativeWriteback },
                 { "mapping_failed", timeOnly },
                 { "failed", !error.isEmpty() } },
               error.isEmpty() ? DiagnosticSeverity::Activity : DiagnosticSeverity::Error);
    return true;
}

ReadingBook::NativeSave ReadingBook::nativeSaveResult() const
{
    if (reconciliation.pending())
        return NativeSave::Conflict;
    if (nativeWriteback == "verified" || nativeWriteback == "time-only")
        return NativeSave::Saved;
    if (nativeWriteback == "local-only")
        return NativeSave::LocalOnly;
    return NativeSave::Failed;
}

bool ReadingBook::saveDictionaryChoice(const QString &id)
{
    // Known exception: like a settings change, the choice waits for its write.
    mainthread::AllowBlocking allow("settings write");
    const auto previous = dictionaryChoice_;
    dictionaryChoice_ = id;
    QJsonObject snapshot;
    if (checkpoint(&snapshot))
        return true;
    dictionaryChoice_ = previous;
    return false;
}

QJsonObject ReadingBook::imageView(const QString &id) const
{
    for (const auto &value : imageViews) {
        const auto view = value.toObject();
        if (view.value("id").toString() == id)
            return view;
    }
    return {};
}

bool ReadingBook::rememberImageView(const QString &id, const QJsonObject &view)
{
    if (!validImageId(id) || !validImageView(view))
        return false;
    auto next = imageView(id);
    if (!next.isEmpty()) {
        bool same = true;
        for (const auto *key : { "zoom", "x", "y" })
            same = same && qAbs(next[key].toDouble() - view[key].toDouble()) < 1e-9;
        if (same)
            return false;
    } else if (view.value("zoom").toDouble() == 1) {
        return false;
    }
    for (int i = imageViews.size() - 1; i >= 0; --i)
        if (imageViews[i].toObject().value("id").toString() == id)
            imageViews.removeAt(i);
    if (view.value("zoom").toDouble() != 1) {
        if (next.isEmpty())
            next = view;
        next["id"] = id;
        for (const auto *key : { "zoom", "x", "y" })
            next[key] = view.value(key);
        imageViews.append(next);
        while (imageViews.size() > maxImageViews)
            imageViews.removeAt(0);
    }
    return true;
}

QJsonObject ReadingBook::checkpointState() const
{
    auto state = session_->state();
    state["book"] = identity_;
    state["fingerprint"] = QString::fromLatin1(digest);
    for (auto it = layoutPreferences_.begin(); it != layoutPreferences_.end(); ++it)
        state[it.key()] = it.value();
    if (!dictionaryChoice_.isEmpty())
        state["dictionary_id"] = dictionaryChoice_;
    state["schema"] = 1;
    state["nickel_progress"] = baseline.json();
    state["finished_position"] = acceptedFinishedAnchor;
    state["suppress_finish_prompt"] = finishPromptSuppressed;
    if (!imageViews.isEmpty())
        state["image_views"] = imageViews;
    // For the Android and desktop library, which shows progress without
    // opening the book.
    if (session_->pages() > 0)
        state["percent"] = pageProgressPercent(session_->page(), session_->pages());
    state["native_writeback"] = nativeWriteback;
    state["sync_pending"] = reconciliation.pending();
    state["native_writeback_error"] = nativeWritebackError;
    return state;
}

QJsonObject ReadingBook::persistedCheckpoint(const QJsonObject &state) const
{
    auto checkpoint = preservedState;
    for (auto it = state.begin(); it != state.end(); ++it)
        checkpoint[it.key()] = it.value();
    if (dictionaryChoice_.isEmpty())
        checkpoint.remove("dictionary_id");
    if (imageViews.isEmpty())
        checkpoint.remove("image_views");
    if (session_->temporaryTypography())
        appearance::copy(checkpoint, session_->persistentState());
    // These describe the displayed frame, not a saved reading position. Drop
    // them from imported records too, before comparing checkpoints or applying
    // the record's byte limit. Diagnostic snapshots above keep the full frame.
    for (const auto *key : { "chapter_title", "chapter_page", "chapter_pages", "page", "pages",
                             "next_page", "text_font_face", "embedded_font_count", "cached_pages" })
        checkpoint.remove(key);
    // Image views are optional. Trim their oldest entries rather than letting
    // them prevent a reading-position checkpoint from fitting its record limit.
    auto views = checkpoint.value("image_views").toArray();
    while (!views.isEmpty()
           && QJsonDocument(checkpoint).toJson(QJsonDocument::Compact).size()
                   > storage::maxJsonBytes) {
        views.removeAt(0);
        if (views.isEmpty())
            checkpoint.remove("image_views");
        else
            checkpoint["image_views"] = views;
    }
    return checkpoint;
}

bool ReadingBook::checkpoint(QJsonObject *snapshot, bool background)
{
    if (!background) {
        ++activityGeneration;
        if (activityWriter)
            activityWriter->cancel();
        activityPending = false;
        ++appearanceGeneration;
        if (appearanceWriter)
            appearanceWriter->cancel();
        appearancePending = false;
        // This synchronous save replaces the waiting background checkpoint.
        // A queued continuation runs any waiting close after this call returns.
        checkpointAfterActivity = false;
        resumeActivityCheckpoints();
    }
    const auto state = checkpointState();
    *snapshot = state;
    // A second writer must not replace its queued checkpoint with a newer
    // position ahead of an older activity snapshot. Wait asynchronously for
    // that snapshot, then capture the current explicit choice.
    if (background && activityPending) {
        checkpointAfterActivity = true;
        return true;
    }
    const auto checkpoint = persistedCheckpoint(state);
    if (closeCheckpointPending)
        return false;
    if (background && (checkpoint == savedCheckpoint || checkpoint == pendingCheckpoint))
        return true;
    auto persisted = checkpoint;
    for (const auto *key : appearance::layoutKeys)
        persisted.remove(key);
    for (const auto *key : appearance::marginKeys)
        persisted.remove(key);
    // A background checkpoint runs while a tap can turn the page, so all of
    // its storage work runs on the writer thread: the first write of an
    // opening can import legacy files. Appearance changes save separately.
    if (background) {
        pendingCheckpoint = checkpoint;
        if (!writer)
            writer = std::make_unique<storage::RecordWriter>();
        const std::weak_ptr<ReadingBook *> guard = alive;
        writer->post(
                [paths = paths_, identity = identity_, persisted, path = statePath] {
                    // The next synced commit or lifecycle checkpoint makes this
                    // explicit progress choice durable.
                    storage::RoutineWrites routine;
                    return storage::writeBookState(paths, identity, persisted, path);
                },
                [guard, checkpoint](bool saved) {
                    kobalt::qt::invoke(
                            QCoreApplication::instance(),
                            [guard, checkpoint, saved] {
                                if (const auto book = guard.lock())
                                    (*book)->finishBackgroundCheckpoint(checkpoint, saved);
                            },
                            Qt::QueuedConnection);
                });
        return true;
    }
    // The profile database has one writer thread. A synchronous checkpoint
    // replaces a queued one there and waits for its result.
    if (!writer)
        writer = std::make_unique<storage::RecordWriter>();
    pendingCheckpoint = {};
    // The layout memory changes preferences.json, which settings changes
    // and appearance changes also write. Once a book is open, the reader
    // runs every such write on the writer thread, one at a time, so none of
    // them loses another's change. The Android and desktop library still
    // writes the file on the main thread.
    bool layoutSaved = true;
    if (history && activeReading.isValid())
        history->update(readingMilliseconds(), state.value("position").toString());
    const auto historySnapshot = history ? history->snapshot() : ReadingHistory::Snapshot();
    const auto speedState = speed && speed->dirty() ? speed->state() : QJsonObject();
    const bool saved =
            writer->writeNow([paths = paths_, identity = identity_, persisted, path = statePath,
                              state, rememberAppearance = !session_->temporaryTypography(),
                              historySnapshot, speedState, &layoutSaved] {
                const bool ok = storage::writeReadingActivity(
                        paths, identity, persisted, speedState, historySnapshot.write, path);
                if (historySnapshot.complete)
                    historySnapshot.complete(ok);
                if (!ok)
                    return false;
                if (!rememberAppearance)
                    return true;
                const auto settings = storage::readPreferences(paths.preferences());
                if (settings.status != storage::ReadStatus::Ok
                    && settings.status != storage::ReadStatus::Missing) {
                    layoutSaved = skipsLayoutMemory(settings.status);
                    return true;
                }
                auto next = settings.object;
                appearance::rememberTextLayout(next, state);
                appearance::rememberPageMargins(next, state);
                layoutSaved = next == settings.object
                        || storage::writePreferences(paths.preferences(), next);
                return true;
            });
    if (!saved) {
        diagnostic("local-save-failed", {}, DiagnosticSeverity::Error);
        return false;
    }
    if (!layoutSaved) {
        diagnostic("appearance-settings-unsaved", {}, DiagnosticSeverity::Error);
        return false;
    }
    if (storage::profileHasUnsyncedWrites())
        storage::checkpointProfile();
    if (storage::profileHasUnsyncedWrites())
        return false;
    appearanceCheckpointSaved(state);
    savedCheckpoint = checkpoint;
    activitySaved.restart();
    activityFailed.invalidate();
    if (speed && speed->state() == speedState)
        speed->saved();
    return true;
}

void ReadingBook::checkpointForClose(std::function<void(bool)> done)
{
    if (activityPending || appearancePending || !closesAfterActivity.isEmpty()) {
        closesAfterActivity.append(std::move(done));
        return;
    }
    if (sleepingReading) {
        sleepingReading = false;
        stopReading(true);
    }
    Q_ASSERT(!closeCheckpointPending);
    closeCheckpointPending = true;
    const auto state = checkpointState();
    const auto checkpoint = persistedCheckpoint(state);
    auto persisted = checkpoint;
    for (const auto *key : appearance::layoutKeys)
        persisted.remove(key);
    for (const auto *key : appearance::marginKeys)
        persisted.remove(key);
    const bool rememberAppearance = !session_->temporaryTypography();
    const bool writeSpeed = speed && speed->dirty();
    const auto speedState = writeSpeed ? speed->state() : QJsonObject();
    if (history && activeReading.isValid())
        history->update(readingMilliseconds(), state.value("position").toString());
    const auto historySnapshot = history ? history->snapshot() : ReadingHistory::Snapshot();
    pendingCheckpoint = {};
    if (!writer)
        writer = std::make_unique<storage::RecordWriter>();
    const std::weak_ptr<ReadingBook *> guard = alive;
    writer->post(
            [paths = paths_, identity = identity_, persisted, path = statePath, state,
             rememberAppearance, historySnapshot, speedState] {
                bool ok = storage::writeReadingActivity(paths, identity, persisted, speedState,
                                                        historySnapshot.write, path);
                if (historySnapshot.complete)
                    historySnapshot.complete(ok);
                if (rememberAppearance) {
                    const auto settings = storage::readPreferences(paths.preferences());
                    if (settings.status == storage::ReadStatus::Ok
                        || settings.status == storage::ReadStatus::Missing) {
                        auto next = settings.object;
                        appearance::rememberTextLayout(next, state);
                        appearance::rememberPageMargins(next, state);
                        if (next != settings.object)
                            ok = storage::writePreferences(paths.preferences(), next) && ok;
                    } else if (!skipsLayoutMemory(settings.status)) {
                        ok = false;
                    }
                }
                // An unchanged book record can leave an earlier routine save in
                // WAL. Complete its sync before declaring the close saved.
                if (storage::profileHasUnsyncedWrites())
                    storage::checkpointProfile();
                return ok && !storage::profileHasUnsyncedWrites();
            },
            [guard, checkpoint, done = std::move(done)](bool saved) {
                kobalt::qt::invoke(
                        QCoreApplication::instance(),
                        [guard, checkpoint, saved, done] {
                            if (const auto book = guard.lock())
                                (*book)->finishCloseCheckpoint(checkpoint, saved, done);
                        },
                        Qt::QueuedConnection);
            });
}

void ReadingBook::finishCloseCheckpoint(const QJsonObject &checkpoint, bool saved,
                                        std::function<void(bool)> done)
{
    if (history && history->saving()) {
        const std::weak_ptr<ReadingBook *> guard = alive;
        QTimer::singleShot(10, QCoreApplication::instance(), [guard, checkpoint, saved, done] {
            if (const auto book = guard.lock())
                (*book)->finishCloseCheckpoint(checkpoint, saved, done);
        });
        return;
    }
    saved = (!history || history->savesSucceeded()) && saved;
    closeCheckpointPending = false;
    if (saved) {
        savedCheckpoint = checkpoint;
        appearanceCheckpointSaved(checkpoint);
        activitySaved.restart();
        activityFailed.invalidate();
        if (speed)
            speed->saved();
    } else {
        diagnostic("local-close-save-failed", {}, DiagnosticSeverity::Error);
    }
    done(saved);
}

void ReadingBook::finishBackgroundCheckpoint(const QJsonObject &checkpoint, bool saved)
{
    // A synchronous checkpoint replaced this one while it ran.
    if (checkpoint != pendingCheckpoint)
        return;
    pendingCheckpoint = {};
    if (saved) {
        savedCheckpoint = checkpoint;
        return;
    }
    // A background save is transient: the next save, sleep or close writes
    // the same state again, and close reports its own failure. Log it only.
    diagnostic("local-save-failed", { { "background", true } }, DiagnosticSeverity::Error);
    if (checkpointFailed)
        checkpointFailed();
}
} // namespace kobalt
