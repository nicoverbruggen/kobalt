#include "reading_activity.h"
#include <QDateTime>
#include <QSet>
#include <algorithm>

namespace kobalt::activity {
QDate localDate(qint64 utcMilliseconds)
{
    return QDateTime::fromMSecsSinceEpoch(utcMilliseconds).date();
}

bool countsAsSession(const ActivitySession &session)
{
    return session.milliseconds >= sessionMinimum;
}

QDate weekStart(const QDate &date)
{
    return date.addDays(1 - date.dayOfWeek());
}

namespace {
QDate monthOf(const QDate &date)
{
    return QDate(date.year(), date.month(), 1);
}

void add(BookTotal &total, const ActivitySession &session, qint64 milliseconds)
{
    if (!total.sessions) {
        total.book = session.book;
        total.first = session.started;
    }
    // The newest snapshot names and finds the book, as it is shown now.
    total.identity = session.identity;
    total.sourceKey = session.sourceKey;
    total.title = session.title;
    total.author = session.author;
    total.milliseconds += milliseconds;
    ++total.sessions;
    total.first = qMin(total.first, session.started);
    total.last = qMax(total.last, session.observed);
}

void describe(BookTotal &total, const ActivityRecords &records)
{
    total.percent = records.percent.value(total.identity, -1);
    total.finished = records.finished.contains(total.identity);
}

// Reading days in order, and the longest run of consecutive ones.
QVector<Streak> streaks(const QMap<QDate, qint64> &days)
{
    QVector<Streak> runs;
    for (auto it = days.cbegin(); it != days.cend(); ++it) {
        if (it.value() < readingDayMinimum)
            continue;
        if (!runs.isEmpty() && runs.last().last.addDays(1) == it.key()) {
            runs.last().last = it.key();
            ++runs.last().days;
        } else {
            runs.append({ 1, it.key(), it.key() });
        }
    }
    return runs;
}

// Older sessions only recorded their total duration. Use their starting
// time to estimate the local hours when actual observations are unavailable.
template <typename Visit>
void visitHours(const ActivitySession &session, Visit visit)
{
    if (!session.hours.isEmpty()) {
        for (auto day = session.hours.cbegin(); day != session.hours.cend(); ++day)
            for (auto hour = day.value().cbegin(); hour != day.value().cend(); ++hour)
                visit(day.key(), hour.key(), hour.value());
        return;
    }
    qint64 left = session.milliseconds;
    auto cursor = QDateTime::fromMSecsSinceEpoch(session.started);
    while (left > 0) {
        const auto time = cursor.time();
        const qint64 hourLeft =
                qint64(60 - time.minute()) * 60000 - time.second() * 1000 - time.msec();
        const qint64 used = qMax<qint64>(1, qMin(left, hourLeft));
        visit(cursor.date(), time.hour(), used);
        left -= used;
        cursor = cursor.addMSecs(used);
    }
}
} // namespace

Summary summarize(const ActivityRecords &records, const QDate &today,
                  const QMap<QDate, qint64> &unsaved)
{
    Summary summary;
    summary.today = today;
    summary.hours = QVector<qint64>(24, 0);
    summary.days = records.days;
    // Old sessions can share an upgrade date with newly recorded time. Compare
    // them only with the anonymous totals from older builds.
    QMap<QDate, qint64> legacy, certain;
    for (const auto &session : records.sessions) {
        if (!session.hours.isEmpty())
            continue;
        visitHours(session, [&](const QDate &date, int, qint64 milliseconds) {
            legacy[date] += milliseconds;
            if (localDate(session.started) == localDate(session.observed))
                certain[date] += milliseconds;
        });
    }
    for (auto it = legacy.cbegin(); it != legacy.cend(); ++it) {
        const auto recorded = records.recordedDays.value(it.key());
        const auto anonymous = records.days.value(it.key()) - recorded;
        summary.days[it.key()] =
                recorded + (anonymous ? qMax(anonymous, certain.value(it.key())) : it.value());
    }
    for (auto it = unsaved.cbegin(); it != unsaved.cend(); ++it)
        summary.days[it.key()] += it.value();
    // Nothing after today belongs to the summary, even after a clock change.
    for (auto it = summary.days.begin(); it != summary.days.end();)
        it = it.key() > today ? summary.days.erase(it) : std::next(it);

    for (auto it = summary.days.cbegin(); it != summary.days.cend(); ++it) {
        summary.totalMilliseconds += it.value();
        if (it.value() >= readingDayMinimum) {
            ++summary.daysRead;
            if (!summary.since.isValid())
                summary.since = it.key();
        }
        if (it.value() > summary.bestDay) {
            summary.bestDay = it.value();
            summary.bestDayDate = it.key();
        }
    }
    if (summary.since.isValid())
        summary.daysSince = int(summary.since.daysTo(today)) + 1;
    summary.todayMilliseconds = summary.days.value(today);

    const auto runs = streaks(summary.days);
    for (const auto &run : runs)
        if (run.days > summary.longest.days)
            summary.longest = run;
    if (!runs.isEmpty() && (runs.last().last == today || runs.last().last == today.addDays(-1)))
        summary.current = runs.last();

    const auto monday = weekStart(today);
    for (int day = 0; day < 7; ++day) {
        const auto date = monday.addDays(day);
        const qint64 value = date > today ? -1 : summary.days.value(date);
        summary.week.append(value);
        summary.weekMilliseconds += qMax<qint64>(0, value);
    }

    QSet<QString> todayBooks, allBooks, monthBooks, bestDayBooks;
    QMap<QDate, QHash<QString, BookTotal>> months;
    QHash<QString, BookTotal> hiddenBooks;
    for (const auto &session : records.sessions) {
        if (!countsAsSession(session))
            continue;
        const auto date = localDate(session.started);
        if (date > today)
            continue;
        ++summary.sessions;
        allBooks.insert(session.book);
        QMap<QDate, qint64> monthTime;
        bool readToday = false;
        visitHours(session, [&](const QDate &day, int hour, qint64 milliseconds) {
            if (day > today)
                return;
            monthTime[monthOf(day)] += milliseconds;
            summary.hours[hour] += milliseconds;
            if (day == today) {
                readToday = true;
                todayBooks.insert(session.book);
            }
            if (monthOf(day) == monthOf(today))
                monthBooks.insert(session.book);
            if (day == summary.bestDayDate)
                bestDayBooks.insert(session.book);
        });
        if (readToday)
            ++summary.todaySessions;
        const bool hidden = records.hidden.contains(session.book);
        if (session.milliseconds > summary.longestSession) {
            summary.longestSession = session.milliseconds;
            summary.longestSessionDate = date;
            summary.longestSessionTitle = hidden ? QString() : session.title;
        }
        if (hidden)
            add(hiddenBooks[session.book], session, session.milliseconds);
        else
            for (auto month = monthTime.cbegin(); month != monthTime.cend(); ++month)
                add(months[month.key()][session.book], session, month.value());
    }
    summary.todayBooks = todayBooks.size();
    summary.books = allBooks.size();
    summary.booksThisMonth = monthBooks.size();
    summary.bestDayBooks = bestDayBooks.size();
    qint64 sessionTime = 0;
    for (const auto &session : records.sessions)
        if (countsAsSession(session) && localDate(session.started) <= today)
            sessionTime += session.milliseconds;
    summary.averageSession = summary.sessions ? sessionTime / summary.sessions : 0;

    for (auto it = months.cend(); it != months.cbegin();) {
        --it;
        MonthBooks month{ it.key(), 0, {} };
        for (auto book : it.value()) {
            describe(book, records);
            month.milliseconds += book.milliseconds;
            month.books.append(book);
        }
        std::sort(month.books.begin(), month.books.end(),
                  [](const BookTotal &left, const BookTotal &right) {
                      return left.milliseconds != right.milliseconds
                              ? left.milliseconds > right.milliseconds
                              : left.title < right.title;
                  });
        summary.months.append(month);
    }

    for (auto book : hiddenBooks) {
        describe(book, records);
        summary.hiddenBooks.append(book);
    }
    std::sort(summary.hiddenBooks.begin(), summary.hiddenBooks.end(),
              [](const BookTotal &left, const BookTotal &right) {
                  return left.last != right.last ? left.last > right.last
                                                 : left.title < right.title;
              });

    summary.finished = records.finished.size();
    QSet<QString> reading;
    for (const auto &session : records.sessions)
        if (!records.finished.contains(session.identity)) {
            const int percent = records.percent.value(session.identity, -1);
            if (percent > 0 && percent < 100)
                reading.insert(session.identity);
        }
    summary.inProgress = reading.size();

    const qint64 weekStarted = QDateTime(monday, QTime(0, 0)).toMSecsSinceEpoch();
    for (const qint64 created : records.words) {
        ++summary.words;
        if (created >= weekStarted)
            ++summary.wordsThisWeek;
    }

    const int hours = int(summary.totalMilliseconds / 3600000);
    // Each kind has a ladder of targets. Activity shows the next and the last
    // reached of each kind, and all of them on their own page.
    using Kind = Milestone::Kind;
    const QVector<QPair<Kind, QVector<int>>> ladders{
        { Kind::Books, { 1, 5, 10, 25, 50, 100 } },
        { Kind::Hours, { 10, 25, 50, 100, 250, 500, 1000 } },
        { Kind::Streak, { 7, 14, 30, 60, 100, 365 } },
        { Kind::DaysRead, { 30, 100, 200, 365, 1000 } },
    };
    for (const auto &ladder : ladders) {
        const int value = ladder.first == Kind::Books ? summary.finished
                : ladder.first == Kind::Hours         ? hours
                : ladder.first == Kind::Streak        ? summary.longest.days
                                                      : summary.daysRead;
        for (const int target : ladder.second)
            summary.milestones.append({ ladder.first, target, value });
    }
    return summary;
}

QVector<ActivitySession> bookSessions(const ActivityRecords &records, const QString &book,
                                      const QDate &today)
{
    QVector<ActivitySession> sessions;
    // Qt 5.2's QVector has no reverse iterators.
    for (int index = records.sessions.size() - 1; index >= 0; --index) {
        const auto &session = records.sessions[index];
        if (session.book == book && countsAsSession(session) && localDate(session.started) <= today)
            sessions.append(session);
    }
    return sessions;
}

QMap<QDate, qint64> bookDays(const ActivityRecords &records, const QString &book,
                             const QDate &today)
{
    QMap<QDate, qint64> days;
    for (const auto &session : records.sessions)
        if (session.book == book && countsAsSession(session) && localDate(session.started) <= today)
            visitHours(session, [&](const QDate &day, int, qint64 milliseconds) {
                if (day <= today)
                    days[day] += milliseconds;
            });
    return days;
}

BookTotal bookTotal(const ActivityRecords &records, const QString &book, const QDate &today)
{
    BookTotal total;
    for (const auto &session : records.sessions)
        if (session.book == book && countsAsSession(session) && localDate(session.started) <= today)
            add(total, session, session.milliseconds);
    describe(total, records);
    return total;
}
} // namespace kobalt::activity
