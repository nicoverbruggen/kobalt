#include "annotation_database.h"
#include "bookmark_store.h"
#include "highlight_store.h"
#include "profile_database.h"
#include "reader_storage.h"
#include "record_writer.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QUuid>
#include <QVariant>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <unistd.h>
#include <utility>

namespace kobalt {
namespace {
constexpr int applicationId = 1262571057; // KAN1, owned annotation database.
constexpr int schemaVersion = 3;
constexpr qint64 maxDatabaseBytes = (qint64(1) << 32) - 1;
constexpr int bookmarkKind = 0, highlightKind = 1;
struct Failure { QString code; };
void check(bool ok, const char *code) {
    if (!ok) throw Failure{QString::fromLatin1(code)};
}
QString utf8Text(const QVariant &value) {
    // Qt 5.2's SQLite TEXT conversion stops at embedded NUL. These columns
    // are selected as BLOBs from an explicitly checked UTF-8 database.
    const auto bytes = value.toByteArray();
    int prefix = 0;
    while (prefix + 2 < bytes.size() && static_cast<unsigned char>(bytes[prefix]) == 0xef &&
           static_cast<unsigned char>(bytes[prefix + 1]) == 0xbb && static_cast<unsigned char>(bytes[prefix + 2]) == 0xbf)
        prefix += 3;
    auto text = QString::fromUtf8(bytes.constData() + prefix, bytes.size() - prefix);
    // A field has no byte-order header. Preserve every literal initial FEFF
    // rather than letting a Qt decoder consume it as an encoding signature.
    if (prefix) text.prepend(QString(prefix / 3, QChar(0xfeff)));
    return text;
}
void run(QSqlDatabase &db, const QString &statement) {
    QSqlQuery q(db);
    check(q.exec(statement), "database-write");
}
// SQLite reports a damaged file as CORRUPT or NOTADB. A lock or an I/O error
// says nothing about the file's content, so it gets its own code.
const char *failureCode(const QSqlError &error) {
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
QVariant scalar(QSqlDatabase &db, const QString &statement) {
    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.exec(statement) || !q.next())
        throw Failure{QString::fromLatin1(failureCode(q.lastError()))};
    return q.value(0);
}
void validateSchema(QSqlDatabase &db, bool sessions = false) {
    // Names alone accept partial tables and CREATE TABLE AS copies whose
    // primary keys are gone. Reject both before changing the journal mode.
    struct Table { const char *name, *columns, *types, *primary; };
    QVector<Table> tables{
             Table{"books", "book,identity,fingerprint,title", "TEXT,TEXT,TEXT,TEXT", "book"},
             Table{"lists", "book,kind,revision,updated,source_key,row_count,bytes", "TEXT,INTEGER,INTEGER,INTEGER,TEXT,INTEGER,INTEGER", "book,kind"},
             Table{"entries", "book,kind,id,ordinal,start,end,text,chapter,created,color,style,note,chosen_start,chosen_end,start_policy,end_policy,bytes",
                   "TEXT,INTEGER,TEXT,INTEGER,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,INTEGER,INTEGER,INTEGER", "book,kind,id"},
             Table{"imports", "source,digest,record", "TEXT,BLOB,TEXT", "source"},
             Table{"book_state", "identity,format,revision,payload", "TEXT,INTEGER,INTEGER,TEXT", "identity"},
             Table{"profile_state", "name,format,revision,payload", "TEXT,INTEGER,INTEGER,TEXT", "name"},
             Table{"library_books", "key,folder,name,title,author,cover,series,series_index,size,modified,added,opened,metadata,percent,finished,mark,hidden",
                   "TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,REAL,INTEGER,INTEGER,INTEGER,INTEGER,INTEGER,INTEGER,INTEGER,TEXT,INTEGER", "key"},
             Table{"library_folders", "key,label,ordinal", "TEXT,TEXT,INTEGER", "key"}};
    if (sessions)
        tables.append({"reading_sessions", "id,book,identity,fingerprint,source_key,title,author,started,observed,active_ms,start_position,end_position,status",
                       "TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,TEXT,INTEGER,INTEGER,INTEGER,TEXT,TEXT,TEXT", "id"});
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
            if (ordinal > 0) primary.insert(ordinal, query.value(1).toString());
        }
        // Positional INSERTs also require the declared column order and types.
        check(!query.lastError().isValid() && columns == QString::fromLatin1(table.columns).split(',') &&
                  types == QString::fromLatin1(table.types).split(',') &&
                  primary.values() == QString::fromLatin1(table.primary).split(','),
              "invalid-database");
    }
    check(scalar(db, "PRAGMA quick_check(1)").toString() == QStringLiteral("ok"), "invalid-database");
    QSqlQuery references(db);
    check(references.exec("PRAGMA foreign_key_check"), "invalid-database");
    check(!references.next() && !references.lastError().isValid(), "invalid-database");
}
void sessionSchema(QSqlDatabase &db) {
    run(db, "CREATE TABLE reading_sessions(id TEXT PRIMARY KEY NOT NULL,book TEXT NOT NULL,"
            "identity TEXT NOT NULL,fingerprint TEXT NOT NULL,source_key TEXT NOT NULL,title TEXT NOT NULL,"
            "author TEXT NOT NULL,started INTEGER NOT NULL CHECK(started>=0),"
            "observed INTEGER NOT NULL CHECK(observed>=started),"
            "active_ms INTEGER NOT NULL CHECK(active_ms>=0),start_position TEXT NOT NULL,end_position TEXT NOT NULL,"
            "status TEXT NOT NULL CHECK(status IN ('active','completed','interrupted')))");
    run(db, "CREATE INDEX reading_sessions_book ON reading_sessions(book,started)");
    run(db, "CREATE INDEX reading_sessions_started ON reading_sessions(started)");
}
void profileSchema(QSqlDatabase &db) {
    run(db, "CREATE TABLE book_state(identity TEXT PRIMARY KEY NOT NULL,format INTEGER NOT NULL CHECK(format>0),"
            "revision INTEGER NOT NULL DEFAULT 0 CHECK(revision>=0),payload TEXT NOT NULL "
            "CHECK(length(CAST(payload AS BLOB))<=16384))");
    run(db, "CREATE TABLE profile_state(name TEXT PRIMARY KEY NOT NULL,format INTEGER NOT NULL CHECK(format>0),"
            "revision INTEGER NOT NULL DEFAULT 0 CHECK(revision>=0),payload TEXT NOT NULL "
            "CHECK(length(CAST(payload AS BLOB))<=262144))");
    run(db, "CREATE TABLE imports(source TEXT PRIMARY KEY,digest BLOB NOT NULL,record TEXT NOT NULL)");
    run(db, "CREATE TABLE library_books(key TEXT PRIMARY KEY,folder TEXT NOT NULL,name TEXT NOT NULL,title TEXT NOT NULL,"
            "author TEXT NOT NULL,cover TEXT NOT NULL,series TEXT NOT NULL,series_index REAL NOT NULL,"
            "size INTEGER NOT NULL,modified INTEGER NOT NULL,added INTEGER NOT NULL,opened INTEGER NOT NULL,"
            "metadata INTEGER NOT NULL,percent INTEGER NOT NULL,finished INTEGER NOT NULL,mark TEXT NOT NULL,hidden INTEGER NOT NULL)");
    run(db, "CREATE TABLE library_folders(key TEXT PRIMARY KEY,label TEXT NOT NULL,ordinal INTEGER NOT NULL)");
}
// Publish only a complete owned schema. An interruption before the rename
// leaves no canonical database, so the next open can retry. The
// initialization lock serializes publication between processes.
void createDatabase(const QString &path, const QString &directory) {
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
            run(db, "CREATE TABLE books(book TEXT PRIMARY KEY,identity TEXT NOT NULL,fingerprint TEXT NOT NULL,title TEXT NOT NULL)");
            run(db, "CREATE INDEX book_identity ON books(identity)");
            run(db, "CREATE TABLE lists(book TEXT NOT NULL,kind INTEGER NOT NULL,revision INTEGER NOT NULL,updated INTEGER NOT NULL,"
                    "source_key TEXT NOT NULL,row_count INTEGER NOT NULL,bytes INTEGER NOT NULL,PRIMARY KEY(book,kind),"
                    "FOREIGN KEY(book) REFERENCES books(book))");
            run(db, "CREATE TABLE entries(book TEXT NOT NULL,kind INTEGER NOT NULL,id TEXT NOT NULL,ordinal INTEGER NOT NULL,"
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
    check(::rename(QFile::encodeName(stagingPath).constData(), QFile::encodeName(path).constData()) == 0,
          "database-publish");
    const int parent = ::open(QFile::encodeName(directory).constData(), O_RDONLY);
    check(parent >= 0, "database-publish");
    const bool synced = ::fsync(parent) == 0;
    ::close(parent);
    check(synced, "database-publish");
}
// Profile state other threads read: whether routine commits wait for a sync,
// when the log was last synced, and whether the file may be opened at all.
std::atomic_bool profileUnsynced{false}, profileBlocked{false}, profileCheckpointRunning{false};
// A failed checkpoint makes standby wait a while before asking for another,
// so a full or failing card cannot keep the device awake.
std::atomic<qint64> profileCheckpointFailed{-1};
constexpr qint64 checkpointRetry = 300000;
qint64 monotonicMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
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
class Profile {
public:
    // Only the writer thread uses the profile. Never destroyed, so a write after
    // Qt's own shutdown cannot meet a destroyed connection.
    static Profile &instance() {
        static auto *profile = new Profile;
        return *profile;
    }
    QSqlDatabase &database(const QString &root, const QString &database) {
        const auto path = database.isEmpty() ? AnnotationDatabase::path(root) : database;
        check(!profileBlocked, "storage-unavailable");
        if (db.isOpen() && path == path_)
            return db;
        close();
        open(root, path);
        return db;
    }
    void beginWrite() {
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
    void committed() {
        if (!wal)
            return;
        if (synchronous == QStringLiteral("NORMAL")) {
            profileUnsynced = true;
        } else {
            profileUnsynced = false;
        }
    }
    // Folds the log into the main file. `truncate` also empties the log file.
    bool checkpoint(bool truncate) {
        if (!db.isOpen() || !wal)
            return true;
        QElapsedTimer timer;
        timer.start();
        QSqlQuery q(db);
        const bool done = q.exec(truncate ? "PRAGMA wal_checkpoint(TRUNCATE)" : "PRAGMA wal_checkpoint(PASSIVE)") &&
                          q.next() && q.value(0).toInt() == 0 && q.value(1).toInt() == q.value(2).toInt();
        q.finish();
        if (done) {
            profileUnsynced = false;
            profileCheckpointFailed = -1;
        } else {
            profileCheckpointFailed = monotonicMilliseconds();
        }
        diagnostic("profile-checkpoint", {{"truncate", truncate}, {"ok", done}, {"elapsed_ms", double(timer.elapsed())}},
                   done ? DiagnosticSeverity::Activity : DiagnosticSeverity::Warning);
        return done;
    }
    void close() {
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
            journal = q.exec("PRAGMA journal_mode=DELETE") && q.next() &&
                      q.value(0).toString().compare("delete", Qt::CaseInsensitive) == 0;
            q.finish();
            journal = journal && folded;
        }
        db.close();
        release();
        // A close that leaves the log is kept in the durable log: the file then
        // needs its -wal next to it, for example on an exported partition.
        diagnostic("profile-close", {{"rollback_journal", journal}, {"elapsed_ms", double(timer.elapsed())}},
                   journal ? DiagnosticSeverity::Activity : DiagnosticSeverity::Error);
    }
    static thread_local bool routineWrite;

private:
    void release() {
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
    void open(const QString &root, const QString &path) {
        QElapsedTimer timer;
        timer.start();
        const auto directory = QFileInfo(path).absolutePath();
        check(!root.isEmpty() && QDir().mkpath(directory), "create-directory");
        QLockFile initialization(directory + "/.kobalt-db-init.lock");
        if (!QFileInfo::exists(path)) check(initialization.tryLock(1500), "database-busy");
        if (!QFileInfo::exists(path)) createDatabase(path, directory);
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
            check(scalar(db, "PRAGMA locking_mode=EXCLUSIVE").toString().compare("exclusive", Qt::CaseInsensitive) == 0,
                  "database-lock");
            // Read ownership before any persistent pragma or schema change. An
            // empty, foreign, future or damaged existing file is never initialized.
            check(scalar(db, "PRAGMA application_id").toInt() == applicationId, "unsupported-database-format");
            const int version = scalar(db, "PRAGMA user_version").toInt();
            check(version == 2 || version == schemaVersion, "unsupported-database-version");
            check(scalar(db, "PRAGMA encoding").toString() == QStringLiteral("UTF-8"), "unsupported-database-encoding");
            const qint64 pageSize = scalar(db, "PRAGMA page_size").toLongLong();
            check(pageSize >= 512 && pageSize <= 65536 && (pageSize & (pageSize - 1)) == 0, "invalid-database");
            const qint64 maxPages = maxDatabaseBytes / pageSize;
            // Keep the main file below FAT32's per-file ceiling. Journal size and
            // available space remain separate limits.
            check(scalar(db, QString("PRAGMA max_page_count=%1").arg(maxPages)).toLongLong() == maxPages,
                  "database-size-limit");
            const int tables = scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                "name IN ('books','lists','entries','imports','library_books','library_folders','book_state','profile_state')").toInt();
            check(tables == 8, "invalid-database");
            validateSchema(db, version == schemaVersion);
            if (version == 2) {
                // Upgrade before changing the journal mode. A rejected or failed
                // migration leaves the old schema and data intact.
                run(db, "PRAGMA synchronous=EXTRA");
                run(db, "PRAGMA fullfsync=ON");
                run(db, "BEGIN IMMEDIATE");
                const int lockedVersion = scalar(db, "PRAGMA user_version").toInt();
                check(lockedVersion == 2 || lockedVersion == schemaVersion, "unsupported-database-version");
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
            wal = scalar(db, "PRAGMA journal_mode=WAL").toString().compare("wal", Qt::CaseInsensitive) == 0;
            if (wal) {
                run(db, "PRAGMA synchronous=FULL");
                check(scalar(db, "PRAGMA synchronous").toInt() == 2, "database-synchronization");
                synchronous = QStringLiteral("FULL");
                // SQLite's default; it bounds the log at about 1000 pages.
                run(db, "PRAGMA wal_autocheckpoint=1000");
            } else {
                check(scalar(db, "PRAGMA journal_mode=DELETE").toString().compare("delete", Qt::CaseInsensitive) == 0,
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
        diagnostic("profile-open", {{"sqlite", scalar(db, "SELECT sqlite_version()").toString()},
                                    {"write_ahead", wal},
                                    {"elapsed_ms", double(timer.elapsed())}},
                   false);
    }
    QSqlDatabase db;
    QString name, path_, synchronous;
    bool wal = false;
};
thread_local bool Profile::routineWrite = false;
// An operation's view of the profile connection. It never closes it; a
// transaction left open by a failure is rolled back.
class Connection {
public:
    ~Connection() {
        if (transaction_) db.rollback();
    }
    void open(const QString &root, const QString &database) {
        db = Profile::instance().database(root, database);
    }
    void execute(const QString &statement) { run(db, statement); }
    void begin() {
        if (diagnosticsActive()) writeTrace = std::make_unique<DiagnosticOperation>("profile-transaction");
        Profile::instance().beginWrite();
        execute("BEGIN IMMEDIATE");
        transaction_ = writing_ = true;
        changes = scalar(db, "SELECT total_changes()").toLongLong();
    }
    void beginRead() { execute("BEGIN"); transaction_ = true; writing_ = false; }
    void commit() {
        check(db.commit(), "database-commit");
        transaction_ = false;
        // Many write transactions only read or find the record unchanged.
        if (writing_ && scalar(db, "SELECT total_changes()").toLongLong() != changes)
            Profile::instance().committed();
        writeTrace.reset();
    }
    QSqlDatabase db;

private:
    std::unique_ptr<DiagnosticOperation> writeTrace;
    bool transaction_ = false, writing_ = false;
    qint64 changes = 0;
};
void execute(QSqlQuery &q) { check(q.exec(), "database-write"); }
QString sqlString(const QString &text) { return text.isNull() ? QStringLiteral("") : text; }
QByteArray sqlUtf8(const QString &text) { return text.isEmpty() ? QByteArray("") : text.toUtf8(); }
QString listName(int kind) { return kind == highlightKind ? "highlights" : "bookmarks"; }
qint64 limit(int kind) { return kind == highlightKind ? HighlightStore::maxFileBytes : BookmarkStore::maxFileBytes; }
QByteArray document(int kind, const QJsonArray &rows) {
    return QJsonDocument(qt::object({{"version", 1}, {listName(kind), rows}})).toJson();
}
int documentBase(int kind) {
    static const auto base = [](int k) {
        const QJsonObject empty;
        return 2 * document(k, qt::array({empty})).size() - document(k, qt::array({empty, empty})).size();
    };
    static const int bookmarks = base(bookmarkKind), highlights = base(highlightKind);
    return kind == highlightKind ? highlights : bookmarks;
}
QJsonObject object(const Highlight &h, int kind) {
    if (kind == highlightKind) return highlightJson(h);
    return bookmarkJson({h.start, h.chapter, h.text, h.created});
}
int rowBytes(const Highlight &h, int kind) {
    return document(kind, qt::array({object(h, kind)})).size() - documentBase(kind);
}
qint64 totalBytes(const QHash<QString, int> &sizes, int kind) {
    if (sizes.isEmpty()) return document(kind, {}).size();
    qint64 result = documentBase(kind);
    for (auto it = sizes.begin(); it != sizes.end(); ++it) result += it.value();
    return result;
}
bool same(const Highlight &a, const Highlight &b) {
    return a.id == b.id && a.start == b.start && a.end == b.end && a.text == b.text && a.chapter == b.chapter &&
           a.detailsLoaded == b.detailsLoaded && a.bookmarkName == b.bookmarkName && a.bookmarkIcon == b.bookmarkIcon &&
           a.created == b.created && a.color == b.color && a.style == b.style && a.note == b.note &&
           a.chosenStart == b.chosenStart && a.chosenEnd == b.chosenEnd &&
           a.sentenceStartPolicy == b.sentenceStartPolicy && a.sentenceEndPolicy == b.sentenceEndPolicy;
}
void validate(const QVector<Highlight> &rows, int kind, bool allowCompact = false) {
    check(rows.size() <= 2000, "storage-limit");
    QSet<QString> ids;
    for (const auto &h : rows) {
        check(!ids.contains(h.id), "invalid-annotation");
        ids.insert(h.id);
        if (kind == highlightKind) {
            if (allowCompact && !h.detailsLoaded)
                check(validHighlightMetadata(h) && h.text.isEmpty() && h.chapter.isEmpty() && h.note.isEmpty(),
                      "invalid-highlight");
            else check(validHighlight(h), "invalid-highlight");
        }
        else check(!h.start.isEmpty() && h.start.size() <= 4096 && h.chapter.size() <= 512 &&
                   h.text.size() <= 512 && h.bookmarkName.size() <= 128 && h.bookmarkIcon.size() <= 64, "invalid-bookmark");
    }
}
QVector<Highlight> normalized(const QVector<Bookmark> &marks) {
    QVector<Highlight> rows;
    QHash<QString, int> occurrences;
    for (const auto &b : marks) {
        const auto key = QString::fromLatin1(QCryptographicHash::hash(
            b.position.toUtf8() + QByteArray(1, '\0') + b.created.toUTC().toString(Qt::ISODate).toUtf8(),
            QCryptographicHash::Sha256).toHex());
        Highlight h;
        h.id = key + '-' + QString::number(occurrences[key]++);
        h.start = b.position; h.chapter = b.chapter; h.text = b.excerpt; h.created = b.created;
        h.bookmarkName = b.name; h.bookmarkIcon = b.icon;
        rows.append(h);
    }
    return rows;
}
// Optional profile record, updated in the same transaction and revision as
// its bookmark list. Missing metadata means an ordinary bookmark.
QJsonObject readPins(Connection &c, const QString &key) {
    QSqlQuery q(c.db);
    check(q.prepare("SELECT format,length(CAST(payload AS BLOB)),CAST(payload AS BLOB) FROM profile_state WHERE name=?"), "invalid-database");
    q.addBindValue("bookmark-pins:" + key);
    check(q.exec(), "invalid-database");
    if (!q.next()) { check(!q.lastError().isValid(), "invalid-database"); return {}; }
    check(q.value(0).toInt() == 1 && q.value(1).toInt() <= 16384, "invalid-bookmark-pins");
    const auto document = QJsonDocument::fromJson(q.value(2).toByteArray());
    check(document.isObject(), "invalid-bookmark-pins");
    const auto pins = document.object();
    check(pins.size() <= BookmarkStore::maxPins, "invalid-bookmark-pins");
    for (auto it = pins.begin(); it != pins.end(); ++it) {
        const auto pin = it.value().toObject();
        check(it.value().isObject() && pin.value("name").isString() && pin.value("icon").isString() &&
              pin.value("name").toString().size() <= 128 && !pin.value("icon").toString().isEmpty() &&
              pin.value("icon").toString().size() <= 64, "invalid-bookmark-pins");
    }
    return pins;
}
void writePins(Connection &c, const QString &key, const QVector<Highlight> &rows) {
    const auto old = readPins(c, key);
    QJsonObject pins;
    for (const auto &row : rows)
        if (!row.bookmarkIcon.isEmpty())
            pins[row.id] = qt::object({{"name", row.bookmarkName}, {"icon", row.bookmarkIcon}});
    check(pins.size() <= BookmarkStore::maxPins, "bookmark-pin-limit");
    if (pins == old) return;
    const auto payload = QJsonDocument(pins).toJson(QJsonDocument::Compact);
    check(payload.size() <= 16384, "storage-limit");
    QSqlQuery q(c.db);
    check(q.prepare("INSERT OR REPLACE INTO profile_state(name,format,revision,payload) VALUES(?,1,0,CAST(? AS TEXT))"), "database-write");
    q.addBindValue("bookmark-pins:" + key); q.addBindValue(payload); execute(q);
}
void putBook(Connection &c, const AnnotationBook &book) {
    check(QJsonDocument(qt::object({{"version", 1}, {"identity", book.identity}, {"title", book.title},
                                   {"fingerprint", QString::fromLatin1(book.fingerprint)}})).toJson().size() <=
              BookmarkStore::maxFileBytes, "storage-limit");
    QSqlQuery q(c.db);
    check(q.prepare("INSERT OR IGNORE INTO books VALUES(CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT))"), "database-write");
    q.addBindValue(sqlUtf8(book.key)); q.addBindValue(sqlUtf8(book.identity)); q.addBindValue(sqlUtf8(QString::fromLatin1(book.fingerprint)));
    q.addBindValue(sqlUtf8(book.title)); execute(q);
    check(q.prepare("UPDATE books SET identity=CAST(? AS TEXT),fingerprint=CAST(? AS TEXT),title=CAST(? AS TEXT) WHERE book=?"), "database-write");
    q.addBindValue(sqlUtf8(book.identity)); q.addBindValue(sqlUtf8(QString::fromLatin1(book.fingerprint))); q.addBindValue(sqlUtf8(book.title));
    q.addBindValue(book.key); execute(q);
}
Highlight readHighlight(const QSqlQuery &q, bool compact = false) {
    Highlight h;
    h.id = utf8Text(q.value(0)); h.start = utf8Text(q.value(1)); h.end = utf8Text(q.value(2));
    h.text = utf8Text(q.value(3)); h.chapter = utf8Text(q.value(4));
    h.created = QDateTime::fromString(utf8Text(q.value(5)), Qt::ISODate).toUTC();
    h.color = utf8Text(q.value(6)); h.style = utf8Text(q.value(7)); h.note = utf8Text(q.value(8));
    h.chosenStart = utf8Text(q.value(9)); h.chosenEnd = utf8Text(q.value(10));
    const int startPolicy = q.value(11).toInt(), endPolicy = q.value(12).toInt();
    check((startPolicy == 0 || startPolicy == 1) && (endPolicy == 0 || endPolicy == 1), "invalid-database");
    h.sentenceStartPolicy = startPolicy; h.sentenceEndPolicy = endPolicy;
    check((startPolicy || h.chosenStart.isEmpty()) && (endPolicy || h.chosenEnd.isEmpty()), "invalid-database");
    h.detailsLoaded = !compact;
    return h;
}
QVector<Highlight> readRows(Connection &c, const QString &key, int kind, AnnotationDatabaseState *state,
                            bool compact = false) {
    QSqlQuery list(c.db);
    list.setForwardOnly(true);
    check(list.prepare("SELECT revision,row_count,bytes,CAST(source_key AS BLOB) FROM lists WHERE book=? AND kind=?"), "invalid-database");
    list.addBindValue(key); list.addBindValue(kind);
    check(list.exec() && list.next(), "invalid-database");
    AnnotationDatabaseState loaded;
    loaded.revision = list.value(0).toLongLong();
    const int expected = list.value(1).toInt();
    const qint64 bytes = list.value(2).toLongLong();
    loaded.adoptedFrom = utf8Text(list.value(3));
    check(loaded.revision >= 0 && expected >= 0 && expected <= 2000 && bytes >= 0 && bytes <= limit(kind), "invalid-database");
    list.finish();
    // Reject oversized fields inside SQLite before transferring their payloads
    // into QStrings. Actual UTF-16 and JSON byte limits are checked below.
    QSqlQuery bounds(c.db);
    bounds.setForwardOnly(true);
    check(bounds.prepare("SELECT count(*) FROM entries WHERE book=? AND kind=? AND ("
                         "length(CAST(id AS BLOB))>512 OR length(CAST(start AS BLOB))>16384 OR "
                         "length(CAST(end AS BLOB))>16384 OR length(CAST(text AS BLOB))>? OR "
                         "length(CAST(chapter AS BLOB))>2048 OR length(CAST(created AS BLOB))>128 OR "
                         "length(CAST(color AS BLOB))>256 OR length(CAST(style AS BLOB))>256 OR "
                         "length(CAST(note AS BLOB))>262144 OR length(CAST(chosen_start AS BLOB))>16384 OR "
                         "length(CAST(chosen_end AS BLOB))>16384)"), "invalid-database");
    bounds.addBindValue(key); bounds.addBindValue(kind);
    bounds.addBindValue(kind == highlightKind ? 4 * HighlightStore::maxTextUnits : 2048);
    check(bounds.exec() && bounds.next() && bounds.value(0).toInt() == 0, "storage-limit");
    bounds.finish();
    if (compact) {
        check(bounds.prepare("SELECT count(*) FROM entries WHERE book=? AND kind=1 AND ("
                             "typeof(text)<>'text' OR typeof(chapter)<>'text' OR typeof(note)<>'text' OR "
                             "typeof(id)<>'text' OR typeof(start)<>'text' OR typeof(end)<>'text' OR "
                             "typeof(created)<>'text' OR typeof(color)<>'text' OR typeof(style)<>'text' OR "
                             "typeof(chosen_start)<>'text' OR typeof(chosen_end)<>'text' OR "
                             "typeof(bytes)<>'integer' OR bytes<=0 OR bytes>8388608 OR "
                             "typeof(ordinal)<>'integer' OR ordinal<0 OR "
                             "typeof(start_policy)<>'integer' OR typeof(end_policy)<>'integer' OR "
                             "length(CAST(text AS BLOB))=0 OR length(CAST(text AS BLOB))+"
                             "length(CAST(chapter AS BLOB))+length(CAST(note AS BLOB))+length(CAST(id AS BLOB))+"
                             "length(CAST(start AS BLOB))+length(CAST(end AS BLOB))+length(CAST(created AS BLOB))+"
                             "length(CAST(color AS BLOB))+length(CAST(style AS BLOB))+length(CAST(chosen_start AS BLOB))+"
                             "length(CAST(chosen_end AS BLOB))>bytes)"), "invalid-database");
        bounds.addBindValue(key);
        check(bounds.exec() && bounds.next() && bounds.value(0).toInt() == 0, "invalid-database");
        bounds.finish();
    }
    check(bounds.prepare("SELECT count(*) FROM entries WHERE book=? AND kind=? AND ("
                         "(start_policy=0 AND chosen_start<>'') OR (end_policy=0 AND chosen_end<>''))"), "invalid-database");
    bounds.addBindValue(key); bounds.addBindValue(kind);
    check(bounds.exec() && bounds.next() && bounds.value(0).toInt() == 0, "invalid-database");
    bounds.finish();
    if (kind == bookmarkKind) {
        // Bookmark rows never carry highlight-only payloads. Do not let hidden
        // fields accumulate outside the bookmark quota during normalization.
        check(bounds.prepare("SELECT count(*) FROM entries WHERE book=? AND kind=0 AND ("
                             "end<>'' OR note<>'' OR chosen_start<>'' OR chosen_end<>'' OR "
                             "start_policy<>0 OR end_policy<>0 OR color<>'gray' OR style<>'shaded')"), "invalid-database");
        bounds.addBindValue(key);
        check(bounds.exec() && bounds.next() && bounds.value(0).toInt() == 0, "invalid-database");
        bounds.finish();
    }
    QSqlQuery q(c.db);
    q.setForwardOnly(true);
    const auto projection = compact
        ? QStringLiteral("CAST(id AS BLOB),CAST(start AS BLOB),CAST(end AS BLOB),X'',X'',CAST(created AS BLOB),"
                         "CAST(color AS BLOB),CAST(style AS BLOB),X'',CAST(chosen_start AS BLOB),CAST(chosen_end AS BLOB),"
                         "start_policy,end_policy,bytes,ordinal,typeof(ordinal)")
        : QStringLiteral("CAST(id AS BLOB),CAST(start AS BLOB),CAST(end AS BLOB),CAST(text AS BLOB),CAST(chapter AS BLOB),"
                         "CAST(created AS BLOB),CAST(color AS BLOB),CAST(style AS BLOB),CAST(note AS BLOB),"
                         "CAST(chosen_start AS BLOB),CAST(chosen_end AS BLOB),start_policy,end_policy,bytes,ordinal,typeof(ordinal)");
    check(q.prepare("SELECT " + projection + " FROM entries WHERE book=? AND kind=? ORDER BY ordinal"), "invalid-database");
    q.addBindValue(key); q.addBindValue(kind); check(q.exec(), "invalid-database");
    QVector<Highlight> rows;
    qint64 lastOrdinal = -1;
    qint64 actualBytes = documentBase(kind);
    while (q.next()) {
        check(rows.size() < 2000, "storage-limit");
        auto h = readHighlight(q, compact);
        const int size = q.value(13).toInt();
        const qint64 ordinal = q.value(14).toLongLong();
        check(size > 0 && size <= limit(kind) && q.value(15).toString() == QStringLiteral("integer") &&
              ordinal >= 0 && ordinal > lastOrdinal, "invalid-database");
        lastOrdinal = ordinal;
        loaded.ordinals.append(ordinal);
        if (kind == highlightKind)
            check(compact ? validHighlightMetadata(h) : validHighlight(h), "invalid-highlight");
        else
            check(!h.start.isEmpty() && h.start.size() <= 4096 && h.chapter.size() <= 512 && h.text.size() <= 512,
                  "invalid-bookmark");
        check(!loaded.rowBytes.contains(h.id) && (compact || rowBytes(h, kind) == size), "invalid-database");
        actualBytes += size;
        check(actualBytes <= limit(kind), "storage-limit");
        loaded.rowBytes.insert(h.id, size); rows.append(h);
    }
    check(!q.lastError().isValid(), "invalid-database");
    check(rows.size() == expected && loaded.rowBytes.size() == rows.size() && totalBytes(loaded.rowBytes, kind) == bytes,
          "invalid-database");
    if (kind == bookmarkKind) {
        const auto pins = readPins(c, key);
        for (auto &row : rows) {
            const auto pin = pins.value(row.id).toObject();
            row.bookmarkName = pin.value("name").toString();
            row.bookmarkIcon = pin.value("icon").toString();
        }
    }
    *state = loaded;
    return rows;
}
void insertRow(QSqlQuery &q, const AnnotationBook &book, int kind, const Highlight &h, qint64 ordinal, int bytes) {
    const QVariantList values{sqlUtf8(book.key), kind, sqlUtf8(h.id), ordinal, sqlUtf8(h.start), sqlUtf8(h.end), sqlUtf8(h.text), sqlUtf8(h.chapter),
        sqlUtf8(h.created.toUTC().toString(Qt::ISODate)), sqlUtf8(h.color), sqlUtf8(h.style), sqlUtf8(h.note),
        sqlUtf8(h.sentenceStartPolicy ? h.chosenStart : QString()), sqlUtf8(h.sentenceEndPolicy ? h.chosenEnd : QString()),
        h.sentenceStartPolicy ? 1 : 0, h.sentenceEndPolicy ? 1 : 0, bytes};
    for (int i = 0; i < values.size(); ++i) q.bindValue(i, values[i]);
    execute(q);
}
void prepareInsert(QSqlQuery &q) {
    // Bind UTF-8 bytes then cast to TEXT. QString binding uses SQLite's UTF-16
    // entry point, which consumes an initial FEFF as a byte-order marker.
    check(q.prepare("INSERT OR REPLACE INTO entries VALUES(CAST(? AS TEXT),?,CAST(? AS TEXT),?,"
                    "CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),"
                    "CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS TEXT),?,?,?)"), "database-write");
}
void revision(Connection &c, const QString &key, int kind, qint64 expected) {
    QSqlQuery q(c.db);
    check(q.prepare("SELECT revision FROM lists WHERE book=? AND kind=?"), "invalid-database");
    q.addBindValue(key); q.addBindValue(kind);
    check(q.exec() && q.next() && expected >= 0 && q.value(0).toLongLong() == expected, "stale-annotations");
}
void prepareDetail(QSqlQuery &q) {
    // Bounds stay inside SQL so malformed payloads cannot allocate unbounded
    // QStrings before full UTF-16 and canonical byte validation below.
    check(q.prepare("SELECT CAST(id AS BLOB),CAST(start AS BLOB),CAST(end AS BLOB),CAST(text AS BLOB),CAST(chapter AS BLOB),"
                    "CAST(created AS BLOB),CAST(color AS BLOB),CAST(style AS BLOB),CAST(note AS BLOB),"
                    "CAST(chosen_start AS BLOB),CAST(chosen_end AS BLOB),start_policy,end_policy,bytes "
                    "FROM entries WHERE book=? AND kind=1 AND id=CAST(? AS TEXT) AND "
                    "length(CAST(id AS BLOB))<=512 AND length(CAST(start AS BLOB))<=16384 AND "
                    "length(CAST(end AS BLOB))<=16384 AND length(CAST(text AS BLOB)) BETWEEN 1 AND 1048576 AND "
                    "length(CAST(chapter AS BLOB))<=2048 AND length(CAST(created AS BLOB))<=128 AND "
                    "length(CAST(color AS BLOB))<=256 AND length(CAST(style AS BLOB))<=256 AND "
                    "length(CAST(note AS BLOB))<=262144 AND length(CAST(chosen_start AS BLOB))<=16384 AND "
                    "length(CAST(chosen_end AS BLOB))<=16384 AND typeof(text)='text' AND typeof(chapter)='text' AND "
                    "typeof(note)='text'"), "invalid-database");
}
Highlight readDetail(QSqlQuery &q, const QString &key, const QString &id, int expectedBytes) {
    check(expectedBytes > 0 && expectedBytes <= HighlightStore::maxFileBytes, "missing-highlight");
    q.bindValue(0, key); q.bindValue(1, sqlUtf8(id));
    check(q.exec() && q.next(), "invalid-highlight");
    const auto h = readHighlight(q);
    check(validHighlight(h) && q.value(13).toInt() == expectedBytes && rowBytes(h, highlightKind) == expectedBytes,
          "invalid-highlight");
    q.finish();
    return h;
}
bool sameRange(const Highlight &a, const Highlight &b) {
    return a.start == b.start && a.end == b.end && a.chosenStart == b.chosenStart && a.chosenEnd == b.chosenEnd &&
           a.sentenceStartPolicy == b.sentenceStartPolicy && a.sentenceEndPolicy == b.sentenceEndPolicy;
}
using LegacyReader = std::function<QVector<Highlight>(const QString &, int)>;
struct Source { QString key, folder; qint64 updated = 0; bool database = false; };
Source source(Connection &c, const AnnotationBook &book, int kind) {
    const QString own = book.root + '/' + book.key;
    const QFileInfo ownFile(own + '/' + listName(kind) + ".json");
    if (ownFile.exists()) return {book.key, own, ownFile.lastModified().toMSecsSinceEpoch(), false};
    if (!book.identity.startsWith("file://")) return {};
    Source best;
    QSet<QString> migrated;
    QSqlQuery q(c.db);
    q.setForwardOnly(true);
    check(q.prepare("SELECT CAST(lists.book AS BLOB),lists.updated,CAST(books.identity AS BLOB) FROM lists JOIN books ON books.book=lists.book "
                    "WHERE lists.kind=? AND lists.book<>? ORDER BY lists.updated DESC,lists.book"),
          "invalid-database");
    q.addBindValue(kind); q.addBindValue(book.key);
    check(q.exec(), "invalid-database");
    while (q.next()) {
        const auto key = utf8Text(q.value(0));
        migrated.insert(key);
        if (best.key.isEmpty() && utf8Text(q.value(2)) == book.identity)
            best = {key, {}, q.value(1).toLongLong(), true};
    }
    check(!q.lastError().isValid(), "invalid-database");
    const auto folders = QDir(book.root).entryList({"file-*"}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (int i = 0; i < folders.size() && i < 10000; ++i) {
        if (folders[i] == book.key || migrated.contains(folders[i])) continue;
        const QString folder = book.root + '/' + folders[i];
        const QFileInfo file(folder + '/' + listName(kind) + ".json");
        if (!file.exists()) continue;
        const auto metadata = storage::readJsonFile(folder + "/book.json", BookmarkStore::maxFileBytes);
        if (metadata.status != storage::ReadStatus::Ok || metadata.object.value("identity").toString() != book.identity) continue;
        const qint64 modified = file.lastModified().toMSecsSinceEpoch();
        if (best.key.isEmpty() || modified > best.updated) best = {folders[i], folder, modified, false};
    }
    return best;
}
void metadata(const QString &folder) {
    const auto read = storage::readJsonFile(folder + "/book.json", BookmarkStore::maxFileBytes);
    if (read.status == storage::ReadStatus::Missing) return;
    check(read.status == storage::ReadStatus::Ok, "invalid-metadata");
    check(read.object.value("version") == QJsonValue(1), "unsupported-format");
    check(read.object.value("identity").isString() && read.object.value("title").isString() &&
          read.object.value("fingerprint").isString(), "invalid-metadata");
}
void ensureBook(Connection &c, const AnnotationBook &book, const LegacyReader &reader) {
    QSqlQuery q(c.db);
    q.setForwardOnly(true);
    check(q.prepare("SELECT count(*) FROM lists WHERE book=?"), "invalid-database");
    q.addBindValue(book.key); check(q.exec() && q.next(), "invalid-database");
    const int existing = q.value(0).toInt(); q.finish();
    if (existing == 2) return;
    check(existing == 0, "invalid-database");
    c.begin();
    // Another operation may have completed migration while this one waited.
    check(q.exec() && q.next(), "invalid-database");
    const int afterLock = q.value(0).toInt(); q.finish();
    if (afterLock == 2) { c.commit(); return; }
    check(afterLock == 0, "invalid-database");
    metadata(book.root + '/' + book.key);
    putBook(c, book);
    for (int kind : {bookmarkKind, highlightKind}) {
        const Source selected = source(c, book, kind);
        QVector<Highlight> rows;
        AnnotationDatabaseState state;
        if (selected.database) rows = readRows(c, selected.key, kind, &state);
        else if (!selected.folder.isEmpty()) {
            metadata(selected.folder);
            rows = reader(selected.folder, kind);
        }
        validate(rows, kind);
        state.rowBytes.clear();
        for (const auto &h : rows) state.rowBytes.insert(h.id, rowBytes(h, kind));
        const qint64 bytes = totalBytes(state.rowBytes, kind);
        check(bytes <= limit(kind), "storage-limit");
        QSqlQuery list(c.db);
        check(list.prepare("INSERT INTO lists VALUES(?,?,0,?,?,?,?)"), "database-write");
        list.addBindValue(book.key); list.addBindValue(kind);
        // Eager profile imports must keep the original source chronology.
        // Otherwise directory traversal order could select an older edition's
        // annotations when a changed sideloaded book adopts its saved data.
        list.addBindValue(!selected.database && selected.updated > 0 ? selected.updated : QDateTime::currentMSecsSinceEpoch());
        list.addBindValue(sqlString(selected.key == book.key ? QString() : selected.key));
        list.addBindValue(rows.size()); list.addBindValue(bytes); execute(list);
        QSqlQuery insert(c.db); prepareInsert(insert);
        for (int i = 0; i < rows.size(); ++i) insertRow(insert, book, kind, rows[i], i, state.rowBytes.value(rows[i].id));
        if (kind == bookmarkKind) writePins(c, book.key, rows);
    }
    c.commit();
}
void saveRows(const AnnotationBook &book, int kind, const QVector<Highlight> &rows, const QVector<Highlight> &previous,
              AnnotationDatabaseState *state) {
    validate(rows, kind, true);
    QHash<QString, int> oldIndices;
    for (int i = 0; i < previous.size(); ++i) oldIndices.insert(previous[i].id, i);
    AnnotationDatabaseState next = *state;
    next.rowBytes.clear();
    check(state->ordinals.size() == previous.size(), "invalid-database");
    next.ordinals.clear();
    qint64 lastOrdinal = -1;
    bool appended = false, reordered = false;
    for (const auto &h : rows) {
        const int old = oldIndices.value(h.id, -1);
        if (old >= 0) {
            const auto ordinal = state->ordinals[old];
            if (appended || ordinal <= lastOrdinal) { reordered = true; break; }
            next.ordinals.append(ordinal);
            lastOrdinal = ordinal;
        } else {
            if (lastOrdinal == std::numeric_limits<qint64>::max()) { reordered = true; break; }
            appended = true;
            next.ordinals.append(++lastOrdinal);
        }
    }
    // Deleting preserves surviving values; appending uses the next value.
    // Reordering, insertion before an existing row, or exhausted integer space
    // requires a new sequence. Only rows whose values change are updated.
    if (reordered) {
        next.ordinals.clear();
        for (int i = 0; i < rows.size(); ++i) next.ordinals.append(i);
    }
    QVector<int> changed, moved;
    QHash<int, Highlight> changedRows;
    Connection c; c.open(book.root, book.database); c.begin();
    revision(c, book.key, kind, state->revision);
    QSqlQuery detail(c.db);
    if (kind == highlightKind) prepareDetail(detail);
    for (int i = 0; i < rows.size(); ++i) {
        const auto &h = rows[i];
        const int old = oldIndices.value(h.id, -1);
        if (old >= 0 && same(h, previous[old])) {
            check(state->rowBytes.contains(h.id), "invalid-database");
            next.rowBytes.insert(h.id, state->rowBytes.value(h.id));
            if (state->ordinals[old] != next.ordinals[i]) moved.append(i);
        } else {
            auto full = h;
            if (!h.detailsLoaded) {
                check(kind == highlightKind && old >= 0 && sameRange(h, previous[old]), "details-required");
                const auto stored = readDetail(detail, book.key, h.id, state->rowBytes.value(h.id));
                full.text = stored.text; full.chapter = stored.chapter; full.note = stored.note;
                full.detailsLoaded = true;
            }
            validate({full}, kind);
            next.rowBytes.insert(h.id, rowBytes(full, kind));
            changedRows.insert(i, full);
            changed.append(i);
        }
    }
    const qint64 bytes = totalBytes(next.rowBytes, kind);
    check(bytes <= limit(kind), "storage-limit");
    QSqlQuery update(c.db);
    check(update.prepare("UPDATE lists SET revision=revision+1,updated=?,source_key='',row_count=?,bytes=? "
                         "WHERE book=? AND kind=? AND revision=?"), "database-write");
    update.addBindValue(QDateTime::currentMSecsSinceEpoch()); update.addBindValue(rows.size()); update.addBindValue(bytes);
    update.addBindValue(book.key); update.addBindValue(kind); update.addBindValue(state->revision);
    execute(update); check(update.numRowsAffected() == 1, "stale-annotations");
    putBook(c, book);
    QSqlQuery remove(c.db);
    check(remove.prepare("DELETE FROM entries WHERE book=? AND kind=? AND id=CAST(? AS TEXT)"), "database-write");
    for (const auto &h : previous) if (!next.rowBytes.contains(h.id)) {
        remove.bindValue(0, book.key); remove.bindValue(1, kind); remove.bindValue(2, sqlUtf8(h.id)); execute(remove);
    }
    QSqlQuery insert(c.db); prepareInsert(insert);
    for (int i : changed) insertRow(insert, book, kind, changedRows.value(i), next.ordinals[i], next.rowBytes.value(rows[i].id));
    QSqlQuery reorder(c.db);
    check(reorder.prepare("UPDATE entries SET ordinal=? WHERE book=? AND kind=? AND id=CAST(? AS TEXT)"), "database-write");
    for (int i : moved) {
        reorder.bindValue(0, next.ordinals[i]); reorder.bindValue(1, book.key); reorder.bindValue(2, kind);
        reorder.bindValue(3, sqlUtf8(rows[i].id)); execute(reorder);
    }
    if (kind == bookmarkKind) writePins(c, book.key, rows);
    c.commit();
    next.revision++; next.adoptedFrom.clear(); *state = next;
}
// Every operation runs on the writer thread, which owns the profile connection.
// It keeps the caller's choice of routine writes.
template<class Function> bool guarded(QString *error, Function operation) {
    bool ok = false;
    const bool routine = Profile::routineWrite;
    storage::runOnWriterThread([&] {
        struct Restore {
            bool previous;
            ~Restore() { Profile::routineWrite = previous; }
        } restore{std::exchange(Profile::routineWrite, routine)};
        try { operation(); if (error) error->clear(); ok = true; }
        catch (const Failure &failure) { if (error) *error = failure.code; }
    });
    return ok;
}
}

bool annotationDatabaseEnabled() {
    const auto option = qgetenv("KOBALT_ANNOTATIONS_SQLITE");
    if (option == "0") return false;
    if (option == "1") return true;
#ifdef KOBALT_SQLITE_ANNOTATIONS_DEFAULT
    return KOBALT_SQLITE_ANNOTATIONS_DEFAULT != 0;
#else
    return true;
#endif
}
QString AnnotationDatabase::path(const QString &root) { return storage::Paths{root}.database(); }
bool AnnotationDatabase::legacy(const QString &folder, QVector<Highlight> *rows, QString *error) {
    HighlightStore store({}, "migration", {}, {});
    const bool ok = store.readFolder(folder, error);
    if (ok) *rows = store.highlights();
    return ok;
}
bool AnnotationDatabase::legacy(const QString &folder, QVector<Bookmark> *rows, QString *error) {
    const auto read = storage::readJsonFile(folder + "/bookmarks.json", BookmarkStore::maxFileBytes);
    if (read.status == storage::ReadStatus::Missing) { rows->clear(); return true; }
    if (read.status != storage::ReadStatus::Ok || read.object.value("version") != QJsonValue(1) ||
        !read.object.value("bookmarks").isArray()) {
        if (error) *error = "invalid-or-unsupported-bookmarks";
        return false;
    }
    BookmarkStore store({}, "migration", {}, {});
    if (!store.readFolder(folder, error)) return false;
    // Verify the complete list before publishing an import to SQLite.
    if (store.bookmarks().size() != read.object.value("bookmarks").toArray().size()) {
        if (error) *error = "invalid-bookmark";
        return false;
    }
    *rows = store.bookmarks(); return true;
}
bool AnnotationDatabase::loadRows(const AnnotationBook &book, int requestedKind, QVector<Highlight> *rows,
                                  AnnotationDatabaseState *state, QString *error, bool compact) {
    return guarded(error, [&] {
        Connection c; c.open(book.root, book.database);
        ensureBook(c, book, [&](const QString &folder, int kind) {
            QString problem;
            if (kind == highlightKind) {
                QVector<Highlight> loaded;
                if (!legacy(folder, &loaded, &problem)) throw Failure{problem};
                return loaded;
            }
            QVector<Bookmark> loaded;
            if (!legacy(folder, &loaded, &problem)) throw Failure{problem};
            return normalized(loaded);
        });
        AnnotationDatabaseState loaded;
        c.beginRead();
        auto result = readRows(c, book.key, requestedKind, &loaded, compact);
        c.commit();
        *rows = std::move(result); *state = loaded;
    });
}
bool AnnotationDatabase::load(const AnnotationBook &book, QVector<Highlight> *rows, AnnotationDatabaseState *state,
                              QString *error) {
    return loadRows(book, highlightKind, rows, state, error);
}
bool AnnotationDatabase::loadCompact(const AnnotationBook &book, QVector<Highlight> *rows, AnnotationDatabaseState *state,
                                     QString *error) {
    return loadRows(book, highlightKind, rows, state, error, true);
}
bool AnnotationDatabase::details(const AnnotationBook &book, const QStringList &ids, const AnnotationDatabaseState &state,
                                 QVector<Highlight> *rows, QString *error) {
    return guarded(error, [&] {
        check(ids.size() <= HighlightStore::maxHighlights, "storage-limit");
        Connection c; c.open(book.root, book.database); c.beginRead();
        revision(c, book.key, highlightKind, state.revision);
        QSqlQuery query(c.db); prepareDetail(query);
        QVector<Highlight> result;
        QSet<QString> seen;
        for (const auto &id : ids) {
            check(!seen.contains(id), "duplicate-highlight"); seen.insert(id);
            result.append(readDetail(query, book.key, id, state.rowBytes.value(id)));
        }
        c.commit(); *rows = std::move(result);
    });
}
bool AnnotationDatabase::excerpts(const AnnotationBook &book, const QStringList &ids, const AnnotationDatabaseState &state,
                                  QVector<HighlightExcerpt> *rows, QString *error) {
    return guarded(error, [&] {
        check(ids.size() <= HighlightStore::maxHighlights, "storage-limit");
        QSet<QString> seen;
        for (const auto &id : ids) {
            check(!seen.contains(id), "duplicate-highlight"); seen.insert(id);
            const int size = state.rowBytes.value(id);
            check(size > 0 && size <= HighlightStore::maxFileBytes, "missing-highlight");
        }
        Connection c; c.open(book.root, book.database); c.beginRead();
        revision(c, book.key, highlightKind, state.revision);
        QHash<QString, HighlightExcerpt> fetched;
        // Stay below older SQLite providers' bound-variable limit while doing
        // one projection query for an ordinary visible page of excerpts.
        for (int offset = 0; offset < ids.size(); offset += 128) {
            const int count = qMin(128, int(ids.size()) - offset);
            QStringList placeholders;
            for (int i = 0; i < count; ++i) placeholders.append("CAST(? AS TEXT)");
            QSqlQuery query(c.db);
            query.setForwardOnly(true);
            check(query.prepare("SELECT CAST(id AS BLOB),CAST(text AS BLOB),CAST(chapter AS BLOB),bytes,"
                                "length(CAST(text AS BLOB))+length(CAST(chapter AS BLOB)) "
                                "FROM entries WHERE book=? AND kind=1 AND id IN (" + placeholders.join(',') + ") AND "
                                "typeof(id)='text' AND length(CAST(id AS BLOB))<=512 AND "
                                "typeof(text)='text' AND typeof(chapter)='text' AND "
                                "length(CAST(text AS BLOB)) BETWEEN 1 AND 1048576 AND length(CAST(chapter AS BLOB))<=2048 AND "
                                "length(CAST(text AS BLOB))+length(CAST(chapter AS BLOB))<=bytes"), "invalid-database");
            query.addBindValue(book.key);
            for (int i = 0; i < count; ++i) query.addBindValue(sqlUtf8(ids[offset + i]));
            check(query.exec(), "invalid-highlight");
            int received = 0;
            while (query.next()) {
                const auto id = utf8Text(query.value(0));
                const int size = state.rowBytes.value(id);
                check(seen.contains(id) && !fetched.contains(id) && size > 0 && query.value(3).toLongLong() == size &&
                      query.value(4).toLongLong() <= size, "invalid-highlight");
                HighlightExcerpt row{id, utf8Text(query.value(1)), utf8Text(query.value(2))};
                check(!row.text.isEmpty() && row.text.size() <= HighlightStore::maxTextUnits && row.chapter.size() <= 512,
                      "invalid-highlight");
                fetched.insert(id, row); ++received;
            }
            check(!query.lastError().isValid() && received == count, "invalid-highlight");
        }
        QVector<HighlightExcerpt> result;
        for (const auto &id : ids) result.append(fetched.value(id));
        c.commit(); *rows = std::move(result);
    });
}
bool AnnotationDatabase::transaction(const QString &directory,
                                   const std::function<bool(QSqlDatabase &)> &operation, QString *error) {
    return guarded(error, [&] {
        Connection connection;
        connection.open(directory, storage::Paths{directory}.database());
        connection.begin();
        check(operation(connection.db), "profile-data");
        connection.commit();
    });
}
bool AnnotationDatabase::load(const AnnotationBook &book, QVector<Bookmark> *rows, AnnotationDatabaseState *state,
                              QString *error) {
    return guarded(error, [&] {
        // Both kinds share exactly the same migration gate.
        QVector<Highlight> result;
        AnnotationDatabaseState loaded;
        QString problem;
        if (!loadRows(book, bookmarkKind, &result, &loaded, &problem)) throw Failure{problem};
        QVector<Bookmark> bookmarks;
        for (const auto &h : result) bookmarks.append({h.start, h.chapter, h.text, h.created, h.bookmarkName, h.bookmarkIcon});
        *rows = std::move(bookmarks); *state = loaded;
    });
}
bool AnnotationDatabase::save(const AnnotationBook &book, const QVector<Highlight> &rows,
                              const QVector<Highlight> &previous, AnnotationDatabaseState *state, QString *error) {
    return guarded(error, [&] { saveRows(book, highlightKind, rows, previous, state); });
}
bool AnnotationDatabase::save(const AnnotationBook &book, const QVector<Bookmark> &rows,
                              const QVector<Bookmark> &previous, AnnotationDatabaseState *state, QString *error) {
    return guarded(error, [&] { saveRows(book, bookmarkKind, normalized(rows), normalized(previous), state); });
}
namespace storage {
void closeProfile(bool keepClosed) {
    drainWriterThread();
    runOnWriterThread([keepClosed] {
        Profile::instance().close();
        if (keepClosed)
            profileBlocked = true;
    });
}
void reopenProfile() {
    profileBlocked = false;
}
ProfileDatabaseState inspectProfileDatabase(const QString &directory, QString *error) {
    const auto path = Paths{directory}.database();
    if (error)
        error->clear();
    if (!QFileInfo(path).exists() && !QFileInfo(path).isSymLink())
        return ProfileDatabaseState::Missing;
    QString code;
    if (guarded(&code, [&] {
            Connection connection;
            connection.open(directory, path);
        }))
        return ProfileDatabaseState::Ready;
    if (error)
        *error = code;
    // Our application ID with a version this build does not know.
    if (code == QLatin1String("unsupported-database-version"))
        return ProfileDatabaseState::Incompatible;
    // A foreign, empty or damaged file, or one with another text encoding.
    if (code == QLatin1String("invalid-database") || code == QLatin1String("unsupported-database-format") ||
        code == QLatin1String("unsupported-database-encoding"))
        return ProfileDatabaseState::Corrupt;
    // Another process holds the file, or storage is exported. The file may be
    // fine; leave it to the normal opening.
    if (code == QLatin1String("database-busy") || code == QLatin1String("database-lock") ||
        code == QLatin1String("storage-unavailable"))
        return ProfileDatabaseState::InUse;
    return ProfileDatabaseState::Unavailable;
}
void checkpointProfile() {
    runOnWriterThread([] {
        Profile::instance().checkpoint(false);
    });
}
bool profileHasUnsyncedWrites() {
    return profileUnsynced;
}
bool profileCheckpointDue() {
    const qint64 failed = profileCheckpointFailed;
    return profileUnsynced && (failed < 0 || monotonicMilliseconds() - failed >= checkpointRetry);
}
void requestProfileCheckpoint() {
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
bool profileCheckpointPending() {
    return profileCheckpointRunning;
}
RoutineWrites::RoutineWrites() : previous(Profile::routineWrite) {
    Profile::routineWrite = true;
}
RoutineWrites::~RoutineWrites() {
    Profile::routineWrite = previous;
}
}
}
