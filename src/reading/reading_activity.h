#pragma once
#include "storage/activity_records.h"
#include <QDate>
#include <QMap>
#include <QString>
#include <QVector>

namespace kobalt::activity {
// A day counts as a reading day from one minute of reading.
inline constexpr qint64 readingDayMinimum = 60 * 1000;
// A session counts from two minutes of reading. Shorter ones, such as opening
// a book to look something up, add their time to the day but are not listed
// or counted as sessions.
inline constexpr qint64 sessionMinimum = 2 * 60 * 1000;
bool countsAsSession(const ActivitySession &session);

struct Streak
{
    int days = 0;
    QDate first, last;
};

// One book's reading in a period.
struct BookTotal
{
    QString book, identity, sourceKey, title, author;
    qint64 milliseconds = 0;
    int sessions = 0;
    // UTC milliseconds of the first session's start and the last observation.
    qint64 first = 0, last = 0;
    // The saved percent, or -1 when the book has no saved record.
    int percent = -1;
    bool finished = false;
};

struct MonthBooks
{
    // The first day of the month.
    QDate month;
    qint64 milliseconds = 0;
    // Most reading first.
    QVector<BookTotal> books;
};

struct Milestone
{
    enum class Kind { Books, Hours, Streak, DaysRead };
    Kind kind = Kind::Books;
    int target = 0, value = 0;

    bool reached() const { return value >= target; }
};

struct Summary
{
    QDate today;
    // Active milliseconds per local date: the daily totals, with sessions
    // filling dates from before daily totals were saved.
    QMap<QDate, qint64> days;
    qint64 todayMilliseconds = 0;
    int todaySessions = 0, todayBooks = 0;
    // The current streak ends today or, while today has no reading yet,
    // yesterday.
    Streak current, longest;
    // Monday to Sunday of this week; -1 for a day after today.
    QVector<qint64> week;
    qint64 weekMilliseconds = 0;
    // The first day with reading, or an invalid date without any.
    QDate since;
    qint64 totalMilliseconds = 0, averageSession = 0;
    int sessions = 0, daysRead = 0, daysSince = 0;
    int finished = 0, inProgress = 0, books = 0, booksThisMonth = 0;
    int words = 0, wordsThisWeek = 0;
    // Newest month first.
    QVector<MonthBooks> months;
    // Active milliseconds by local hour of the day, 0 to 23.
    QVector<qint64> hours;
    // The longest single session and the day with the most reading.
    qint64 longestSession = 0;
    QDate longestSessionDate;
    QString longestSessionTitle;
    qint64 bestDay = 0;
    QDate bestDayDate;
    int bestDayBooks = 0;
    QVector<Milestone> milestones;
};

// `unsaved` holds the open book's daily totals that the records left out.
Summary summarize(const ActivityRecords &records, const QDate &today,
                  const QMap<QDate, qint64> &unsaved = {});

// One book's sessions, newest first, its active milliseconds per local date
// and its totals, up to `today`.
QVector<ActivitySession> bookSessions(const ActivityRecords &records, const QString &book,
                                      const QDate &today);
QMap<QDate, qint64> bookDays(const ActivityRecords &records, const QString &book,
                             const QDate &today);
BookTotal bookTotal(const ActivityRecords &records, const QString &book, const QDate &today);

// The local date of a UTC time in milliseconds.
QDate localDate(qint64 utcMilliseconds);
// Monday of the week that contains `date`.
QDate weekStart(const QDate &date);
} // namespace kobalt::activity
