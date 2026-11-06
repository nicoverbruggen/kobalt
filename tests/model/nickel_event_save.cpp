#include "platforms/kobo/common/native_database_wait.h"
#include "platforms/kobo/common/native_event_save.h"
#include "check.h"
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QSemaphore>
#include <thread>
#include <utility>

// Checks the event flush against a competing SQLite writer: kept buffers, bounded recovery, timeout and rollback.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "Event-save fixture directory");
    const auto path = directory.filePath("events.sqlite");
    auto connection = QSqlDatabase::addDatabase("QSQLITE", "events");
    connection.setDatabaseName(path);
    require(connection.open(), "Open event-save database");
    QSqlQuery query(connection);
    require(query.exec("CREATE TABLE events (id INTEGER PRIMARY KEY)"),
            "Create event-save fixture");
    require(query.exec("PRAGMA busy_timeout=17"), "Set fixture timeout");
    int buffered = 1, flushes = 0, ends = 0;
    const auto save = [&](int timeout) {
        kobalt::NativeDatabaseWait wait(connection, timeout);
        require(wait.ready(), "Set bounded event wait");
        return kobalt::nickel::saveNativeEvents(
                [&] { return query.exec("BEGIN IMMEDIATE"); },
                [&] {
                    ++flushes;
                    // Match Nickel's destructive buffer consumption, including on failure.
                    const int pending = std::exchange(buffered, 0);
                    return !pending || query.exec("INSERT INTO events VALUES (1)");
                },
                [&](bool commit) {
                    ++ends;
                    return query.exec(commit ? "COMMIT" : "ROLLBACK");
                });
    };
    QSemaphore locked, release;
    std::thread writer([&] {
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "writer");
            db.setDatabaseName(path);
            require(db.open(), "Open competing writer");
            QSqlQuery lock(db);
            require(lock.exec("BEGIN IMMEDIATE"), "Hold competing transaction");
            locked.release();
            release.acquire();
            require(lock.exec("ROLLBACK"), "Release competing transaction");
        }
        QSqlDatabase::removeDatabase("writer");
    });
    locked.acquire();
    require(!save(0), "A locked event transaction unexpectedly began");
    require(buffered == 1 && flushes == 0 && ends == 0,
            "Busy save consumed pending events or ended another writer's transaction");
    std::thread unlock([&] {
        QThread::msleep(100);
        release.release();
    });
    require(save(2000), "Bounded worker wait did not recover from transient contention");
    unlock.join();
    writer.join();
    require(buffered == 0 && flushes == 1 && ends == 1, "Event save was lost or repeated");
    require(query.exec("PRAGMA busy_timeout") && query.next() && query.value(0).toInt() == 17,
            "Event wait changed Nickel's connection timeout");
    query.finish();
    require(query.exec("BEGIN IMMEDIATE"), "Begin existing native transaction");
    require(query.exec("INSERT INTO events VALUES (2)"), "Write existing transaction");
    require(!save(0) && flushes == 1 && ends == 1,
            "Event save joined or committed a transaction it did not own");
    require(query.exec("ROLLBACK"), "Existing transaction lost ownership");
    require(query.exec("SELECT COUNT(*) FROM events") && query.next()
                    && query.value(0).toInt() == 1,
            "Failed transaction ownership changed saved events");
    query.finish();
    // BEGIN IMMEDIATE can succeed while another connection is reading, but
    // COMMIT must wait for that reader. A failed commit must release our lock.
    {
        auto reader = QSqlDatabase::addDatabase("QSQLITE", "late-event-reader");
        reader.setDatabaseName(path);
        require(reader.open(), "Open reader across event commit");
        QSqlQuery reading(reader);
        require(reading.exec("BEGIN") && reading.exec("SELECT * FROM events") && reading.next(),
                "Hold a shared lock across event commit");
        require(!kobalt::nickel::saveNativeEvents(
                        [&] { return query.exec("BEGIN IMMEDIATE"); },
                        [&] { return query.exec("INSERT INTO events VALUES (3)"); },
                        [&](bool commit) { return query.exec(commit ? "COMMIT" : "ROLLBACK"); }),
                "A shared reader unexpectedly allowed the event commit");
        reading.finish();
        require(reading.exec("ROLLBACK"), "Release event reader");
        reader.close();
    }
    QSqlDatabase::removeDatabase("late-event-reader");
    require(save(0), "Failed event commit left a transaction that blocked every retry");
    require(query.exec("SELECT COUNT(*) FROM events WHERE id=3") && query.next()
                    && query.value(0).toInt() == 0,
            "A failed event commit was not rolled back");
    query.finish();
    bool rolledBack = false;
    require(!kobalt::nickel::saveNativeEvents([] { return true; }, [] { return false; },
                                              [&](bool commit) {
                                                  rolledBack = !commit;
                                                  return true;
                                              })
                    && rolledBack,
            "Failed event flush was committed");
}
