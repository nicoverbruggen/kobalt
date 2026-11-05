#include "profile_database.h"
#include "profile_connection.h"
#include "reader_storage.h"
#include "record_writer.h"
#include "support/diagnostics.h"
#include <cstring>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <QVector>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace kobalt {
namespace {
constexpr int applicationId = 1262571057; // KAN1, owned annotation database.
constexpr int schemaVersion = 3;
constexpr qint64 maxDatabaseBytes = (qint64(1) << 32) - 1;

// SQLite reports a damaged file as CORRUPT or NOTADB. A lock or an I/O error
// says nothing about the file's content, so it gets its own code.
const char *failureCode(const QSqlError &error)
{
    if (!error.isValid())
        return "invalid-database";
#if QT_VERSION >= QT_VERSION_CHECK(5, 3, 0)
    const int code = error.nativeErrorCode().toInt() & 0xff;
#else
    const int code = error.number() & 0xff;
#endif
    if (code == 11 || code == 26)
        return "invalid-database";
    if (code == 5 || code == 6)
        return "database-busy";
    return "database-io";
}
} // namespace

namespace storage::profile {
void check(bool ok, const char *code)
{
    if (!ok) {
        // Only steps that open or write files can run out of descriptors.
        for (const auto *step :
             { "database-open", "database-write", "database-commit", "database-publish" })
            if (std::strcmp(code, step) == 0)
                reportDescriptorExhaustion(code);
        throw Failure{ QString::fromLatin1(code) };
    }
}
} // namespace storage::profile

namespace {
using storage::profile::check;
using storage::profile::Failure;

void run(QSqlDatabase &db, const QString &statement)
{
    QSqlQuery q(db);
    check(q.exec(statement), "database-write");
}

QVariant scalar(QSqlDatabase &db, const QString &statement)
{
    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.exec(statement) || !q.next())
        throw Failure{ QString::fromLatin1(failureCode(q.lastError())) };
    return q.value(0);
}

void validateSchema(QSqlDatabase &db, bool sessions = false)
{
    // Names alone accept partial tables and CREATE TABLE AS copies whose
    // primary keys are gone. Reject both before changing the journal mode.
    struct Table
    {
        const char *name, *columns, *types, *primary;
    };

    QVector<Table> tables{
        Table{ "books", "book,identity,fingerprint,title", "TEXT,TEXT,TEXT,TEXT", "book" },
        Table{ "lists", "book,kind,revision,updated,source_key,row_count,bytes",
               "TEXT,INTEGER,INTEGER,INTEGER,TEXT,INTEGER,INTEGER", "book,kind" },
        Table{ "entries",
               "book,kind,id,ordinal,start,end,text,chapter,created,color,style,note,chosen_start,chosen_end,start_policy,end_policy,bytes",
               "TEXT,INTEGER,TEXT,INTEGER,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,INTEGER,INTEGER,INTEGER",
               "book,kind,id" },
        Table{ "imports", "source,digest,record", "TEXT,BLOB,TEXT", "source" },
        Table{ "book_state", "identity,format,revision,payload", "TEXT,INTEGER,INTEGER,TEXT",
               "identity" },
        Table{ "profile_state", "name,format,revision,payload", "TEXT,INTEGER,INTEGER,TEXT",
               "name" },
        Table{ "library_books",
               "key,folder,name,title,author,cover,series,series_index,size,modified,added,opened,metadata,percent,finished,mark,hidden",
               "TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,REAL,INTEGER,INTEGER,INTEGER,INTEGER,INTEGER,INTEGER,INTEGER,TEXT,INTEGER",
               "key" },
        Table{ "library_folders", "key,label,ordinal", "TEXT,TEXT,INTEGER", "key" }
    };
    if (sessions)
        tables.append(
                { "reading_sessions",
                  "id,book,identity,fingerprint,source_key,title,author,started,observed,active_ms,start_position,end_position,status",
                  "TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,INTEGER,INTEGER,INTEGER,TEXT,TEXT,TEXT",
                  "id" });
    for (const auto &table : tables) {
        QSqlQuery query(db);
        const auto name = QString::fromLatin1(table.name);
        check(query.exec("PRAGMA table_info(" + name + ')'), "invalid-database");
        QMap<int, QString> primary;
        QStringList columns, types;
        while (query.next()) {
            columns.append(query.value(1).toString());
            types.append(query.value(2).toString().toUpper());
            const int ordinal = query.value(5).toInt();
            if (ordinal > 0)
                primary.insert(ordinal, query.value(1).toString());
        }
        // Positional INSERTs also require the declared column order and types.
        check(!query.lastError().isValid()
                      && columns == QString::fromLatin1(table.columns).split(',')
                      && types == QString::fromLatin1(table.types).split(',')
                      && primary.values() == QString::fromLatin1(table.primary).split(','),
              "invalid-database");
    }
    check(scalar(db, "PRAGMA quick_check(1)").toString() == QStringLiteral("ok"),
          "invalid-database");
    QSqlQuery references(db);
    check(references.exec("PRAGMA foreign_key_check"), "invalid-database");
    check(!references.next() && !references.lastError().isValid(), "invalid-database");
}

