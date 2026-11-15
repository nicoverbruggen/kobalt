#pragma once
#include "record_writer.h"
#include <QDate>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

namespace kobalt {
// One saved reading interval from reading_sessions.
struct ActivitySession
{
    QString book, identity, sourceKey, title, author, startPosition, endPosition;
    // UTC milliseconds; `milliseconds` is active reading time.
    qint64 started = 0, observed = 0, milliseconds = 0;
    QMap<QDate, QMap<int, qint64>> hours;
};

// What Activity reads from the profile. Nothing here is written for it: the
// sessions, the daily totals, each book's saved progress and the saved words
// already exist for other features.
struct ActivityRecords
{
    // Oldest first.
    QVector<ActivitySession> sessions;
    // Active milliseconds per local date, summed over every opening except
    // the one the caller excluded.
    QMap<QDate, qint64> days;
    // Portion of days written by the per-session hour recorder.
    QMap<QDate, qint64> recordedDays;
    // Saved percent per reading identity, from each book's record.
    QHash<QString, int> percent;
    // Reading identities the reader marked as finished.
    QSet<QString> finished;
    // UTC milliseconds at which each saved word was first looked up.
    QVector<qint64> words;
    // Book keys hidden from Activity's book lists. Their time still counts.
    QSet<QString> hidden;
};

// Reads run on the profile writer thread, so the interface never waits for
// the database. Results return to the thread that owns the store.
class ActivityRecordStore final : public QObject
{
public:
    using Loaded = std::function<void(bool ok, ActivityRecords records)>;
    // Work on the loaded records that belongs on the store's thread too, such
    // as a summary the interface would otherwise compute.
    using Prepare = std::function<void(const ActivityRecords &)>;
    using Done = std::function<void(bool ok)>;
    explicit ActivityRecordStore(QString directory, QObject *parent = nullptr);
    ~ActivityRecordStore() override;
    // `excludeOpening` leaves out that opening's daily totals, so the reader
    // can add its own unsaved time without counting the saved part twice.
    // Only the newest load's result arrives.
    // `prepare` runs after a successful read.
    void load(const QString &excludeOpening, Prepare prepare, Loaded loaded);
    // Hides a book from the lists, or shows it again.
    void hide(const QString &book, bool hidden, Done done);
    // Deletes a book's sessions and takes their time out of the daily totals
    // of their dates. `keepOpening` is the open book's opening, whose totals
    // the reader keeps writing; they are never changed. This cannot be undone.
    void remove(const QString &book, const QString &keepOpening, Done done);

    // The same read, for callers already on a thread that may wait.
    static bool read(const QString &directory, const QString &excludeOpening,
                     ActivityRecords *records);
    static bool remove(const QString &directory, const QString &book, const QString &keepOpening);

private:
    void post(std::function<bool()> work, Done done);
    QString directory;
    quint64 generation = 0;
    // Every load and edit runs, in the order posted, after any save posted
    // before it. Only the newest load's result arrives.
    storage::RecordWriter writer{ storage::RecordWriter::Delivery::All };
};
} // namespace kobalt
