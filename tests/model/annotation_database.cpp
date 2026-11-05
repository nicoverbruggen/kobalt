#include <mutex>
#include <vector>
#include "storage/annotation_database.h"
#include "storage/profile_database.h"
#include "storage/bookmark_store.h"
#include "storage/highlight_store.h"
#include "storage/reader_storage.h"
#include "support/qt_compat.h"
#include "check.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>
#include <atomic>
#include <cstdlib>
#include <thread>

using namespace kobalt;

namespace {
const QByteArray fingerprint(64, 'a');
const QString identity = QStringLiteral("file:///books/legacy.epub");

QString folder(const QString &root, const QByteArray &fp = fingerprint)
{
    return root + '/' + BookmarkStore::bookKey(identity, fp);
}

QByteArray bytes(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read fixture");
    return file.readAll();
}

void write(const QString &path, const QByteArray &data)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                    && file.write(data) == data.size(),
            "Cannot write fixture");
}

// Kobalt holds the profile file exclusively while it is open. Close it, as
// USB export does, before probing the file with another connection.
QVariant sql(const QString &root, const QString &statement)
{
    storage::closeProfile();
    QVariant result;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "annotation-test-probe");
        db.setDatabaseName(AnnotationDatabase::path(root));
        require(db.open(), "Cannot open fixture database");
        {
            QSqlQuery q(db);
            require(q.exec(statement), "Cannot execute fixture SQL");
            if (q.next())
                result = q.value(0);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("annotation-test-probe");
    return result;
}

Highlight highlight()
{
    Highlight h{ "stable-id",
                 "/body/p[1]/text().0",
                 "/body/p[1]/text().40",
                 QString::fromUtf8("Café 日本語 😀"),
                 "Chapter",
                 QDateTime::fromString("2026-09-20T10:00:00Z", Qt::ISODate).toUTC() };
    h.note = QString::fromUtf8("First line\nSecond line: Ελληνικά 😀");
    h.color = "futurecolor";
    h.style = "futurestyle";
    h.sentenceStartPolicy = true;
    h.chosenStart = "/body/p[1]/text().1";
    h.sentenceEndPolicy = true;
    h.chosenEnd = "/body/p[1]/text().39";
    return h;
}

Bookmark bookmark()
{
    return { "/body/p[2]/text().0", "Chapter", QString::fromUtf8("Legacy excerpt: café 日本語"),
             QDateTime::fromString("2026-09-19T11:00:00Z", Qt::ISODate).toUTC() };
}

void legacyCurrent(const QString &root)
{
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    BookmarkStore marks(root, identity, fingerprint, "Book");
    marks.setBookmarks({ bookmark() });
    require(marks.save(), "Cannot seed current bookmarks");
    HighlightStore highlights(root, identity, fingerprint, "Book");
    highlights.setHighlights({ highlight() });
    require(highlights.save(), "Cannot seed current highlights");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
}

void pinnedBookmarks()
{
    QTemporaryDir root;
    auto pinned = bookmark();
    pinned.name = QString::fromUtf8("Carte 日本語");
    pinned.icon = "map";
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    BookmarkStore legacy(root.path(), identity, fingerprint, "Book");
    legacy.setBookmarks({ pinned });
    require(legacy.save(), "Cannot save legacy pin");
    BookmarkStore legacyAgain(root.path(), identity, fingerprint, "Book");
    require(legacyAgain.load() && legacyAgain.bookmarks().first() == pinned,
            "Legacy pin did not round trip");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    BookmarkStore store(root.path(), identity, fingerprint, "Book");
    require(store.load() && store.bookmarks().first() == pinned, "Import lost pin metadata");
    auto second = pinned;
    second.position = "another anchor";
    second.name = "Second";
    second.icon = "future-icon";
    store.setBookmarks({ pinned, second });
    require(store.save(), "Cannot add second pin");
    require(!store.merge([](const QString &) { return 1; },
                         [](int, const QDateTime &) { return bookmark(); }),
            "Reflow merged pinned anchors");
    auto ordinaryA = bookmark();
    ordinaryA.position = "ordinary A";
    auto ordinaryB = bookmark();
    ordinaryB.position = "ordinary B";
    BookmarkStore merging = store;
    merging.setBookmarks({ pinned, ordinaryA, ordinaryB });
    require(merging.merge([](const QString &) { return 1; },
                          [](int, const QDateTime &) { return bookmark(); })
                    && merging.bookmarks() == QVector<Bookmark>({ pinned }),
            "Ordinary merge duplicated a pinned anchor");
    BookmarkStore again(root.path(), identity, fingerprint, "Book");
    require(again.load() && again.bookmarks() == QVector<Bookmark>({ pinned, second }),
            "Pin metadata changed on reload");
    sql(root.path(),
        "CREATE TRIGGER deny_pin BEFORE INSERT ON profile_state WHEN NEW.name LIKE 'bookmark-pins:%' BEGIN SELECT RAISE(ABORT,'injected'); END");
    auto edited = pinned;
    edited.name = "Changed";
    store.setBookmarks({ edited });
    require(!store.save(), "Pin write unexpectedly succeeded");
    require(again.load() && again.bookmarks() == QVector<Bookmark>({ pinned, second }),
            "Failed pin write changed bookmarks");
    sql(root.path(), "DROP TRIGGER deny_pin");
    require(store.save(), "Pin retry failed");
    require(again.load() && again.bookmarks() == QVector<Bookmark>({ edited }),
            "Pin retry lost metadata");
    BookmarkStore stale = again;
    edited.icon.clear();
    edited.name.clear();
    store.setBookmarks({ edited });
    require(store.save(), "Unpin failed");
    stale.setBookmarks({ pinned });
    require(!stale.save(), "Stale pin writer replaced newer data");
    require(again.load() && !again.bookmarks().first().pinned(), "Unpin did not persist");
    QVector<Bookmark> many;
    for (int i = 0; i < 9; ++i) {
        auto b = pinned;
        b.position = QString::number(i);
        many.append(b);
    }
    store.setBookmarks(many);
    require(!store.save(), "Nine pins accepted");
    require(again.load() && !again.bookmarks().first().pinned(),
            "Rejected pins replaced saved bookmarks");
    sql(root.path(), "UPDATE profile_state SET format=99 WHERE name LIKE 'bookmark-pins:%'");
    require(!again.load(), "Unsupported pin format accepted");
    again.setBookmarks({ pinned });
    require(!again.save(), "Unsupported pin format overwritten");
    storage::closeProfile();
}

void verifyHighlight(const Highlight &a, const Highlight &b)
{
    require(highlightJson(a) == highlightJson(b), "Highlight field, Unicode or provenance changed");
}

void verifyQuota(const QString &root, int kind, const QJsonArray &rows)
{
    const QString name = kind ? "highlights" : "bookmarks";
    const auto expected =
            QJsonDocument(qt::object({ { "version", 1 }, { name, rows } })).toJson().size();
    require(sql(root, QString("SELECT bytes FROM lists WHERE kind=%1").arg(kind)).toLongLong()
                    == expected,
            "Database quota differs from production JSON byte count");
}