void sessionSchema(QSqlDatabase &db)
{
    run(db,
        "CREATE TABLE reading_sessions(id TEXT PRIMARY KEY NOT NULL,book TEXT NOT NULL,"
        "identity TEXT NOT NULL,fingerprint TEXT NOT NULL,source_key TEXT NOT NULL,title TEXT NOT NULL,"
        "author TEXT NOT NULL,started INTEGER NOT NULL CHECK(started>=0),"
        "observed INTEGER NOT NULL CHECK(observed>=started),"
        "active_ms INTEGER NOT NULL CHECK(active_ms>=0),start_position TEXT NOT NULL,end_position TEXT NOT NULL,"
        "status TEXT NOT NULL CHECK(status IN ('active','completed','interrupted')))");
    run(db, "CREATE INDEX reading_sessions_book ON reading_sessions(book,started)");
    run(db, "CREATE INDEX reading_sessions_started ON reading_sessions(started)");
}

void profileSchema(QSqlDatabase &db)
{
    run(db,
        "CREATE TABLE book_state(identity TEXT PRIMARY KEY NOT NULL,format INTEGER NOT NULL CHECK(format>0),"
        "revision INTEGER NOT NULL DEFAULT 0 CHECK(revision>=0),payload TEXT NOT NULL "
        "CHECK(length(CAST(payload AS BLOB))<=16384))");
    run(db,
        "CREATE TABLE profile_state(name TEXT PRIMARY KEY NOT NULL,format INTEGER NOT NULL CHECK(format>0),"
        "revision INTEGER NOT NULL DEFAULT 0 CHECK(revision>=0),payload TEXT NOT NULL "
        "CHECK(length(CAST(payload AS BLOB))<=262144))");
    run(db,
        "CREATE TABLE imports(source TEXT PRIMARY KEY,digest BLOB NOT NULL,record TEXT NOT NULL)");
    run(db,
        "CREATE TABLE library_books(key TEXT PRIMARY KEY,folder TEXT NOT NULL,name TEXT NOT NULL,title TEXT NOT NULL,"
        "author TEXT NOT NULL,cover TEXT NOT NULL,series TEXT NOT NULL,series_index REAL NOT NULL,"
        "size INTEGER NOT NULL,modified INTEGER NOT NULL,added INTEGER NOT NULL,opened INTEGER NOT NULL,"
        "metadata INTEGER NOT NULL,percent INTEGER NOT NULL,finished INTEGER NOT NULL,mark TEXT NOT NULL,hidden INTEGER NOT NULL)");
    run(db,
        "CREATE TABLE library_folders(key TEXT PRIMARY KEY,label TEXT NOT NULL,ordinal INTEGER NOT NULL)");
}

