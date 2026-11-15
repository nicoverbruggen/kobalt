#pragma once
#include "support/qt_compat.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>

namespace kobalt::nickel {
// Use Nickel's status when its stored percentage still describes an earlier reading.
inline int nativeSleepPercent(const QVariant &status, const QVariant &percent)
{
    bool ok = false;
    const int readingStatus = status.toInt(&ok);
    if (ok && !status.isNull()) {
        if (readingStatus == 2)
            return 100;
        if (readingStatus == 0)
            return 0;
    }
    const int value = percent.toInt(&ok);
    return ok && !percent.isNull() ? qBound(0, value, 100) : -1;
}

// An ambiguous latest timestamp is not enough evidence to replace Nickel's
// screen. Include the timestamp so sync or another reader invalidates a frame.
inline QString lastReadBookToken(QSqlDatabase database, const QString &book = {})
{
    QSqlQuery query(database);
    if (!query.exec(
                "SELECT ContentID, DateLastRead FROM content WHERE ContentType = 6 "
                "AND DateLastRead IS NOT NULL AND DateLastRead != '' ORDER BY DateLastRead DESC LIMIT 2")
        || !query.next())
        return {};
    const auto identity = query.value(0).toString();
    const auto stamp = query.value(1).toString();
    if (identity.isEmpty() || (!book.isEmpty() && identity != book)
        || (query.next() && query.value(1).toString() == stamp))
        return {};
    return QString::fromUtf8(
            QJsonDocument(qt::array({ identity, stamp })).toJson(QJsonDocument::Compact));
}
} // namespace kobalt::nickel
