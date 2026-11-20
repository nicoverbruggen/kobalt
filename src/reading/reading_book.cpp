#include "support/strings.h"
#include "support/save_wait.h"
#include "reading_book.h"
#include "text_presets.h"
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
                         ReadingHistory::UtcClock utc, ReadingTimer::Clock readingClock)
    : session_(std::move(session)),
      statePath(std::move(statePath)),
      identity_(std::move(identity)),
      paths_(std::move(paths)),
      digest(std::move(digest)),
      bridge(std::move(bridge)),
      nativeSync(this->bridge ? this->bridge->nativeSyncPolicy() : NativeSyncPolicy{}),
      baseline(std::move(baseline)),
      reconciliation(std::move(diskBaseline),
                     this->bridge ? this->bridge->positionNormalization() : nullptr),
      activeReading(std::move(readingClock))
{
    this->utc = utc ? std::move(utc)
                    : ReadingHistory::UtcClock([] { return QDateTime::currentMSecsSinceEpoch(); });
    sourceKey_ = sourceKey;
    auto saved = storage::readBookState(paths_, identity_, this->statePath).object;
    // Privacy belongs to the publication, including after Mark as unread.
    hideBookOnSleepScreen_ = saved.value("book").toString() == identity_
            && saved.value("hide_book_on_sleep_screen").toBool(false);
    if (saved.value("book").toString() == identity_)
        sleepPositions_ = saved.value("sleep_positions").toObject();
    // The native opener resets its snapshot before the database record is
    // replaced. Apply the same reset before retaining fields or preferences.
    const bool unreadReset = this->bridge && resetNativeUnread(saved, reconciliation.baseline());
    preservedState = saved;
    nativeFailure = saved.value("native_writeback_failure").toObject();
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
    if (!unreadReset && session_->savedStateMatched()
        && saved.value("book").toString() == identity_) {
        textPreset_ = saved.value(textpresets::bookKey).toString().left(64);
    } else {
        const auto choice = textpresets::newBookChoice(preferences);
        if (choice != "last" && choice != "book")
            textPreset_ = choice;
    }
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
    return activityPending || appearancePending || deferredCheckpointEpoch
            || !deferredCloses.isEmpty() || (activityWriter && !activityWriter->idle())
            || (appearanceWriter && !appearanceWriter->idle()) || closeCheckpointPending
            || !pendingCheckpoint.isEmpty() || (writer && !writer->idle())
            || (history && history->saving()) || (speedWriter && !speedWriter->idle());
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
        callback(NativeActivityResult::Abandoned);
}

void ReadingBook::finishReading(std::function<void(NativeActivityResult)> done)
{
    sleepingReading = false;
    stopReading(true);
    drainReadingSave(std::move(done));
}

void ReadingBook::drainReadingSave(std::function<void(NativeActivityResult)> done)
{
    if (!bridge) {
        done(NativeActivityResult::NotRequired);
        return;
    }
    nativeFinishCallbacks.append(std::move(done));
    if (nativeFinishCallbacks.size() > 1)
        return;
    const auto generation = ++nativeFinishGeneration;
    const std::weak_ptr<ReadingBook *> guard = alive;
    bridge->finishReading([guard, generation](NativeActivityResult result) {
        // Whatever the bridge did, its reading save is no longer pending.
        saves::settled();
        if (const auto book = guard.lock())
            (*book)->finishNativeReading(generation, result);
    });
}

void ReadingBook::finishNativeReading(quint64 generation, NativeActivityResult result)
{
    if (generation != nativeFinishGeneration)
        return;
    const auto callbacks = std::exchange(nativeFinishCallbacks, {});
    for (const auto &callback : callbacks)
        callback(result);
}

bool ReadingBook::readingSavePending() const
{
    return bridge && bridge->readingSavePending();
}

void ReadingBook::retryFailedReadingSave(std::function<void(NativeActivityResult)> done,
                                         ReadingSaveRetry mode)
{
    if (bridge)
        bridge->retryFailedReadingSave(std::move(done), mode);
    else if (done)
        done(NativeActivityResult::NotRequired);
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
                pageTurns = 0;
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
             qMin(99.0, double(session_->progressPercent())), session_->bookPage(),
             session_->bookPages(), session_->turnDirection(), session_->totalWords(),
             session_->chapterGraph(), session_->graphSelection(),
             error.isEmpty() ? nativePosition : QString() };
}