void beta3()
{
    QTemporaryDir root;
    require(root.isValid(), "No temporary folder");
    // v1.0-beta.3: BookmarkStore::save writes these fields and version 1.
    // That tagged tree contains no HighlightStore or highlights.json writer.
    const QByteArray marks =
            R"({"version":1,"bookmarks":[{"position":"/body/p[2]/text().0","chapter":"Chapter","excerpt":"Legacy excerpt: café 日本語","created":"2026-09-19T11:00:00Z"}]})";
    const auto meta =
            QJsonDocument(qt::object({ { "version", 1 },
                                       { "identity", identity },
                                       { "title", "Book" },
                                       { "fingerprint", QString::fromLatin1(fingerprint) } }))
                    .toJson();
    write(folder(root.path()) + "/book.json", meta);
    write(folder(root.path()) + "/bookmarks.json", marks);
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    BookmarkStore store(root.path(), identity, fingerprint, "Book");
    require(store.load() && store.bookmarks() == QVector<Bookmark>{ bookmark() },
            "Beta 3 bookmarks did not migrate");
    HighlightStore empty(root.path(), identity, fingerprint, "Book");
    require(empty.load() && empty.highlights().isEmpty(),
            "Absent Beta 3 highlights were not empty");
    empty.setHighlights({ highlight() });
    require(empty.save(), "Cannot add first SQLite highlight");
    HighlightStore reopened(root.path(), identity, fingerprint, "Book");
    require(reopened.load() && reopened.highlights().size() == 1,
            "SQLite highlight did not reopen");
    verifyHighlight(reopened.highlights().first(), highlight());
    require(bytes(folder(root.path()) + "/bookmarks.json") == marks
                    && bytes(folder(root.path()) + "/book.json") == meta
                    && !QFile::exists(folder(root.path()) + "/highlights.json"),
            "Migration modified legacy files or dual-wrote JSON");
    verifyQuota(root.path(), 1, qt::array({ highlightJson(highlight()) }));
    verifyQuota(root.path(), 0, qt::array({ bookmarkJson(bookmark()) }));
    // The successful migration marker is authoritative even if an old build
    // later changes its stale JSON. No automatic merge can safely infer intent.
    write(folder(root.path()) + "/bookmarks.json", "{\"version\":1,\"bookmarks\":[]}");
    BookmarkStore again(root.path(), identity, fingerprint, "Book");
    require(again.load() && again.bookmarks().size() == 1, "Stale JSON was reimported");
    require(sql(root.path(), "SELECT count(*) FROM entries").toInt() == 2,
            "Migration duplicated records");
    require(sql(root.path(), "PRAGMA journal_mode").toString() == "delete",
            "Production database did not retain DELETE journaling");
    // EXTRA is connection-local. Production open reads it back and refuses
    // access unless the provider returns 3, before migration or editing.
    require(!QFile::exists(AnnotationDatabase::path(root.path()) + "-wal"),
            "Production save left a WAL file");
}

void atomicMigration()
{
    QTemporaryDir root;
    legacyCurrent(root.path());
    const auto path = folder(root.path());
    const auto marks = bytes(path + "/bookmarks.json"), original = bytes(path + "/highlights.json");
    auto corrupt = QJsonDocument::fromJson(original).object();
    corrupt["version"] = 2;
    const auto unsupported = QJsonDocument(corrupt).toJson();
    write(path + "/highlights.json", unsupported);
    BookmarkStore denied(root.path(), identity, fingerprint, "Book");
    require(!denied.load() && !denied.save(), "Future highlight format allowed partial migration");
    require(sql(root.path(), "SELECT count(*) FROM books").toInt() == 0
                    && sql(root.path(), "SELECT count(*) FROM lists").toInt() == 0
                    && sql(root.path(), "SELECT count(*) FROM entries").toInt() == 0,
            "Failed import left partial rows or markers");
    require(bytes(path + "/bookmarks.json") == marks
                    && bytes(path + "/highlights.json") == unsupported,
            "Failed import changed JSON bytes");
    write(path + "/highlights.json", original);
    HighlightStore retry(root.path(), identity, fingerprint, "Book");
    require(retry.load() && retry.highlights().size() == 1, "Import did not resume after repair");
    verifyHighlight(retry.highlights().first(), highlight());
    require(sql(root.path(), "SELECT count(*) FROM lists").toInt() == 2,
            "Import did not cover both lists");
    require(bytes(path + "/highlights.json") == original, "Successful import changed JSON");
}

void malformedProfile()
{
    for (const auto *table : { "books", "lists", "entries", "imports", "library_books",
                               "library_folders", "book_state", "profile_state" }) {
        for (const bool keepColumns : { false, true }) {
            QTemporaryDir root;
            require(storage::profileTransaction(root.path(), [](QSqlDatabase &) { return true; }),
                    "Cannot initialize malformed-schema fixture");
            const auto name = QString::fromLatin1(table);
            sql(root.path(), "CREATE TABLE damaged AS SELECT * FROM " + name);
            sql(root.path(), "DROP TABLE " + name);
            sql(root.path(),
                keepColumns ? "CREATE TABLE " + name + " AS SELECT * FROM damaged"
                            : "CREATE TABLE " + name + "(bogus TEXT)");
            sql(root.path(), "DROP TABLE damaged");
            const auto path = AnnotationDatabase::path(root.path());
            const auto before = bytes(path);
            QString error;
            require(!storage::profileTransaction(
                            root.path(), [](QSqlDatabase &) { return true; }, &error)
                            && error == "invalid-database",
                    "Malformed columns or missing primary key were accepted");
            require(bytes(path) == before && !QFile::exists(path + "-wal")
                            && !QFile::exists(path + "-shm"),
                    "Rejecting a malformed schema changed its bytes or created sidecars");
            require(QSqlDatabase::connectionNames().isEmpty(),
                    "Rejected schema retained a connection");
        }
    }
    for (const auto *definition :
         { "identity TEXT NOT NULL,book TEXT PRIMARY KEY,fingerprint TEXT NOT NULL,title TEXT NOT NULL",
           "book INTEGER PRIMARY KEY,identity TEXT NOT NULL,fingerprint TEXT NOT NULL,title TEXT NOT NULL" }) {
        QTemporaryDir root;
        require(storage::profileTransaction(root.path(), [](QSqlDatabase &) { return true; }),
                "Cannot initialize reordered-schema fixture");
        sql(root.path(), "DROP TABLE books");
        sql(root.path(), "CREATE TABLE books(" + QString::fromLatin1(definition) + ')');
        const auto path = AnnotationDatabase::path(root.path());
        const auto before = bytes(path);
        require(!storage::profileTransaction(root.path(), [](QSqlDatabase &) { return true; })
                        && bytes(path) == before && !QFile::exists(path + "-wal"),
                "Reordered or mistyped columns were accepted");
    }
    QTemporaryDir root;
    require(storage::profileTransaction(root.path(), [](QSqlDatabase &) { return true; }),
            "Cannot initialize orphaned-list fixture");
    sql(root.path(), "INSERT INTO lists VALUES('missing-book',0,0,0,'',0,0)");
    const auto path = AnnotationDatabase::path(root.path());
    const auto before = bytes(path);
    require(!storage::profileTransaction(root.path(), [](QSqlDatabase &) { return true; })
                    && bytes(path) == before && !QFile::exists(path + "-wal"),
            "Opening an orphaned list changed the profile");
}

