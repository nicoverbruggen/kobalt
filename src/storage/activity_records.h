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
    // Saved percent per reading identity, from each book's record.
    QHash<QString, int> percent;
    // Reading identities the reader marked as finished.
    QSet<QString> finished;
    // UTC milliseconds at which each saved word was first looked up.
    QVector<qint64> words;
};

// Reads run on the profile writer thread, so the interface never waits for
// the database. Results return to the thread that owns the store.
class ActivityRecordStore final : public QObject
{
public:
    using Loaded = std::function<void(bool ok, ActivityRecords records)>;
    explicit ActivityRecordStore(QString directory, QObject *parent = nullptr);
    ~ActivityRecordStore() override;
    // `excludeOpening` leaves out that opening's daily totals, so the reader
    // can add its own unsaved time without counting the saved part twice. A
    // newer load replaces one that has not started; only its result arrives.
    void load(const QString &excludeOpening, Loaded loaded);

    // The same read, for callers already on a thread that may wait.
    static bool read(const QString &directory, const QString &excludeOpening,
                     ActivityRecords *records);

private:
    QString directory;
    quint64 generation = 0;
    storage::RecordWriter writer;
};
} // namespace kobalt
