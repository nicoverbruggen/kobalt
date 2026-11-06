#include "storage/reader_storage.h"
#include "storage/profile_database.h"
#include "storage/record_writer.h"
#include "storage/reading_history.h"
#include "storage/bookmark_store.h"
#include "reading/sleep_screen.h"
#ifndef KOBALT_TEST_NO_LIBRARY
#  include "library/library_index.h"
#endif
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QVariant>
#include <utime.h>

using namespace kobalt;

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

QByteArray bytes(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Read fixture");
    return file.readAll();
}

void write(const QString &path, const QByteArray &value)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(value) == value.size(), "Write fixture");
}

// Stands in for a change made while Kobalt was closed: the profile closes
// afterwards, so the next operation opens and checks the file again.
void execute(const QString &directory, const QString &sql)
{
    require(storage::profileTransaction(directory,
                                        [&](QSqlDatabase &db) {
                                            QSqlQuery query(db);
                                            return query.exec(sql);
                                        }),
            "SQL fixture");
    storage::closeProfile();
}

// Kobalt holds the profile file exclusively while it is open. Close it, as
// USB export does, before probing the file with another connection.
QVariant rawValue(const QString &directory, const QString &sql)
{
    storage::closeProfile();
    const auto name = QStringLiteral("profile-schema-probe");
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(directory + "/kobalt.sqlite");
    require(db.open(), "Open raw schema fixture");
    QVariant result;
    {
        QSqlQuery query(db);
        require(query.exec(sql) && query.next(), "Query raw schema fixture");
        result = query.value(0);
    }
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
    return result;
}

} // namespace

