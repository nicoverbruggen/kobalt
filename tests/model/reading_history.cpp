#include "storage/reading_history.h"
#include "storage/profile_database.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include "storage/reader_storage.h"
#include "storage/bookmark_store.h"
#include "storage/highlight_store.h"
#include "library/library_index.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <sys/stat.h>
#include <QProcess>
#include <QSocketNotifier>
#include <unistd.h>
#include <cstdio>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

using namespace kobalt;

namespace {
void require(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

bool recoverProfile(const QString &directory)
{
    return bool(ReadingHistory::openProfile(directory));
}

QVariant sql(const QString &directory, const QString &statement)
{
    QVariant value;
    require(storage::profileTransaction(directory,
                                        [&](QSqlDatabase &db) {
                                            QSqlQuery query(db);
                                            if (!query.exec(statement))
                                                return false;
                                            if (query.next())
                                                value = query.value(0);
                                            return true;
                                        }),
            "History SQL fixture");
    return value;
}

QVariant raw(const QString &directory, const QString &statement)
{
    storage::closeProfile();
    const QString name = "history-schema-probe";
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(directory + "/kobalt.sqlite");
    require(db.open(), "Open raw history fixture");
    QVariant result;
    {
        QSqlQuery query(db);
        require(query.exec(statement), "Raw history SQL");
        if (query.next())
            result = query.value(0);
    }
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
    return result;
}

QByteArray bytes(const QString &directory)
{
    QFile file(directory + "/kobalt.sqlite");
    require(file.open(QIODevice::ReadOnly), "Read database fixture");
    return file.readAll();
}

void versionTwo(const QString &directory)
{
    sql(directory, "DROP TABLE reading_sessions");
    raw(directory, "PRAGMA user_version=2");
}

void migration()
{
    QTemporaryDir root;
    const storage::Paths paths{ root.path() };
    const QString identity = "file:///books/history.epub";
    const QJsonObject state{ { "book", identity },
                             { "position", "/body/p[2]" },
                             { "optional", "Keep me" } };
    require(storage::writeBookState(paths, identity, state), "Book-state fixture");
    BookmarkStore marks(paths.annotations(), identity, QByteArray(64, 'c'), "History",
                        paths.database());
    require(marks.load(), "Bookmark fixture load");
    const Bookmark mark{ "/body/p[2]", "Chapter", "Text",
                         QDateTime::fromString("2020-09-13T12:26:40Z", Qt::ISODate) };
    marks.setBookmarks({ mark });
    require(marks.save(), "Bookmark fixture save");
    HighlightStore highlights(paths.annotations(), identity, QByteArray(64, 'c'), "History",
                              paths.database());
    require(highlights.load(), "Highlight fixture load");
    Highlight highlight;
    highlight.id = "preserve-highlight";
    highlight.start = "/body/p[3]";
    highlight.end = "/body/p[4]";
    highlight.text = "Saved passage";
    highlight.chapter = "Chapter";
    highlight.created = mark.created;
    highlights.setHighlights({ highlight });
    require(highlights.save(), "Highlight fixture save");
    require(storage::writeRecord(paths.directory, "reading-speed", {},
                                 { { "schema", 1 }, { "samples", 2 } }),
            "Profile fixture");
    sql(root.path(), "INSERT INTO imports VALUES('legacy-source',X'1234','preserve-import')");
    library::Index catalogue;
    require(catalogue.loadDatabase(root.path()), "Initialize library fixture");
    catalogue.addFolder({ "/books", "Books" });
    library::Book book;
    book.key = "/books/history.epub";
    book.folder = "/books";
    book.name = "history.epub";
    book.title = "History";
    catalogue.books.insert(book.key, book);
    require(catalogue.saveDatabase(root.path()), "Library fixture");
    versionTwo(root.path());
    require(raw(root.path(), "PRAGMA user_version").toInt() == 2, "Not a version-2 fixture");
    require(storage::readBookState(paths, identity).object == state,
            "Migration changed book state");
    require(raw(root.path(), "PRAGMA user_version").toInt() == 3,
            "Migration did not advance schema");
    require(marks.load() && marks.bookmarks() == QVector<Bookmark>{ mark },
            "Migration changed bookmarks");
    require(highlights.load() && highlights.highlights().size() == 1
                    && highlights.highlights().first().id == highlight.id
                    && highlights.highlights().first().text == highlight.text,
            "Migration changed highlights");
    require(storage::readRecord(paths.directory, "reading-speed", {}).object.value("samples") == 2,
            "Migration changed profile state");
    require(sql(root.path(), "SELECT record FROM imports WHERE source='legacy-source'").toString()
                    == "preserve-import",
            "Migration changed import ledger");
    require(catalogue.loadDatabase(root.path()) && catalogue.books.contains(book.key)
                    && catalogue.folders.size() == 1,
            "Migration changed library");
    require(sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 0,
            "Migration invented history");
    require(sql(root.path(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name LIKE 'reading_sessions_%'")
                            .toInt()
                    == 2,
            "Missing history indexes");
    require(recoverProfile(root.path()) && recoverProfile(root.path()), "Repeated open failed");

    QTemporaryDir failed;
    versionTwo(failed.path());
    raw(failed.path(), "CREATE INDEX reading_sessions_started ON book_state(revision)");
    const auto original = bytes(failed.path());
    require(!recoverProfile(failed.path()), "Conflicting migration index was accepted");
    require(bytes(failed.path()) == original
                    && raw(failed.path(), "PRAGMA user_version").toInt() == 2
                    && raw(failed.path(),
                           "SELECT COUNT(*) FROM sqlite_master WHERE name='reading_sessions'")
                                    .toInt()
                            == 0,
            "Failed migration was not rolled back");
    raw(failed.path(), "DROP INDEX reading_sessions_started");
    require(recoverProfile(failed.path()), "Failed migration could not retry");

    QTemporaryDir partial;
    versionTwo(partial.path());
    raw(partial.path(), "DROP TABLE library_folders");
    raw(partial.path(), "CREATE TABLE library_folders(key TEXT)");
    const auto partialBytes = bytes(partial.path());
    require(!recoverProfile(partial.path()) && bytes(partial.path()) == partialBytes,
            "Partial old schema was changed");
}

void rejectsVersionTwoWithoutBookIdentityUniqueness()
{
    QTemporaryDir root;
    versionTwo(root.path());
    raw(root.path(), "ALTER TABLE book_state RENAME TO previous_state");
    raw(root.path(), "CREATE TABLE book_state AS SELECT * FROM previous_state");
    raw(root.path(), "DROP TABLE previous_state");
    raw(root.path(), "INSERT INTO book_state VALUES('duplicate-book',1,0,'{}')");
    raw(root.path(), "INSERT INTO book_state VALUES('duplicate-book',1,0,'{}')");
    const auto original = bytes(root.path());
    require(!recoverProfile(root.path()) && bytes(root.path()) == original,
            "Migration accepted duplicate book identities after the primary key was removed");
}

void migrationRejectsOrphanedAnnotations()
{
    QTemporaryDir root;
    versionTwo(root.path());
    raw(root.path(),
        "INSERT INTO lists(book,kind,revision,updated,source_key,row_count,bytes) VALUES('missing-book',0,0,0,'',0,0)");
    require(raw(root.path(), "PRAGMA foreign_key_check").toString() == "lists",
            "Orphan fixture is not broken");
    const auto original = bytes(root.path());
    require(!recoverProfile(root.path()) && bytes(root.path()) == original,
            "Migration modified a profile containing orphaned annotations");
}

void crashRecoveryDoesNotStealLiveProfileOwnership()
{

    QTemporaryDir root, different;
    QString error;
    auto owner = ReadingHistory::openProfile(root.path(), &error);
    require(bool(owner) && error.isEmpty(), "Cannot acquire profile ownership");
    auto same = ReadingHistory::openProfile(root.path() + "/.");
    require(same == owner && bool(ReadingHistory::openProfile(different.path())),
            "Profile owners did not share locally or isolate different folders");
    ReadingHistory live(root.path(), "live-book", {}, {}, "Live", "Author");
    live.start(0, "/body");
    require(live.save(), "Live profile fixture");
    require(QProcess::execute(QCoreApplication::applicationFilePath(),
                              { "--claim-profile", root.path() })
                    == 23,
            "A second process recovered a live profile");
    require(sql(root.path(), "SELECT status FROM reading_sessions").toString() == "active",
            "Refusing another process changed the live session");
    live.stop(100, "/body/p[2]");
    require(live.save(), "Complete live profile fixture");
    owner.reset();
    same.reset();
    storage::closeProfile();
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), { "--hold-active-profile", root.path() });
    require(child.waitForStarted(5000) && child.waitForReadyRead(5000)
                    && child.readLine() == "ready\n",
            "Crash writer did not reach its committed checkpoint");
    child.kill();
    require(child.waitForFinished(5000) && child.exitStatus() == QProcess::CrashExit,
            "Crash writer did not stop");
    require(QFile::exists(root.path() + "/.reading-history.lock"),
            "Crash fixture did not leave its ownership marker");
    auto recovered = ReadingHistory::openProfile(root.path());
    require(bool(recovered), "A crashed process kept the profile locked");
    require(sql(root.path(), "SELECT status FROM reading_sessions WHERE identity='crash-book'")
                                    .toString()
                            == "interrupted"
                    && sql(root.path(),
                           "SELECT active_ms FROM reading_sessions WHERE identity='crash-book'")
                                    .toLongLong()
                            == 2000
                    && sql(root.path(),
                           "SELECT observed FROM reading_sessions WHERE identity='crash-book'")
                                    .toLongLong()
                            == 10000,
            "Recovery lost committed reading time or counted downtime");
}

void interruptedOwnershipMarkerCreationDoesNotBlockProfile()
{
    QTemporaryDir root;
    QFile marker(root.path() + "/.reading-history.lock");
    require(marker.open(QIODevice::WriteOnly), "Interrupted ownership marker fixture");
    marker.close();
    require(bool(ReadingHistory::openProfile(root.path())),
            "An empty marker left by a crash permanently blocked the profile");
}

void unsafeOwnershipMarkersAreRefused()
{
    QTemporaryDir linkRoot, directoryRoot, fifoRoot;
    QFile target(linkRoot.path() + "/target");
    require(target.open(QIODevice::WriteOnly) && target.write("preserve") == 8,
            "Symlink marker target fixture");
    target.close();
    require(QFile::link(target.fileName(), linkRoot.path() + "/.reading-history.lock"),
            "Symlink marker fixture");
    require(!ReadingHistory::openProfile(linkRoot.path()),
            "Profile followed a symlink ownership marker");
    require(target.open(QIODevice::ReadOnly) && target.readAll() == "preserve",
            "Ownership attempt changed the symlink target");
    require(QDir().mkpath(directoryRoot.path() + "/.reading-history.lock")
                    && !ReadingHistory::openProfile(directoryRoot.path()),
            "Profile accepted a directory ownership marker");
    require(::mkfifo(QFile::encodeName(fifoRoot.path() + "/.reading-history.lock").constData(),
                     0600) == 0
                    && !ReadingHistory::openProfile(fifoRoot.path()),
            "Profile accepted a FIFO ownership marker");
}

void failedRecoveryReleasesOwnershipForRetry()
{
    QTemporaryDir root;
    ReadingHistory history(root.path(), "failed-recovery", {}, {}, "Recovery", "Author");
    history.start(0, "/body");
    require(history.save(), "Failed recovery fixture");
    sql(root.path(),
        "CREATE TRIGGER fail_recovery BEFORE UPDATE ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
    require(!ReadingHistory::openProfile(root.path()), "Failed recovery returned a usable owner");
    sql(root.path(), "DROP TRIGGER fail_recovery");
    storage::closeProfile();
    require(QProcess::execute(QCoreApplication::applicationFilePath(),
                              { "--claim-profile", root.path() })
                    == 0,
            "Failed recovery leaked its OS lock into a later process");
    require(sql(root.path(), "SELECT status FROM reading_sessions").toString() == "interrupted",
            "Recovery retry did not finalize the interrupted interval");
}

void commitFailureRetainsPendingDuration()
{

    QTemporaryDir root;
    auto owner = ReadingHistory::openProfile(root.path());
    require(bool(owner), "Commit fixture profile ownership");
    ReadingHistory history(root.path(), "commit-book", {}, {}, "Commit", "Author");
    history.start(0, "/body");
    require(history.save(), "Commit fixture initial save");
    // A deferred constraint fails COMMIT after the interval UPDATE succeeds.
    // A reader on another connection cannot block commits in WAL mode.
    sql(root.path(),
        "CREATE TABLE commit_guard(book TEXT REFERENCES books(book) DEFERRABLE INITIALLY DEFERRED)");
    sql(root.path(),
        "CREATE TRIGGER fail_commit AFTER UPDATE ON reading_sessions BEGIN INSERT INTO commit_guard VALUES('missing-book'); END");
    history.update(500, "/body/p[2]");
    require(!history.save(), "A failed commit reported success");
    require(sql(root.path(), "SELECT active_ms FROM reading_sessions").toLongLong() == 0
                    && sql(root.path(), "SELECT COUNT(*) FROM commit_guard").toInt() == 0,
            "Failed commit published its interval or trigger writes");
    sql(root.path(), "DROP TRIGGER fail_commit");
    require(history.save()
                    && sql(root.path(), "SELECT active_ms FROM reading_sessions").toLongLong()
                            == 500
                    && sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 1,
            "Commit retry lost duration or duplicated the interval");
}

void migrationCommitFailureLeavesVersionTwoRetryable()
{
    QTemporaryDir root;
    versionTwo(root.path());
    raw(root.path(),
        "INSERT INTO book_state VALUES('preserved-book',1,7,'{\"position\":\"/body/p[2]\"}')");
    const auto original = bytes(root.path());
    const QString name = "migration-commit-blocker";
    auto blocker = QSqlDatabase::addDatabase("QSQLITE", name);
    blocker.setDatabaseName(root.path() + "/kobalt.sqlite");
    require(blocker.open(), "Open migration commit blocker");
    {
        QSqlQuery query(blocker);
        require(query.exec("BEGIN") && query.exec("SELECT payload FROM book_state") && query.next(),
                "Hold old-schema snapshot");
        query.finish();
        QString error;
        require(!storage::profileTransaction(
                        root.path(), [](QSqlDatabase &) { return true; }, &error)
                        && error == "database-commit",
                "Migration did not exercise a failed commit");
        require(query.exec("ROLLBACK"), "Release migration commit blocker");
    }
    blocker.close();
    blocker = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
    require(bytes(root.path()) == original && raw(root.path(), "PRAGMA user_version").toInt() == 2,
            "Failed migration commit changed the old profile");
    require(recoverProfile(root.path())
                    && raw(root.path(),
                           "SELECT revision FROM book_state WHERE identity='preserved-book'")
                                    .toInt()
                            == 7
                    && raw(root.path(),
                           "SELECT payload FROM book_state WHERE identity='preserved-book'")
                                    .toString()
                            == "{\"position\":\"/body/p[2]\"}",
            "Migration could not retry without changing saved data");
}

void killedMigrationLeavesOldDataRecoverable()
{
    QTemporaryDir root;
    versionTwo(root.path());
    raw(root.path(),
        "INSERT INTO book_state VALUES('preserved-book',1,7,'{\"position\":\"/body/p[2]\"}')");
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), { "--hold-migration", root.path() });
    require(child.waitForStarted(5000) && child.waitForReadyRead(5000)
                    && child.readLine() == "ready\n",
            "Migration crash fixture did not reach its uncommitted schema change");
    child.kill();
    require(child.waitForFinished(5000) && child.exitStatus() == QProcess::CrashExit,
            "Migration writer did not stop");
    QFile journal(root.path() + "/kobalt.sqlite-journal");
    require(journal.open(QIODevice::ReadOnly)
                    && journal.read(8) == QByteArray::fromHex("d9d505f920a163d7"),
            "Killed migration did not leave a hot rollback journal");
    journal.close();
    require(recoverProfile(root.path()),
            "Production open did not recover an interrupted migration");
    require(sql(root.path(), "PRAGMA user_version").toInt() == 3
                    && sql(root.path(),
                           "SELECT revision FROM book_state WHERE identity='preserved-book'")
                                    .toInt()
                            == 7
                    && sql(root.path(),
                           "SELECT payload FROM book_state WHERE identity='preserved-book'")
                                    .toString()
                            == "{\"position\":\"/body/p[2]\"}"
                    && sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 0,
            "Interrupted migration lost saved data or published a partial schema");
}

void batchFailureDoesNotPublishEarlierIntervals()
{

    QTemporaryDir root;
    ReadingHistory history(root.path(), "batch-book", {}, {}, "Batch", "Author");
    history.start(0, "/first");
    history.stop(100, "/first-end");
    history.start(100, "/second");
    history.stop(300, "/second-end");
    sql(root.path(),
        "CREATE TRIGGER fail_second BEFORE INSERT ON reading_sessions WHEN NEW.start_position='/second' BEGIN SELECT RAISE(ABORT,'test'); END");
    require(!history.save()
                    && sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 0,
            "Failed batch published its first interval");
    sql(root.path(), "DROP TRIGGER fail_second");
    require(history.save() && sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 2
                    && sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong()
                            == 300,
            "Batch retry lost or duplicated intervals");
}

// Keep the shared writer occupied so session snapshots are replaced before
// they can run. No scheduler timing is needed to exercise coalescing.
class Gate
{
public:
    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return open; });
    }

    void awaitEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, open = false;
};

