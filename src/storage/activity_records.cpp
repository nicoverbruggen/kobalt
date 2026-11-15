#include "activity_records.h"
#include "profile_database.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <cmath>
#include <memory>

namespace kobalt {
namespace {
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
        session.milliseconds = query.value(9).toLongLong();
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
                && readBooks(db, &result) && readWords(db, &result);
    });
    if (!ok) {
        diagnostic("activity-read-failed", {}, DiagnosticSeverity::Warning);
        return false;
    }
    *records = std::move(result);
    return true;
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