void versionsAndCorruption()
{
    for (const QByteArray &bad :
         { QByteArray("{broken"), QByteArray("{\"version\":2,\"bookmarks\":[]}"),
           QByteArray("{\"version\":1,\"bookmarks\":[{\"position\":\"\"}]}"),
           QByteArray("{\"version\":1,\"bookmarks\":{}}") }) {
        QTemporaryDir root;
        legacyCurrent(root.path());
        const auto path = folder(root.path()) + "/bookmarks.json";
        write(path, bad);
        HighlightStore denied(root.path(), identity, fingerprint, "Book");
        require(!denied.load() && !denied.save() && bytes(path) == bad,
                "Corrupt or unsupported bookmarks were lost");
    }
    QTemporaryDir root;
    legacyCurrent(root.path());
    HighlightStore first(root.path(), identity, fingerprint, "Book");
    require(first.load(), "Cannot migrate format fixture");
    sql(root.path(), "PRAGMA user_version=99");
    const auto file = AnnotationDatabase::path(root.path());
    const auto before = bytes(file);
    HighlightStore future(root.path(), identity, fingerprint, "Book");
    require(!future.load() && !future.save() && bytes(file) == before,
            "Unsupported SQLite version was modified");
    QTemporaryDir foreign;
    sql(foreign.path(), "CREATE TABLE unrelated(value TEXT)");
    const auto foreignBytes = bytes(AnnotationDatabase::path(foreign.path()));
    BookmarkStore noOwner(foreign.path(), identity, fingerprint, "Book");
    require(!noOwner.load() && bytes(AnnotationDatabase::path(foreign.path())) == foreignBytes,
            "Foreign SQLite file was initialized");
    QTemporaryDir interrupted;
    legacyCurrent(interrupted.path());
    const auto abandoned = interrupted.path() + "/.kobalt-db-init-interrupted";
    write(abandoned, "incomplete unpublished schema");
    HighlightStore recovered(interrupted.path(), identity, fingerprint, "Book");
    require(recovered.load() && recovered.highlights().size() == 1
                    && bytes(abandoned) == "incomplete unpublished schema",
            "Interrupted initialization prevented migration retry");
    QTemporaryDir crashed;
    legacyCurrent(crashed.path());
    require(QProcess::execute(QCoreApplication::applicationFilePath(),
                              { "--interrupted-bootstrap", crashed.path() })
                    == 0,
            "Cannot simulate interrupted initialization process");
    require(QFile::exists(crashed.path() + "/.kobalt-db-init.lock"),
            "Crash fixture did not retain its lock");
    HighlightStore afterCrash(crashed.path(), identity, fingerprint, "Book");
    require(afterCrash.load() && afterCrash.highlights().size() == 1
                    && !QFile::exists(crashed.path() + "/.kobalt-db-init.lock"),
            "Dead initialization lock prevented retry");
    QTemporaryDir empty;
    write(AnnotationDatabase::path(empty.path()), {});
    HighlightStore unowned(empty.path(), identity, fingerprint, "Book");
    require(!unowned.load() && bytes(AnnotationDatabase::path(empty.path())).isEmpty(),
            "Unowned empty canonical file was initialized");
}

void editsAndFailures()
{
    QTemporaryDir root;
    legacyCurrent(root.path());
    HighlightStore first(root.path(), identity, fingerprint, "Book"),
            stale(root.path(), identity, fingerprint, "Book");
    require(first.load() && stale.load(), "Cannot load concurrent stores");
    auto edited = highlight();
    edited.note = "first edit";
    first.setHighlights({ edited });
    require(first.save(), "Cannot save edit");
    auto conflicting = highlight();
    conflicting.note = "stale edit";
    stale.setHighlights({ conflicting });
    QString error;
    require(!stale.save(&error) && error == "stale-annotations",
            "Stale snapshot overwrote a newer edit");
    require(stale.highlights().first().note == "stale edit",
            "Store failure unexpectedly changed pending memory");
    require(stale.load(), "Cannot reload stale store");
    verifyHighlight(stale.highlights().first(), edited);
    sql(root.path(),
        "CREATE TRIGGER deny_note BEFORE INSERT ON entries WHEN NEW.note='fail' BEGIN SELECT RAISE(ABORT,'injected'); END");
    auto pending = edited;
    pending.note = "fail";
    first.setHighlights({ pending });
    require(!first.save(), "Injected SQL write failure was accepted");
    HighlightStore persisted(root.path(), identity, fingerprint, "Book");
    require(persisted.load(), "Cannot reload after rollback");
    verifyHighlight(persisted.highlights().first(), edited);
    // Existing session code restores its previous vector after save failure.
    // A retry must also retain the old optimistic revision and byte accounting.
    first.setHighlights({ edited });
    require(first.save(), "Failed transaction advanced the store revision");
    auto second = edited;
    second.id = "second";
    first.setHighlights({ second, edited });
    require(first.save() && first.load(), "Cannot reorder and insert rows");
    require(first.highlights()[0].id == "second" && first.highlights()[1].id == edited.id,
            "Stored ordering changed");
    verifyQuota(root.path(), 1, qt::array({ highlightJson(second), highlightJson(edited) }));
    first.setHighlights({ edited });
    require(first.save() && first.load(), "Cannot remove one row");
    require(first.highlights().size() == 1, "Removed row remained");
    first.setHighlights({});
    require(first.save(), "Cannot remove final row");
    verifyQuota(root.path(), 1, {});
}

void adoption()
{
    QTemporaryDir root;
    legacyCurrent(root.path());
    HighlightStore first(root.path(), identity, fingerprint, "Book");
    require(first.load(), "Cannot seed adoption");
    auto latest = highlight();
    latest.note = "newer SQLite note";
    first.setHighlights({ latest });
    require(first.save(), "Cannot edit adoption source");
    // A restored or downgraded JSON writer must not become authoritative again,
    // even if its mtime is later than the last SQLite edit.
    sql(root.path(), "UPDATE lists SET updated=0");
    write(folder(root.path()) + "/highlights.json",
          QJsonDocument(qt::object({ { "version", 1 },
                                     { "highlights", qt::array({ highlightJson(highlight()) }) } }))
                  .toJson());
    const QByteArray changed(64, 'b');
    HighlightStore adopted(root.path(), identity, changed, "Changed");
    require(adopted.load() && adopted.highlights().size() == 1,
            "Changed fingerprint lost highlights");
    verifyHighlight(adopted.highlights().first(), latest);
    BookmarkStore marks(root.path(), identity, changed, "Changed");
    require(marks.load() && marks.bookmarks().size() == 1 && !marks.adoptedFrom().isEmpty(),
            "Changed fingerprint lost bookmarks or provenance");
    const QString moved = "file:///moved/book.epub";
    HighlightStore relocated(root.path(), moved, changed, "Moved");
    require(relocated.load() && relocated.save(), "Same fingerprint did not survive relocation");
    HighlightStore afterMove(root.path(), moved, QByteArray(64, 'c'), "Changed after move");
    require(afterMove.load() && afterMove.highlights().size() == 1,
            "Updated path was not used for later adoption");
    verifyHighlight(afterMove.highlights().first(), latest);
    require(sql(root.path(),
                "SELECT count(*) FROM books WHERE fingerprint='" + QString::fromLatin1(changed)
                        + "'")
                            .toInt()
                    == 1,
            "Source fingerprint was not stored");
}

void quotas()
{
    QTemporaryDir root;
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    require(store.load(), "Cannot load quota store");
    const auto original = highlight();
    store.setHighlights({ original });
    require(store.save(), "Cannot seed quota");
    auto tooLong = original;
    tooLong.note = QString(HighlightStore::maxNoteUnits + 1, 'x');
    store.setHighlights({ tooLong });
    require(!store.save(), "Oversized note was accepted");
    QVector<Highlight> many;
    for (int i = 0; i < 2001; ++i) {
        auto h = original;
        h.id = QString::number(i);
        many.append(h);
    }
    store.setHighlights(many);
    require(!store.save(), "Too many annotations were accepted");
    many.clear();
    for (int i = 0; i < 33; ++i) {
        auto h = original;
        h.id = QString::number(i);
        h.text = QString(HighlightStore::maxTextUnits, 'x');
        many.append(h);
    }
    store.setHighlights(many);
    require(!store.save(), "Annotation file-byte quota was ignored");
    require(store.load() && store.highlights().size() == 1,
            "Failed quota save changed stored data");
    verifyHighlight(store.highlights().first(), original);
    store.setHighlights({ original, original });
    require(!store.save(), "Duplicate highlight IDs were accepted");
    BookmarkStore marks(root.path(), identity, fingerprint, "Book");
    require(marks.load(), "Cannot load bookmark quota");
    QVector<Bookmark> large;
    for (int i = 0; i < 2000; ++i) {
        auto b = bookmark();
        b.position += QString::number(i);
        b.excerpt = QString(500, 'x');
        large.append(b);
    }
    marks.setBookmarks(large);
    require(!marks.save(), "Bookmark file-byte quota was ignored");
    marks.setBookmarks({ bookmark(), bookmark() });
    require(marks.save() && marks.load() && marks.bookmarks().size() == 2,
            "Duplicate legacy bookmark positions were lost");
}

