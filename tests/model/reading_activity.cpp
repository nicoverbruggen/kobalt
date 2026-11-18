#include "reading/reading_activity.h"
#include "storage/activity_records.h"
#include "storage/reading_history.h"
#include "storage/profile_database.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
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

constexpr qint64 minute = 60 * 1000;

// Local times, so the dates hold in every time zone.
qint64 at(const QDate &date, int hour, int minuteOfHour = 0)
{
    return QDateTime(date, QTime(hour, minuteOfHour)).toMSecsSinceEpoch();
}

ActivitySession session(const QString &book, const QDate &date, int hour, qint64 minutes,
                        const QString &title = {})
{
    ActivitySession value;
    value.book = book;
    value.identity = "file:///books/" + book + ".epub";
    value.title = title.isEmpty() ? book : title;
    value.author = "Author";
    value.started = at(date, hour);
    value.milliseconds = minutes * minute;
    value.observed = value.started + value.milliseconds;
    return value;
}

const QDate today(2026, 11, 14);

// Daily totals win over sessions; dates without totals use their sessions.
void daysAndFallback()
{
    ActivityRecords records;
    records.sessions = { session("a", QDate(2026, 9, 3), 20, 30), session("a", today, 8, 10) };
    records.days.insert(today, 25 * minute);
    const auto summary = activity::summarize(records, today, { { today, 5 * minute } });
    require(summary.days.value(QDate(2026, 9, 3)) == 30 * minute,
            "A date before daily totals lost its sessions");
    require(summary.todayMilliseconds == 30 * minute,
            "Today did not add the open book's unsaved time to the saved totals");
    require(summary.totalMilliseconds == 60 * minute, "The total did not follow the days");
    require(summary.since == QDate(2026, 9, 3) && summary.daysSince == 73,
            "The first reading day was wrong");
    require(summary.todaySessions == 1 && summary.todayBooks == 1, "Today's sessions were wrong");
}

void matchingBooks()
{
    ActivityRecords records;
    records.sessions = { session("file-a", today, 7, 30, "The Fortress"),
                         session("file-b", today, 9, 20, "The Fortress"),
                         session("native-c", today, 10, 10, "The Fortress"),
                         session("file-d", today, 11, 5, "The Fortress") };
    records.sessions.last().author = "Different Author";
    const auto summary = activity::summarize(records, today);
    const auto &month = summary.months.first();
    require(summary.books == 2 && summary.booksThisMonth == 2 && summary.todayBooks == 2
                    && month.books.size() == 2,
            "Matching titles and authors should count as one book");
    const auto &first = month.books.first();
    require(first.book == "native-c" && first.milliseconds == 60 * minute && first.sessions == 3
                    && month.copies.value(first.book).size() == 3,
            "The monthly row lost time or copy identities");
    records.hidden.insert("file-b");
    const auto hidden = activity::summarize(records, today);
    require(hidden.months.first().books.first().milliseconds == 40 * minute
                    && hidden.hiddenBooks.first().book == "file-b" && hidden.books == 2,
            "Hiding one copy changed the totals of another");
    for (auto &item : records.sessions)
        item.author.clear();
    require(activity::summarize(records, today).books == 4,
            "Missing authors must not group unrelated books");
}

// A streak survives a day that has not had reading yet, and breaks after it.
void streaks()
{
    ActivityRecords records;
    for (int day = 1; day <= 5; ++day)
        records.days.insert(QDate(2026, 10, day), 20 * minute);
    for (int day = 10; day <= 13; ++day)
        records.days.insert(QDate(2026, 11, day), 20 * minute);
    records.days.insert(QDate(2026, 11, 9), 30 * 1000);
    auto summary = activity::summarize(records, today);
    require(summary.current.days == 4 && summary.current.first == QDate(2026, 11, 10),
            "Yesterday's streak ended before today's reading");
    require(summary.longest.days == 5 && summary.longest.first == QDate(2026, 10, 1),
            "The longest streak was wrong");
    require(summary.daysRead == 9, "Half a minute counted as a reading day");
    summary = activity::summarize(records, today.addDays(1));
    require(summary.current.days == 0, "A streak survived a whole day without reading");
}

