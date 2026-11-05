#pragma once
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <utility>

namespace kobalt {
// Change only the calling thread's connection, and restore Nickel's setting
// before releasing it. Progress saves remain nonblocking. The event worker
// can wait for its transaction before calling a destructive firmware flush.
class NativeDatabaseWait
{
public:
    NativeDatabaseWait(QSqlDatabase connection, bool nonblocking)
        : NativeDatabaseWait(std::move(connection), nonblocking ? 0 : -1)
    {
    }

    NativeDatabaseWait(QSqlDatabase connection, int milliseconds)
        : connection(std::move(connection))
    {
        if (milliseconds < 0)
            return;
        QSqlQuery query(this->connection);
        ready_ = query.exec(QStringLiteral("PRAGMA busy_timeout")) && query.next();
        if (!ready_)
            return;
        previous = query.value(0).toInt();
        ready_ = query.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(milliseconds));
    }

    ~NativeDatabaseWait()
    {
        if (previous >= 0) {
            QSqlQuery restore(connection);
            restore.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(previous));
        }
    }

    NativeDatabaseWait(const NativeDatabaseWait &) = delete;
    NativeDatabaseWait &operator=(const NativeDatabaseWait &) = delete;

    bool ready() const { return ready_; }

private:
    QSqlDatabase connection;
    int previous = -1;
    bool ready_ = true;
};
} // namespace kobalt