void tamperedQuota()
{
    QTemporaryDir root;
    legacyCurrent(root.path());
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    require(store.load(), "Cannot seed tampering check");
    sql(root.path(),
        "UPDATE entries SET text='different payload but old cached bytes' WHERE kind=1");
    require(!store.load() && !store.save(), "Altered payload bypassed cached byte accounting");
    sql(root.path(), "UPDATE entries SET text=zeroblob(9*1024*1024) WHERE kind=1");
    HighlightStore huge(root.path(), identity, fingerprint, "Book");
    require(!huge.load() && !huge.save(), "Oversized database payload bypassed load bounds");
    sql(root.path(), "UPDATE entries SET note='unaccounted highlight payload' WHERE kind=0");
    BookmarkStore marks(root.path(), identity, fingerprint, "Book");
    require(!marks.load() && !marks.save(), "Bookmark row accepted hidden highlight-only payload");
}

void disabledProvenance()
{
    QTemporaryDir root;
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    auto mark = highlight();
    mark.sentenceStartPolicy = false;
    mark.sentenceEndPolicy = false;
    mark.chosenStart = QString(20000, 'x');
    mark.chosenEnd = QString(20000, 'y');
    store.setHighlights({ mark });
    require(store.save() && store.load() && store.highlights().size() == 1,
            "Disabled provenance prevented roundtrip");
    verifyHighlight(store.highlights().first(), mark);
    require(store.highlights().first().chosenStart.isEmpty()
                    && store.highlights().first().chosenEnd.isEmpty(),
            "SQLite retained provenance omitted by JSON");
    sql(root.path(), "UPDATE entries SET chosen_start='hidden' WHERE kind=1");
    require(!store.load() && !store.save(), "Inactive stored provenance bypassed quota accounting");
}

void sqlitePageLimit()
{
    QTemporaryDir root;
    legacyCurrent(root.path());
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    require(store.load(), "Production page limit rejected a small database");
    storage::closeProfile();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "annotation-limit-probe");
        db.setDatabaseName(AnnotationDatabase::path(root.path()));
        require(db.open(), "Cannot open page-limit fixture");
        {
            QSqlQuery q(db);
            require(q.exec("PRAGMA page_count") && q.next(), "Cannot read allocated page count");
            const auto pages = q.value(0).toLongLong();
            q.finish();
            // Exercise the provider's FULL/rollback behavior with a small
            // connection-local cap. This is not a 4 GiB exhaustion test.
            require(q.exec(QString("PRAGMA max_page_count=%1").arg(pages)) && q.next()
                            && q.value(0).toLongLong() == pages,
                    "Provider did not enforce the requested page cap");
            q.finish();
            require(db.transaction(), "Cannot start page-limit transaction");
            require(q.exec("UPDATE entries SET note='must roll back' WHERE kind=1"),
                    "Cannot update page-limit fixture");
            require(!q.exec("CREATE TABLE beyond_page_limit(value TEXT)"),
                    "Page exhaustion accepted another table");
#if QT_VERSION < QT_VERSION_CHECK(5, 3, 0)
            require(q.lastError().number() == 13, "Page exhaustion did not report SQLITE_FULL");
#else
            require(q.lastError().nativeErrorCode() == "13",
                    "Page exhaustion did not report SQLITE_FULL");
#endif
            q.finish();
            db.rollback(); // SQLITE_FULL may already have rolled back the transaction.
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("annotation-limit-probe");
    require(store.load() && store.highlights().size() == 1,
            "Cannot reopen after SQLITE_FULL rollback");
    verifyHighlight(store.highlights().first(), highlight());
}

void concurrentSnapshots()
{
    QTemporaryDir root;
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    require(store.load(), "Cannot initialize concurrent snapshot fixture");
    const QString shortNote(64, 'a'), longNote(96, 'b');
    QVector<Highlight> marks;
    for (int i = 0; i < 128; ++i) {
        auto h = highlight();
        h.id = "race-" + QString::number(i);
        h.note = shortNote;
        marks.append(h);
    }
    store.setHighlights(marks);
    require(store.save(), "Cannot seed concurrent snapshot fixture");
    // Another thread saves through Kobalt while this one loads. Both run on
    // the profile writer thread, so every load sees one whole saved list.
    std::atomic<bool> stop{ false };
    std::atomic<int> commits{ 0 };
    QString writerError;
    std::thread writer([&] {
        HighlightStore saver(root.path(), identity, fingerprint, "Book");
        if (!saver.load(&writerError))
            return;
        bool longer = true;
        while (!stop) {
            auto rows = saver.highlights();
            rows.first().note = longer ? longNote : shortNote;
            saver.setHighlights(rows);
            if (!saver.save(&writerError))
                break;
            ++commits;
            longer = !longer;
        }
    });
    bool valid = true;
    QString error;
    for (int i = 0; i < 200 && valid; ++i) {
        valid = store.load(&error) && store.highlights().size() == marks.size();
        if (valid) {
            const auto note = store.highlights().first().note;
            valid = note == shortNote || note == longNote;
        }
    }
    stop = true;
    writer.join();
    if (!valid || !writerError.isEmpty())
        std::fprintf(stderr, "Concurrent snapshot: reader=%s writer=%s\n", qPrintable(error),
                     qPrintable(writerError));
    require(valid && writerError.isEmpty() && commits > 0,
            "Concurrent saves broke annotation read snapshots");
}

// While Kobalt holds the profile, no other connection can read or write it.
// Closing returns one rollback-journal file that any connection can use.
void exclusiveProfile()
{
    QTemporaryDir root;
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    require(store.load(), "Cannot initialize exclusive profile fixture");
    store.setHighlights({ highlight() });
    require(store.save(), "Cannot save exclusive profile fixture");
    const auto database = AnnotationDatabase::path(root.path());
    require(QFile::exists(database + "-wal") && !QFile::exists(database + "-shm"),
            "An open profile did not use write-ahead logging with an exclusive lock");
    const auto probe = [&](bool expectOpen) {
        bool readable = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "annotation-exclusive-probe");
            db.setDatabaseName(database);
            db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=100");
            require(db.open(), "Cannot open exclusive profile probe");
            {
                QSqlQuery q(db);
                readable = q.exec("SELECT count(*) FROM entries") && q.next();
                if (expectOpen) {
                    require(q.exec("PRAGMA journal_mode") && q.next()
                                    && q.value(0).toString().compare("delete", Qt::CaseInsensitive)
                                            == 0,
                            "A closed profile was left in write-ahead mode");
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase("annotation-exclusive-probe");
        return readable;
    };
    require(!probe(false), "Another connection read the profile while Kobalt held it");
    storage::closeProfile();
    require(!QFile::exists(database + "-wal") && !QFile::exists(database + "-shm"),
            "Closing the profile left its log files");
    require(probe(true), "Another connection could not read the closed profile");
    // The next operation opens the profile again.
    require(store.load() && store.highlights().size() == 1, "Cannot reopen a closed profile");
    // While the profile is kept closed, for USB export or power-off, every
    // operation fails and nothing opens the file.
    storage::closeProfile(true);
    require(!store.load() && !QFile::exists(database + "-wal"), "A profile kept closed was opened");
    storage::reopenProfile();
    require(store.load() && store.highlights().size() == 1,
            "Cannot reopen after keeping the profile closed");
    storage::closeProfile();
}
} // namespace

