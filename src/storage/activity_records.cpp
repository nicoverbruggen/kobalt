#include "activity_records.h"
#include "profile_database.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <memory>

namespace kobalt {
namespace {
// No session reads for more than a month. A damaged row cannot make the
// per-hour and per-day loops over a session run for long.
qint64 clampedActive(qint64 milliseconds)
{
    return qBound<qint64>(0, milliseconds, qint64(31) * 24 * 3600 * 1000);
}

// Text columns are selected as BLOBs, as in reading sessions and saved words,
// so embedded NUL characters survive Qt's SQLite TEXT conversion.
QString text(const QVariant &value)
{
    const auto bytes = value.toByteArray();
    return QString::fromUtf8(bytes.constData(), bytes.size());
}

bool readSessions(QSqlDatabase &db, ActivityRecords *records)
{
    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.exec("SELECT CAST(book AS BLOB),CAST(identity AS BLOB),CAST(source_key AS BLOB),"
                    "CAST(title AS BLOB),"
                    "CAST(author AS BLOB),CAST(start_position AS BLOB),CAST(end_position AS BLOB),"
                    "started,observed,active_ms FROM reading_sessions ORDER BY started,id"))
        return false;
    while (query.next()) {
        ActivitySession session;
        session.book = text(query.value(0));
        session.identity = text(query.value(1));
        session.sourceKey = text(query.value(2));
        session.title = text(query.value(3));
        session.author = text(query.value(4));
        session.startPosition = text(query.value(5));
        session.endPosition = text(query.value(6));
        session.started = query.value(7).toLongLong();
        session.observed = query.value(8).toLongLong();
        session.milliseconds = clampedActive(query.value(9).toLongLong());
        records->sessions.append(session);
    }
    return true;
}

// `reading-day:<ISO date>:<opening>` records, as the sleep widget reads them.
// A damaged record is skipped instead of failing the whole screen.
bool readDays(QSqlDatabase &db, const QString &excludeOpening, ActivityRecords *records)
{
    const QString prefix = QStringLiteral("reading-day:");
    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare("SELECT name,format,CASE WHEN length(CAST(payload AS BLOB))<=16384 THEN payload "
                  "END FROM profile_state WHERE name>=? AND name<?");
    query.addBindValue(prefix);
    query.addBindValue(prefix + QChar(0xffff));
    if (!query.exec())
        return false;
    while (query.next()) {
        const auto name = query.value(0).toString();
        const auto parts = name.mid(prefix.size()).split(':');
        if (parts.size() != 2 || parts[1] == excludeOpening || query.value(1).toInt() != 1)
            continue;
        const auto date = QDate::fromString(parts[0], Qt::ISODate);
        const double milliseconds = QJsonDocument::fromJson(query.value(2).toByteArray())
                                            .object()
                                            .value("milliseconds")
                                            .toDouble(-1);
        if (!date.isValid() || !std::isfinite(milliseconds) || milliseconds < 0
            || milliseconds > 9007199254740991.0)
            continue;
        records->days[date] += qint64(milliseconds);
    }
    return true;
}

bool readBooks(QSqlDatabase &db, ActivityRecords *records)
{
    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.exec(
                "SELECT identity,CASE WHEN length(CAST(payload AS BLOB))<=16384 THEN payload END "
                "FROM book_state WHERE format=1"))
        return false;
    while (query.next()) {
        const auto identity = query.value(0).toString();
        const auto state = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
        if (state.contains("percent"))
            records->percent.insert(identity, qBound(0, state.value("percent").toInt(), 100));
        if (!state.value("finished_position").toString().isEmpty())
            records->finished.insert(identity);
    }
    return true;
}

// The book keys hidden from Activity's lists, in one small profile record.
const QString hiddenRecord = QStringLiteral("activity-hidden-books");

// False when the query fails. `writable` is false for a record this build
// cannot read, damaged or newer: it hides nothing and is never rewritten.
bool readHidden(QSqlDatabase &db, QSet<QString> *hidden, bool *writable = nullptr)
{
    QSqlQuery query(db);
    query.prepare("SELECT format,CASE WHEN length(CAST(payload AS BLOB))<=65536 THEN payload END "
                  "FROM profile_state WHERE name=?");
    query.addBindValue(hiddenRecord);
    if (!query.exec())
        return false;
    bool readable = true;
    if (query.next()) {
        const auto document = QJsonDocument::fromJson(query.value(1).toByteArray());
        const auto books = document.object().value("books");
        readable = query.value(0).toInt() == 1 && document.isObject() && books.isArray();
        if (readable)
            for (const auto &book : books.toArray())
                if (book.isString())
                    hidden->insert(book.toString());
    }
    if (writable)
        *writable = readable;
    return true;
}