// Publish only a complete owned schema. An interruption before the rename
// leaves no canonical database, so the next open can retry. The
// initialization lock serializes publication between processes.
void createDatabase(const QString &path, const QString &directory)
{
    QTemporaryFile stage(directory + "/.kobalt-db-init-XXXXXX");
    check(stage.open(), "database-open");
    const auto stagingPath = stage.fileName();
    stage.close();
    const QString name = "kobalt-profile-init-" + QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(stagingPath);
        const bool opened = db.open();
        try {
            check(opened, "database-open");
            run(db, "PRAGMA encoding='UTF-8'");
            run(db, "PRAGMA journal_mode=DELETE");
            run(db, "PRAGMA synchronous=EXTRA");
            check(scalar(db, "PRAGMA synchronous").toInt() == 3, "database-synchronization");
            run(db, "PRAGMA fullfsync=ON");
            run(db, "BEGIN IMMEDIATE");
            run(db,
                "CREATE TABLE books(book TEXT PRIMARY KEY,identity TEXT NOT NULL,fingerprint TEXT NOT NULL,title TEXT NOT NULL)");
            run(db, "CREATE INDEX book_identity ON books(identity)");
            run(db,
                "CREATE TABLE lists(book TEXT NOT NULL,kind INTEGER NOT NULL,revision INTEGER NOT NULL,updated INTEGER NOT NULL,"
                "source_key TEXT NOT NULL,row_count INTEGER NOT NULL,bytes INTEGER NOT NULL,PRIMARY KEY(book,kind),"
                "FOREIGN KEY(book) REFERENCES books(book))");
            run(db,
                "CREATE TABLE entries(book TEXT NOT NULL,kind INTEGER NOT NULL,id TEXT NOT NULL,ordinal INTEGER NOT NULL,"
                "start TEXT NOT NULL,end TEXT NOT NULL,text TEXT NOT NULL,chapter TEXT NOT NULL,created TEXT NOT NULL,"
                "color TEXT NOT NULL,style TEXT NOT NULL,note TEXT NOT NULL,chosen_start TEXT NOT NULL,chosen_end TEXT NOT NULL,"
                "start_policy INTEGER NOT NULL,end_policy INTEGER NOT NULL,bytes INTEGER NOT NULL CHECK(bytes>0),"
                "PRIMARY KEY(book,kind,id),FOREIGN KEY(book,kind) REFERENCES lists(book,kind))");
            run(db, "CREATE INDEX entry_order ON entries(book,kind,ordinal)");
            profileSchema(db);
            sessionSchema(db);
            run(db, QString("PRAGMA application_id=%1").arg(applicationId));
            run(db, QString("PRAGMA user_version=%1").arg(schemaVersion));
            check(db.commit(), "database-commit");
        } catch (...) {
            db.close();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(name);
            throw;
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    check(::rename(QFile::encodeName(stagingPath).constData(), QFile::encodeName(path).constData())
                  == 0,
          "database-publish");
    const int parent = ::open(QFile::encodeName(directory).constData(), O_RDONLY);
    check(parent >= 0, "database-publish");
    const bool synced = ::fsync(parent) == 0;
    ::close(parent);
    check(synced, "database-publish");
}

// Profile state other threads read: whether routine commits wait for a sync,
// when the log was last synced, and whether the file may be opened at all.
std::atomic_bool profileUnsynced{ false }, profileBlocked{ false },
        profileCheckpointRunning{ false };
// A failed checkpoint makes standby wait a while before asking for another,
// so a full or failing card cannot keep the device awake.
std::atomic<qint64> profileCheckpointFailed{ -1 };
constexpr qint64 checkpointRetry = 300000;

qint64 monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

// The writer thread keeps one connection to one profile database open and
// lends it to each operation. Nothing else opens the file while it is open.
//
// The connection holds an exclusive lock and uses write-ahead logging. A
// commit appends to the `-wal` file; a checkpoint folds it into the main file.
// Commits sync the log (synchronous=FULL), except routine position saves
// (NORMAL), which a checkpoint syncs later. Closing checkpoints, returns the
// file to a rollback journal and closes it, so a copy, the computer over USB
// and older builds see one ordinary file. If SQLite refuses write-ahead
// logging, the connection keeps the rollback journal with synchronous=EXTRA.
class Profile
{
public:
    // Only the writer thread uses the profile. Never destroyed, so a write after
    // Qt's own shutdown cannot meet a destroyed connection.
    static Profile &instance()
    {
        static auto *profile = new Profile;
        return *profile;
    }

    QSqlDatabase &database(const QString &root, const QString &database)
    {
        const auto path = database.isEmpty() ? storage::Paths{ root }.database() : database;
        check(!profileBlocked, "storage-unavailable");
        if (db.isOpen() && path == path_)
            return db;
        close();
        open(root, path);
        return db;
    }

    void beginWrite()
    {
        if (!wal)
            return;
        const auto wanted = routineWrite ? QStringLiteral("NORMAL") : QStringLiteral("FULL");
        if (wanted != synchronous) {
            run(db, "PRAGMA synchronous=" + wanted);
            synchronous = wanted;
        }
    }

    // Called after a commit that changed rows. A synced commit also syncs every
    // earlier commit in the log; a commit without changes syncs nothing.
    void committed()
    {
        if (!wal)
            return;
        if (synchronous == QStringLiteral("NORMAL")) {
            profileUnsynced = true;
        } else {
            profileUnsynced = false;
        }
    }

    // Folds the log into the main file. `truncate` also empties the log file.
    bool checkpoint(bool truncate)
    {
        if (!db.isOpen() || !wal)
            return true;
        QElapsedTimer timer;
        timer.start();
        QSqlQuery q(db);
        const bool done = q.exec(truncate ? "PRAGMA wal_checkpoint(TRUNCATE)"
                                          : "PRAGMA wal_checkpoint(PASSIVE)")
                && q.next() && q.value(0).toInt() == 0 && q.value(1).toInt() == q.value(2).toInt();
        q.finish();
        if (done) {
            profileUnsynced = false;
            profileCheckpointFailed = -1;
        } else {
            profileCheckpointFailed = monotonicMilliseconds();
        }
        diagnostic("profile-checkpoint",
                   { { "truncate", truncate },
                     { "ok", done },
                     { "elapsed_ms", double(timer.elapsed()) } },
                   done ? DiagnosticSeverity::Activity : DiagnosticSeverity::Warning);
        return done;
    }

    void close()
    {
        if (!db.isOpen()) {
            release();
            return;
        }
        QElapsedTimer timer;
        timer.start();
        bool journal = true;
        if (wal) {
            const bool folded = checkpoint(true);
            QSqlQuery q(db);
            journal = q.exec("PRAGMA journal_mode=DELETE") && q.next()
                    && q.value(0).toString().compare("delete", Qt::CaseInsensitive) == 0;
            q.finish();
            journal = journal && folded;
        }
        db.close();
        release();
        // A close that leaves the log is kept in the durable log: the file then
        // needs its -wal next to it, for example on an exported partition.
        diagnostic("profile-close",
                   { { "rollback_journal", journal }, { "elapsed_ms", double(timer.elapsed()) } },
                   journal ? DiagnosticSeverity::Activity : DiagnosticSeverity::Error);
    }

    static thread_local bool routineWrite;

private:
    void release()
    {
        if (!name.isEmpty()) {
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(name);
            name.clear();
        }
        path_.clear();
        wal = false;
        synchronous.clear();
        // Closing the last connection syncs and folds the log.
        profileUnsynced = false;
    }

    void open(const QString &root, const QString &path)
    {
        QElapsedTimer timer;
        timer.start();
        const auto directory = QFileInfo(path).absolutePath();
        check(!root.isEmpty() && QDir().mkpath(directory), "create-directory");
        QLockFile initialization(directory + "/.kobalt-db-init.lock");
        if (!QFileInfo::exists(path))
            check(initialization.tryLock(1500), "database-busy");
        if (!QFileInfo::exists(path))
            createDatabase(path, directory);
        initialization.unlock();
        // SQLite would open a read-only file read-only, and that connection
        // would stay open and fail every write. Refuse it, so a later
        // operation can open the file once it is writable.
        check(QFileInfo(path).isWritable(), "database-read-only");
        name = "kobalt-profile-" + QUuid::createUuid().toString();
        db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1500");
        db.setDatabaseName(path);
        try {
            check(db.open(), "database-open");
            // Take the exclusive lock before the first read. A log left by a
            // crash is then replayed with its index in memory, and no -shm
            // file is ever created.
            check(scalar(db, "PRAGMA locking_mode=EXCLUSIVE")
                                  .toString()
                                  .compare("exclusive", Qt::CaseInsensitive)
                          == 0,
                  "database-lock");
            // Read ownership before any persistent pragma or schema change. An
            // empty, foreign, future or damaged existing file is never initialized.
            check(scalar(db, "PRAGMA application_id").toInt() == applicationId,
                  "unsupported-database-format");
            const int version = scalar(db, "PRAGMA user_version").toInt();
            check(version == 2 || version == schemaVersion, "unsupported-database-version");
            check(scalar(db, "PRAGMA encoding").toString() == QStringLiteral("UTF-8"),
                  "unsupported-database-encoding");
            const qint64 pageSize = scalar(db, "PRAGMA page_size").toLongLong();
            check(pageSize >= 512 && pageSize <= 65536 && (pageSize & (pageSize - 1)) == 0,
                  "invalid-database");
            const qint64 maxPages = maxDatabaseBytes / pageSize;
            // Keep the main file below FAT32's per-file ceiling. Journal size and
            // available space remain separate limits.
            check(scalar(db, QString("PRAGMA max_page_count=%1").arg(maxPages)).toLongLong()
                          == maxPages,
                  "database-size-limit");
            const int tables =
                    scalar(db,
                           "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                           "name IN ('books','lists','entries','imports','library_books','library_folders','book_state','profile_state')")
                            .toInt();
            check(tables == 8, "invalid-database");
            validateSchema(db, version == schemaVersion);
            if (version == 2) {
                // Upgrade before changing the journal mode. A rejected or failed
                // migration leaves the old schema and data intact.
                run(db, "PRAGMA synchronous=EXTRA");
                run(db, "PRAGMA fullfsync=ON");
                run(db, "BEGIN IMMEDIATE");
                const int lockedVersion = scalar(db, "PRAGMA user_version").toInt();
                check(lockedVersion == 2 || lockedVersion == schemaVersion,
                      "unsupported-database-version");
                validateSchema(db, lockedVersion == schemaVersion);
                if (lockedVersion == 2) {
                    sessionSchema(db);
                    run(db, QString("PRAGMA user_version=%1").arg(schemaVersion));
                }
                check(db.commit(), "database-commit");
                validateSchema(db, true);
            }
            // Validate and, if needed, migrate before changing journal mode.
            // A rejected file keeps its content; only a log left by a crash
            // is folded into it when the connection closes.
            wal = scalar(db, "PRAGMA journal_mode=WAL")
                            .toString()
                            .compare("wal", Qt::CaseInsensitive)
                    == 0;
            if (wal) {
                run(db, "PRAGMA synchronous=FULL");
                check(scalar(db, "PRAGMA synchronous").toInt() == 2, "database-synchronization");
                synchronous = QStringLiteral("FULL");
                // SQLite's default; it bounds the log at about 1000 pages.
                run(db, "PRAGMA wal_autocheckpoint=1000");
            } else {
                check(scalar(db, "PRAGMA journal_mode=DELETE")
                                      .toString()
                                      .compare("delete", Qt::CaseInsensitive)
                              == 0,
                      "database-journal");
                run(db, "PRAGMA synchronous=EXTRA");
                check(scalar(db, "PRAGMA synchronous").toInt() == 3, "database-synchronization");
            }
            run(db, "PRAGMA fullfsync=ON");
            run(db, "PRAGMA foreign_keys=ON");
            run(db, "PRAGMA cache_size=-2048");
            run(db, "PRAGMA mmap_size=0");
        } catch (...) {
            db.rollback();
            db.close();
            release();
            throw;
        }
        path_ = path;
        profileUnsynced = false;
        // Mac and Android leave through Qt; close the file cleanly then.
        static bool closesAtExit = false;
        if (!std::exchange(closesAtExit, true) && QCoreApplication::instance())
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                             QCoreApplication::instance(), [] { storage::closeProfile(); });
        diagnostic("profile-open",
                   { { "sqlite", scalar(db, "SELECT sqlite_version()").toString() },
                     { "write_ahead", wal },
                     { "elapsed_ms", double(timer.elapsed()) } },
                   false);
    }

    QSqlDatabase db;
    QString name, path_, synchronous;
    bool wal = false;
};

thread_local bool Profile::routineWrite = false;
} // namespace