void weekAndMonths()
{
    ActivityRecords records;
    records.sessions = { session("a", QDate(2026, 10, 30), 21, 40, "October"),
                         session("a", QDate(2026, 11, 9), 21, 30, "November"),
                         session("b", QDate(2026, 11, 10), 21, 50),
                         session("a", today, 7, 20, "November") };
    records.sessions.last().identity = "file:///books/a-renamed.epub";
    records.sessions.last().sourceKey = "/books/a-renamed.epub";
    records.percent.insert("file:///books/a-renamed.epub", 22);
    records.percent.insert("file:///books/b.epub", 100);
    records.finished.insert("file:///books/b.epub");
    const auto summary = activity::summarize(records, today);
    require(summary.week.size() == 7 && summary.week[0] == 30 * minute
                    && summary.week[1] == 50 * minute && summary.week[5] == 20 * minute
                    && summary.week[6] == -1,
            "The week did not run from Monday with the future left empty");
    require(summary.weekMilliseconds == 100 * minute, "The week total was wrong");
    require(summary.months.size() == 2 && summary.months[0].month == QDate(2026, 11, 1),
            "Months were not newest first");
    const auto &october = summary.months[1].books.first();
    require(october.identity == records.sessions.last().identity
                    && october.sourceKey == records.sessions.last().sourceKey
                    && october.percent == 22 && october.milliseconds == 40 * minute
                    && october.sessions == 1,
            "October must use the renamed book without including November's reading time");
    const auto &november = summary.months[0];
    require(november.books.size() == 2 && november.books[0].book == "a"
                    && november.books[0].milliseconds == 50 * minute
                    && november.books[0].sessions == 2 && november.books[0].percent == 22,
            "November's books were wrong");
    require(november.books[1].finished, "A finished book lost its mark");
    require(summary.finished == 1 && summary.inProgress == 1 && summary.books == 2
                    && summary.booksThisMonth == 2,
            "Book counts were wrong");
    require(summary.longestSession == 50 * minute && summary.longestSessionTitle == "b",
            "The longest session was wrong");
}

// A session that crosses the hour fills both hours.
void hours()
{
    ActivityRecords records;
    auto value = session("a", today, 21, 0);
    value.started = at(today, 21, 40);
    value.milliseconds = 50 * minute;
    records.sessions = { value };
    const auto summary = activity::summarize(records, today);
    require(summary.hours[21] == 20 * minute && summary.hours[22] == 30 * minute,
            "A session did not spread over the hours it covered");
}

// A session under two minutes keeps its time on the day but is not counted.
void shortSessions()
{
    ActivityRecords records;
    records.sessions = { session("a", today, 8, 10), session("b", today, 9, 1) };
    const auto summary = activity::summarize(records, today);
    require(summary.todayMilliseconds == 11 * minute, "A short session lost its time");
    require(summary.sessions == 1 && summary.todaySessions == 1 && summary.todayBooks == 1,
            "A short session counted as a session");
    require(activity::bookSessions(records, "b", today).isEmpty()
                    && activity::bookTotal(records, "b", today).sessions == 0,
            "A short session was listed for its book");
}

void milestones()
{
    ActivityRecords records;
    for (int day = 0; day < 8; ++day)
        records.days.insert(today.addDays(-day), 2 * 3600 * 1000);
    records.finished.insert("x");
    const auto summary = activity::summarize(records, today);
    int reached = 0;
    for (const auto &milestone : summary.milestones)
        reached += milestone.reached();
    // One book, 10 hours and a 7-day streak.
    require(reached == 3, "Milestones were not reached as expected");
}

QVariant sql(const QString &directory, const QString &statement, const QVariantList &values = {})
{
    QVariant value;
    require(storage::profileTransaction(directory,
                                        [&](QSqlDatabase &db) {
                                            QSqlQuery query(db);
                                            query.prepare(statement);
                                            for (const auto &item : values)
                                                query.addBindValue(item);
                                            if (!query.exec())
                                                return false;
                                            if (query.next())
                                                value = query.value(0);
                                            return true;
                                        }),
            "Activity SQL fixture");
    return value;
}