void compactHighlights()
{
    QTemporaryDir root;
    legacyCurrent(root.path());
    auto first = highlight(), second = first;
    first.text = QString::fromUtf8("Long excerpt 日本語 😀 ").repeated(4000);
    first.note = QString::fromUtf8("Private note Ελληνικά 😀 ").repeated(1500);
    second.id = "second-id";
    HighlightStore full(root.path(), identity, fingerprint, "Book");
    require(full.load(), "Cannot initialize compact fixture");
    full.setHighlights({ first, second });
    require(full.save(), "Cannot save compact fixture");
    HighlightStore compact(root.path(), identity, fingerprint, "Book");
    require(compact.loadCompact(), "Cannot load compact highlights");
    const auto unloaded = [&] {
        for (const auto &h : compact.highlights())
            require(!h.detailsLoaded && h.text.isEmpty() && h.chapter.isEmpty() && h.note.isEmpty(),
                    "Compact store retained highlight payload");
    };
    unloaded();
    QVector<Highlight> detail;
    require(compact.details({ first.id }, &detail) && detail.size() == 1,
            "Cannot fetch full detail");
    verifyHighlight(detail.first(), first);
    unloaded();
    QVector<HighlightExcerpt> excerpts;
    require(compact.excerpts({ second.id, first.id }, &excerpts) && excerpts.size() == 2
                    && excerpts[0].id == second.id && excerpts[1].text == first.text
                    && excerpts[1].chapter == first.chapter,
            "Excerpt projection changed order or text");
    unloaded();
    auto rows = compact.highlights();
    rows[0].color = "pink";
    compact.setHighlights(rows);
    require(compact.save(), "Compact recolor failed");
    unloaded();
    first.color = "pink";
    require(full.load(), "Cannot reopen recolored compact record");
    verifyHighlight(full.highlights()[0], first);
    rows = compact.highlights();
    rows[0].end += "1";
    compact.setHighlights(rows);
    QString error;
    require(!compact.save(&error) && error == "details-required",
            "Unloaded endpoint edit was accepted");
    require(compact.loadCompact() && compact.details({ first.id }, &detail),
            "Cannot hydrate endpoint edit");
    first.end += "1";
    first.text += " adjusted";
    rows = compact.highlights();
    rows[0] = first;
    compact.setHighlights(rows);
    require(compact.save(), "Hydrated endpoint edit failed");
    unloaded();
    require(full.load(), "Cannot reopen adjusted record");
    verifyHighlight(full.highlights()[0], first);
    // A malformed payload is not read just to delete its compact row.
    sql(root.path(),
        "UPDATE entries SET text='bad cached byte count' WHERE kind=1 AND id='second-id'");
    require(compact.loadCompact(), "Compact load eagerly validated full payload bytes");
    require(!compact.details({ second.id }, &detail),
            "Full details accepted mismatched cached bytes");
    rows = compact.highlights();
    rows.removeLast();
    compact.setHighlights(rows);
    require(compact.save() && full.load() && full.highlights().size() == 1,
            "Compact deletion fetched damaged details");
    verifyHighlight(full.highlights().first(), first);
    unloaded();
    require(compact.loadCompact(), "Cannot refresh compact revision");
    full.setHighlights({ second });
    require(full.save(), "Cannot make concurrent edit");
    require(!compact.details({ first.id }, &detail, &error) && error == "stale-annotations",
            "Stale detail fetch succeeded");
    require(!compact.excerpts({ first.id }, &excerpts, &error) && error == "stale-annotations",
            "Stale excerpt fetch succeeded");
    require(!compact.save(&error) && error == "stale-annotations", "Stale compact save succeeded");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    HighlightStore json(root.path(), identity, fingerprint, "Book");
    require(json.loadCompact() && json.highlights().first().detailsLoaded
                    && json.details({ highlight().id }, &detail)
                    && json.excerpts({ highlight().id }, &excerpts),
            "JSON compact fallback lost full data");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
}

void compactBounds()
{
    for (const auto &mutation :
         QStringList{ "UPDATE entries SET start='' WHERE kind=1",
                      "UPDATE entries SET ordinal=-1 WHERE kind=1",
                      "UPDATE entries SET ordinal=1.5 WHERE kind=1",
                      "UPDATE entries SET start_policy=2 WHERE kind=1",
                      "UPDATE entries SET bytes=1 WHERE kind=1",
                      "UPDATE entries SET note=zeroblob(262145) WHERE kind=1",
                      "UPDATE lists SET row_count=2001 WHERE kind=1" }) {
        QTemporaryDir root;
        legacyCurrent(root.path());
        qunsetenv("KOBALT_ANNOTATIONS_SQLITE");
        HighlightStore store(root.path(), identity, fingerprint, "Book");
        require(store.load(), "Cannot seed compact bounds");
        sql(root.path(), mutation);
        require(!store.loadCompact() && !store.save(),
                "Compact metadata or physical bounds were bypassed");
    }
    QTemporaryDir root;
    legacyCurrent(root.path());
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    require(store.loadCompact(), "Cannot seed deferred validation");
    sql(root.path(), "UPDATE entries SET note='short changed note' WHERE kind=1");
    require(store.loadCompact(), "Compact load materialized and validated a note");
    QVector<HighlightExcerpt> excerpt;
    require(store.excerpts({ highlight().id }, &excerpt)
                    && excerpt.first().text == highlight().text,
            "Excerpt projection fetched or validated an unused note");
    QVector<Highlight> detail;
    require(!store.details({ highlight().id }, &detail),
            "Detail fetch skipped exact payload validation");
    auto rows = store.highlights();
    rows[0].color = "blue";
    store.setHighlights(rows);
    require(!store.save(), "Compact appearance edit overwrote invalid unloaded payload");
}

void deletionPreservesRows()
{
    QTemporaryDir root;
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    QVector<Highlight> expected;
    for (int i = 0; i < 2000; ++i) {
        auto h = highlight();
        h.id = QString::number(i);
        h.note = QString(1024, QChar('n'));
        expected.append(h);
    }
    store.setHighlights(expected);
    require(store.save() && store.loadCompact(), "Cannot seed deletion regression");
    BookmarkStore marks(root.path(), identity, fingerprint, "Book");
    auto other = bookmark();
    other.position += "1";
    marks.setBookmarks({ bookmark(), other });
    require(marks.save(), "Cannot seed bookmark deletion");
    // A deletion must not rewrite any surviving entry, including replacing
    // its payload through INSERT OR REPLACE. Count the actual deleted rows.
    sql(root.path(), "CREATE TABLE mutations(value INTEGER)");
    sql(root.path(),
        "CREATE TRIGGER deny_update BEFORE UPDATE ON entries BEGIN SELECT RAISE(ABORT,'survivor updated'); END");
    sql(root.path(),
        "CREATE TRIGGER deny_insert BEFORE INSERT ON entries BEGIN SELECT RAISE(ABORT,'survivor replaced'); END");
    sql(root.path(),
        "CREATE TRIGGER count_delete AFTER DELETE ON entries BEGIN INSERT INTO mutations VALUES(1); END");
    for (const int id : { 0, 1000, 1999 }) {
        auto pending = store.highlights();
        for (int i = 0; i < pending.size(); ++i)
            if (pending[i].id == QString::number(id)) {
                pending.removeAt(i);
                expected.removeAt(i);
                break;
            }
        store.setHighlights(pending);
        require(store.save() && store.loadCompact(), "Deletion rewrote a surviving annotation");
    }
    require(sql(root.path(), "SELECT count(*) FROM mutations").toInt() == 3,
            "Deletion changed more than its target rows");
    require(sql(root.path(), "SELECT ordinal FROM entries WHERE kind=1 AND id='1'").toLongLong()
                            == 1
                    && sql(root.path(), "SELECT ordinal FROM entries WHERE kind=1 AND id='1001'")
                                    .toLongLong()
                            == 1001,
            "Deletion renumbered surviving order values");
    marks.setBookmarks({ other });
    require(marks.save() && marks.load() && marks.bookmarks() == QVector<Bookmark>{ other },
            "Bookmark deletion rewrote a survivor or changed its data");
    require(sql(root.path(), "SELECT count(*) FROM mutations").toInt() == 4,
            "Bookmark deletion changed extra rows");
    sql(root.path(), "DROP TRIGGER deny_insert");
    auto added = highlight();
    added.id = "appended";
    auto pending = store.highlights();
    pending.append(added);
    store.setHighlights(pending);
    expected.append(added);
    require(store.save(), "Appending after deletion renumbered survivors");
    HighlightStore full(root.path(), identity, fingerprint, "Book");
    require(full.load() && full.highlights().size() == expected.size(), "Cannot reopen order gaps");
    for (int i = 0; i < expected.size(); ++i)
        verifyHighlight(full.highlights()[i], expected[i]);
}