ReadingEstimates ReadingBook::estimates(bool wholeVolume) const
{
    const int remaining = wholeVolume ? qMax(0, session_->bookPages() - session_->bookPage())
            : qBound(0, session_->mainPages() - session_->mainPage(), session_->mainPages());
    const auto mainTime = [&](ReadingEstimates estimate) {
        if (!wholeVolume && estimate.bookMilliseconds >= 0 && session_->mainPages() != session_->bookPages()) {
            const int wholeRemaining = qMax(1, session_->bookPages() - session_->bookPage());
            estimate.bookMilliseconds =
                    qint64(double(estimate.bookMilliseconds) * remaining / wholeRemaining);
        }
        return estimate;
    };
    if (speed)
        return mainTime(speed->estimates(readingActivity()));
    if (bridge && !released && bridge->writable()) {
        ReadingEstimates result;
        if (bridge->estimates(readingActivity(), &result))
            return mainTime(result);
    }
    const auto reading = readingMilliseconds();
    const int chapterLeft = session_->chapterPages() > 0
            ? session_->chapterPages() - session_->chapterPage() + 1
            : -1;
    const int bookLeft = session_->bookPages() > 0 && session_->bookPage() >= 0 ? remaining : -1;
    return { pace_.estimate(chapterLeft, reading), pace_.estimate(bookLeft, reading),
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
    // Pause the host's interval. Kobo ends it at the boundary, inside the
    // transaction that saves the host's progress, as Nickel's reader does.
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
    quint64 generation = 0, epoch = 0;
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
    if (activityPending || deferredCheckpointEpoch || !deferredCloses.isEmpty()
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
    beginCheckpoint();
    save->generation = ++activityGeneration;
    save->epoch = positionSaveEpoch;
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
                            saves::settled();
                            if (ok) {
                                book->activitySaved.restart();
                                book->activityFailed.invalidate();
                                book->recordSavedCheckpoint(save->checkpoint, save->epoch);
                                if (book->speed && book->speed->state() == save->speed)
                                    book->speed->saved();
                            } else {
                                book->activityFailed.start();
                                diagnostic("reading-activity-unsaved",
                                           { { "attempts", attempt + 1 } },
                                           DiagnosticSeverity::Error);
                            }
                            book->resumeDeferredCheckpoints();
                            if (!ok && book->checkpointFailed)
                                book->checkpointFailed();
                        },
                        Qt::QueuedConnection);
            });
}