bool writeHidden(QSqlDatabase &db, const QSet<QString> &hidden)
{
    QSqlQuery query(db);
    if (hidden.isEmpty()) {
        query.prepare("DELETE FROM profile_state WHERE name=?");
        query.addBindValue(hiddenRecord);
        return query.exec();
    }
    auto books = hidden.values();
    std::sort(books.begin(), books.end());
    // Qt 5.2's QJsonObject has no initializer-list constructor.
    QJsonObject object;
    object.insert("books", QJsonArray::fromStringList(books));
    const auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.size() > 65536)
        return false;
    query.prepare("INSERT OR REPLACE INTO profile_state(name,format,revision,payload) "
                  "VALUES(?,1,0,CAST(? AS TEXT))");
    query.addBindValue(hiddenRecord);
    query.addBindValue(payload);
    return query.exec();
}

// A session's active time by local date, counted back from its last
// observation and split at midnight. This matches the reader's daily totals
// for continuous reading. A session that went on after a short sleep keeps no
// record of its separate stretches, so its split is an estimate there.
void addByDate(QMap<QString, qint64> &dates, qint64 started, qint64 observed, qint64 active)
{
    const qint64 end = qMax(started, observed);
    qint64 cursor = end - qMax<qint64>(0, active);
    while (cursor < end) {
        const auto date = QDateTime::fromMSecsSinceEpoch(cursor).date();
        const qint64 next = QDateTime(date.addDays(1), QTime(0, 0)).toMSecsSinceEpoch();
        // Calendar boundaries, rather than 24-hour steps, keep DST days right.
        const qint64 stop = next > cursor ? qMin(end, next) : end;
        dates[date.toString(Qt::ISODate)] += stop - cursor;
        cursor = stop;
    }
}

// Takes a deleted book's time out of a date's daily totals. The totals do not
// name their book, so the date's sum drops by `removed`, but never below the
// time of the sessions that remain on that date: a date from before daily
// totals existed has no total for the deleted session. The open opening's
// total is never changed; a total that reaches zero is deleted.
bool reduceDay(QSqlDatabase &db, const QString &date, qint64 removed, qint64 remaining,
               const QString &keepOpening)
{
    const QString prefix = "reading-day:" + date + ':';
    QSqlQuery query(db);
    query.prepare("SELECT name,format,CASE WHEN length(CAST(payload AS BLOB))<=16384 THEN payload "
                  "END FROM profile_state WHERE name>=? AND name<? ORDER BY name");
    query.addBindValue(prefix);
    query.addBindValue(prefix + QChar(0xffff));
    if (!query.exec())
        return false;

    struct Total
    {
        QString name;
        QJsonObject payload;
        qint64 milliseconds;
    };

    QVector<Total> totals;
    qint64 sum = 0;
    while (query.next()) {
        if (query.value(1).toInt() != 1)
            continue;
        const auto payload = QJsonDocument::fromJson(query.value(2).toByteArray()).object();
        const double value = payload.value("milliseconds").toDouble(-1);
        if (!std::isfinite(value) || value <= 0 || value > 9007199254740991.0)
            continue;
        sum += qint64(value);
        const auto name = query.value(0).toString();
        if (name.mid(prefix.size()) != keepOpening)
            totals.append({ name, payload, qint64(value) });
    }
    qint64 take = sum - qMax(sum - removed, qMin(sum, remaining));
    for (auto &total : totals) {
        if (take <= 0)
            break;
        const qint64 taken = qMin(take, total.milliseconds);
        take -= taken;
        QSqlQuery write(db);
        if (taken == total.milliseconds) {
            write.prepare("DELETE FROM profile_state WHERE name=?");
            write.addBindValue(total.name);
        } else {
            total.payload["milliseconds"] = double(total.milliseconds - taken);
            write.prepare("UPDATE profile_state SET payload=CAST(? AS TEXT) WHERE name=?");
            write.addBindValue(QJsonDocument(total.payload).toJson(QJsonDocument::Compact));
            write.addBindValue(total.name);
        }
        if (!write.exec())
            return false;
    }
    return true;
}

bool readWords(QSqlDatabase &db, ActivityRecords *records)
{
    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.exec("SELECT created FROM saved_words ORDER BY created"))
        return false;
    while (query.next())
        records->words.append(query.value(0).toLongLong());
    return true;
}
} // namespace

