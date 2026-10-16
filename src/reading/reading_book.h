#pragma once
#include "reading_pace.h"
#include "reading_progress.h"
#include "storage/reader_storage.h"
#include "reading_bridge.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <memory>

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
                ReadingProgress diskBaseline);
    ~ReadingBook();
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
    // A sideloaded EPUB file that is not a kepub.
    bool plainEpub() const {
        return identity_.startsWith("file://") && identity_.endsWith(".epub", Qt::CaseInsensitive) &&
               !identity_.endsWith(".kepub.epub", Qt::CaseInsensitive);
    }
    void releaseNative();
    void observeReading();
    void stopReading();
    // Active time for this opening, excluding sleep and hidden reader time.
    qint64 readingMilliseconds() const;
    ReadingEstimates estimates() const;
    // Forward turns in this opening, for the Stats time estimates.
    ReadingPace &pace() {
        return pace_;
    }
    bool offerFinish() const;
    void markFinished();
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
    // False leaves a sync conflict pending for the window to resolve.
    bool writeNative(const QString &location, const QString &mappingError);
    bool checkpoint(QJsonObject *snapshot);
    const QJsonObject &layoutPreferences() const {
        return layoutPreferences_;
    }
    bool saveLayoutPreferences(const QJsonObject &preferences);
    const QString &dictionaryChoice() const {
        return dictionaryChoice_;
    }
    bool saveDictionaryChoice(const QString &id);

private:
    ReadingActivity readingActivity() const;
    std::unique_ptr<ReaderSession> model;
    QString statePath, identity_;
    storage::Paths paths_;
    QByteArray digest;
    std::unique_ptr<ReadingBridge> bridge;
    bool released = false;
    ReadingProgress baseline;
    ProgressReconciler reconciliation;
    QElapsedTimer progressPoll;
    QElapsedTimer activeReading;
    qint64 completedReadingMilliseconds = 0;
    ReadingPace pace_;
    QString nativeWriteback = "not-attempted", nativeWritebackError, acceptedFinishedAnchor;
    // The last checkpoint that reached storage. An identical one is not written again.
    QJsonObject savedCheckpoint, layoutPreferences_;
    QString dictionaryChoice_;
};
}