void coalescedFailureRetainsEveryInterval()
{
    QTemporaryDir root;
    sql(root.path(),
        "CREATE TRIGGER fail_second BEFORE INSERT ON reading_sessions WHEN NEW.start_position='/second' BEGIN SELECT RAISE(ABORT,'test'); END");
    ReadingHistory history(root.path(), "queued-book", {}, {}, "Queued", "Author");
    Gate gate;
    storage::RecordWriter busy;
    busy.post(
            [&] {
                gate.wait();
                return true;
            },
            {});
    gate.awaitEntered();
    history.start(0, "/first");
    history.stop(100, "/first-end");
    require(history.save(true) && history.saving(), "Queued save waited for the occupied writer");
    history.start(100, "/second");
    history.stop(300, "/second-end");
    require(history.save(true), "Replacement snapshot was rejected");
    gate.release();
    storage::drainWriterThread();
    require(sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 0,
            "Failed coalesced snapshot published a partial interval batch");
    sql(root.path(), "DROP TRIGGER fail_second");
    require(history.save(true), "Retry of the same generation was suppressed");
    storage::drainWriterThread();
    // The successful snapshot is not acknowledged until after this new
    // interval is added. Acknowledgement must keep that newer interval.
    history.start(300, "/third");
    history.stop(600, "/third-end");
    require(history.save(true), "Async retry was rejected");
    storage::drainWriterThread();
    require(history.save() && sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 3
                    && sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong()
                            == 600,
            "Async retry lost or duplicated completed intervals");
}