void orderChangesAndLimits()
{
    QTemporaryDir root;
    auto a = highlight(), b = a, c = a, inserted = a;
    b.id = "b";
    c.id = "c";
    inserted.id = "inserted";
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    store.setHighlights({ a, b, c });
    require(store.save(), "Cannot seed ordering regression");
    store.setHighlights({ b, c });
    require(store.save(), "Cannot make order gap");
    store.setHighlights({ c, b });
    require(store.save() && store.loadCompact(), "Cannot reorder sparse entries");
    require(store.highlights()[0].id == c.id && store.highlights()[1].id == b.id,
            "Reordered IDs lost their order");
    auto pending = store.highlights();
    pending.insert(1, inserted);
    store.setHighlights(pending);
    require(store.save() && store.load(), "Cannot insert between sparse entries");
    verifyHighlight(store.highlights()[0], c);
    verifyHighlight(store.highlights()[1], inserted);
    verifyHighlight(store.highlights()[2], b);
    store.setHighlights({ a, b });
    require(store.save(), "Cannot seed integer limit");
    sql(root.path(),
        "UPDATE entries SET ordinal=9223372036854775806 WHERE kind=1 AND id='stable-id'");
    sql(root.path(), "UPDATE entries SET ordinal=9223372036854775807 WHERE kind=1 AND id='b'");
    require(store.loadCompact(), "64-bit order values were truncated");
    pending = store.highlights();
    pending.append(c);
    store.setHighlights(pending);
    require(store.save() && store.load(), "Appending at the integer limit overflowed");
    verifyHighlight(store.highlights()[0], a);
    verifyHighlight(store.highlights()[1], b);
    verifyHighlight(store.highlights()[2], c);
    sql(root.path(), "UPDATE entries SET ordinal=0 WHERE kind=1");
    require(!store.load() && !store.loadCompact() && !store.save(),
            "Duplicate order values were accepted");
}

void compactCapacity()
{
    QTemporaryDir root;
    legacyCurrent(root.path());
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    require(store.load(), "Cannot seed compact capacity");
    QVector<Highlight> rows;
    for (int i = 0; i < HighlightStore::maxHighlights; ++i) {
        auto h = highlight();
        h.id = QString::number(i);
        rows.append(h);
    }
    store.setHighlights(rows);
    require(store.save() && store.loadCompact(), "Compact load rejected 2000 rows");
    require(store.highlights().size() == 2000 && !store.highlights().last().detailsLoaded,
            "Compact count changed");
    QStringList batch;
    for (int i = 1999; i >= 1871; --i)
        batch.append(QString::number(i));
    QVector<HighlightExcerpt> excerpts;
    require(store.excerpts(batch, &excerpts) && excerpts.size() == batch.size(),
            "Chunked excerpt query lost rows");
    for (int i = 0; i < batch.size(); ++i)
        require(excerpts[i].id == batch[i] && excerpts[i].text == highlight().text,
                "Chunked excerpts changed requested order");
    require(!store.excerpts({ "0", "0" }, &excerpts), "Duplicate excerpt request succeeded");
    QVector<Highlight> result{ highlight() };
    require(!store.details({ "0", "0" }, &result) && result.size() == 1,
            "Repeated IDs expanded details or changed failed output");
    QStringList tooMany;
    for (int i = 0; i < 2001; ++i)
        tooMany.append(QString::number(i));
    require(!store.details(tooMany, &result), "Oversized detail batch succeeded");
    auto added = highlight();
    added.id = "new-compact";
    added.text.clear();
    added.chapter.clear();
    added.note.clear();
    added.detailsLoaded = false;
    store.setHighlights({ added });
    require(!store.save(), "New compact record discarded missing payload");
}

void embeddedNulText()
{
    QTemporaryDir root;
    QString suffix;
    suffix.append(QChar(0));
    suffix.append(QString::fromUtf8("after 日本語 😀"));
    const auto nulIdentity = identity + suffix;
    auto h = highlight();
    h.id += suffix;
    h.start += suffix;
    h.end += suffix;
    h.text += suffix;
    h.chapter += suffix;
    h.note += suffix;
    h.chosenStart += suffix;
    h.chosenEnd += suffix;
    for (auto *field :
         { &h.id, &h.start, &h.end, &h.text, &h.chapter, &h.note, &h.chosenStart, &h.chosenEnd })
        field->prepend(QChar(0xfeff));
    h.text.prepend(QChar(0xfeff));
    auto b = bookmark();
    b.position += suffix;
    b.chapter += suffix;
    b.excerpt += suffix;
    for (auto *field : { &b.position, &b.chapter, &b.excerpt })
        field->prepend(QChar(0xfeff));
    require(validHighlight(h), "Embedded-NUL fixture violates the stored string contract");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    HighlightStore json(root.path(), nulIdentity, fingerprint, "Book");
    json.setHighlights({ h });
    BookmarkStore jsonMarks(root.path(), nulIdentity, fingerprint, "Book");
    jsonMarks.setBookmarks({ b });
    require(json.save() && jsonMarks.save(), "Cannot seed embedded-NUL JSON");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    HighlightStore full(root.path(), nulIdentity, fingerprint, "Book");
    BookmarkStore marks(root.path(), nulIdentity, fingerprint, "Book");
    require(full.load() && full.highlights().size() == 1 && marks.load()
                    && marks.bookmarks() == QVector<Bookmark>{ b },
            "Full SQLite load truncated embedded NUL");
    verifyHighlight(full.highlights().first(), h);
    HighlightStore compact(root.path(), nulIdentity, fingerprint, "Book");
    require(compact.loadCompact() && compact.highlights().size() == 1,
            "Cannot load compact embedded-NUL record");
    const auto &metadata = compact.highlights().first();
    require(!metadata.detailsLoaded && metadata.id == h.id && metadata.start == h.start
                    && metadata.end == h.end && metadata.chosenStart == h.chosenStart
                    && metadata.chosenEnd == h.chosenEnd,
            "Compact metadata truncated embedded NUL");
    QVector<HighlightExcerpt> excerpts;
    require(compact.excerpts({ h.id }, &excerpts) && excerpts.size() == 1 && excerpts[0].id == h.id
                    && excerpts[0].text == h.text && excerpts[0].chapter == h.chapter,
            "Excerpt projection truncated embedded NUL");
    QVector<Highlight> details;
    require(compact.details({ h.id }, &details) && details.size() == 1,
            "Cannot fetch embedded-NUL details");
    verifyHighlight(details[0], h);
    auto changed = compact.highlights();
    changed[0].color = "pink";
    compact.setHighlights(changed);
    require(compact.save() && full.load(), "Compact recolor failed for embedded-NUL record");
    h.color = "pink";
    verifyHighlight(full.highlights().first(), h);
    HighlightStore adopted(root.path(), nulIdentity, QByteArray(64, 'b'), "Changed book");
    require(adopted.load() && adopted.highlights().size() == 1,
            "Embedded-NUL identity prevented database adoption");
    verifyHighlight(adopted.highlights().first(), h);
}

void unsupportedDatabaseEncoding()
{
    QTemporaryDir root;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "annotation-utf16-fixture");
        db.setDatabaseName(AnnotationDatabase::path(root.path()));
        require(db.open(), "Cannot create UTF-16 fixture");
        {
            QSqlQuery q(db);
            require(q.exec("PRAGMA encoding='UTF-16le'")
                            && q.exec("CREATE TABLE sentinel(value TEXT)")
                            && q.exec("INSERT INTO sentinel VALUES('preserve this')")
                            && q.exec("PRAGMA application_id=1262571057")
                            && q.exec("PRAGMA user_version=2"),
                    "Cannot initialize UTF-16 fixture");
        }
        db.close();
    }
    QSqlDatabase::removeDatabase("annotation-utf16-fixture");
    const auto path = AnnotationDatabase::path(root.path());
    const auto before = bytes(path);
    HighlightStore store(root.path(), identity, fingerprint, "Book");
    QString error;
    require(!store.loadCompact(&error) && error == "unsupported-database-encoding" && !store.save()
                    && bytes(path) == before,
            "Non-UTF-8 owned database was misdecoded or modified");
}