// The store reads the profile on the writer thread and leaves out the
// excluded opening's daily totals.
void storeReadsProfile()
{
    QTemporaryDir root;
    const auto directory = root.path();
    const auto started = at(QDate(2026, 11, 13), 21);
    sql(directory,
        "INSERT INTO reading_sessions(id,book,identity,fingerprint,source_key,title,author,started,"
        "observed,active_ms,start_position,end_position,status) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        { "s1", "book-a", "file:///a.epub", QString(64, 'a'), "/a.epub", "War and Peace",
          "Leo Tolstoy", started, started + 48 * minute, 48 * minute, "/body/p[1]", "/body/p[9]",
          "completed" });
    for (const auto &name : { QStringLiteral("reading-day:2026-11-13:open-1"),
                              QStringLiteral("reading-day:2026-11-13:open-2"),
                              QStringLiteral("reading-day:2026-11-14:open-2") })
        sql(directory, "INSERT INTO profile_state(name,format,revision,payload) VALUES(?,1,0,?)",
            { name, QStringLiteral("{\"milliseconds\":600000}") });
    sql(directory, "INSERT INTO book_state(identity,format,revision,payload) VALUES(?,1,0,?)",
        { "file:///a.epub", QStringLiteral("{\"percent\":22,\"finished_position\":\"\"}") });
    sql(directory, "INSERT INTO book_state(identity,format,revision,payload) VALUES(?,1,0,?)",
        { "file:///b.epub", QStringLiteral("{\"percent\":100,\"finished_position\":\"/end\"}") });

    ActivityRecordStore store(directory);
    bool done = false, ok = false;
    ActivityRecords records;
    store.load("open-2", {}, [&](bool success, ActivityRecords result) {
        ok = success;
        records = std::move(result);
        done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(done && ok, "Activity records did not load");
    require(records.sessions.size() == 1 && records.sessions[0].title == "War and Peace"
                    && records.sessions[0].milliseconds == 48 * minute,
            "The session did not load");
    require(records.days.size() == 1 && records.days.value(QDate(2026, 11, 13)) == 10 * minute,
            "The excluded opening's daily totals were counted");
    require(records.percent.value("file:///a.epub") == 22 && records.finished.size() == 1
                    && records.finished.contains("file:///b.epub"),
            "Book progress or the finished mark did not load");
}

void wait(const bool &done)
{
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(done, "The Activity store did not answer");
}

ActivityRecords load(ActivityRecordStore &store)
{
    bool done = false, ok = false;
    ActivityRecords records;
    store.load({}, {}, [&](bool success, ActivityRecords result) {
        ok = success;
        records = std::move(result);
        done = true;
    });
    wait(done);
    require(ok, "Activity records did not load");
    return records;
}

// Hiding takes a book out of the lists only. Deleting removes its sessions
// and their time from the daily totals, but never from the open opening's.
void hideAndRemove()
{
    storage::closeProfile();
    QTemporaryDir root;
    const auto directory = root.path();
    const auto insert = [&](const char *id, const char *book, const QDate &date, int hour,
                            int minutes) {
        const auto started = at(date, hour);
        sql(directory,
            "INSERT INTO reading_sessions(id,book,identity,fingerprint,source_key,title,author,"
            "started,observed,active_ms,start_position,end_position,status) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,'completed')",
            { id, book, QString("file:///%1.epub").arg(book), QString(64, 'a'), "/x.epub",
              QString("Title %1").arg(book), "Author", started, started + minutes * minute,
              minutes * minute, "/body/p[1]", "/body/p[2]" });
    };
    insert("s1", "a", QDate(2026, 11, 10), 21, 30);
    insert("s2", "a", QDate(2026, 11, 11), 20, 20);
    insert("s3", "a", QDate(2026, 11, 12), 20, 40);
    insert("s4", "b", QDate(2026, 11, 10), 8, 15);
    const auto day = [&](const char *name, int minutes) {
        sql(directory, "INSERT INTO profile_state(name,format,revision,payload) VALUES(?,1,0,?)",
            { QString("reading-day:%1").arg(name),
              QString("{\"milliseconds\":%1}").arg(minutes * minute) });
    };
    day("2026-11-10:op-a", 30);
    day("2026-11-10:op-b", 15);
    day("2026-11-10:op-keep", 5);
    day("2026-11-11:op-a", 20);
    // Only the open opening has a total on this date; it is never changed.
    day("2026-11-12:op-keep", 40);

    ActivityRecordStore store(directory);
    bool done = false, ok = false;
    store.hide("b", true, [&](bool success) {
        ok = success;
        done = true;
    });
    wait(done);
    require(ok, "Hiding a book failed");
    auto records = load(store);
    require(records.hidden == QSet<QString>{ "b" }, "The hidden book did not load");
    auto summary = activity::summarize(records, today);
    for (const auto &month : summary.months)
        for (const auto &book : month.books)
            require(book.book != "b", "A hidden book stayed in the lists");
    require(summary.hiddenBooks.size() == 1 && summary.hiddenBooks[0].book == "b"
                    && summary.hiddenBooks[0].milliseconds == 15 * minute,
            "The hidden book is not listed with its total");
    require(summary.days.value(QDate(2026, 11, 10)) == 50 * minute,
            "Hiding a book changed the daily totals");

    done = ok = false;
    store.remove("a", "op-keep", [&](bool success) {
        ok = success;
        done = true;
    });
    wait(done);
    require(ok, "Deleting a book's activity failed");
    records = load(store);
    require(records.sessions.size() == 1 && records.sessions[0].book == "b",
            "The deleted book's sessions remain");
    require(records.days.value(QDate(2026, 11, 10)) == 20 * minute
                    && !records.days.contains(QDate(2026, 11, 11)),
            "The deleted time stayed in the daily totals");
    require(records.days.value(QDate(2026, 11, 12)) == 40 * minute,
            "The open opening's total was changed");
    require(records.hidden == QSet<QString>{ "b" }, "Deleting a book changed another's hiding");

    done = ok = false;
    store.hide("b", false, [&](bool success) {
        ok = success;
        done = true;
    });
    wait(done);
    require(ok && load(store).hidden.isEmpty(), "Showing a hidden book again failed");
    require(sql(directory, "SELECT count(*) FROM profile_state WHERE name='activity-hidden-books'")
                            .toInt()
                    == 0,
            "An empty hidden list was kept");
}

// Deleting splits a session's time at midnight as the reader does, keeps the
// time of other books on a date from before daily totals, and runs even when
// loads are posted around it. An unreadable hidden list is never replaced.
void deleteEdgeCases()
{
    storage::closeProfile();
    QTemporaryDir root;
    const auto directory = root.path();
    const auto insert = [&](const char *id, const char *book, qint64 started, int minutes) {
        sql(directory,
            "INSERT INTO reading_sessions(id,book,identity,fingerprint,source_key,title,author,"
            "started,observed,active_ms,start_position,end_position,status) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,'completed')",
            { id, book, QString("file:///%1.epub").arg(book), QString(64, 'a'), "/x.epub",
              QString("Title %1").arg(book), "Author", started, started + minutes * minute,
              minutes * minute, "/body/p[1]", "/body/p[2]" });
    };
    const auto day = [&](const QString &name, int minutes) {
        sql(directory, "INSERT INTO profile_state(name,format,revision,payload) VALUES(?,1,0,?)",
            { "reading-day:" + name, QString("{\"milliseconds\":%1}").arg(minutes * minute) });
    };
    // Book c reads from 23:40 to 00:20; book a reads 30 minutes earlier that day.
    insert("c1", "c", at(QDate(2026, 11, 20), 23, 40), 40);
    insert("a1", "a", at(QDate(2026, 11, 20), 20), 30);
    day("2026-11-20:op-a", 30);
    day("2026-11-20:op-c", 20);
    day("2026-11-21:op-c", 20);
    // A date from before daily totals: only book y's later opening has one.
    insert("x1", "x", at(QDate(2026, 11, 5), 9), 25);
    insert("y1", "y", at(QDate(2026, 11, 5), 21), 15);
    day("2026-11-05:op-y", 15);
    // A hidden list from a newer build.
    sql(directory, "INSERT INTO profile_state(name,format,revision,payload) VALUES(?,2,0,?)",
        { "activity-hidden-books", QStringLiteral("{\"books\":[\"q\"],\"more\":1}") });

    ActivityRecordStore store(directory);
    int removals = 0, hides = 0;
    bool removedOk = false, hiddenOk = true;
    store.remove("c", "op-keep", [&](bool ok) {
        removedOk = ok;
        ++removals;
    });
    // A load posted between edits must not replace them.
    bool loaded = false;
    store.load({}, {}, [&](bool, ActivityRecords) { loaded = true; });
    store.remove("x", "op-keep", [&](bool ok) {
        removedOk = removedOk && ok;
        ++removals;
    });
    store.hide("a", true, [&](bool ok) {
        hiddenOk = ok;
        ++hides;
    });
    QElapsedTimer timer;
    timer.start();
    while ((removals < 2 || hides < 1 || !loaded) && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(removals == 2 && hides == 1 && loaded, "A load replaced a pending edit");
    require(removedOk, "Deleting failed");
    require(!hiddenOk, "Hiding wrote over an unreadable hidden list");
    const auto records = load(store);
    require(records.days.value(QDate(2026, 11, 20)) == 30 * minute
                    && !records.days.contains(QDate(2026, 11, 21)),
            "A session past midnight was not split by date");
    require(records.days.value(QDate(2026, 11, 5)) == 15 * minute,
            "Another book's time was taken on a date before daily totals");
    require(records.hidden.isEmpty(), "An unreadable hidden list hid books");
    require(sql(directory, "SELECT format FROM profile_state WHERE name='activity-hidden-books'")
                            .toInt()
                    == 2,
            "The newer hidden list was replaced");
}

// A beta 8 session and a new opening share the upgrade date. The new
// session sleeps over midnight, then resumes on the first of the month.
void upgradeAndSleep()
{
    storage::closeProfile();
    QTemporaryDir root;
    const QDate first(2026, 10, 31), second = first.addDays(1);
    const auto directory = root.path();
    const auto morning = at(first, 10);
    sql(directory,
        "INSERT INTO reading_sessions VALUES('old','older','older','','','','',?,?,?,'','','completed')",
        { morning, morning + 30 * minute, 30 * minute });
    qint64 now = at(first, 23, 40);
    ReadingHistory history(directory, "current", {}, {}, "New book", {}, [&] { return now; });
    history.start(0, "before");
    now += 10 * minute;
    history.pause(10 * minute, "middle");
    require(history.save(), "Could not save reading before midnight");
    now = at(second, 0, 5);
    history.start(10 * minute, "middle");
    now += 5 * minute;
    history.stop(15 * minute, "after");
    require(history.save(), "Could not save reading after midnight");
    ActivityRecords records;
    require(ActivityRecordStore::read(directory, {}, &records), "Cannot load both openings");
    const auto summary = activity::summarize(records, second);
    require(summary.days.value(first) == 40 * minute && summary.days.value(second) == 5 * minute,
            "Upgrade-day or post-midnight time was lost");
    require(summary.hours[23] == 10 * minute && summary.hours[0] == 5 * minute,
            "Sleep moved reading into the wrong hour");
    const auto book = records.sessions.last().book;
    const auto days = activity::bookDays(records, book, second);
    require(days.value(first) == 10 * minute && days.value(second) == 5 * minute
                    && summary.months.first().milliseconds == 5 * minute,
            "The book's time went to the wrong date or month");
    require(ActivityRecordStore::read(directory, history.openingId(), &records),
            "Could not load the open book's history");
    require(activity::summarize(records, second, days).days == summary.days,
            "The open book's time was counted twice");
    require(ActivityRecordStore::remove(directory, book, QString()),
            "Could not delete the new book");
    require(ActivityRecordStore::read(directory, {}, &records) && records.days.isEmpty()
                    && records.sessions.size() == 1
                    && activity::summarize(records, second).totalMilliseconds == 30 * minute,
            "Deletion changed the other book or left part of the deleted book");
}

void deleteAnonymousOvernight(bool survivorHasHourlyRecords)
{
    storage::closeProfile();
    QTemporaryDir root;
    const auto day = QDate(2026, 11, 1);
    sql(root.path(),
        "INSERT INTO reading_sessions VALUES('one','only','only','','','','',?,?,?,'','','completed')",
        { at(day, 23, 40), at(day.addDays(1), 0, 10), 15 * minute });
    for (int i : { 0, 1 })
        sql(root.path(), "INSERT INTO profile_state VALUES(?,1,0,?)",
            { "reading-day:" + day.addDays(i).toString(Qt::ISODate) + ":anonymous",
              QString("{\"milliseconds\":%1}").arg((i ? 5 : 10) * minute) });
    sql(root.path(),
        "INSERT INTO reading_sessions VALUES('two','other','other','','','','',?,?,?,'','','completed')",
        { at(day, 9), at(day, 9, 20), 20 * minute });
    if (survivorHasHourlyRecords)
        sql(root.path(), "INSERT INTO profile_state VALUES(?,1,0,?)",
            { "reading-hours:two:" + day.toString(Qt::ISODate),
              QStringLiteral("{\"9\":1200000}") });
    const auto survivor = survivorHasHourlyRecords
            ? QStringLiteral("{\"milliseconds\":1200000,\"book\":\"other\"}")
            : QStringLiteral("{\"milliseconds\":1200000}");
    sql(root.path(), "INSERT INTO profile_state VALUES(?,1,0,?)",
        { "reading-day:" + day.toString(Qt::ISODate) + ":other", survivor });
    require(ActivityRecordStore::remove(root.path(), "only", QString()),
            "Cannot remove overnight history");
    ActivityRecords records;
    require(ActivityRecordStore::read(root.path(), {}, &records) && records.days.size() == 1
                    && records.days.value(day) == 20 * minute,
            "Deleting the overnight book should keep only the other book's time");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    daysAndFallback();
    matchingBooks();
    streaks();
    weekAndMonths();
    hours();
    shortSessions();
    milestones();
    storeReadsProfile();
    hideAndRemove();
    deleteEdgeCases();
    upgradeAndSleep();
    deleteAnonymousOvernight(true);
    deleteAnonymousOvernight(false);
    storage::closeProfile();
    return 0;
}