void destructionDrainsLatestSnapshot()
{
    QTemporaryDir root;
    auto history = std::make_unique<ReadingHistory>(root.path(), "closing-book", QByteArray(),
                                                    QString(), "Closing", "Author");
    Gate gate;
    storage::RecordWriter busy;
    busy.post(
            [&] {
                gate.wait();
                return true;
            },
            {});
    gate.awaitEntered();
    history->start(0, "/body");
    require(history->save(true), "Queue initial snapshot");
    history->stop(700, "/end");
    require(history->save(true), "Queue final snapshot");
    std::thread release([&] { gate.release(); });
    history.reset();
    release.join();
    require(sql(root.path(), "SELECT active_ms FROM reading_sessions").toLongLong() == 700
                    && sql(root.path(), "SELECT status FROM reading_sessions").toString()
                            == "completed",
            "Destruction lost the latest queued boundary");
}

void sleepingObservations()
{
    QTemporaryDir root;
    qint64 utc = 100000;
    ReadingHistory history(root.path(), "sleep", {}, {}, "Sleep", "Author", [&] { return utc; });
    history.start(0, "start");
    utc += 1000;
    history.pause(1000, "sleep");
    require(history.save(), "Sleep fixture");
    const auto id = sql(root.path(), "SELECT id FROM reading_sessions");
    utc += 60000;
    history.pause(999999, "wrong");
    history.update(999999, "wrong");
    require(history.save()
                    && sql(root.path(), "SELECT active_ms FROM reading_sessions").toLongLong()
                            == 1000
                    && sql(root.path(), "SELECT observed FROM reading_sessions").toLongLong()
                            == 101000,
            "Repeated sleeping checkpoints changed the final observation");
    history.start(1000, "wake");
    require(history.save() && sql(root.path(), "SELECT id FROM reading_sessions") == id,
            "Resuming replaced the paused UUID");
    utc += 1000;
    history.pause(2000, "second-sleep");
    const auto observed = utc;
    utc += 900000;
    history.stop(2000, "wrong");
    sql(root.path(),
        "CREATE TRIGGER fail_sleep BEFORE UPDATE ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
    require(!history.save(), "Failed completion write succeeded");
    history.start(2000, "second-wake");
    history.stop(3000, "end");
    sql(root.path(), "DROP TRIGGER fail_sleep");
    require(history.save()
                    && sql(root.path(),
                           "SELECT COUNT(*) FROM reading_sessions WHERE status='completed'")
                                    .toInt()
                            == 2
                    && sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong()
                            == 3000
                    && sql(root.path(),
                           "SELECT observed FROM reading_sessions WHERE id='" + id.toString() + "'")
                                    .toLongLong()
                            == observed
                    && sql(root.path(),
                           "SELECT end_position FROM reading_sessions WHERE id='" + id.toString()
                                   + "'")
                                    .toString()
                            == "second-sleep",
            "Completion retry lost a sleeping interval or attached wake time/position");
}

void intervals()
{

    QTemporaryDir root;
    qint64 utc = 100000;
    const QString identity = "file:///cache/temporary.epub";
    const QByteArray fingerprint(64, 'a');
    const QString title = QString(QChar(0xfeff)) + QString::fromUtf8("Livre é") + QChar(0) + "tail";
    ReadingHistory history(root.path(), identity, fingerprint, "content://library/book", title,
                           "Author", [&] { return utc; });
    require(history.save(), "Empty save failed");
    history.start(500, "/body/p[1]");
    require(history.save(), "Initial interval save");
    utc += 2000;
    history.update(2500, "/body/p[2]");
    require(history.save() && history.save(), "Checkpoint interval");
    require(sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 1
                    && sql(root.path(), "SELECT active_ms FROM reading_sessions").toLongLong()
                            == 2000,
            "Repeated saves duplicated duration");
    require(sql(root.path(), "SELECT CAST(title AS BLOB) FROM reading_sessions").toByteArray()
                            == title.toUtf8()
                    && sql(root.path(), "SELECT source_key FROM reading_sessions").toString()
                            == "content://library/book",
            "History lost metadata or original Android source");
    utc -= 60000;
    history.stop(3500, "/body/p[3]");
    require(history.save(), "Stop interval");
    require(sql(root.path(), "SELECT observed FROM reading_sessions").toLongLong() == 102000
                    && sql(root.path(), "SELECT active_ms FROM reading_sessions").toLongLong()
                            == 3000
                    && sql(root.path(), "SELECT status FROM reading_sessions").toString()
                            == "completed",
            "Clock adjustment changed duration or chronology");
    utc = 1000000;
    history.start(3500, "/body/p[3]");
    history.stop(4500, "/body/p[4]");
    require(history.save(), "Resume interval");
    require(sql(root.path(), "SELECT COUNT(*) FROM reading_sessions").toInt() == 2
                    && sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong()
                            == 4000,
            "Resuming counted paused time");

    sql(root.path(),
        "CREATE TRIGGER fail_history BEFORE INSERT ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
    history.start(4500, "/body/p[4]");
    history.stop(6500, "/body/p[5]");
    require(!history.save(), "Failed history write succeeded");
    history.start(6500, "/body/p[5]");
    history.stop(7500, "/body/p[6]");
    sql(root.path(), "DROP TRIGGER fail_history");
    require(history.save() && history.save()
                    && sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong()
                            == 7000,
            "Retry lost or duplicated pending intervals");
    // An update failure must preserve the prior observation and retry the same UUID.
    history.start(7500, "/body/p[6]");
    require(history.save(), "Start before failed update");
    sql(root.path(),
        "CREATE TRIGGER fail_history BEFORE UPDATE ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
    history.update(8500, "/body/p[7]");
    require(!history.save(), "Failed update succeeded");
    require(sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong() == 7000,
            "Failed update leaked duration");
    sql(root.path(), "DROP TRIGGER fail_history");
    require(history.save(), "Retry failed update");
    const auto observed =
            sql(root.path(), "SELECT observed FROM reading_sessions WHERE status='active'");
    utc += 10000000;
    require(recoverProfile(root.path()) && recoverProfile(root.path()),
            "Recover interrupted interval");
    require(sql(root.path(), "SELECT COUNT(*) FROM reading_sessions WHERE status='interrupted'")
                                    .toInt()
                            == 1
                    && sql(root.path(),
                           "SELECT observed FROM reading_sessions WHERE status='interrupted'")
                            == observed
                    && sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong()
                            == 8000,
            "Recovery counted downtime or lost duration");
    history.update(9500, "/body/p[8]");
    require(!history.save(), "Stale writer revived interrupted session");

    const storage::Paths paths{ root.path() };
    require(storage::writeBookState(paths, identity, { { "book", identity } }), "Reset progress");
    library::Index catalogue;
    require(catalogue.loadDatabase(root.path()), "Initialize retained library fixture");
    library::Book entry;
    entry.key = "content://library/book";
    entry.name = "history.epub";
    entry.title = "History";
    catalogue.books.insert(entry.key, entry);
    require(catalogue.saveDatabase(root.path()), "Library history fixture");
    catalogue.removeBook(entry.key);
    require(catalogue.saveDatabase(root.path()), "Remove library book");
    require(sql(root.path(), "SELECT SUM(active_ms) FROM reading_sessions").toLongLong() == 8000,
            "Reset progress or library removal erased history");
    ReadingHistory changed(root.path(), identity, QByteArray(64, 'b'), {}, "Changed", "Author",
                           [&] { return utc; });
    changed.start(0, "/body");
    changed.stop(100, "/body");
    require(changed.save(), "Changed edition save");
    require(sql(root.path(), "SELECT COUNT(DISTINCT book) FROM reading_sessions").toInt() == 2,
            "Different editions were merged");
}
} // namespace

// Checks reading history: the version-2 upgrade, profile ownership and crash recovery, failed commits and retries, clock changes and editions.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    if (app.arguments().size() == 3 && app.arguments()[1] == "--claim-profile") {
        QString error;
        const auto owner = ReadingHistory::openProfile(app.arguments()[2], &error);
        return owner ? 0 : error == "profile-in-use" ? 23 : 24;
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--hold-migration") {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "interrupted-migration");
        db.setDatabaseName(app.arguments()[2] + "/kobalt.sqlite");
        require(db.open(), "Open interrupted migration fixture");
        QSqlQuery query(db);
        // Force dirty pages out before COMMIT, leaving a rollback journal for
        // the production connection to recover after the process is killed.
        for (const auto *statement :
             { "PRAGMA cache_size=1", "BEGIN IMMEDIATE",
               "CREATE TABLE reading_sessions(partial TEXT)", "PRAGMA user_version=3",
               "INSERT INTO reading_sessions VALUES(zeroblob(65536))" })
            require(query.exec(statement), "Uncommitted migration fixture");
        QSocketNotifier parentInput(STDIN_FILENO, QSocketNotifier::Read);
        QObject::connect(&parentInput, &QSocketNotifier::activated, &app, [&] {
            char byte;
            if (::read(STDIN_FILENO, &byte, 1) <= 0)
                app.quit();
        });
        std::puts("ready");
        std::fflush(stdout);
        return app.exec();
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--hold-active-profile") {
        const auto owner = ReadingHistory::openProfile(app.arguments()[2]);
        require(bool(owner), "Child profile ownership");
        ReadingHistory history(app.arguments()[2], "crash-book", {}, {}, "Crash", "Author",
                               [] { return qint64(10000); });
        history.start(0, "/body");
        history.update(2000, "/body/p[2]");
        require(history.save(), "Child committed checkpoint");
        // The parent's QProcess owns stdin's write end. If an assertion aborts
        // the parent, EOF must stop this helper instead of leaving an orphan.
        QSocketNotifier parentInput(STDIN_FILENO, QSocketNotifier::Read);
        QObject::connect(&parentInput, &QSocketNotifier::activated, &app, [&] {
            char byte;
            if (::read(STDIN_FILENO, &byte, 1) <= 0)
                app.quit();
        });
        std::puts("ready");
        std::fflush(stdout);

        return app.exec();
    }
    migration();

    rejectsVersionTwoWithoutBookIdentityUniqueness();
    migrationRejectsOrphanedAnnotations();
    intervals();
    sleepingObservations();
    coalescedFailureRetainsEveryInterval();
    destructionDrainsLatestSnapshot();
    crashRecoveryDoesNotStealLiveProfileOwnership();
    interruptedOwnershipMarkerCreationDoesNotBlockProfile();
    unsafeOwnershipMarkersAreRefused();
    failedRecoveryReleasesOwnershipForRetry();
    commitFailureRetainsPendingDuration();
    migrationCommitFailureLeavesVersionTwoRetryable();
    killedMigrationLeavesOldDataRecoverable();
    batchFailureDoesNotPublishEarlierIntervals();
    storage::closeProfile();
    require(QSqlDatabase::connectionNames().isEmpty(), "History checks left SQL connections open");
}
