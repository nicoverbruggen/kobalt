#pragma once
#include <QString>
#include <QByteArray>
#include <QVector>
#include <functional>
#include <memory>
#include "record_writer.h"

class QSqlDatabase;

namespace kobalt {
// Durable intervals use the reader's cumulative active clock for duration and
// UTC only for chronology. A clock adjustment must not change reading time.
class ReadingHistory
{
public:
    using UtcClock = std::function<qint64()>;
    ReadingHistory(QString directory, QString identity, QByteArray fingerprint, QString sourceKey,
                   QString title, QString author, UtcClock utc = {});
    ~ReadingHistory();
    void start(qint64 activeMilliseconds, const QString &position);
    void update(qint64 activeMilliseconds, const QString &position);
    void stop(qint64 activeMilliseconds, const QString &position);
    // Freeze the final observation across sleep. start() resumes the same UUID;
    // stop() completes it without attaching the wake time or position.
    void pause(qint64 activeMilliseconds, const QString &position);
    // Failed writes retain every pending interval for the next attempt.
    // Periodic checkpoints queue an immutable snapshot. Lifecycle boundaries
    // wait for the latest snapshot; replacing a queued one preserves intervals.
    bool save(bool background = false);

    struct Snapshot
    {
        std::function<bool(QSqlDatabase &)> write;
        std::function<void(bool)> complete;
    };

    // Capture on the owner thread. Complete only after the enclosing transaction.
    Snapshot snapshot();
    bool saving() const;
    // Acknowledge completed background work without waiting or starting a write.
    bool savesSucceeded();
    class Profile;
    // Keep this handle for the app's profile lifetime. Controllers in the same
    // process share it; another process cannot recover or start a reader here.
    // Failed acquisition returns a diagnostic code through error.
    static std::shared_ptr<Profile> openProfile(const QString &directory, QString *error = nullptr);

private:
    static bool recover(const QString &directory);
    void acknowledge();

    struct Interval
    {
        QString id, startPosition, endPosition, status;
        qint64 started = 0, observed = 0, milliseconds = 0;
    };

    QString directory, identity, book, sourceKey, title, author;
    QByteArray fingerprint;
    UtcClock utc;
    QVector<Interval> pending;
    qint64 baseline = 0;
    bool paused = false;
    bool dirty = false;
    quint64 generation = 0, posted = 0, acknowledged = 0;
    struct Saved;
    std::shared_ptr<Saved> saved;
    // Destroy first: writes own snapshots and results until this has drained.
    std::unique_ptr<storage::RecordWriter> writer;
};
} // namespace kobalt
