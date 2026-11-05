#pragma once
#include "check.h"
#include "storage/profile_database.h"
#include <QCoreApplication>
#include <QProcess>
#include <QSqlQuery>

// Abort real annotation transactions without making unrelated profile writes fail.
class SqliteSessionWriteFailure
{
public:
    SqliteSessionWriteFailure(QString directory, int kind) : directory(std::move(directory))
    {
        require(kobalt::storage::profileTransaction(
                        this->directory,
                        [kind](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            for (const auto *operation : { "INSERT", "UPDATE", "DELETE" }) {
                                const auto row = QByteArray(operation) == "DELETE" ? "OLD" : "NEW";
                                if (!query.exec(
                                            QString("CREATE TRIGGER session_fail_%1 BEFORE %1 ON entries WHEN %2.kind=%3 "
                                                    "BEGIN SELECT RAISE(ABORT,'session-write-fixture'); END")
                                                    .arg(operation, row)
                                                    .arg(kind)))
                                    return false;
                            }
                            return true;
                        }),
                "Cannot obstruct SQLite session writes");
    }

    ~SqliteSessionWriteFailure()
    {
        require(kobalt::storage::profileTransaction(
                        directory,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            for (const auto *operation : { "INSERT", "UPDATE", "DELETE" })
                                if (!query.exec(
                                            QString("DROP TRIGGER session_fail_%1").arg(operation)))
                                    return false;
                            return true;
                        }),
                "Cannot restore SQLite session writes");
    }

private:
    QString directory;
};

// A fresh process also exercises startup and teardown with the production backend.
inline int sqliteSessionVariant(const QString &book)
{
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(QCoreApplication::applicationFilePath(), { book, "--sqlite" });
    require(child.waitForStarted(), "Cannot start SQLite session check");
    if (!child.waitForFinished(75000)) {
        child.kill();
        child.waitForFinished(5000);
        require(false, "SQLite session check timed out");
    }
    require(child.exitStatus() == QProcess::NormalExit, "SQLite session check crashed");
    return child.exitCode();
}
