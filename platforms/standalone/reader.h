#pragma once
#include "platform/api/reader_orientation.h"
#include "platform/api/reader_host_services.h"
#include "reading/reader_session.h"
#include "app/reader_window.h"
#include "storage/reader_storage.h"
#include "storage/reading_history.h"
#include "publication/publication_endpoint.h"
#include <QPointer>

namespace kobalt::standalone {
// Own the publication until engine work has drained.
// A newer document replaces a pending opening; closing always cancels it.
class ReaderController : public QObject
{
    Q_OBJECT
public:
    ReaderController(storage::Paths paths, EngineFontInputs fonts, QString placeholder,
                     std::function<ReaderHostServices()> services,
                     std::function<void(Qt::ScreenOrientation)> rotate, QObject *parent = nullptr);
    ~ReaderController() override;
    void open(const QString &path, QSize pixels, QString sourceKey = {},
              bool restartAtStart = false);
    void close();
    QJsonObject lastBook() const;
    // The state the reader saved for the book at this path.
    QJsonObject savedState(const QString &path) const;
    // Reset only this book's checkpoint. Annotations and global preferences stay intact.
    bool resetBook(const QString &path);
    // Reader preferences such as dark mode, shared by every book.
    QJsonObject preferences() const;

    ReaderWindow *window() const { return window_; }

    bool idle() const { return phase == Phase::Idle; }

    const QString &path() const { return current; }

signals:
    void opened();
    void openingProgress(int percent);
    void openingCover(QImage cover);
    void failed(QString error);
    // Emitted before shutdown can remove the reader from its canvas.
    void retiring();
    void closed();
    void exitRequested();

private:
    enum class Phase { Idle, Opening, Reading, Closing };
    Phase phase = Phase::Idle;
    storage::Paths paths;
    EngineFontInputs fonts;
    QString placeholder, current, pending, currentSourceKey, pendingSourceKey;
    std::shared_ptr<ReadingHistory::Profile> historyProfile;
    QSize pendingSize;
    bool restartAtStart = false, pendingRestartAtStart = false;
    std::function<ReaderHostServices()> services;
    ReaderOrientation orientation;
    std::unique_ptr<native::PublicationEndpoint> source;
    std::unique_ptr<ReaderSession> opening;
    QPointer<ReaderWindow> window_;
    std::shared_ptr<SourceLease> lease;
    void begin(const QString &path, QSize pixels);
    // Moves unusable saved data aside. Until the storage works, books do not open.
    void prepareProfile();
    bool storageFailed = false;
    // Shown by the next reader after saved data was reset.
    ReaderNotice recoveryNotice;
    bool retirementNotified = false;
    // When the current opening began, and the stages of the current close.
    QElapsedTimer openingStarted;
    ActionTiming closing;
    void notifyRetirement();
    void retire();
    void release();
};
} // namespace kobalt::standalone