void profileDatabaseLocation()
{
    QTemporaryDir profile;
    storage::Paths paths{ profile.path() + "/profile" };
    require(paths.database() == AnnotationDatabase::path(paths.directory),
            "Profile database names disagree");
    legacyCurrent(paths.annotations());
    const auto originalHighlights = bytes(folder(paths.annotations()) + "/highlights.json");
    const auto originalBookmarks = bytes(folder(paths.annotations()) + "/bookmarks.json");
    HighlightStore highlights(paths.annotations(), identity, fingerprint, "Book", paths.database());
    BookmarkStore bookmarks(paths.annotations(), identity, fingerprint, "Book", paths.database());
    require(highlights.loadCompact() && bookmarks.load(), "Cannot import into profile database");
    require(QFile::exists(paths.database())
                    && !QFile::exists(AnnotationDatabase::path(paths.annotations()))
                    && !QFile::exists(paths.annotations() + "/annotations.sqlite3"),
            "Database was placed inside legacy annotations");
    require(bookmarks.bookmarks() == QVector<Bookmark>{ bookmark() },
            "Profile database lost bookmarks");
    auto rows = highlights.highlights();
    rows[0].color = "blue";
    highlights.setHighlights(rows);
    require(highlights.save(), "Cannot edit profile database");
    HighlightStore reopened(paths.annotations(), identity, fingerprint, "Book", paths.database());
    auto expected = highlight();
    expected.color = "blue";
    require(reopened.load() && reopened.highlights().size() == 1, "Cannot reopen profile database");
    verifyHighlight(reopened.highlights().first(), expected);
    BookmarkStore second(paths.annotations(), "second-book", {}, "Second book", paths.database());
    require(second.load() && second.bookmarks().isEmpty()
                    && sql(paths.directory, "SELECT count(*) FROM books").toInt() == 2,
            "Books did not share the profile database");
    require(bytes(folder(paths.annotations()) + "/highlights.json") == originalHighlights
                    && bytes(folder(paths.annotations()) + "/bookmarks.json") == originalBookmarks,
            "Profile database import or edits changed legacy JSON");
    storage::Paths empty{ profile.path() + "/empty-profile" };
    BookmarkStore isolated(empty.annotations(), identity, fingerprint, "Book", empty.database());
    require(isolated.load() && isolated.bookmarks().isEmpty() && QFile::exists(empty.database())
                    && !QFile::exists(empty.annotations()),
            "Empty profile did not create an independent database");
}

// A process that ends without closing the profile leaves its log. The next
// opening replays it, and so does a connection opened the way earlier builds
// open the file; that connection also returns it to a rollback journal.
void crashRecovery(const QString &program)
{
    for (const bool previousBuild : { false, true }) {
        QTemporaryDir root;
        QProcess child;
        child.start(program, { "--crash-after-save", root.path() });
        require(child.waitForFinished(30000) && child.exitStatus() == QProcess::NormalExit
                        && child.exitCode() == 0,
                "Crash fixture process failed");
        const auto database = AnnotationDatabase::path(root.path());
        require(QFileInfo(database + "-wal").size() > 0 && !QFile::exists(database + "-shm"),
                "A crash did not leave the write-ahead log");
        if (previousBuild) {
            QString note, position, mode;
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", "annotation-previous-build");
                db.setDatabaseName(database);
                db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1500");
                require(db.open(), "Cannot open crashed profile like the previous build");
                {
                    QSqlQuery q(db);
                    // The previous build reads ownership first, then sets its journal.
                    require(q.exec("PRAGMA application_id") && q.next()
                                    && q.value(0).toInt() == 1262571057,
                            "Previous build could not read a crashed profile");
                    q.finish();
                    require(q.exec("PRAGMA journal_mode=DELETE") && q.next(),
                            "Previous build could not set its journal");
                    mode = q.value(0).toString();
                    q.finish();
                    require(q.exec("SELECT CAST(note AS BLOB) FROM entries WHERE kind=1")
                                    && q.next(),
                            "Previous build lost the crashed highlight");
                    note = QString::fromUtf8(q.value(0).toByteArray());
                    q.finish();
                    require(q.exec("SELECT CAST(payload AS BLOB) FROM book_state WHERE identity='crash-book'")
                                    && q.next(),
                            "Previous build lost the crashed position");
                    position = QJsonDocument::fromJson(q.value(0).toByteArray())
                                       .object()
                                       .value("position")
                                       .toString();
                }
                db.close();
            }
            QSqlDatabase::removeDatabase("annotation-previous-build");
            require(mode.compare("delete", Qt::CaseInsensitive) == 0
                            && note == "saved before the crash" && position == "routine position",
                    "Previous build did not recover the crashed profile");
            require(!QFile::exists(database + "-wal") && !QFile::exists(database + "-shm"),
                    "Previous build left log files behind");
        } else {
            HighlightStore store(root.path(), identity, fingerprint, "Book");
            require(store.load() && store.highlights().size() == 1
                            && store.highlights().first().note == "saved before the crash",
                    "Opening did not recover the crashed highlight");
            const auto state = storage::readBookState(storage::Paths{ root.path() }, "crash-book");
            require(state.status == storage::ReadStatus::Ok
                            && state.object.value("position").toString() == "routine position",
                    "Opening did not recover a routine write after a crash");
            storage::closeProfile();
            require(!QFile::exists(database + "-wal"), "Closing after recovery left the log");
        }
        BookmarkStore bookmarks(root.path(), identity, fingerprint, "Book");
        require(bookmarks.load() && bookmarks.bookmarks().size() == 1
                        && bookmarks.bookmarks().first().position == "saved bookmark position"
                        && bookmarks.bookmarks().first().excerpt
                                == QString::fromUtf8("saved \xc3\xa9") + QChar(0) + " text",
                "Crash recovery lost the bookmark or its Unicode payload");
        storage::closeProfile();
    }
}

// Routine commits wait for a sync. A synced commit that changes rows syncs
// them; one that changes nothing, such as an unchanged record, does not.
void routineSyncs()
{
    QTemporaryDir root;
    const storage::Paths paths{ root.path() };
    const auto state = qt::object({ { "book", "sync-book" }, { "position", "first" } });
    require(storage::writeBookState(paths, "sync-book", state)
                    && !storage::profileHasUnsyncedWrites(),
            "A synced commit left routine writes waiting");
    {
        storage::RoutineWrites routine;
        require(storage::writeBookState(
                        paths, "sync-book",
                        qt::object({ { "book", "sync-book" }, { "position", "second" } })),
                "Cannot write a routine record");
    }
    require(storage::profileHasUnsyncedWrites(), "A routine commit was reported as synced");
    require(storage::writeBookState(
                    paths, "sync-book",
                    qt::object({ { "book", "sync-book" }, { "position", "second" } }))
                    && storage::readBookState(paths, "sync-book").status == storage::ReadStatus::Ok,
            "Cannot repeat an unchanged record");
    require(storage::profileHasUnsyncedWrites(), "A commit without changes was counted as a sync");
    require(storage::profileCheckpointDue(),
            "Unsynced routine writes did not make a checkpoint due");
    require(storage::writeBookState(
                    paths, "sync-book",
                    qt::object({ { "book", "sync-book" }, { "position", "third" } }))
                    && !storage::profileHasUnsyncedWrites(),
            "A changed synced commit did not sync earlier routine writes");
    {
        storage::RoutineWrites routine;
        require(storage::writeBookState(
                        paths, "sync-book",
                        qt::object({ { "book", "sync-book" }, { "position", "fourth" } })),
                "Cannot write a second routine record");
    }
    storage::checkpointProfile();
    require(!storage::profileHasUnsyncedWrites(), "A checkpoint did not sync routine writes");
    storage::closeProfile();
}