void ReadingBook::resumeDeferredCheckpoints()
{
    if (!deferredCheckpointEpoch && deferredCloses.isEmpty())
        return;
    const std::weak_ptr<ReadingBook *> guard = alive;
    const auto generation = activityGeneration;
    qt::invoke(
            QCoreApplication::instance(),
            [guard, generation] {
                const auto owner = guard.lock();
                if (!owner || (*owner)->activityGeneration != generation
                    || (*owner)->activityPending || (*owner)->appearancePending
                    || (*owner)->closeCheckpointPending)
                    return;
                auto *book = *owner;
                const auto epoch = std::exchange(book->deferredCheckpointEpoch, 0);
                saves::settled();
                auto closes = std::exchange(book->deferredCloses, {});
                if (!closes.isEmpty()) {
                    // Closing captures the latest state and supersedes a waiting
                    // background checkpoint. Keep every close completion.
                    book->checkpointForClose(
                            [guard, closes](bool saved) {
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
                            },
                            epoch);
                } else if (epoch) {
                    QJsonObject ignored;
                    book->checkpoint(&ignored, true, epoch);
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
                            saves::settled();
                            book->appearanceDirty = !ok;
                            if (ok)
                                book->appearanceFailed.invalidate();
                            else {
                                book->appearanceFailed.start();
                                diagnostic("appearance-settings-unsaved",
                                           { { "attempts", attempt + 1 } },
                                           DiagnosticSeverity::Error);
                            }
                            book->resumeDeferredCheckpoints();
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

QString ReadingBook::readingDay() const
{
    return history ? history->day()
                   : QDateTime::fromMSecsSinceEpoch(utc()).date().toString(Qt::ISODate);
}

QString ReadingBook::readingOpeningId() const
{
    return history ? history->openingId() : QString();
}

qint64 ReadingBook::readingDayMilliseconds()
{
    if (!history)
        return 0;
    if (activeReading.isValid())
        history->update(readingMilliseconds(), session_->state().value("position").toString());
    return history->dayMilliseconds(readingDay());
}

QMap<QDate, qint64> ReadingBook::readingDays()
{
    QMap<QDate, qint64> result;
    if (!history)
        return result;
    if (activeReading.isValid())
        history->update(readingMilliseconds(), session_->state().value("position").toString());
    const auto &days = history->dayTotals();
    for (auto it = days.cbegin(); it != days.cend(); ++it)
        result.insert(QDate::fromString(it.key(), Qt::ISODate), it.value());
    return result;
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
    // With only a part of the book laid out, its pages are not those of the
    // book: the percent of the whole book decides.
    if (session_->state().contains("book_fraction"))
        return session_->progressFraction() >= 0.9
                || (!session_->continuesAfter() && session_->nextPage() == session_->page());
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
    const auto result = bridge->markReading();
    if (!activityCompleted(result)) {
        diagnostic("native-opening-status-failed", {}, DiagnosticSeverity::Warning);
        return;
    }
    if (result == NativeActivityResult::NotRequired)
        return;
    baseline.status = 1;
    auto acknowledged = reconciliation.baseline();
    acknowledged.status = 1;
    reconciliation.reset(acknowledged);
}

NativeSaveResult ReadingBook::writeNative(const QString &location, const QString &mappingError,
                                          bool wholeVolume)
{
    if (!bridge) {
        nativeWriteback = NativeSaveResult::localOnly();
        return nativeSaveResult();
    }
    // The host decides whether this publication supports an activity-only
    // update when its exact position cannot be mapped.
    const bool timeOnly = !mappingError.isEmpty();
    QString error = timeOnly && !bridge->allowsUnmappedPosition() ? mappingError : QString();
    // Unsupported mappings intentionally stay local. Storage failures do not.
    nativeWriteback =
            error.isEmpty() ? NativeSaveResult::rejected() : NativeSaveResult::localOnly(error);
    if (error.isEmpty() && canWriteNative()) {
        const auto current = bridge->read(&error);
        reconciliation.observe(current);
        if (reconciliation.pending())
            return nativeSaveResult();
        if (reconciliation.mayWrite(current)) {
            // The same whole percentage as Kobalt's own views. The firmware 4
            // bridge keeps an unfinished book below 100, as Nickel's reader does.
            const double percent = session_->statsPercent(wholeVolume);
            const bool preserveFinished = finishedHere();
            diagnostic("native-save-start");
            const auto activity = readingActivity();
            const auto epoch = positionSaveEpoch;
            nativeWriteback = bridge->write(current, timeOnly ? QString() : location, percent,
                                            preserveFinished, activity);
            error = nativeWriteback.message();
            if (nativeWriteback.saved()) {
                baseline = nativeWriteback.baseline();
                reconciliation.reset(baseline);
                if (!timeOnly && nativeWriteback.status() == NativeSaveResult::Status::Saved
                    && positionSaved)
                    positionSaved(activity.position, epoch);
            } else {
                // A commit can lose a race after the earlier read. Ask about
                // that update before letting the window finish its exit.
                observeProgress();
                if (reconciliation.pending() && canWriteNative())
                    return nativeSaveResult();
            }
        } else {
            if (error.isEmpty())
                error = strings::get("error.nickel_progress_changed_before_exit");
            nativeWriteback = NativeSaveResult::progressChanged(error);
        }
    } else if (error.isEmpty()) {
        error = strings::get(
                "error.nickel_save_deferred_while_progress_or_device_state_is_unresolved");
        nativeWriteback = NativeSaveResult::deferred(error);
    }
    const bool failed = nativeWriteback.failed() || !error.isEmpty();
    auto fields = qt::object({ { "result", nativeWriteback.diagnosticName() },
                               { "mapping_failed", timeOnly },
                               { "failed", failed },
                               { "error_code",
                                 failed && error.isEmpty() ? QStringLiteral("unknown")
                                                           : diagnosticErrorCode(error) },
                               { "mapping_error_code", diagnosticErrorCode(mappingError) } });
    if (failed || timeOnly) {
        nativeFailure = fields;
        nativeFailure["utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    }
    diagnostic("native-save", fields,
               failed ? DiagnosticSeverity::Error : DiagnosticSeverity::Activity);
    return nativeSaveResult();
}

NativeSaveResult ReadingBook::nativeSaveResult() const
{
    if (reconciliation.pending())
        return NativeSaveResult::conflict();
    return nativeWriteback;
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
    state.remove("main_text_bounded");
    state.remove("main_text_first");
    state.remove("main_text_last");
    state["book"] = identity_;
    state["fingerprint"] = QString::fromLatin1(digest);
    for (auto it = layoutPreferences_.begin(); it != layoutPreferences_.end(); ++it)
        state[it.key()] = it.value();
    if (!dictionaryChoice_.isEmpty())
        state["dictionary_id"] = dictionaryChoice_;
    if (!textPreset_.isEmpty())
        state[textpresets::bookKey] = textPreset_;
    state["schema"] = 1;
    state["nickel_progress"] = baseline.json();
    state["finished_position"] = acceptedFinishedAnchor;
    state["suppress_finish_prompt"] = finishPromptSuppressed;
    state["hide_book_on_sleep_screen"] = hideBookOnSleepScreen_;
    if (sleepPositions_.isEmpty())
        state.remove("sleep_positions");
    else
        state["sleep_positions"] = sleepPositions_;
    if (!imageViews.isEmpty())
        state["image_views"] = imageViews;
    // For the Android and desktop library, which shows progress without
    // opening the book.
    if (session_->pages() > 0) {
        state["percent"] = session_->mainPercent();
        state["stats_main_percent"] = session_->statsPercent(false);
        state["stats_volume_percent"] = session_->statsPercent(true);
    }
    state["native_writeback"] = QString::fromLatin1(nativeWriteback.diagnosticName());
    state["sync_pending"] = reconciliation.pending();
    state["native_writeback_error"] = nativeWriteback.message();
    if (!nativeFailure.isEmpty())
        state["native_writeback_failure"] = nativeFailure;
    return state;
}

QJsonObject ReadingBook::persistedCheckpoint(const QJsonObject &state) const
{
    auto checkpoint = preservedState;
    for (auto it = state.begin(); it != state.end(); ++it)
        checkpoint[it.key()] = it.value();
    if (dictionaryChoice_.isEmpty())
        checkpoint.remove("dictionary_id");
    if (textPreset_.isEmpty())
        checkpoint.remove(textpresets::bookKey);
    if (imageViews.isEmpty())
        checkpoint.remove("image_views");
    if (session_->temporaryTypography())
        session_->persistentAppearance().restoreIn(checkpoint);
    // These describe the displayed frame, not a saved reading position. Drop
    // them from imported records too, before comparing checkpoints or applying
    // the record's byte limit. Diagnostic snapshots above keep the full frame.
    for (const auto *key : { "chapter_title",
                             "chapter_page",
                             "chapter_pages",
                             "page",
                             "pages",
                             "next_page",
                             "text_font_face",
                             "embedded_font_count",
                             "cached_pages",
                             "continues_before",
                             "continues_after",
                             "book_fraction",
                             "whole_book_ready",
                             "slice_start",
                             "rough_page",
                             "rough_pages",
                             "rough_item_page",
                             "rough_item_pages",
                             "rough_pages_estimated",
                             "rough_item_pages_estimated",
                             "chapter_load_ms" })
        checkpoint.remove(key);
    const auto fits = [&checkpoint] {
        return QJsonDocument(checkpoint).toJson(QJsonDocument::Compact).size()
                <= storage::maxJsonBytes;
    };
    // The page estimate is optional and can be rebuilt. Its read spine items
    // go first, then the whole record.
    if (checkpoint.contains("page_estimate") && !fits()) {
        auto estimate = checkpoint.value("page_estimate").toObject();
        estimate.remove("read");
        checkpoint["page_estimate"] = estimate;
        if (!fits())
            checkpoint.remove("page_estimate");
    }
    // Image views are optional. Trim their oldest entries rather than letting
    // them prevent a reading-position checkpoint from fitting its record limit.
    auto views = checkpoint.value("image_views").toArray();
    while (!views.isEmpty() && !fits()) {
        views.removeAt(0);
        if (views.isEmpty())
            checkpoint.remove("image_views");
        else
            checkpoint["image_views"] = views;
    }
    return checkpoint;
}

quint64 ReadingBook::beginCheckpoint()
{
    // An older write can still land even if its GUI completion is superseded.
    // Only the newest successful completion can establish the stored state again.
    savedCheckpoint = {};
    pendingCheckpoint = {};
    return ++checkpointRequest;
}

bool ReadingBook::checkpoint(QJsonObject *snapshot, bool background)
{
    return checkpoint(snapshot, background, positionSaveEpoch);
}

bool ReadingBook::checkpoint(QJsonObject *snapshot, bool background, quint64 epoch)
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
        if (deferredCloses.isEmpty())
            deferredCheckpointEpoch = 0;
        resumeDeferredCheckpoints();
    }
    const auto state = checkpointState();
    *snapshot = state;
    // A second writer must not replace its queued checkpoint with a newer
    // position ahead of an older activity snapshot. Wait asynchronously for
    // that snapshot, then capture the current explicit choice.
    if (background && (activityPending || closeCheckpointPending)) {
        deferredCheckpointEpoch = epoch;
        return true;
    }
    const auto checkpoint = persistedCheckpoint(state);
    if (background && (checkpoint == pendingCheckpoint || checkpoint == savedCheckpoint))
        return true;
    const auto request = beginCheckpoint();
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
                [guard, request, epoch](bool saved) {
                    kobalt::qt::invoke(
                            QCoreApplication::instance(),
                            [guard, request, epoch, saved] {
                                if (const auto book = guard.lock())
                                    (*book)->finishBackgroundCheckpoint(request, epoch, saved);
                            },
                            Qt::QueuedConnection);
                });
        return true;
    }
    // The profile database has one writer thread. A synchronous checkpoint
    // replaces a queued one there and waits for its result.
    if (!writer)
        writer = std::make_unique<storage::RecordWriter>();
    // writeNow replaces pending work on this writer. A close owns a completion
    // that must still be delivered, even if sleep needs a newer durable save.
    if (closeCheckpointPending)
        writer->flush();
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
    recordSavedCheckpoint(checkpoint, epoch);
    activitySaved.restart();
    activityFailed.invalidate();
    if (speed && speed->state() == speedState)
        speed->saved();
    return true;
}

struct ReadingBook::CloseCheckpoint
{
    QJsonObject state, speed, rememberedAppearance;
    quint64 request, epoch;
};

void ReadingBook::checkpointForClose(std::function<void(bool)> done)
{
    checkpointForClose(std::move(done), positionSaveEpoch);
}

void ReadingBook::checkpointForClose(std::function<void(bool)> done, quint64 epoch)
{
    if (activityPending || appearancePending || closeCheckpointPending
        || !deferredCloses.isEmpty()) {
        deferredCheckpointEpoch = epoch;
        deferredCloses.append(std::move(done));
        return;
    }
    if (sleepingReading) {
        sleepingReading = false;
        stopReading(true);
    }
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
    const CloseCheckpoint save{ checkpoint, speedState, rememberedAppearance, beginCheckpoint(),
                                epoch };
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
            [guard, save, done = std::move(done)](bool saved) {
                kobalt::qt::invoke(
                        QCoreApplication::instance(),
                        [guard, save, saved, done] {
                            if (const auto book = guard.lock())
                                (*book)->finishCloseCheckpoint(save, saved, done);
                        },
                        Qt::QueuedConnection);
            });
}

void ReadingBook::finishCloseCheckpoint(const CloseCheckpoint &save, bool saved,
                                        std::function<void(bool)> done)
{
    if (history && history->saving()) {
        // The history writer reports when its write ends.
        const std::weak_ptr<ReadingBook *> guard = alive;
        saveWait.after([guard, save, saved, done] {
            if (const auto book = guard.lock())
                (*book)->finishCloseCheckpoint(save, saved, done);
        });
        return;
    }
    saved = (!history || history->savesSucceeded()) && saved;
    closeCheckpointPending = false;
    saves::settled();
    if (saved && save.request == checkpointRequest) {
        recordSavedCheckpoint(save.state, save.epoch);
        if (rememberedAppearance == save.rememberedAppearance)
            appearanceCheckpointSaved(save.state);
        activitySaved.restart();
        activityFailed.invalidate();
        if (speed && speed->state() == save.speed)
            speed->saved();
    } else if (!saved) {
        diagnostic("local-close-save-failed", {}, DiagnosticSeverity::Error);
    }
    resumeDeferredCheckpoints();
    done(saved);
}

void ReadingBook::recordSavedCheckpoint(const QJsonObject &checkpoint, quint64 epoch)
{
    savedCheckpoint = checkpoint;
    if (positionSaved)
        positionSaved(checkpoint.value("position").toString(), epoch);
}

void ReadingBook::finishBackgroundCheckpoint(quint64 request, quint64 epoch, bool saved)
{
    if (request != checkpointRequest)
        return;
    const auto checkpoint = std::exchange(pendingCheckpoint, QJsonObject());
    saves::settled();
    if (saved) {
        recordSavedCheckpoint(checkpoint, epoch);
        return;
    }
    // A background save is transient: the next save, sleep or close writes
    // the same state again, and close reports its own failure. Log it only.
    diagnostic("local-save-failed", { { "background", true } }, DiagnosticSeverity::Error);
    if (checkpointFailed)
        checkpointFailed();
}
} // namespace kobalt
