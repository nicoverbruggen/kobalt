#pragma once
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QStringList>

namespace kobalt {
enum class BookProtection { None, Kobo, Other, Unavailable };

// Query presence and classification only. Key material stays in Nickel.
inline BookProtection bookProtection(const QSqlDatabase &database, const QString &identity)
{
    if (!database.isValid() || !database.isOpen() || identity.isEmpty())
        return BookProtection::Unavailable;
    QSqlQuery schema(database);
    if (!schema.exec("SELECT name FROM sqlite_master WHERE type='table' AND name IN "
                     "('content_keys','tolino_contentItem','tolino_drmKeysKobo')"))
        return BookProtection::Unavailable;
    QStringList tables;
    while (schema.next())
        tables.append(schema.value(0).toString());
    if (schema.lastError().isValid())
        return BookProtection::Unavailable;
    schema.finish();
    bool kobo = false;
    for (const auto &table :
         { QStringLiteral("content_keys"), QStringLiteral("tolino_drmKeysKobo") }) {
        if (!tables.contains(table))
            continue;
        QSqlQuery query(database);
        if (!query.prepare("SELECT 1 FROM " + table + " WHERE "
                           + (table == "content_keys" ? "volumeid" : "uuid") + " = ? LIMIT 1"))
            return BookProtection::Unavailable;
        query.addBindValue(identity);
        if (!query.exec())
            return BookProtection::Unavailable;
        kobo = query.next() || kobo;
        if (query.lastError().isValid())
            return BookProtection::Unavailable;
    }
    if (tables.contains("tolino_contentItem")) {
        QSqlQuery query(database);
        if (!query.prepare("SELECT contentProtectionType FROM tolino_contentItem WHERE uuid = ?"))
            return BookProtection::Unavailable;
        query.addBindValue(identity);
        if (!query.exec())
            return BookProtection::Unavailable;
        if (query.next()) {
            bool valid = false;
            const int type = query.value(0).toInt(&valid);
            // Tolino uses 1 for Adobe, 2 for Kobo, 4 for Tolino encryption,
            // 8 for no DRM and 16 for a watermark. Unknown flags stay native.
            if (!valid || type < 0 || (type & ~26) != 0)
                return BookProtection::Other;
            kobo = kobo || (type & 2);
        }
        if (query.lastError().isValid())
            return BookProtection::Unavailable;
    }
    return kobo ? BookProtection::Kobo : BookProtection::None;
}
} // namespace kobalt
