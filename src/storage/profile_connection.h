#pragma once
#include "support/diagnostics.h"
#include <QSqlDatabase>
#include <QString>
#include <functional>
#include <memory>

// Internal to src/storage. The profile database (profile_database.cpp) lends
// its connection to the stores that keep tables in it.
namespace kobalt::storage::profile {
// A failed step. Its code is the error that the public operation reports.
struct Failure
{
    QString code;
};

// Throws Failure{code} when `ok` is false.
void check(bool ok, const char *code);

// An operation's view of the profile connection. It never closes it; a
// transaction left open by a failure is rolled back.
class Connection
{
public:
    ~Connection();
    void open(const QString &root, const QString &database);
    void execute(const QString &statement);
    void begin();
    void beginRead();
    void commit();
    QSqlDatabase db;

private:
    std::unique_ptr<DiagnosticOperation> writeTrace;
    bool transaction_ = false, writing_ = false;
    qint64 changes = 0;
};

// Every operation runs on the writer thread, which owns the profile connection.
// It keeps the caller's choice of routine writes. A Failure becomes `error`
// and a false result.
bool guarded(QString *error, const std::function<void()> &operation);
} // namespace kobalt::storage::profile
