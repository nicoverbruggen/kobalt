#pragma once
#include <QString>
#include <functional>

class QSqlDatabase;

namespace kobalt::storage {
// The profile database stays open on the writer thread while Kobalt runs. See
// the profile connection in profile_database.cpp.

// Runs `operation` in a write transaction on the profile database in
// `directory`. Shared profile tables use the same ownership, version and
// durability checks. A false result rolls the transaction back. The handle
// must not escape.
bool profileTransaction(const QString &directory,
                        const std::function<bool(QSqlDatabase &)> &operation,
                        QString *error = nullptr);

// Waits for waiting writes, then folds the log into the main file, returns it
// to a rollback journal and closes it. With `keepClosed`, every operation
// fails until reopenProfile(), for example while USB storage is exported or
// the device powers off.
void closeProfile(bool keepClosed = false);
void reopenProfile();
// Whether the profile database in `directory` opens. Incompatible is a Kobalt
// database of a version this build does not know. Corrupt is a damaged, empty
// or foreign file. Unavailable means the storage failed. InUse means another
// process holds the file or storage is exported; the file may be fine. A
// database that opens stays open on the writer thread.
enum class ProfileDatabaseState { Missing, Ready, Incompatible, Corrupt, InUse, Unavailable };
ProfileDatabaseState inspectProfileDatabase(const QString &directory, QString *error = nullptr);
// Syncs recent commits and folds them into the main file, for example before
// sleep.
void checkpointProfile();
// True when routine commits need durability. A failed checkpoint suppresses
// further attempts for five minutes. Low-battery standby uses it.
bool profileCheckpointDue();
// True while routine commits wait for a sync.
bool profileHasUnsyncedWrites();
// Starts a checkpoint on the writer thread without waiting for it.
void requestProfileCheckpoint();
bool profileCheckpointPending();

// Use WAL NORMAL for transactions started in this scope. There is no extra
// per-commit sync; automatic checkpoints and the rollback-journal fallback
// can still sync. A lifecycle checkpoint makes prior commits durable.
class RoutineWrites
{
public:
    RoutineWrites();
    ~RoutineWrites();
    RoutineWrites(const RoutineWrites &) = delete;
    RoutineWrites &operator=(const RoutineWrites &) = delete;

private:
    bool previous;
};
} // namespace kobalt::storage