// Checks the SQLite profile: batched commits and rollback, JSON and beta 3 import, schema checks, backups of unusable files and the catalogue.
int main(int argc, char **argv)
{
    if (argc == 2)
        qputenv("TMPDIR", QByteArray(argv[1]));
    QCoreApplication app(argc, argv);
    using storage::ReadStatus;
    QTemporaryDir root;
    require(root.isValid(), "Temporary profile");
    {
        QTemporaryDir batchRoot;
        const storage::Paths batch{ batchRoot.path() };
        const QJsonObject oldBook{ { "schema", 1 },
                                   { "book", "batch-book" },
                                   { "position", "old" } };
        const QJsonObject newBook{ { "schema", 1 },
                                   { "book", "batch-book" },
                                   { "position", "new" } };
        const QJsonObject oldSpeed{ { "schema", 1 }, { "samples", 1 } };
        const QJsonObject newSpeed{ { "schema", 1 }, { "samples", 2 } };
        require(storage::writeBookState(batch, "batch-book", oldBook)
                        && storage::writeRecord(batch.directory, "reading-speed",
                                                batch.readingSpeed(), oldSpeed),
                "Batch baseline");
        ReadingHistory history(batch.directory, "batch-book", {}, {}, "Batch", "Author",
                               [] { return 1000; });
        history.start(0, "old");
        history.update(100, "new");
        auto snapshot = history.snapshot();
        bool saved = true;
        const auto writeBatch = [&](bool fail) {
            storage::runOnWriterThread([&] {
                storage::RoutineWrites routine;
                saved = storage::writeReadingActivity(
                        batch, "batch-book", newBook, newSpeed, [&](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            require(query.exec("PRAGMA synchronous") && query.next()
                                            && query.value(0).toInt() == 1,
                                    "Reading batch must use WAL NORMAL");
                            return snapshot.write(db) && !fail;
                        });
                snapshot.complete(saved);
            });
        };
        writeBatch(true);
        require(!saved && !history.savesSucceeded(), "Failed activity must retain its history");
        require(storage::readBookState(batch, "batch-book").object == oldBook
                        && storage::readRecord(batch.directory, "reading-speed",
                                               batch.readingSpeed())
                                        .object
                                == oldSpeed,
                "History failure must roll back position and speed");
        require(storage::profileTransaction(
                        batch.directory,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            return query.exec("SELECT count(*) FROM reading_sessions")
                                    && query.next() && query.value(0).toInt() == 0;
                        }),
                "Failed activity must roll back history too");
        writeBatch(false);
        require(saved && history.savesSucceeded() && storage::profileHasUnsyncedWrites(),
                "Retry commits the complete unsynced batch");
        require(storage::readBookState(batch, "batch-book").object == newBook
                        && storage::readRecord(batch.directory, "reading-speed",
                                               batch.readingSpeed())
                                        .object
                                == newSpeed,
                "Successful activity commits both records");
        storage::checkpointProfile();
        require(!storage::profileHasUnsyncedWrites(),
                "Lifecycle checkpoint syncs prior NORMAL commits");
        storage::closeProfile();
    }
    const storage::Paths paths{ root.path() };
    const QString identity = "file:///books/invented.epub";
    QJsonObject state;
    state["schema"] = 1;
    state["book"] = identity;
    state["position"] = QStringLiteral("/body/section[2]/p[4]/text().12");
    state["font_face"] = QStringLiteral("Publisher");
    state["font_pixels"] = 41;
    state["dictionary_id"] = QStringLiteral("custom-fr");
    state["nickel_progress"] = qt::object({ { "location", "native-position" }, { "percent", 24 } });
    state["future_optional"] = qt::array({ 1, false, qt::object({ { "note", "Preserve me" } }) });
    state["literal"] = QString(QChar(0xfeff)) + QString::fromUtf8("é") + QChar(0) + "tail";
    state["null"] = QJsonValue();
    require(storage::writeJson(paths.bookState(identity), state), "Legacy book fixture");
    const auto original = bytes(paths.bookState(identity));
    require(storage::prepareProfile(paths), "Import complete profile");
    require(storage::readBookState(paths, identity).object == state, "Imported state changed");
    const auto backup =
            root.path() + "/migration-backup/" + QFileInfo(paths.bookState(identity)).fileName();
    require(!QFile::exists(paths.bookState(identity)) && bytes(backup) == original,
            "Import backup changed");
    auto edited = state;
    edited["position"] = QStringLiteral("/body/p[50]");
    require(storage::writeBookState(paths, identity, edited), "Save database checkpoint");
    execute(root.path(),
            "CREATE TRIGGER unchanged_checkpoint BEFORE UPDATE ON book_state BEGIN SELECT RAISE(ABORT,'unchanged'); END");
    require(storage::writeBookState(paths, identity, edited),
            "Unchanged JSON checkpoint rewrote its row");
    execute(root.path(), "DROP TRIGGER unchanged_checkpoint");
    edited["position"] = QStringLiteral("/body/p[51]");
    require(storage::writeBookState(paths, identity, edited), "Update one JSON checkpoint");
    require(rawValue(root.path(), "SELECT count(*) FROM book_state").toInt() == 1
                    && rawValue(root.path(), "SELECT revision FROM book_state").toInt() == 2,
            "Book state is not one JSON row per book");
    write(paths.bookState(identity), original);
    require(storage::readBookState(paths, identity).object == edited,
            "Stale JSON replaced a newer checkpoint");
    require(storage::writeBookState(paths, identity, qt::object({ { "book", identity } }))
                    && storage::readBookState(paths, identity).object
                            == qt::object({ { "book", identity } }),
            "Mark unread retained old settings or position");
    execute(root.path(),
            "CREATE TRIGGER fail_checkpoint BEFORE UPDATE ON book_state BEGIN SELECT RAISE(ABORT,'test'); END");
    require(!storage::writeBookState(paths, identity, edited)
                    && storage::readBookState(paths, identity).object
                            == qt::object({ { "book", identity } }),
            "Failed write changed state");
    execute(root.path(), "DROP TRIGGER fail_checkpoint");
    auto oversized = edited;
    oversized["large"] = QString(int(storage::maxJsonBytes), 'x');
    require(!storage::writeBookState(paths, identity, oversized)
                    && storage::readBookState(paths, identity).object
                            == qt::object({ { "book", identity } }),
            "Oversized book state replaced saved data");
    QTemporaryDir invalidPayload;
    require(storage::writeBookState(storage::Paths{ invalidPayload.path() }, identity, state),
            "Initialize damaged JSON fixture");
    execute(invalidPayload.path(), "UPDATE book_state SET payload='{broken'");
    require(storage::readBookState(storage::Paths{ invalidPayload.path() }, identity).status
                            == ReadStatus::Invalid
                    && !storage::writeBookState(storage::Paths{ invalidPayload.path() }, identity,
                                                edited),
            "Damaged database JSON was overwritten");
    const auto broken = paths.bookState("broken");
    write(broken, "{ invalid");
    require(storage::readBookState(paths, "broken").status == ReadStatus::Invalid
                    && !storage::writeBookState(paths, "broken",
                                                qt::object({ { "book", "broken" } }))
                    && bytes(broken) == "{ invalid",
            "Malformed input was replaced");
    const auto future = paths.bookState("future");
    require(storage::writeJson(future, qt::object({ { "book", "future" }, { "schema", 99 } })),
            "Future fixture");
    const auto futureBytes = bytes(future);
    require(storage::readBookState(paths, "future").status == ReadStatus::Unsupported
                    && !storage::writeBookState(paths, "future",
                                                qt::object({ { "book", "future" } }))
                    && bytes(future) == futureBytes,
            "Future book format was changed");
    require(storage::writeJson(paths.preferences(),
                               qt::object({ { "bionic_reading", "bold" },
                                            { "smooth_images", true },
                                            { "last_header_footer", "alternative_b" },
                                            { "inherit_header_footer", true },
                                            { "heavier_interface_text", true },
                                            { "native_hide_status_bar", false },
                                            { "pin_stats", true },
                                            { "hide_tab_labels", true },
                                            { "columns", 2 },
                                            { "column_spacing", 20 },
                                            { "word_spacing", 17 },
                                            { "private_optional", "keep" } })),
            "Settings fixture");
    const auto clean = storage::readPreferences(paths.preferences());
    require(clean.status == ReadStatus::Ok && clean.object.value("schema") == 1
                    && clean.object.value("bold_reading").toString() == "bold"
                    && clean.object.value("header_footer").toString() == "alternative_b"
                    && clean.object.value("default_menu_view").toString() == "stats"
                    && clean.object.value("hide_tab_labels") == true
                    && clean.object.value("private_optional").toString() == "keep"
                    && !clean.object.contains("word_spacing")
                    && !clean.object.contains("smooth_images")
                    && !clean.object.contains("bionic_reading")
                    && !clean.object.contains("native_hide_status_bar")
                    && !clean.object.contains("pin_stats") && !clean.object.contains("columns"),
            "Settings cleanup lost intent");
    require(storage::readJson(paths.preferences()) == clean.object,
            "Clean settings were not persisted");
    // Until beta 7 the Wi-Fi power saver setting is never saved, and reading
    // the settings removes a saved one.
    require(storage::writePreferences(paths.preferences(),
                                      qt::object({ { "turn_off_wifi_on_open", true } }))
                    && !storage::readJson(paths.preferences()).contains("turn_off_wifi_on_open"),
            "Wi-Fi power saver setting was saved");
    require(storage::writeJson(paths.preferences(),
                               qt::object({ { "schema", 1 }, { "turn_off_wifi_on_open", true } })),
            "Wi-Fi power saver fixture");
    require(!storage::readPreferences(paths.preferences()).object.contains("turn_off_wifi_on_open")
                    && !storage::readJson(paths.preferences()).contains("turn_off_wifi_on_open"),
            "Saved Wi-Fi power saver setting was not removed");
    write(paths.preferences(), "{\"schema\":99,\"new_option\":true}");
    const auto settingsBytes = bytes(paths.preferences());
    require(storage::readPreferences(paths.preferences()).status == ReadStatus::Unsupported
                    && !storage::writePreferences(paths.preferences(), {})
                    && bytes(paths.preferences()) == settingsBytes,
            "Future settings were overwritten");
    QTemporaryDir interrupted;
    const storage::Paths retry{ interrupted.path() };
    require(storage::readRecord(retry.directory, "initialize", {}).status == ReadStatus::Missing,
            "Initialize retry profile");
    require(storage::writeJson(retry.bookState(identity), state), "Interrupted import fixture");
    execute(retry.directory,
            "CREATE TRIGGER fail_import BEFORE INSERT ON book_state BEGIN SELECT RAISE(ABORT,'test'); END");
    require(storage::readBookState(retry, identity).status == ReadStatus::DatabaseError
                    && QFile::exists(retry.bookState(identity)),
            "Failed import removed its source");
    execute(retry.directory, "DROP TRIGGER fail_import");
    write(retry.directory + "/migration-backup", "Blocked archive");
    require(storage::readBookState(retry, identity).object == state
                    && QFile::exists(retry.bookState(identity)),
            "Archive failure prevented a committed import");
    require(storage::writeBookState(retry, identity, edited), "Edit after interrupted archive");
    require(QFile::remove(retry.directory + "/migration-backup")
                    && storage::readBookState(retry, identity).object == edited
                    && !QFile::exists(retry.bookState(identity)),
            "Retry imported stale data or failed to finish cleanup");
    write(retry.directory + "/unknown.json", "Do not touch");
    require(storage::prepareProfile(retry)
                    && bytes(retry.directory + "/unknown.json") == "Do not touch",
            "Cleanup changed an unrecognized file");
    require(QDir().mkpath(retry.directory + "/diagnostics"), "Diagnostic directory fixture");
    write(retry.directory + "/diagnostics/events.jsonl", "New session\n");
    write(retry.directory + "/events.jsonl", "Previous session\n");
    require(storage::prepareProfile(retry) && !QFile::exists(retry.directory + "/events.jsonl")
                    && bytes(retry.directory + "/diagnostics/events.jsonl") == "New session\n"
                    && bytes(retry.directory + "/migration-backup/diagnostics/events.jsonl")
                            == "Previous session\n",
            "Startup log prevented cleanup or replaced previous diagnostics");
    QTemporaryDir beta3;
    const storage::Paths oldPaths{ beta3.path() };
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    BookmarkStore oldMarks(oldPaths.annotations(), "native-beta3", {}, "Invented book");
    const Bookmark mark{ "/body/p[3]", "First chapter", "An invented passage",
                         QDateTime::currentDateTimeUtc() };
    oldMarks.setBookmarks({ mark });
    require(oldMarks.save(), "Beta 3 bookmark fixture");
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    require(storage::writeJson(oldPaths.bookState("native-beta3"),
                               qt::object({ { "schema", 1 },
                                            { "book", "native-beta3" },
                                            { "position", mark.position },
                                            { "font_pixels", 39 } })),
            "Beta 3 reading fixture");
    require(storage::prepareProfile(oldPaths) && !QFile::exists(oldMarks.folder()),
            "Beta 3 profile cleanup");
    require(storage::profileTransaction(beta3.path(),
                                        [&](QSqlDatabase &db) {
                                            QSqlQuery query(db);
                                            return query.exec("PRAGMA user_version") && query.next()
                                                    && query.value(0).toInt() == 3;
                                        }),
            "Beta 3 migration did not create SQLite schema version 3");
    BookmarkStore newMarks(oldPaths.annotations(), "native-beta3", {}, "Invented book",
                           oldPaths.database());
    require(newMarks.load() && newMarks.bookmarks().size() == 1
                    && newMarks.bookmarks().first().position == mark.position
                    && storage::readBookState(oldPaths, "native-beta3").object.value("font_pixels")
                            == 39
                    && storage::prepareProfile(oldPaths),
            "Beta 3 bookmark or settings lost on retry");
    QTemporaryDir editions;
    const storage::Paths editionPaths{ editions.path() };
    const QString editedIdentity = "file:///books/changed.epub";
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
    for (const char version : { 'a', 'b' }) {
        BookmarkStore old(editionPaths.annotations(), editedIdentity, QByteArray(64, version),
                          "Changed book");
        auto entry = mark;
        entry.excerpt = version == 'a' ? "Newest saved edition" : "Older saved edition";
        old.setBookmarks({ entry });
        require(old.save(), "Edition fixture");
        // Alphabetical traversal imports the newer source first.
        struct utimbuf stamp{ 1600000000, version == 'a' ? 1600000200 : 1600000100 };
        require(::utime(QFile::encodeName(old.folder() + "/bookmarks.json").constData(), &stamp)
                        == 0,
                "Set edition chronology");
    }
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    require(storage::prepareProfile(editionPaths), "Import saved editions");
    BookmarkStore changed(editionPaths.annotations(), editedIdentity, QByteArray(64, 'c'),
                          "Changed book", editionPaths.database());
    require(changed.load() && changed.bookmarks().size() == 1
                    && changed.bookmarks().first().excerpt == "Newest saved edition",
            "Bulk migration chose an edition by traversal order instead of source chronology");
    QTemporaryDir unsupportedDatabase;
    execute(unsupportedDatabase.path(), "PRAGMA user_version=1");
    const auto unsupportedBytes = bytes(unsupportedDatabase.path() + "/kobalt.sqlite");
    require(storage::readBookState(storage::Paths{ unsupportedDatabase.path() }, "new").status
                            == ReadStatus::DatabaseError
                    && bytes(unsupportedDatabase.path() + "/kobalt.sqlite") == unsupportedBytes,
            "Unsupported SQLite schema was changed");
    QTemporaryDir partialDatabase;
    execute(partialDatabase.path(), "DROP TABLE library_folders");
    const auto partialBytes = bytes(partialDatabase.path() + "/kobalt.sqlite");
    require(storage::readBookState(storage::Paths{ partialDatabase.path() }, "new").status
                            == ReadStatus::DatabaseError
                    && bytes(partialDatabase.path() + "/kobalt.sqlite") == partialBytes,
            "Partial schema was repaired or replaced");
    QTemporaryDir futureDatabase;
    execute(futureDatabase.path(), "PRAGMA user_version=4");
    const auto futureDatabaseBytes = bytes(futureDatabase.path() + "/kobalt.sqlite");
    require(storage::readBookState(storage::Paths{ futureDatabase.path() }, "new").status
                            == ReadStatus::DatabaseError
                    && bytes(futureDatabase.path() + "/kobalt.sqlite") == futureDatabaseBytes,
            "Future SQLite schema was changed");
    {
        // Profile preparation moves unusable saved data into backups/ and
        // starts again from defaults. The backups keep the original bytes.
        QTemporaryDir recovery;
        const storage::Paths recover{ recovery.path() };
        const QString backups = recovery.path() + "/backups/";
        execute(recovery.path(), "PRAGMA user_version=4");
        write(recover.preferences(), "{\"schema\":1,");
        write(recover.syncAccount(), "{\"schema\":2,\"server\":\"https://sync.invented\"}");
        const auto settingsBytes = bytes(recover.preferences());
        const auto accountBytes = bytes(recover.syncAccount());
        const auto databaseBytes = bytes(recover.database());
        storage::ProfileRecovery result;
        require(storage::prepareProfile(recover, &result) && result.settings && result.syncAccount
                        && result.database && !result.storageFailed && result.backups.size() == 3,
                "Unusable saved data was not moved aside");
        require(bytes(backups + "reader-settings.json.corrupted.bak") == settingsBytes
                        && bytes(backups + "kosync.json.bak") == accountBytes
                        && bytes(backups + "kobalt.sqlite.bak") == databaseBytes,
                "A backup changed the original bytes or got the wrong name");
        const auto defaults = storage::readJsonFile(recover.preferences());
        require(defaults.status == ReadStatus::Ok
                        && defaults.object
                                == QJsonObject{ { "schema", 1 }, { "sleep_defaults_migrated", true } }
                        && sleep::mode(defaults.object) == sleep::Mode::Cover
                        && sleep::tile(defaults.object) == sleep::Tile::Bookmark
                        && !QFile::exists(recover.syncAccount()),
                "Recovery did not start again from defaults");
        require(storage::writeBookState(recover, "fresh", { { "book", "fresh" } }),
                "The new database is not usable");
        result = {};
        require(storage::prepareProfile(recover, &result) && !result.recovered(),
                "Usable data was moved aside");
        // A second failure keeps the first backup. SQLite drops the log of a
        // file that is not a database when it opens it, so only the main file
        // reaches the backup; no stale log stays beside the new database.
        storage::closeProfile();
        write(recover.preferences(), "[]");
        write(recover.database(), QByteArray(4096, 'x'));
        write(recover.database() + "-wal", "log");
        result = {};
        require(storage::prepareProfile(recover, &result) && result.settings && result.database
                        && bytes(backups + "reader-settings.json.corrupted.bak") == settingsBytes
                        && bytes(backups + "reader-settings.json.corrupted.2.bak") == "[]",
                "A second backup replaced the first");
        require(bytes(backups + "kobalt.sqlite.corrupted.bak") == QByteArray(4096, 'x')
                        && !QFile::exists(recover.database() + "-wal"),
                "A damaged database was not moved aside");
        storage::closeProfile();
        // Settings without a schema are older settings, not damaged ones.
        write(recover.preferences(), "{\"dark_mode\":true}");
        result = {};
        require(storage::prepareProfile(recover, &result) && !result.settings
                        && storage::readPreferences(recover.preferences()).object.value("dark_mode")
                                == true,
                "Older settings were moved aside");
        storage::closeProfile();
        // A backup that cannot be written leaves the original in place.
        QTemporaryDir blocked;
        const storage::Paths stuck{ blocked.path() };
        write(blocked.path() + "/backups", "not a folder");
        write(stuck.preferences(), "{");
        result = {};
        require(!storage::prepareProfile(stuck, &result) && result.storageFailed && !result.settings
                        && bytes(stuck.preferences()) == "{",
                "A failed backup was not reported, or moved the original");
        // A file that cannot be read stops preparation without changes.
        QTemporaryDir unreadable;
        const storage::Paths locked{ unreadable.path() };
        write(locked.preferences(), "{\"schema\":1}");
        require(QFile::setPermissions(locked.preferences(), QFileDevice::WriteOwner),
                "Make settings unreadable");
        result = {};
        require(!storage::prepareProfile(locked, &result) && result.storageFailed
                        && !QFileInfo::exists(unreadable.path() + "/backups"),
                "Unreadable settings were not reported, or were moved");
        QFile::setPermissions(locked.preferences(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        storage::closeProfile();
    }

#ifndef KOBALT_TEST_NO_LIBRARY
    QTemporaryDir catalogue;
    library::Index index;
    index.addFolder({ "/invented", "Invented books" });
    library::Book book;
    book.key = "/invented/river.epub";
    book.folder = "/invented";
    book.name = "river.epub";
    book.title = "The Copper River";
    book.author = "Robin Example";
    book.seriesIndex = 2.5;
    book.opened = 1792849147000;
    book.percent = 43;
    book.mark = library::Book::Mark::Finished;
    index.books.insert(book.key, book);
    index.settings.sort = library::Sort::Author;
    index.settings.hideFinished = true;
    require(index.save(catalogue.path() + "/library.json"), "Legacy catalogue fixture");
    library::Index imported;
    require(imported.loadDatabase(catalogue.path())
                    && imported.books.value(book.key).json() == book.json()
                    && imported.settings.sort == library::Sort::Author
                    && imported.settings.hideFinished
                    && !QFile::exists(catalogue.path() + "/library.json"),
            "Catalogue import changed books or preferences");
    imported.books[book.key].percent = 81;
    require(imported.saveDatabase(catalogue.path()), "Save catalogue update");
    library::Index reopened;
    require(reopened.loadDatabase(catalogue.path()) && reopened.books.value(book.key).percent == 81,
            "Catalogue update lost");
    execute(catalogue.path(),
            "CREATE TRIGGER unchanged_book BEFORE INSERT ON library_books BEGIN SELECT RAISE(ABORT,'duplicate'); END");
    require(reopened.saveDatabase(catalogue.path()), "Unchanged catalogue rewrote every book");
    execute(catalogue.path(), "DROP TRIGGER unchanged_book");
    reopened.books.clear();
    require(reopened.saveDatabase(catalogue.path()), "Remove catalogue book");
    require(imported.loadDatabase(catalogue.path()) && imported.books.isEmpty(),
            "Removed book reimported");
    execute(catalogue.path(), "PRAGMA user_version=99");
    const auto databaseBytes = bytes(catalogue.path() + "/kobalt.sqlite");
    require(!imported.loadDatabase(catalogue.path())
                    && bytes(catalogue.path() + "/kobalt.sqlite") == databaseBytes,
            "Future database was modified");

#endif
    return 0;
}
