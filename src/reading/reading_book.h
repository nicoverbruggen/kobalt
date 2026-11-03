#pragma once
#include "reading_pace.h"
#include "reading_speed.h"
#include "reading_progress.h"
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "reading_bridge.h"
#include "reading_timer.h"
#include "storage/reading_history.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>
#include <utility>

namespace kobalt {
class ReaderSession;

// Owns the open engine facade and its local/native progress. The window decides
// when to present a conflict; this object decides which progress may be saved.
// Like Nickel, a book opens as reading: it is saved as finished only after it
// is finished again or a finished position from sync is accepted.
class ReadingBook {
public:
    ReadingBook(std::unique_ptr<ReaderSession> session, QString statePath, storage::Paths paths, QString identity,
                QByteArray digest, std::unique_ptr<ReadingBridge> bridge, ReadingProgress baseline,
                ReadingProgress diskBaseline, QString sourceKey = {}, ReadingHistory::UtcClock utc = {});
    ~ReadingBook();
    void discardPendingSaves() { discardSaves = true; }
    ReaderSession *session() const {
        return model.get();
    }
    const QString &identity() const {
        return identity_;
    }
    const storage::Paths &paths() const {
        return paths_;
    }
    const QByteArray &fingerprint() const {
        return digest;
    }
    bool deviceTransition() const;
    bool canWriteNative() const;
    bool hasNativeProgress() const { return bool(bridge); }
    // Downloaded native-library titles use that platform's cloud sync.
    // Sideloaded file identities keep native local progress and use KOsync.
    bool usesNativeCloudSync() const { return nativeCloudSync; }
    // A sideloaded EPUB file that is not a kepub.
    bool plainEpub() const {
        return identity_.startsWith("file://") && identity_.endsWith(".epub", Qt::CaseInsensitive) &&
               !identity_.endsWith(".kepub.epub", Qt::CaseInsensitive);
    }
    // Revocation abandons native work but keeps local data available to save.
    void releaseNative(bool save = true);
    void observeReading(bool learnPace = true);
    void stopReading(bool background = false, bool nativeAllowed = true);
    void sleepReading(const QJsonObject &preferences);
    void finishReading(std::function<void(bool)> done);
    // Drain native writes without completing the local session retained by sleep.
    void drainReadingSave(std::function<void(bool)> done);
    bool readingSavePending() const;
    void retryFailedReadingSave();
    bool checkpointReadingHistory(bool background = false);
    bool activitySaveDue(int intervalMilliseconds = 300000) const;
    bool activitySaveCoolingDown() const;
    void saveReadingActivity(bool durable = false);
    void rememberAppearance();
    void retryAppearanceSave();
    bool tracksReadingHistory() const { return bool(history); }
    bool usesLocalPace() const {
        return bool(speed);
    }
    void pauseLocalPace() {
        if (speed) {
            speed->pause();
        }
    }
    // Active time for this opening, excluding sleep and hidden reader time.
    qint64 readingMilliseconds() const;
    qint64 sessionReadingMilliseconds() const { return readingMilliseconds() - sessionBaseline; }
    bool readingActive() const {
        return activeReading.isValid();
    }
    ReadingEstimates estimates() const;
    void onEstimatesChanged(std::function<void()> changed) {
        if (bridge) {
            bridge->estimatesChanged = std::move(changed);
        }
    }
    // Session fallback for native bridges without a reading estimate.
    ReadingPace &pace() {
        return pace_;
    }
    bool offerFinish() const;
    void markFinished();
    void suppressFinishPrompt() { finishPromptSuppressed = true; }
    void pollProgress();
    void invalidateProgress() {
        progressPoll.invalidate();
    }
    ReadingProgress readProgress() const;
    void observeProgress();
    bool pendingProgress() const {
        return reconciliation.pending();
    }
    ReadingProgress candidate() const {
        return reconciliation.candidate();
    }
    void acknowledge(const ReadingProgress &shown, bool moved);
    void markNativeReading();
    // False leaves a sync conflict pending for the window to resolve.
    bool writeNative(const QString &location, const QString &mappingError);
    enum class NativeSave { Saved, LocalOnly, Conflict, Failed };
    NativeSave nativeSaveResult() const;
    const QString &nativeSaveError() const { return nativeWritebackError; }
    // A background checkpoint is queued on the book's writer thread and
    // returns at once; a failure is reported through onCheckpointFailed. A
    // synchronous checkpoint first drops a queued one and waits for a running
    // one, so an older position cannot replace it.
    bool checkpoint(QJsonObject *snapshot, bool background = false);
    // Save the final immutable snapshot and acknowledge history without blocking
    // the GUI. A false result retains the book's state for another attempt.
    void checkpointForClose(std::function<void(bool)> done);
    QJsonObject checkpointState() const;
    bool savesPending() const;
    // Called after a failed background checkpoint, which is already logged.
    // The reader does not notify the user; tests observe failures here.
    void onCheckpointFailed(std::function<void()> failed) {
        checkpointFailed = std::move(failed);
    }
    const QJsonObject &layoutPreferences() const {
        return layoutPreferences_;
    }
    const QString &dictionaryChoice() const {
        return dictionaryChoice_;
    }
    bool saveDictionaryChoice(const QString &id);
    QJsonObject imageView(const QString &id) const;
    // Memory only. The caller can queue the existing background checkpoint.
    bool rememberImageView(const QString &id, const QJsonObject &view);

private:
    bool canFinish() const;
    bool finishPromptSuppressed = false;
    struct ActivitySave;
    void writeActivity(std::shared_ptr<ActivitySave>, int attempt);
    void resumeActivityCheckpoints();
    QElapsedTimer activitySaved, activityFailed;
    bool activityPending = false, checkpointAfterActivity = false;
    QVector<std::function<void(bool)>> closesAfterActivity;
    quint64 activityGeneration = 0;
    std::unique_ptr<storage::RecordWriter> activityWriter, appearanceWriter;
    QJsonObject rememberedAppearance;
    bool appearanceDirty = false, appearancePending = false;
    QElapsedTimer appearanceFailed;
    quint64 appearanceGeneration = 0;
    void writeAppearance(int attempt);
    void appearanceCheckpointSaved(const QJsonObject &state);
    void finishNativeReading(quint64 generation, bool saved);
    quint64 nativeFinishGeneration = 0;
    QVector<std::function<void(bool)>> nativeFinishCallbacks;
    QJsonObject persistedCheckpoint(const QJsonObject &state) const;
    void finishCloseCheckpoint(const QJsonObject &checkpoint, bool saved, std::function<void(bool)> done);
    bool closeCheckpointPending = false;
    ReadingActivity readingActivity() const;
    // `background` queues the write instead of waiting for it.
    void saveSpeed(bool force = false, bool background = false);
    std::unique_ptr<ReaderSession> model;
    QString statePath, identity_;
    storage::Paths paths_;
    QByteArray digest;
    std::unique_ptr<ReadingBridge> bridge;
    const bool nativeCloudSync;
    bool released = false, openingStatusAttempted = false, discardSaves = false;
    ReadingProgress baseline;
    ProgressReconciler reconciliation;
    QElapsedTimer progressPoll;
    ReadingTimer activeReading;
    bool sleepingReading = false;
    ReadingHistory::UtcClock utc;
    qint64 sleepingAt = 0, sleepingTimeout = 0, sessionBaseline = 0;
    qint64 completedReadingMilliseconds = 0;
    ReadingPace pace_;
    std::unique_ptr<ReadingSpeed> speed;
    std::unique_ptr<ReadingHistory> history;
    QElapsedTimer speedSaved;
    QString nativeWriteback = "not-attempted", nativeWritebackError, acceptedFinishedAnchor;
    // The last checkpoint that reached storage. An identical one is not written again.
    QJsonObject savedCheckpoint, layoutPreferences_;
    // Keep extra fields from the supported record loaded at opening. Reader
    // fields are regenerated at each checkpoint.
    QJsonObject preservedState;
    QJsonArray imageViews;
    // The background checkpoint that has not reported back yet.
    QJsonObject pendingCheckpoint;
    std::function<void()> checkpointFailed;
    void finishBackgroundCheckpoint(const QJsonObject &checkpoint, bool saved);
    // A writer holds one waiting write, so each record has its own: a speed
    // record must not replace a waiting position. The destructor resets both
    // after their last writes have finished.
    std::unique_ptr<storage::RecordWriter> writer, speedWriter;
    std::shared_ptr<ReadingBook *> alive = std::make_shared<ReadingBook *>(this);
    QString dictionaryChoice_;
};
}