ActivityRecordStore::ActivityRecordStore(QString directory, QObject *parent)
    : QObject(parent), directory(std::move(directory))
{
}

// An idle writer closes without waiting.
ActivityRecordStore::~ActivityRecordStore() = default;

bool ActivityRecordStore::read(const QString &directory, const QString &excludeOpening,
                               ActivityRecords *records)
{
    ActivityRecords result;
    const bool ok = storage::profileTransaction(directory, [&](QSqlDatabase &db) {
        return readSessions(db, &result) && readDays(db, excludeOpening, &result)
                && readBooks(db, &result) && readWords(db, &result)
                && readHidden(db, &result.hidden);
    });
    if (!ok) {
        diagnostic("activity-read-failed", {}, DiagnosticSeverity::Warning);
        return false;
    }
    *records = std::move(result);
    return true;
}

bool ActivityRecordStore::remove(const QString &directory, const QString &book,
                                 const QString &keepOpening)
{
    const bool ok = storage::profileTransaction(directory, [&](QSqlDatabase &db) {
        // Every session's time by date: the deleted book's, and the rest
        // that each date keeps.
        QSqlQuery sessions(db);
        sessions.setForwardOnly(true);
        if (!sessions.exec("SELECT CAST(book AS BLOB),started,observed,active_ms "
                           "FROM reading_sessions"))
            return false;
        QMap<QString, qint64> removed, remaining;
        const auto key = book.toUtf8();
        while (sessions.next())
            addByDate(sessions.value(0).toByteArray() == key ? removed : remaining,
                      sessions.value(1).toLongLong(), sessions.value(2).toLongLong(),
                      clampedActive(sessions.value(3).toLongLong()));
        QSqlQuery remove(db);
        remove.prepare("DELETE FROM reading_sessions WHERE book=CAST(? AS TEXT)");
        remove.addBindValue(key);
        if (!remove.exec())
            return false;
        for (auto it = removed.cbegin(); it != removed.cend(); ++it)
            if (!reduceDay(db, it.key(), it.value(), remaining.value(it.key()), keepOpening))
                return false;
        QSet<QString> hidden;
        bool writable = false;
        if (!readHidden(db, &hidden, &writable))
            return false;
        return !writable || !hidden.remove(book) || writeHidden(db, hidden);
    });
    diagnostic("activity-book-removed", { { "ok", ok } },
               ok ? DiagnosticSeverity::Activity : DiagnosticSeverity::Warning);
    return ok;
}

void ActivityRecordStore::post(std::function<bool()> work, Done done)
{
    QPointer<ActivityRecordStore> guard(this);
    writer.post(std::move(work), [guard, done](bool ok) {
        qt::invoke(
                guard.data(),
                [guard, done, ok] {
                    if (guard && done)
                        done(ok);
                },
                Qt::QueuedConnection);
    });
}

void ActivityRecordStore::hide(const QString &book, bool hidden, Done done)
{
    post(
            [directory = directory, book, hidden] {
                return storage::profileTransaction(directory, [&](QSqlDatabase &db) {
                    QSet<QString> books;
                    bool writable = false;
                    if (!readHidden(db, &books, &writable) || !writable)
                        return false;
                    if (hidden ? books.contains(book) : !books.contains(book))
                        return true;
                    if (hidden)
                        books.insert(book);
                    else
                        books.remove(book);
                    return writeHidden(db, books);
                });
            },
            std::move(done));
}

void ActivityRecordStore::remove(const QString &book, const QString &keepOpening, Done done)
{
    post([directory = directory, book,
          keepOpening] { return remove(directory, book, keepOpening); },
         std::move(done));
}

void ActivityRecordStore::load(const QString &excludeOpening, Prepare prepare, Loaded loaded)
{
    const auto ticket = ++generation;
    auto result = std::make_shared<ActivityRecords>();
    QPointer<ActivityRecordStore> guard(this);
    writer.post(
            [directory = directory, excludeOpening, result, prepare] {
                if (!read(directory, excludeOpening, result.get()))
                    return false;
                if (prepare)
                    prepare(*result);
                return true;
            },
            [guard, ticket, result, loaded](bool ok) {
                qt::invoke(
                        guard.data(),
                        [guard, ticket, result, loaded, ok] {
                            if (guard && guard->generation == ticket)
                                loaded(ok, ok ? std::move(*result) : ActivityRecords{});
                        },
                        Qt::QueuedConnection);
            });
}
} // namespace kobalt