namespace storage::profile {
Connection::~Connection()
{
    if (transaction_)
        db.rollback();
}

void Connection::open(const QString &root, const QString &database)
{
    db = Profile::instance().database(root, database);
}

void Connection::execute(const QString &statement)
{
    run(db, statement);
}

void Connection::begin()
{
    if (diagnosticsActive())
        writeTrace = std::make_unique<DiagnosticOperation>("profile-transaction");
    Profile::instance().beginWrite();
    execute("BEGIN IMMEDIATE");
    transaction_ = writing_ = true;
    changes = scalar(db, "SELECT total_changes()").toLongLong();
}

void Connection::beginRead()
{
    execute("BEGIN");
    transaction_ = true;
    writing_ = false;
}

void Connection::commit()
{
    check(db.commit(), "database-commit");
    transaction_ = false;
    // Many write transactions only read or find the record unchanged.
    if (writing_ && scalar(db, "SELECT total_changes()").toLongLong() != changes)
        Profile::instance().committed();
    writeTrace.reset();
}

bool guarded(QString *error, const std::function<void()> &operation)
{
    bool ok = false;
    const bool routine = Profile::routineWrite;
    storage::runOnWriterThread([&] {
        struct Restore
        {
            bool previous;
            ~Restore() { Profile::routineWrite = previous; }
        } restore{ std::exchange(Profile::routineWrite, routine) };
        try {
            operation();
            if (error)
                error->clear();
            ok = true;
        } catch (const Failure &failure) {
            if (error)
                *error = failure.code;
        }
    });
    return ok;
}
} // namespace storage::profile