// Sideloaded books are keyed by content, so each stress book needs its own.
QByteArray stressFingerprint(int book)
{
    return QByteArray(64, char('a' + book));
}

// Threads save, load and close at the same time. Every save lands, and the
// closed profile is one rollback-journal file.
void concurrentStress()
{
    QTemporaryDir root;
    constexpr int books = 3, rounds = 60;
    std::atomic<bool> failed{ false };
    std::mutex problemMutex;
    QString problem;
    const auto fail = [&](const QString &step, const QString &error) {
        std::lock_guard<std::mutex> lock(problemMutex);
        if (problem.isEmpty())
            problem = step + ": " + error;
        failed = true;
    };
    std::vector<std::thread> threads;
    for (int book = 0; book < books; ++book) {
        threads.emplace_back([&, book] {
            const auto key = QString("file:///stress-%1.epub").arg(book);
            HighlightStore store(root.path(), key, stressFingerprint(book), "Stress");
            QString error;
            if (!store.load(&error)) {
                fail("load", error);
                return;
            }
            for (int round = 0; round < rounds && !failed; ++round) {
                auto h = highlight();
                h.note = QString("round %1").arg(round);
                store.setHighlights({ h });
                if (!store.save(&error)) {
                    fail("save", error);
                    return;
                }
                const auto state =
                        qt::object({ { "book", key }, { "position", QString::number(round) } });
                storage::RoutineWrites routine;
                if (!storage::writeBookState(storage::Paths{ root.path() }, key, state)) {
                    fail("state", {});
                    return;
                }
            }
        });
    }
    threads.emplace_back([&] {
        for (int i = 0; i < 20 && !failed; ++i) {
            QThread::msleep(5);
            storage::closeProfile();
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < rounds && !failed; ++i) {
            for (int book = 0; book < books; ++book) {
                HighlightStore reader(root.path(), QString("file:///stress-%1.epub").arg(book),
                                      stressFingerprint(book), "Stress");
                QString error;
                if (!reader.load(&error) || reader.highlights().size() > 1) {
                    fail("read", error);
                    return;
                }
            }
        }
    });
    for (auto &thread : threads)
        thread.join();
    if (failed)
        std::fprintf(stderr, "Concurrent stress: %s\n", qPrintable(problem));
    require(!failed, "Concurrent profile operations failed");
    for (int book = 0; book < books; ++book) {
        const auto key = QString("file:///stress-%1.epub").arg(book);
        HighlightStore store(root.path(), key, stressFingerprint(book), "Stress");
        require(store.load() && store.highlights().size() == 1
                        && store.highlights().first().note == QString("round %1").arg(rounds - 1),
                "A concurrent save was lost");
        require(storage::readBookState(storage::Paths{ root.path() }, key)
                                .object.value("position")
                                .toString()
                        == QString::number(rounds - 1),
                "A concurrent routine write was lost");
    }
    storage::closeProfile();
    const auto database = AnnotationDatabase::path(root.path());
    require(!QFile::exists(database + "-wal") && !QFile::exists(database + "-shm"),
            "Stress left log files");
}

void defaultBackend()
{
    qunsetenv("KOBALT_ANNOTATIONS_SQLITE");
    require(annotationDatabaseEnabled(), "SQLite must be the default backend");
    {
        QTemporaryDir root;
        legacyCurrent(root.path());
        qunsetenv("KOBALT_ANNOTATIONS_SQLITE");
        HighlightStore store(root.path(), identity, fingerprint, "Book");
        require(store.load() && sql(root.path(), "PRAGMA user_version").toInt() == 3,
                "Default backend did not migrate JSON to current schema");
    }
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    require(!annotationDatabaseEnabled(), "Explicit JSON override ignored");
    {
        QTemporaryDir root;
        HighlightStore json(root.path(), identity, fingerprint, "Book");
        json.setHighlights({ highlight() });
        require(json.save() && !QFile::exists(AnnotationDatabase::path(root.path())),
                "Normal JSON path created a database");
    }
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    require(annotationDatabaseEnabled(), "Explicit SQLite override ignored");
}

// Checks the SQLite annotation database: JSON import, profile location, schema checks, failed writes, quotas and order limits.
int main(int argc, char **argv)
{
    // Firmware runners supply a writable isolated parent on user storage.
    if (argc == 3 && QByteArray(argv[1]) == "--order-regressions")
        qputenv("TMPDIR", QByteArray(argv[2]));
    QCoreApplication app(argc, argv);
    if (app.arguments().size() >= 2 && app.arguments()[1] == "--order-regressions") {
        require(argc == 2 || argc == 3, "Expected --order-regressions [TEMP_PARENT]");
        defaultBackend();
        profileDatabaseLocation();
        beta3();
        atomicMigration();
        deletionPreservesRows();
        orderChangesAndLimits();
        storage::closeProfile();
        require(QSqlDatabase::connectionNames().isEmpty(),
                "Ordering checks retained database connections");
        std::printf("{\"qt\":\"%s\",\"order_regressions\":\"passed\",\"annotations\":2000}\n",
                    qVersion());
        return 0;
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--interrupted-bootstrap") {
        const auto root = app.arguments()[2];
        QLockFile lock(root + "/.kobalt-db-init.lock");
        require(lock.tryLock(), "Cannot create crash fixture lock");
        auto db = QSqlDatabase::addDatabase("QSQLITE");
        db.setDatabaseName(root + "/.kobalt-db-init-crashed");
        require(db.open(), "Cannot create crash fixture database");
        QSqlQuery query(db);
        require(query.exec("BEGIN IMMEDIATE") && query.exec("CREATE TABLE incomplete(value TEXT)"),
                "Cannot start interrupted schema transaction");
        std::_Exit(0); // Deliberately skip database and lock destructors.
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--crash-after-save") {
        const auto root = app.arguments()[2];
        HighlightStore store(root, identity, fingerprint, "Book");
        require(store.load(), "Cannot initialize crash fixture");
        auto h = highlight();
        h.note = "saved before the crash";
        store.setHighlights({ h });
        require(store.save(), "Cannot save crash fixture");
        BookmarkStore bookmarks(root, identity, fingerprint, "Book");
        require(bookmarks.load(), "Cannot initialize crashed bookmark fixture");
        bookmarks.setBookmarks({ { "saved bookmark position", "Chapter",
                                   QString::fromUtf8("saved \xc3\xa9") + QChar(0) + " text",
                                   QDateTime::currentDateTimeUtc() } });
        require(bookmarks.save(), "Cannot save crashed bookmark fixture");
        {
            storage::RoutineWrites routine;
            require(storage::writeBookState(storage::Paths{ root }, "crash-book",
                                            qt::object({ { "book", "crash-book" },
                                                         { "position", "routine position" } })),
                    "Cannot write routine crash fixture");
        }
        std::_Exit(0); // End without closing the profile, like a crash.
    }
    defaultBackend();
    profileDatabaseLocation();
    pinnedBookmarks();
    beta3();
    atomicMigration();
    malformedProfile();
    versionsAndCorruption();
    editsAndFailures();
    adoption();
    quotas();
    tamperedQuota();
    compactHighlights();
    compactBounds();
    compactCapacity();
    deletionPreservesRows();
    orderChangesAndLimits();
    embeddedNulText();
    unsupportedDatabaseEncoding();
    disabledProvenance();
    sqlitePageLimit();
    concurrentSnapshots();
    exclusiveProfile();
    crashRecovery(app.applicationFilePath());
    concurrentStress();
    routineSyncs();
    require(QSqlDatabase::connectionNames().isEmpty(),
            "Annotation operations retained database connections");
    qunsetenv("KOBALT_ANNOTATIONS_SQLITE");
    return 0;
}
