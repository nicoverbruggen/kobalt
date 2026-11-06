#pragma once
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace kobalt::nickel_qt5 {
// An empty identity confirms sign-out only when the query succeeded.
struct AccountRead
{
    QString identity;
    bool valid = false;
};

inline AccountRead readAccount(QSqlDatabase database)
{
    QSqlQuery query(database);
    AccountRead result;
    if (!query.exec("SELECT UserID FROM user"))
        return result;
    if (query.next())
        result.identity = query.value(0).toString();
    result.valid = !query.lastError().isValid();
    return result;
}
enum class AccountCheck { Deferred, Current, Changed };

inline AccountCheck checkAccount(const AccountRead &read, const QString &known)
{
    if (!read.valid)
        return AccountCheck::Deferred;
    return read.identity == known ? AccountCheck::Current : AccountCheck::Changed;
}
} // namespace kobalt::nickel_qt5