namespace storage {
bool profileTransaction(const QString &directory,
                        const std::function<bool(QSqlDatabase &)> &operation, QString *error)
{
    return profile::guarded(error, [&] {
        profile::Connection connection;
        connection.open(directory, Paths{ directory }.database());
        connection.begin();
        profile::check(operation(connection.db), "profile-data");
        connection.commit();
    });
}

void closeProfile(bool keepClosed)
{
    drainWriterThread();
    runOnWriterThread([keepClosed] {
        Profile::instance().close();
        if (keepClosed)
            profileBlocked = true;
    });
}

void reopenProfile()
{
    profileBlocked = false;
}

ProfileDatabaseState inspectProfileDatabase(const QString &directory, QString *error)
{
    const auto path = Paths{ directory }.database();
    if (error)
        error->clear();
    if (!QFileInfo(path).exists() && !QFileInfo(path).isSymLink())
        return ProfileDatabaseState::Missing;
    QString code;
    if (profile::guarded(&code, [&] {
            profile::Connection connection;
            connection.open(directory, path);
        }))
        return ProfileDatabaseState::Ready;
    if (error)
        *error = code;
    // Our application ID with a version this build does not know.
    if (code == QLatin1String("unsupported-database-version"))
        return ProfileDatabaseState::Incompatible;
    // A foreign, empty or damaged file, or one with another text encoding.
    if (code == QLatin1String("invalid-database")
        || code == QLatin1String("unsupported-database-format")
        || code == QLatin1String("unsupported-database-encoding"))
        return ProfileDatabaseState::Corrupt;
    // Another process holds the file, or storage is exported. The file may be
    // fine; leave it to the normal opening.
    if (code == QLatin1String("database-busy") || code == QLatin1String("database-lock")
        || code == QLatin1String("storage-unavailable"))
        return ProfileDatabaseState::InUse;
    return ProfileDatabaseState::Unavailable;
}

void checkpointProfile()
{
    runOnWriterThread([] { Profile::instance().checkpoint(false); });
}

bool profileHasUnsyncedWrites()
{
    return profileUnsynced;
}

bool profileCheckpointDue()
{
    const qint64 failed = profileCheckpointFailed;
    return profileUnsynced && (failed < 0 || monotonicMilliseconds() - failed >= checkpointRetry);
}

void requestProfileCheckpoint()
{
    if (profileCheckpointRunning.exchange(true))
        return;
    // Never destroyed: a checkpoint can be requested until the process ends.
    static auto *writer = new RecordWriter;
    writer->post(
            [] {
                Profile::instance().checkpoint(false);
                return true;
            },
            [](bool) { profileCheckpointRunning = false; });
}

bool profileCheckpointPending()
{
    return profileCheckpointRunning;
}

RoutineWrites::RoutineWrites() : previous(Profile::routineWrite)
{
    Profile::routineWrite = true;
}

RoutineWrites::~RoutineWrites()
{
    Profile::routineWrite = previous;
}
} // namespace storage
} // namespace kobalt
