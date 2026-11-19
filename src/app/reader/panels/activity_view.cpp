#include "activity_view.h"
#include "stats_cell.h"
#include "app/reader/settings/settings_panel.h"
#include "platform/api/kobo_refresh.h"
#include "support/strings.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/paging.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include <QUrl>
#include <QDateTime>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSet>
#include <QtMath>
#include <algorithm>

namespace kobalt {
namespace chrome = ui::metrics::chrome;
namespace chapters = ui::metrics::chapters;
namespace sizes = ui::metrics::activity;

namespace {
constexpr qint64 minute = 60 * 1000, hour = 60 * minute;

// The catalog is English, so dates and day names are English on every device.
const QLocale &english()
{
    static const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale;
}

QString day(const QDate &date, bool year = false)
{
    return english().toString(date, year ? QStringLiteral("d MMM yyyy") : QStringLiteral("d MMM"));
}

QString clock(qint64 utc)
{
    return QDateTime::fromMSecsSinceEpoch(utc).time().toString(QStringLiteral("HH:mm"));
}

// "42 min" or "6 h 10 min".
QString duration(qint64 milliseconds)
{
    const qint64 minutes = qMax<qint64>(0, (milliseconds + 30000) / minute);
    return minutes < 60 ? strings::get("stats.duration_minutes").arg(minutes)
                        : strings::get("stats.duration_hours").arg(minutes / 60).arg(minutes % 60);
}

// A short form for small places, such as a bar or a calendar day: "38 min" or "1 h 04".
QString shortDuration(qint64 milliseconds)
{
    const qint64 minutes = qMax<qint64>(0, (milliseconds + 30000) / minute);
    return minutes < 60 ? strings::get("stats.duration_minutes").arg(minutes)
                        : strings::get("activity.duration_short")
                                  .arg(minutes / 60)
                                  .arg(minutes % 60, 2, 10, QChar('0'));
}

// The shortest form, for a label that does not fit otherwise: "45m" or "1h04".
QString compactDuration(qint64 milliseconds)
{
    const qint64 minutes = (milliseconds + 30000) / minute;
    return minutes < 60 ? strings::get("activity.duration_compact_minutes").arg(minutes)
                        : strings::get("activity.duration_compact_hours")
                                  .arg(minutes / 60)
                                  .arg(minutes % 60, 2, 10, QChar('0'));
}

QString plural(const char *one, const char *many, qint64 count)
{
    return strings::get(count == 1 ? one : many).arg(count);
}

QString authorCaption(const QString &author)
{
    static const QRegularExpression lineBreak(QStringLiteral("\\s*\\R\\s*"),
                                              QRegularExpression::UseUnicodePropertiesOption);
    return author.trimmed().toUpper().replace(lineBreak, QStringLiteral(", "));
}

// A figure cell's value: minutes under an hour, otherwise hours with the
// minutes in the unit.
void setDuration(StatsCell *cell, qint64 milliseconds, const QString &detail)
{
    const qint64 minutes = qMax<qint64>(0, (milliseconds + 30000) / minute);
    if (minutes < 60)
        cell->set(QString::number(minutes), strings::get("stats.minutes"), detail);
    else if (minutes % 60 == 0 || minutes >= 100 * 60)
        cell->set(QString::number(minutes / 60), strings::get("activity.hours_unit"), detail);
    else
        cell->set(QString::number(minutes / 60),
                  strings::get("activity.hours_minutes_unit").arg(minutes % 60), detail);
}

QColor mix(const QColor &paper, const QColor &ink, qreal amount)
{
    return QColor::fromRgbF(paper.redF() + (ink.redF() - paper.redF()) * amount,
                            paper.greenF() + (ink.greenF() - paper.greenF()) * amount,
                            paper.blueF() + (ink.blueF() - paper.blueF()) * amount);
}

// Five steps that e-ink shows apart: nothing, under half an hour, under an
// hour, under two hours, and more.
int level(qint64 milliseconds)
{
    if (milliseconds < activity::readingDayMinimum)
        return 0;
    return milliseconds < 30 * minute ? 1
            : milliseconds < hour     ? 2
            : milliseconds < 2 * hour ? 3
                                      : 4;
}

QColor levelColor(const QWidget *widget, int value, bool strong = false)
{
    const auto paper = widget->palette().color(QPalette::Window);
    const auto ink = ui::labelInk(widget);
    static const qreal soft[] = { 0, .12, .32, .62, 1 }, dark[] = { 0, .35, .55, .78, 1 };
    const int index = qBound(0, value, 4);
    return mix(paper, ink, strong ? dark[index] : soft[index]);
}

void drawCaption(QPainter &painter, const QRect &area, const QString &text, Qt::Alignment flags,
                 const QColor &color, int pixels = chrome::captionText)
{
    painter.setFont(ui::captionFont(pixels));
    painter.setPen(color);
    ui::drawInterfaceText(painter, area, text, flags);
}

void drawText(QPainter &painter, const QRect &area, const QString &text, Qt::Alignment flags,
              const QFont &font, const QColor &color)
{
    painter.setFont(font);
    painter.setPen(color);
    ui::drawInterfaceText(painter, area, text, flags);
}

// "6 h 10 min" with the numbers in the brand face and the units small, as in
// the figure cells. Returns the width; draws only when `painter` is set.
int drawFigure(QPainter *painter, int right, int baseline, qint64 milliseconds, int pixels,
               const QColor &color)
{
    const qint64 minutes = qMax<qint64>(0, (milliseconds + 30000) / minute);
    QVector<QPair<QString, bool>> parts;
    if (minutes >= 60) {
        parts.append({ QString::number(minutes / 60), true });
        parts.append({ strings::get("activity.hours_unit"), false });
        if (minutes % 60)
            parts.append({ QString("%1").arg(minutes % 60, 2, 10, QChar('0')), true });
    } else {
        parts.append({ QString::number(minutes), true });
    }
    if (minutes < 60 || minutes % 60)
        parts.append({ strings::get("stats.minutes"), false });
    const QFont number(ui::brandFont(pixels)), unit(ui::interfaceFont(chapters::unitText));
    const int gap = chapters::unitGap;
    int width = 0;
    for (const auto &part : parts)
        width += ui::interfaceTextWidth(part.first, part.second ? number : unit) + gap;
    width -= gap;
    if (!painter)
        return width;
    int x = right - width;
    painter->setPen(color);
    for (const auto &part : parts) {
        const auto &font = part.second ? number : unit;
        const int textWidth = ui::interfaceTextWidth(part.first, font);
        const int ascent = ui::interfaceLineAscent(font);
        painter->setFont(font);
        ui::drawInterfaceText(
                *painter, QRect(x, baseline - ascent, textWidth + 2, ui::interfaceLineHeight(font)),
                part.first, Qt::AlignLeft | Qt::AlignTop);
        x += textWidth + gap;
    }
    return width;
}

// A shaded band with a caption on each side, as in Settings.
ui::ListHeader *band(QWidget *parent, const QString &name)
{
    auto *header = new ui::ListHeader(parent);
    header->setObjectName(name);
    header->setShaded(true);
    header->setTopDivider(true);
    header->setFixedHeight(ui::metrics::list::headerHeight);
    return header;
}

void setBand(ui::ListHeader *header, const QString &left, const QString &right = {})
{
    header->setCaptions(left.toUpper(), right.toUpper());
}

// Places cells in rows of `columns` from `origin`, each `height` tall.
int placeCells(const QVector<StatsCell *> &cells, int top, int width, int columns, int origin = 0,
               int height = chapters::cellHeight)
{
    const int border = ui::metrics::control::border;
    for (int index = 0; index < cells.size(); ++index) {
        const int row = index / columns, column = index % columns;
        const int left = width * column / columns + (column ? border : 0);
        cells[index]->setGeometry(origin + left, top + row * height + (row ? border : 0),
                                  width * (column + 1) / columns - left,
                                  height - (row ? border : 0));
    }
    return top + (int(cells.size()) + columns - 1) / columns * height;
}

// A landscape screen puts parts side by side instead of above each other.
bool wideLayout(const QWidget *page)
{
    return page->width() > page->height();
}

// A list row is at least its design height and grows with larger text.
int rowHeight(int design, int lines)
{
    return qMax(design, lines + 2 * ui::px(20));
}

void paintVerticalRule(QPainter &painter, const QWidget *view, int x, int top, int bottom)
{
    painter.fillRect(x, top, ui::metrics::control::border, bottom - top,
                     view->palette().color(QPalette::Mid));
}

// The rules between cells: one under each row and one between columns.
void paintCellRules(QPainter &painter, const QWidget *view, const QVector<StatsCell *> &cells,
                    int columns)
{
    const int border = ui::metrics::control::border;
    for (int index = 0; index < cells.size(); ++index) {
        const auto area = cells[index]->geometry();
        if (!cells[index]->isVisible())
            continue;
        if (index % columns)
            painter.fillRect(area.left() - border, area.top(), border, area.height(),
                             view->palette().color(QPalette::Mid));
        if (index >= columns)
            painter.fillRect(area.left(), area.top() - border, area.width(), border,
                             ui::dividerColor(view));
    }
}

// Bars for the days of a week, today filled and the days still to come as a
// dashed baseline, with the time above each bar and the day below.
class WeekBars final : public QWidget
{
public:
    explicit WeekBars(QWidget *parent) : QWidget(parent) { setObjectName("rrActivityWeek"); }

    void setWeek(const QVector<qint64> &values, int todayIndex)
    {
        week = values;
        today = todayIndex;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        const auto paper = palette().color(QPalette::Window);
        const auto note = ui::noteColor(this);
        if (week.size() != 7)
            return;
        const QFont value(ui::interfaceFont(chapters::unitText));
        const QFont label(ui::captionFont());
        const int valueHeight = ui::interfaceLineHeight(value) + sizes::valueGap;
        const int labelHeight = ui::interfaceLineHeight(label) + sizes::labelGap;
        const int top = chrome::blockTop + valueHeight;
        const int base = height() - chrome::blockTop - labelHeight;
        const int room = qMax(1, base - top);
        const int columnWidth = (width() - 2 * chrome::inset) / 7;
        const int barWidth = qMin<int>(sizes::maximumBarWidth, columnWidth - sizes::barGap / 2);
        qint64 maximum = 1;
        for (const qint64 day : week)
            maximum = qMax(maximum, day);
        for (int index = 0; index < 7; ++index) {
            const int center = chrome::inset + columnWidth * index + columnWidth / 2;
            const int left = center - barWidth / 2;
            // Today keeps its day name in a narrow column; the underline
            // still marks it.
            const auto dayName = english().dayName(index + 1, QLocale::ShortFormat);
            const QString name = index == today
                            && ui::interfaceTextWidth(strings::get("activity.today").toUpper(),
                                                      label)
                                    <= columnWidth - ui::px(4)
                    ? strings::get("activity.today")
                    : dayName;
            const QRect labelArea(center - columnWidth / 2, base + sizes::labelGap, columnWidth,
                                  ui::interfaceLineHeight(label));
            drawCaption(painter, labelArea, name, Qt::AlignHCenter | Qt::AlignTop,
                        week[index] < 0 ? note : ink);
            if (index == today) {
                const int textWidth = ui::interfaceTextWidth(name.toUpper(), label);
                painter.fillRect(center - textWidth / 2, labelArea.top() + labelArea.height(),
                                 textWidth, ui::metrics::control::border, ink);
            }
            if (week[index] < 0) {
                QPen dash(ui::dividerColor(this), ui::metrics::control::border, Qt::DashLine);
                painter.setPen(dash);
                painter.drawLine(left, base - 1, left + barWidth, base - 1);
                continue;
            }
            const int barHeight = week[index] >= activity::readingDayMinimum
                    ? qMax(int(chapters::minimumBarHeight / 2),
                           int(room * double(week[index]) / double(maximum)))
                    : 0;
            const QRect bar(left, base - barHeight, barWidth, barHeight);
            if (barHeight) {
                if (index == today)
                    painter.fillRect(bar, ink);
                else {
                    painter.fillRect(bar, paper);
                    ui::paintEdges(painter, bar,
                                   ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge
                                           | Qt::RightEdge,
                                   ink, chapters::outline);
                }
            } else {
                painter.fillRect(left, base - ui::metrics::control::border, barWidth,
                                 ui::metrics::control::border, ink);
            }
            // A narrow column, as beside today and the streak at a large text
            // size, writes the time in its shortest form.
            QString time;
            if (week[index] >= activity::readingDayMinimum) {
                time = shortDuration(week[index]);
                if (ui::interfaceTextWidth(time, value) > columnWidth - ui::px(4))
                    time = compactDuration(week[index]);
            }
            drawText(painter,
                     QRect(center - columnWidth / 2, bar.top() - valueHeight, columnWidth,
                           ui::interfaceLineHeight(value)),
                     time, Qt::AlignHCenter | Qt::AlignTop, value, ink);
        }
    }

private:
    QVector<qint64> week;
    int today = -1;
};

class ActivityPage : public QWidget
{
public:
    explicit ActivityPage(QWidget *parent) : QWidget(parent) { }

    virtual void setSummary(const activity::Summary &, const ActivityRecords &) { }

protected:
    QVector<StatsCell *> cells;
    int columns = 3;

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintCellRules(painter, this, cells, columns);
    }
};
} // namespace

class ActivityOverview final : public ActivityPage
{
public:
    explicit ActivityOverview(QWidget *parent) : ActivityPage(parent)
    {
        setObjectName("rrActivityOverview");
        todayCell = new StatsCell(this, "rrActivityToday", strings::get("activity.today"));
        streakCell = new StatsCell(this, "rrActivityStreak", strings::get("activity.streak"));
        weekBand = band(this, "rrActivityWeekBand");
        week = new WeekBars(this);
        totalBand = band(this, "rrActivityTotalBand");
        const char *const names[] = { "Reading", "Finished", "Sessions", "Days", "Words", "Books" };
        const char *const titles[] = { "activity.reading",     "activity.finished",
                                       "activity.sessions",    "activity.days_read",
                                       "activity.saved_words", "activity.books" };
        for (int index = 0; index < 6; ++index)
            totals.append(new StatsCell(this, QString("rrActivity") + names[index],
                                        strings::get(titles[index])));
    }

    void setSummary(const activity::Summary &summary, const ActivityRecords &) override
    {
        setDuration(todayCell, summary.todayMilliseconds,
                    summary.todaySessions ? plural("activity.session_count",
                                                   "activity.sessions_count", summary.todaySessions)
                                    + QStringLiteral(" · ")
                                    + plural("activity.book_count", "activity.books_count",
                                             summary.todayBooks)
                                          : strings::get("activity.no_reading_today"));
        streakCell->set(QString::number(summary.current.days),
                        summary.current.days == 1 ? strings::get("activity.day")
                                                  : strings::get("activity.days"),
                        summary.longest.days > summary.current.days
                                ? plural("activity.best_streak_one", "activity.best_streak",
                                         summary.longest.days)
                                : summary.current.days ? strings::get("activity.best_yet")
                                                       : QString());
        setBand(weekBand, strings::get("activity.this_week"), duration(summary.weekMilliseconds));
        week->setWeek(summary.week, summary.today.dayOfWeek() - 1);
        setBand(totalBand, strings::get("activity.all_time"),
                summary.since.isValid()
                        ? strings::get("activity.since").arg(day(summary.since, true))
                        : QString());
        const qint64 hours = summary.totalMilliseconds / hour;
        if (hours >= 10)
            totals[0]->set(QString::number(hours), strings::get("activity.hours"),
                           summary.daysSince ? strings::get("activity.per_day")
                                                       .arg(duration(summary.totalMilliseconds
                                                                     / summary.daysSince))
                                             : QString());
        else
            setDuration(totals[0], summary.totalMilliseconds,
                        summary.daysSince ? strings::get("activity.per_day")
                                                    .arg(duration(summary.totalMilliseconds
                                                                  / summary.daysSince))
                                          : QString());
        totals[1]->set(QString::number(summary.finished),
                       summary.finished == 1 ? strings::get("activity.book")
                                             : strings::get("activity.books_unit"),
                       strings::get("activity.in_progress").arg(summary.inProgress));
        totals[2]->set(
                QString::number(summary.sessions), QString(),
                summary.sessions
                        ? strings::get("activity.average").arg(duration(summary.averageSession))
                        : QString());
        totals[3]->set(QString::number(summary.daysRead), QString(),
                       summary.daysSince ? plural("activity.of_days_one", "activity.of_days",
                                                  summary.daysSince)
                                         : QString());
        totals[4]->set(QString::number(summary.words), QString(),
                       strings::get("activity.this_week_count").arg(summary.wordsThisWeek));
        totals[5]->set(QString::number(summary.books), QString(),
                       strings::get("activity.this_month_count").arg(summary.booksThisMonth));
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *) override { arrange(); }

private:
    StatsCell *todayCell, *streakCell;
    ui::ListHeader *weekBand, *totalBand;
    WeekBars *week;
    QVector<StatsCell *> totals;

    // Six totals in a row, or three when a caption would not fit, as at a
    // large text size.
    int wideColumns() const
    {
        for (const auto *cell : totals)
            if (ui::interfaceTextWidth(cell->caption->text().toUpper(), cell->caption->font())
                        + 2 * chrome::inset
                > width() / 6)
                return 3;
        return 6;
    }

    void arrange()
    {
        const int header = ui::metrics::list::headerHeight;
        const int border = ui::metrics::control::border;
        cells = totals;
        // Landscape: today and the streak in a column beside the week, and
        // all six totals in one row, or in two when their captions do not fit.
        if (wideLayout(this)) {
            columns = wideColumns();
            const int upper = height() - header - 6 / columns * chapters::cellHeight;
            if (columns == 3) {
                // Too little height to stack today and the streak: they take a
                // quarter each beside the week, and keep their detail lines.
                const int quarter = width() / 4;
                todayCell->setGeometry(0, 0, quarter, upper);
                streakCell->setGeometry(quarter + border, 0, quarter - border, upper);
                weekBand->setGeometry(2 * quarter + border, 0, width() - 2 * quarter - border,
                                      header);
                week->setGeometry(2 * quarter + border, header, width() - 2 * quarter - border,
                                  upper - header);
                totalBand->setGeometry(0, upper, width(), header);
                placeCells(totals, upper + header, width(), columns);
                update();
                return;
            }
            const int left = width() / 3;
            todayCell->setGeometry(0, 0, left, upper / 2);
            streakCell->setGeometry(0, upper / 2 + border, left, upper - upper / 2 - border);
            weekBand->setGeometry(left + border, 0, width() - left - border, header);
            week->setGeometry(left + border, header, width() - left - border, upper - header);
            totalBand->setGeometry(0, upper, width(), header);
            placeCells(totals, upper + header, width(), columns);
            update();
            return;
        }
        columns = 3;
        const int fixed = 2 * chapters::cellHeight + 2 * header + chapters::cellHeight;
        const int chart = qMax(ui::px(160), height() - fixed);
        todayCell->setGeometry(0, 0, width() / 2, chapters::cellHeight);
        streakCell->setGeometry(width() / 2 + border, 0, width() - width() / 2 - border,
                                chapters::cellHeight);
        weekBand->setGeometry(0, chapters::cellHeight, width(), header);
        week->setGeometry(0, chapters::cellHeight + header, width(), chart);
        totalBand->setGeometry(0, chapters::cellHeight + header + chart, width(), header);
        placeCells(totals, chapters::cellHeight + 2 * header + chart, width(), columns);
        update();
    }

    void paintEvent(QPaintEvent *event) override
    {
        ActivityPage::paintEvent(event);
        QPainter painter(this);
        if (wideLayout(this)) {
            const int upper = height() - ui::metrics::list::headerHeight
                    - 6 / wideColumns() * chapters::cellHeight;
            if (wideColumns() == 3) {
                paintVerticalRule(painter, this, width() / 4, 0, upper);
                paintVerticalRule(painter, this, width() / 4 * 2, 0, upper);
                return;
            }
            paintVerticalRule(painter, this, width() / 3, 0, upper);
            painter.fillRect(0, upper / 2, width() / 3, ui::metrics::control::border,
                             ui::dividerColor(this));
        } else {
            paintVerticalRule(painter, this, width() / 2, 0, chapters::cellHeight);
        }
    }
};

namespace {
// The days of a month from Monday to Sunday, shaded by reading time, with
// today outlined and the days still to come dashed.
class MonthGrid final : public QWidget
{
public:
    explicit MonthGrid(QWidget *parent) : QWidget(parent) { setObjectName("rrActivityMonth"); }

    void setMonth(const QDate &first, const QMap<QDate, qint64> &values, const QDate &now)
    {
        month = first;
        days = values;
        today = now;
        update();
    }

    int headerHeight() const
    {
        return ui::interfaceLineHeight(ui::captionFont()) + 2 * sizes::dayPadding;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        const auto paper = palette().color(QPalette::Window);
        const auto note = ui::noteColor(this);
        const int inner = width() - 2 * chrome::inset;
        const int head = headerHeight();
        painter.fillRect(0, 0, width(), ui::metrics::control::border, ui::dividerColor(this));
        for (int column = 0; column < 7; ++column)
            drawCaption(painter, QRect(chrome::inset + inner * column / 7, 0, inner / 7, head),
                        english().dayName(column + 1, QLocale::ShortFormat), Qt::AlignCenter, ink);
        const QDate first = activity::weekStart(month);
        const int rowHeight = (height() - head) / 6;
        const QFont value(ui::interfaceFont(chapters::unitText));
        for (int index = 0; index < 42; ++index) {
            const auto date = first.addDays(index);
            const int row = index / 7, column = index % 7;
            QRect cell(chrome::inset + inner * column / 7, head + row * rowHeight,
                       inner * (column + 1) / 7 - inner * column / 7, rowHeight);
            cell.adjust(sizes::dayGap / 2, sizes::dayGap / 2, -sizes::dayGap / 2,
                        -sizes::dayGap / 2);
            const bool future = date > today, other = date.month() != month.month();
            const qint64 time = future ? 0 : days.value(date);
            const int shade = future ? 0 : level(time);
            painter.fillRect(cell, levelColor(this, shade));
            if (shade < 3) {
                QPen edge(future ? ui::dividerColor(this) : palette().color(QPalette::Mid),
                          ui::metrics::control::border, future ? Qt::DashLine : Qt::SolidLine);
                edge.setJoinStyle(Qt::MiterJoin);
                painter.setPen(edge);
                const int half = ui::metrics::control::border / 2;
                painter.drawRect(cell.adjusted(half, half, -half - 1, -half - 1));
            }
            const auto text = shade >= 3 ? paper : ink;
            const auto inside = cell.adjusted(sizes::dayPadding, sizes::dayPadding / 2,
                                              -sizes::dayPadding, -sizes::dayPadding / 2);
            drawCaption(painter, inside, QString::number(date.day()), Qt::AlignLeft | Qt::AlignTop,
                        future || other ? (shade >= 3 ? text : note) : text);
            if (shade && rowHeight >= ui::px(72)) {
                // A narrow cell, as on a phone, writes "45m" or "1h04".
                auto label = shortDuration(time);
                if (ui::interfaceTextWidth(label, value) > inside.width())
                    label = compactDuration(time);
                if (ui::interfaceTextWidth(label, value) <= inside.width())
                    drawText(painter, inside, label, Qt::AlignRight | Qt::AlignBottom, value, text);
            }
            if (date == today) {
                QPen outline(ink, sizes::todayOutline);
                outline.setJoinStyle(Qt::MiterJoin);
                painter.setPen(outline);
                painter.setBrush(Qt::NoBrush);
                const int out = sizes::todayOutline / 2 + ui::px(2);
                painter.drawRect(cell.adjusted(-out, -out, out, out));
            }
        }
    }

private:
    QDate month, today;
    QMap<QDate, qint64> days;
};

// "Less", the five shades and "More".
class ShadeLegend final : public QWidget
{
public:
    explicit ShadeLegend(QWidget *parent) : QWidget(parent) { }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        const QFont caption(ui::captionFont());
        const auto less = strings::get("activity.less").toUpper();
        int x = chrome::inset;
        drawCaption(painter, QRect(x, 0, width(), height()), less, Qt::AlignLeft | Qt::AlignVCenter,
                    ink);
        x += ui::interfaceTextWidth(less, caption) + sizes::dayPadding;
        const int side = sizes::legendSwatch;
        for (int shade = 0; shade < 5; ++shade) {
            const QRect swatch(x, (height() - side) / 2, side, side);
            painter.fillRect(swatch, levelColor(this, shade));
            if (shade < 3)
                ui::paintEdges(painter, swatch,
                               ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge
                                       | Qt::RightEdge,
                               palette().color(QPalette::Mid));
            x += side + ui::px(8);
        }
        drawCaption(painter, QRect(x + ui::px(4), 0, width(), height()),
                    strings::get("activity.more"), Qt::AlignLeft | Qt::AlignVCenter, ink);
    }
};

// Every week of the year as a column of seven days, with month initials below.
class YearStrip final : public QWidget
{
public:
    explicit YearStrip(QWidget *parent) : QWidget(parent) { setObjectName("rrActivityYear"); }

    void setYear(const QMap<QDate, qint64> &values, const QDate &now, const QDate &since)
    {
        days = values;
        today = now;
        first = since;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QDate start = activity::weekStart(QDate(today.year(), 1, 1));
        const int weeks = int(start.daysTo(QDate(today.year(), 12, 31))) / 7 + 1;
        const int inner = width() - 2 * chrome::inset;
        const QFont initial(ui::captionFont(chrome::captionLabelText));
        const int labels = ui::interfaceLineHeight(initial) + ui::px(6);
        const int step = qMax(3, qMin(inner / weeks, (height() - labels) / 7));
        const int square = qMax(2, qMin<int>(sizes::yearSquare, step - sizes::yearGap));
        const int left = (width() - (weeks * step - (step - square))) / 2;
        const int top = qMax(0, (height() - labels - 7 * step) / 2);
        for (int week = 0; week < weeks; ++week)
            for (int weekday = 0; weekday < 7; ++weekday) {
                const auto date = start.addDays(week * 7 + weekday);
                const QRect area(left + week * step, top + weekday * step, square, square);
                const bool known = first.isValid() && date >= first && date <= today;
                const int shade = known ? level(days.value(date)) : 0;
                if (shade)
                    painter.fillRect(area, levelColor(this, shade, true));
                else
                    ui::paintEdges(painter, area,
                                   ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge
                                           | Qt::RightEdge,
                                   known ? palette().color(QPalette::Mid) : levelColor(this, 1), 1);
                if (date == today) {
                    QPen outline(ui::labelInk(this), ui::px(3));
                    outline.setJoinStyle(Qt::MiterJoin);
                    painter.setPen(outline);
                    painter.setBrush(Qt::NoBrush);
                    painter.drawRect(area.adjusted(-ui::px(3), -ui::px(3), ui::px(3), ui::px(3)));
                }
            }
        for (int month = 1; month <= 12; ++month) {
            const int week = int(start.daysTo(QDate(today.year(), month, 1))) / 7;
            drawCaption(painter,
                        QRect(left + week * step, top + 7 * step + ui::px(4), step * 4, labels),
                        english().monthName(month, QLocale::ShortFormat).left(1),
                        Qt::AlignLeft | Qt::AlignTop, ui::noteColor(this),
                        chrome::captionLabelText);
        }
    }

private:
    QMap<QDate, qint64> days;
    QDate today, first;
};
} // namespace

class ActivityCalendar final : public ActivityPage
{
public:
    explicit ActivityCalendar(QWidget *parent) : ActivityPage(parent)
    {
        setObjectName("rrActivityCalendar");
        months = new ui::PagerBar(this, "rrActivityMonths");
        months->setTopDivider(false);
        ui::readerStyle(months->counter,
                        ui::styles::font(ui::metrics::text::small, true)
                                + "background:transparent;");
        connect(months->previous, &QPushButton::clicked, this, [this] { step(-1); });
        connect(months->next, &QPushButton::clicked, this, [this] { step(1); });
        streak = new StatsCell(this, "rrActivityCurrentStreak", strings::get("activity.streak"));
        longest = new StatsCell(this, "rrActivityLongest", strings::get("activity.longest"));
        month = new StatsCell(this, "rrActivityMonthTotal", strings::get("activity.this_month"));
        grid = new MonthGrid(this);
        legend = new ShadeLegend(this);
        yearBand = band(this, "rrActivityYearBand");
        year = new YearStrip(this);
        cells = { streak, longest, month };
    }

    void reset()
    {
        summary = {};
        shown = {};
        grid->setMonth({}, {}, {});
        year->setYear({}, {}, {});
    }

    void setSummary(const activity::Summary &value, const ActivityRecords &) override
    {
        summary = value;
        // A reload, such as after hiding a book, keeps the month in view while
        // it is still between the first month with reading and this one.
        const QDate thisMonth(summary.today.year(), summary.today.month(), 1);
        if (!shown.isValid() || shown > thisMonth || shown < firstMonth())
            shown = thisMonth;
        streak->set(QString::number(summary.current.days),
                    summary.current.days == 1 ? strings::get("activity.day")
                                              : strings::get("activity.days"),
                    summary.current.days
                            ? strings::get("activity.since").arg(day(summary.current.first))
                            : QString());
        longest->set(QString::number(summary.longest.days),
                     summary.longest.days == 1 ? strings::get("activity.day")
                                               : strings::get("activity.days"),
                     summary.longest.days ? day(summary.longest.first) + QStringLiteral(" – ")
                                     + day(summary.longest.last)
                                          : QString());
        int read = 0;
        for (auto it = summary.days.cbegin(); it != summary.days.cend(); ++it)
            if (it.key().year() == summary.today.year()
                && it.value() >= activity::readingDayMinimum)
                ++read;
        setBand(yearBand, QString::number(summary.today.year()),
                plural("activity.day_read", "activity.days_read_count", read));
        year->setYear(summary.days, summary.today, summary.since);
        showMonth();
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *) override { arrange(); }

private:
    activity::Summary summary;
    QDate shown;
    ui::PagerBar *months;
    StatsCell *streak, *longest, *month;
    MonthGrid *grid;
    ShadeLegend *legend;
    ui::ListHeader *yearBand;
    YearStrip *year;

    QDate firstMonth() const
    {
        const auto since = summary.since.isValid() ? summary.since : summary.today;
        return QDate(since.year(), since.month(), 1);
    }

    int monthIndex(const QDate &date) const
    {
        return (date.year() - firstMonth().year()) * 12 + date.month() - firstMonth().month();
    }

    void step(int direction)
    {
        const auto next = shown.addMonths(direction);
        if (next < firstMonth() || next > QDate(summary.today.year(), summary.today.month(), 1))
            return;
        shown = next;
        showMonth();
    }

    void showMonth()
    {
        qint64 total = 0;
        int read = 0, days = 0;
        for (auto date = shown; date.month() == shown.month() && date <= summary.today;
             date = date.addDays(1)) {
            ++days;
            total += summary.days.value(date);
            read += summary.days.value(date) >= activity::readingDayMinimum;
        }
        const bool thisMonth =
                shown.year() == summary.today.year() && shown.month() == summary.today.month();
        month->caption->setText(thisMonth ? strings::get("activity.this_month")
                                          : english().monthName(shown.month()));
        setDuration(month, total,
                    strings::get(days == 1 ? "activity.days_of_one" : "activity.days_of")
                            .arg(read)
                            .arg(days));
        grid->setMonth(shown, summary.days, summary.today);
        months->setPage(monthIndex(shown), monthIndex(summary.today) + 1,
                        english().toString(shown, QStringLiteral("MMMM yyyy")), false, {}, true);
    }

    void arrange()
    {
        const int pager = chrome::actionHeight;
        const int legendHeight = ui::px(52);
        // Landscape: the month on the left and the figures stacked on the right.
        if (wideLayout(this)) {
            const int left = width() * 2 / 3;
            columns = 1;
            months->setGeometry(0, 0, left, pager);
            placeCells(cells, 0, width() - left - ui::metrics::control::border, 1,
                       left + ui::metrics::control::border, height() / 3);
            yearBand->hide();
            year->hide();
            grid->setGeometry(0, pager + ui::metrics::control::border, left,
                              height() - pager - ui::metrics::control::border - legendHeight);
            legend->setGeometry(0, height() - legendHeight, left, legendHeight);
            update();
            return;
        }
        columns = 3;
        // The pager keeps its own height; the rule under it sits below.
        months->setGeometry(0, 0, width(), pager);
        const int cellsBottom =
                placeCells(cells, pager + ui::metrics::control::border, width(), columns);
        const int header = ui::metrics::list::headerHeight;
        // The year needs room for seven rows of squares and the month initials.
        const int yearHeight = ui::px(150);
        const bool showYear =
                height() - cellsBottom - legendHeight - header - yearHeight >= ui::px(420);
        yearBand->setVisible(showYear);
        year->setVisible(showYear);
        const int gridHeight =
                height() - cellsBottom - legendHeight - (showYear ? header + yearHeight : 0);
        grid->setGeometry(0, cellsBottom, width(), gridHeight);
        legend->setGeometry(0, cellsBottom + gridHeight, width(), legendHeight);
        yearBand->setGeometry(0, cellsBottom + gridHeight + legendHeight, width(), header);
        year->setGeometry(0, cellsBottom + gridHeight + legendHeight + header, width(), yearHeight);
        update();
    }

    void paintEvent(QPaintEvent *event) override
    {
        ActivityPage::paintEvent(event);
        QPainter painter(this);
        const bool wide = wideLayout(this);
        painter.fillRect(0, chrome::actionHeight, wide ? width() * 2 / 3 : width(),
                         ui::metrics::control::border, ui::dividerColor(this));
        if (wide)
            paintVerticalRule(painter, this, width() * 2 / 3, 0, height());
    }
};

// A row that opens more: a title, a caption on the right and a chevron.
class LinkRow final : public ui::Button
{
public:
    LinkRow(QWidget *parent, const QString &name, const QString &label)
        : ui::Button(QString(), parent), title(label)
    {
        setObjectName(name);
        setAccessibleName(label);
    }

    void setCaption(const QString &text)
    {
        caption = text;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        painter.fillRect(rect(),
                         isDown() ? ui::themeColors(this).pressedPaper
                                  : palette().color(QPalette::Window));
        painter.fillRect(0, 0, width(), ui::metrics::control::border, ui::dividerColor(this));
        painter.setRenderHint(QPainter::Antialiasing);
        const int icon = chrome::cellIcon;
        const int chevronLeft = width() - chrome::inset - icon;
        ui::controlIcon(ui::ControlIcon::Next, ink)
                .paint(&painter, QRect(chevronLeft, (height() - icon) / 2, icon, icon));
        const QFont name(ui::interfaceFont(chrome::labelText));
        drawText(painter, QRect(chrome::inset, 0, chevronLeft - chrome::inset, height()), title,
                 Qt::AlignLeft | Qt::AlignVCenter, name, ink);
        drawCaption(painter,
                    QRect(chrome::inset, 0, chevronLeft - chapters::activityIconGap - chrome::inset,
                          height()),
                    caption, Qt::AlignRight | Qt::AlignVCenter, ink);
    }

private:
    QString title, caption;
};

// Rows made after a load, on a page turn or a book's page, need the same
// e-ink preparation as the rest of Activity.
static void prepareRows(QWidget *page)
{
    platform::textRefresh(page);
    for (auto *widget : page->findChildren<QWidget *>())
        platform::textRefresh(widget);
}

namespace {
// A book's time in a period: the title, author and progress on the left, the
// time and sessions on the right.
class BookRow final : public ui::Button
{
public:
    explicit BookRow(QWidget *parent) : ui::Button(QString(), parent) { }

    static int preferredHeight()
    {
        const int caption = ui::interfaceLineHeight(ui::captionFont());
        return rowHeight(sizes::rowHeight,
                         ui::interfaceLineHeight(ui::interfaceFont(chrome::labelText)) + ui::px(6)
                                 + caption + ui::px(18) + caption);
    }

    // The cover's size in this row: the row's height less its padding, at a
    // book's proportions.
    static QSize coverSize()
    {
        const int height = preferredHeight() - 2 * ui::px(22);
        return { height * 2 / 3, height };
    }

    void setBook(const activity::BookTotal &value, const QString &caption = {})
    {
        book = value;
        copyCaption = caption;
        author = authorCaption(book.author);
        setObjectName("rrActivityBook");
        setAccessibleName(book.title);
        setAccessibleDescription(caption);
        update();
    }

    void setCover(const QImage &image, bool gone)
    {
        cover = image;
        missing = gone;
        update();
    }

    activity::BookTotal book;
    QString copyCaption;
    QDate month;
    QImage cover;
    bool missing = false;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        const auto note = ui::noteColor(this);
        painter.fillRect(rect(),
                         isDown() ? ui::themeColors(this).pressedPaper
                                  : palette().color(QPalette::Window));
        painter.fillRect(0, height() - ui::metrics::control::border, width(),
                         ui::metrics::control::border, ui::dividerColor(this));
        const int figureWidth =
                drawFigure(nullptr, 0, 0, book.milliseconds, sizes::figureText * 3 / 4, ink);
        const QFont caption(ui::captionFont());
        const auto sessions =
                plural("activity.session_count", "activity.sessions_count", book.sessions)
                        .toUpper();
        const int right = width() - chrome::inset;
        const int textRight = right - qMax(figureWidth, ui::interfaceTextWidth(sessions, caption))
                - chrome::inset;
        // The cover, or until it arrives or without one, a frame with the
        // title's first letter.
        const auto frame = coverSize();
        const QRect coverArea(chrome::inset, (height() - frame.height()) / 2, frame.width(),
                              frame.height());
        if (!cover.isNull()) {
            const auto shown = cover.size().scaled(frame, Qt::KeepAspectRatio);
            const QRect image(coverArea.left() + (frame.width() - shown.width()) / 2,
                              coverArea.top() + (frame.height() - shown.height()) / 2,
                              shown.width(), shown.height());
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(image, cover);
            ui::paintEdges(painter, image,
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           palette().color(QPalette::Mid), 1);
        } else {
            painter.fillRect(coverArea, levelColor(this, 1));
            ui::paintEdges(painter, coverArea,
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           palette().color(QPalette::Mid));
            if (missing) {
                // A book no longer on the device gets a plain cover: a title
                // block and an author line, without a letter.
                const int inset = frame.width() / 6, rule = qMax(2, frame.height() / 40);
                const int width = frame.width() - 2 * inset;
                const auto lines = levelColor(this, 3);
                painter.fillRect(coverArea.left() + inset, coverArea.top() + frame.height() / 5,
                                 width, rule, lines);
                painter.fillRect(coverArea.left() + inset,
                                 coverArea.top() + frame.height() / 5 + 3 * rule, width * 2 / 3,
                                 rule, lines);
                painter.fillRect(coverArea.left() + inset, coverArea.bottom() - frame.height() / 5,
                                 width / 2, rule, lines);
            } else {
                drawText(painter, coverArea, book.title.left(1).toUpper(), Qt::AlignCenter,
                         ui::brandFont(frame.width() * 3 / 5), note);
            }
        }
        const int left = coverArea.right() + 1 + ui::px(24);
        const QFont title(ui::interfaceFont(chrome::labelText));
        const int titleHeight = ui::interfaceLineHeight(title);
        const int captionHeight = ui::interfaceLineHeight(caption);
        const int block = titleHeight + ui::px(6) + captionHeight + ui::px(18) + captionHeight;
        int y = (height() - block) / 2;
        drawText(painter, QRect(left, y, textRight - left, titleHeight),
                 ui::elideInterfaceText(book.title, title, textRight - left),
                 Qt::AlignLeft | Qt::AlignTop, title, ink);
        y += titleHeight + ui::px(6);
        drawCaption(painter, QRect(left, y, textRight - left, captionHeight),
                    ui::elideInterfaceText(author, caption, textRight - left),
                    Qt::AlignLeft | Qt::AlignTop, note);
        y += captionHeight + ui::px(18);
        if (!copyCaption.isEmpty()) {
            drawCaption(painter, QRect(left, y, textRight - left, captionHeight),
                        ui::elideInterfaceText(copyCaption, caption, textRight - left),
                        Qt::AlignLeft | Qt::AlignTop, ink);
        } else if (book.finished) {
            drawCaption(painter, QRect(left, y, textRight - left, captionHeight),
                        strings::get("activity.finished_mark"), Qt::AlignLeft | Qt::AlignTop, ink);
        } else if (book.percent >= 0) {
            const int bar = qMin<int>(sizes::progressWidth, textRight - left - ui::px(100));
            const QRect track(left, y + (captionHeight - sizes::progressHeight) / 2, bar,
                              sizes::progressHeight);
            painter.fillRect(track.adjusted(0, 0, -(track.width() * (100 - book.percent) / 100), 0),
                             ink);
            ui::paintEdges(painter, track,
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           ink);
            drawCaption(painter, QRect(left + bar + ui::px(16), y, ui::px(120), captionHeight),
                        strings::get("common.percent").arg(book.percent),
                        Qt::AlignLeft | Qt::AlignTop, ink);
        }
        const int baseline = height() / 2 - ui::px(4);
        drawFigure(&painter, right, baseline, book.milliseconds, sizes::figureText * 3 / 4, ink);
        drawCaption(painter,
                    QRect(right - ui::px(400), baseline + ui::px(14), ui::px(400), captionHeight),
                    sessions, Qt::AlignRight | Qt::AlignTop, note);
    }

private:
    QString author;
};
} // namespace

class ActivityBooks final : public ActivityPage
{
public:
    // `name` names the page and its pager; `row` names its book rows. The
    // hidden books' list has its own names and no row to itself.
    ActivityBooks(QWidget *parent, const QString &name, const QString &row, bool hiddenRow)
        : ActivityPage(parent), rowName(row)
    {
        setObjectName(name);
        pager = new ui::PagerBar(this, name);
        connect(pager->previous, &QPushButton::clicked, this,
                [this] { showPage(pager->turnTarget(-1)); });
        connect(pager->next, &QPushButton::clicked, this,
                [this] { showPage(pager->turnTarget(1)); });
        empty = new ui::InterfaceLabel(strings::get("activity.no_books"), this);
        empty->setObjectName(name + "Empty");
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        ui::readerStyle(empty, ui::styles::font(chrome::labelText) + "background:transparent;");
        if (!hiddenRow)
            return;
        hiddenLink =
                new LinkRow(this, "rrActivityHiddenBooks", strings::get("activity.hidden_books"));
        hiddenLink->hide();
        connect(hiddenLink, &QPushButton::clicked, this, [this] {
            if (hiddenRequested)
                hiddenRequested();
        });
    }

    void reset()
    {
        months.clear();
        hiddenCount = page = 0;
        pages.clear();
        copyCaptions.clear();
        copyDescriptions.clear();
        clearCovers();
        for (auto *row : rows) {
            row->setBook({}, {});
            row->setCover({}, false);
            row->hide();
        }
        setProperty("activity_covers_pending", 0);
    }

    void setSummary(const activity::Summary &summary, const ActivityRecords &) override
    {
        setGroups(summary.months, int(summary.hiddenBooks.size()));
    }

    // The books in groups, each under its band, and a row to the hidden
    // books after them when there are any. A group without a month is the
    // hidden books' own list.
    void setGroups(const QVector<activity::MonthBooks> &groups, int hidden, bool firstPage = false)
    {
        clearCovers();
        months = groups;
        hiddenCount = hidden;
        // A reload, such as after hiding a book, keeps the page in view.
        showPage(firstPage ? 0 : page);
    }

    QHash<QString, QString> copyCaptions, copyDescriptions;
    bool showCopySource = false;
    std::function<void(QDate, const QString &)> chosen;
    std::function<void()> hiddenRequested;

protected:
    void resizeEvent(QResizeEvent *) override { showPage(page); }

    // Covers are asked for only while the page shows: the hidden books' list
    // and Books share one cover loader, whose newest request wins.
    void showEvent(QShowEvent *) override { showPage(page); }

private:
    struct Item
    {
        int month = 0, book = -1;
    };

    QVector<activity::MonthBooks> months;
    QString rowName;
    int hiddenCount = 0;
    LinkRow *hiddenLink = nullptr;
    QVector<QVector<Item>> pages;
    QVector<ui::ListHeader *> bands;
    QVector<BookRow *> rows;
    ui::PagerBar *pager;
    QLabel *empty;
    int page = 0;

    // Fills pages top to bottom. A month's band starts each page it continues
    // on, and never ends a page alone.
    void paginate()
    {
        pages.clear();
        const int header = ui::metrics::list::headerHeight;
        const int room = height() - chrome::actionHeight;
        const int row = BookRow::preferredHeight();
        QVector<Item> current;
        int used = 0;
        for (int month = 0; month < months.size(); ++month) {
            for (int book = 0; book < months[month].books.size(); ++book) {
                const bool needsBand = book == 0 || current.isEmpty();
                const int need = row + (needsBand ? header : 0);
                if (!current.isEmpty() && used + need > room) {
                    pages.append(current);
                    current.clear();
                    used = 0;
                }
                if (book == 0 || current.isEmpty()) {
                    current.append({ month, -1 });
                    used += header;
                }
                current.append({ month, book });
                used += row;
            }
        }
        // The row to the hidden books ends the list.
        if (hiddenCount && hiddenLink) {
            if (!current.isEmpty() && used + chapters::activityHeight > room) {
                pages.append(current);
                current.clear();
            }
            current.append({ hiddenItem, hiddenItem });
        }
        if (!current.isEmpty())
            pages.append(current);
    }

    static constexpr int hiddenItem = -2;

    void showPage(int target)
    {
        paginate();
        page = qBound(0, target, qMax(0, int(pages.size()) - 1));
        int bandsUsed = 0, rowsUsed = 0, y = 0;
        QVector<activity::BookTotal> wanted;
        if (hiddenLink)
            hiddenLink->hide();
        if (!pages.isEmpty())
            for (const auto &item : pages[page]) {
                if (item.month == hiddenItem) {
                    hiddenLink->setCaption(QString::number(hiddenCount));
                    hiddenLink->setGeometry(0, y, width(), chapters::activityHeight);
                    hiddenLink->show();
                    y += chapters::activityHeight;
                    continue;
                }
                const auto &month = months[item.month];
                if (item.book < 0) {
                    if (bandsUsed == bands.size())
                        bands.append(band(this, "rrActivityMonthBand"));
                    auto *header = bands[bandsUsed++];
                    setBand(header,
                            month.month.isValid()
                                    ? english().toString(month.month, QStringLiteral("MMMM yyyy"))
                                    : strings::get("activity.hidden"),
                            (showCopySource
                                     ? strings::get("activity.copies_count").arg(month.books.size())
                                     : plural("activity.book_count", "activity.books_count",
                                              month.books.size()))
                                    + QStringLiteral(" · ") + duration(month.milliseconds));
                    header->setGeometry(0, y, width(), ui::metrics::list::headerHeight);
                    header->show();
                    y += ui::metrics::list::headerHeight;
                    continue;
                }
                if (rowsUsed == rows.size()) {
                    auto *row = new BookRow(this);
                    connect(row, &QPushButton::clicked, this, [this, row] {
                        if (chosen)
                            chosen(row->month, row->book.book);
                    });
                    rows.append(row);
                }
                auto *row = rows[rowsUsed++];
                const auto &book = month.books[item.book];
                const int copies = month.copies.value(book.book).size();
                row->month = month.month;
                QString caption;
                if (copies)
                    caption = strings::get("activity.copies_count").arg(copies);
                else if (showCopySource)
                    caption = copyCaptions.value(book.book);
                row->setBook(book, caption);
                if (showCopySource)
                    row->setAccessibleDescription(copyDescriptions.value(book.book));
                row->setCover(covers.value(month.books[item.book].book),
                              missingBooks.contains(month.books[item.book].book));
                if (!known.contains(month.books[item.book].book))
                    wanted.append(month.books[item.book]);
                row->setObjectName(rowName + QString::number(rowsUsed));
                row->setGeometry(0, y, width(), BookRow::preferredHeight());
                row->show();
                y += BookRow::preferredHeight();
            }
        for (int index = bandsUsed; index < bands.size(); ++index)
            bands[index]->hide();
        for (int index = rowsUsed; index < rows.size(); ++index)
            rows[index]->hide();
        empty->setVisible(pages.isEmpty());
        empty->setGeometry(chrome::inset, 0, width() - 2 * chrome::inset, height());
        pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
        pager->setPage(page, qMax(1, int(pages.size())));
        // Only the shown rows ask for covers. A new request replaces an older
        // one that has not finished. Tests wait until the shown rows have theirs.
        setProperty("activity_covers_pending", int(wanted.size()));
        if (!wanted.isEmpty() && coversRequested && isVisible())
            coversRequested(wanted, BookRow::coverSize());
        prepareRows(this);
    }

public:
    std::function<void(const QVector<activity::BookTotal> &, QSize)> coversRequested;

    void setCover(const QString &book, const QImage &cover, bool missing)
    {
        known.insert(book);
        covers.insert(book, cover);
        if (missing)
            missingBooks.insert(book);
        // Covers can arrive while another tab hides the page, so rows count as
        // shown when the page shows them.
        for (auto *row : rows)
            if (row->isVisibleTo(this) && row->book.book == book)
                row->setCover(cover, missing);
        int waiting = 0;
        for (auto *row : rows)
            waiting += row->isVisibleTo(this) && !known.contains(row->book.book);
        setProperty("activity_covers_pending", waiting);
    }

private:
    void clearCovers()
    {
        known.clear();
        missingBooks.clear();
        covers.clear();
    }

    QHash<QString, QImage> covers;
    QSet<QString> known, missingBooks;
};

namespace {
// Reading time by hour of the day, the busiest two hours filled.
class HourBars final : public QWidget
{
public:
    explicit HourBars(QWidget *parent) : QWidget(parent) { setObjectName("rrActivityHours"); }

    void setHours(const QVector<qint64> &values, int peak)
    {
        hours = values;
        busiest = peak;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        const auto paper = palette().color(QPalette::Window);
        if (hours.size() != 24)
            return;
        const QFont caption(ui::captionFont());
        const int labels = ui::interfaceLineHeight(caption) + sizes::labelGap;
        const int base = height() - chrome::blockTop - labels;
        const int room = qMax(1, base - chrome::blockTop);
        const int inner = width() - 2 * chrome::inset;
        const int step = inner / 24;
        const int bar = qMax(2, step - ui::px(6));
        const int left = chrome::inset + (inner - 24 * step) / 2;
        qint64 maximum = 1;
        for (const qint64 value : hours)
            maximum = qMax(maximum, value);
        for (int index = 0; index < 24; ++index) {
            const int x = left + index * step;
            const int barHeight = hours[index]
                    ? qMax(ui::px(4), int(room * double(hours[index]) / double(maximum)))
                    : 0;
            const QRect area(x, base - barHeight, bar, barHeight);
            if (!barHeight) {
                QPen dash(ui::dividerColor(this), ui::metrics::control::border, Qt::DashLine);
                painter.setPen(dash);
                painter.drawLine(x, base - 1, x + bar, base - 1);
            } else if (busiest >= 0 && (index == busiest || index == (busiest + 1) % 24)) {
                painter.fillRect(area, ink);
            } else {
                painter.fillRect(area, paper);
                ui::paintEdges(painter, area,
                               ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge
                                       | Qt::RightEdge,
                               ink, chapters::outline);
            }
        }
        for (int index = 0; index <= 24; index += 6) {
            const int x = left + index * step - (index == 24 ? ui::px(6) : 0);
            const auto text = QString("%1:00").arg(index, 2, 10, QChar('0'));
            const int textWidth = ui::interfaceTextWidth(text, caption);
            const int textLeft = qBound(chrome::inset / 2, x - textWidth / 2,
                                        width() - chrome::inset / 2 - textWidth);
            drawCaption(painter, QRect(textLeft, base + sizes::labelGap, textWidth + 2, labels),
                        text, Qt::AlignLeft | Qt::AlignTop, ink);
        }
    }

private:
    QVector<qint64> hours;
    int busiest = -1;
};

// A row with a name and a caption on the left and a figure on the right.
class RecordRow final : public QWidget
{
public:
    explicit RecordRow(QWidget *parent) : QWidget(parent) { }

    void setRecord(const QString &name, const QString &detail, qint64 milliseconds,
                   const QString &count = {}, const QString &unit = {})
    {
        title = name;
        caption = detail;
        time = milliseconds;
        number = count;
        units = unit;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        painter.fillRect(0, height() - ui::metrics::control::border, width(),
                         ui::metrics::control::border, ui::dividerColor(this));
        const QFont name(ui::interfaceFont(chrome::labelText)), small(ui::captionFont());
        const int nameHeight = ui::interfaceLineHeight(name);
        const int captionHeight = ui::interfaceLineHeight(small);
        const int top = (height() - nameHeight - ui::px(4) - captionHeight) / 2;
        const int right = width() - chrome::inset;
        const int pixels = sizes::figureText * 7 / 10;
        const QFont figure(ui::brandFont(pixels)), unit(ui::interfaceFont(chapters::unitText));
        const int baseline = top + ui::interfaceLineAscent(name) + ui::px(10);
        int figureWidth = 0;
        if (!number.isEmpty()) {
            const int unitWidth = ui::interfaceTextWidth(units, unit);
            const int numberWidth = ui::interfaceTextWidth(number, figure);
            figureWidth = numberWidth + chapters::unitGap + unitWidth;
            drawText(painter,
                     QRect(right - figureWidth, baseline - ui::interfaceLineAscent(figure),
                           numberWidth + 2, ui::interfaceLineHeight(figure)),
                     number, Qt::AlignLeft | Qt::AlignTop, figure, ink);
            drawText(painter,
                     QRect(right - unitWidth, baseline - ui::interfaceLineAscent(unit),
                           unitWidth + 2, ui::interfaceLineHeight(unit)),
                     units, Qt::AlignLeft | Qt::AlignTop, unit, ink);
        } else if (time > 0) {
            figureWidth = drawFigure(&painter, right, baseline, time, pixels, ink);
        }
        const int textWidth = right - figureWidth - 2 * chrome::inset;
        drawText(painter, QRect(chrome::inset, top, textWidth, nameHeight), title,
                 Qt::AlignLeft | Qt::AlignTop, name, ink);
        drawCaption(painter,
                    QRect(chrome::inset, top + nameHeight + ui::px(4), textWidth, captionHeight),
                    ui::elideInterfaceText(caption.toUpper(), small, textWidth),
                    Qt::AlignLeft | Qt::AlignTop, ui::noteColor(this));
    }

private:
    QString title, caption, number, units;
    qint64 time = 0;
};

// A milestone's shape tells its kind apart: a book for finished books, a
// circle for hours, a hexagon for streaks and a rounded square for reading
// days. Reached ones are filled; ones ahead are dashed.
QPainterPath milestoneShape(activity::Milestone::Kind kind, const QRectF &area)
{
    using Kind = activity::Milestone::Kind;
    QPainterPath path;
    const qreal side = qMin(area.width(), area.height());
    const QRectF square(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
    switch (kind) {
    case Kind::Books: {
        const QRectF book(square.center().x() - side * .38, square.top(), side * .76, side);
        path.addRoundedRect(book, side * .06, side * .06);
        break;
    }
    case Kind::Hours:
        path.addEllipse(square);
        break;
    case Kind::Streak: {
        QPolygonF hexagon;
        for (int corner = 0; corner < 6; ++corner) {
            const qreal angle = M_PI / 3 * corner - M_PI / 2;
            hexagon << square.center()
                            + QPointF(std::cos(angle) * side / 2, std::sin(angle) * side / 2);
        }
        hexagon << hexagon.first();
        path.addPolygon(hexagon);
        break;
    }
    case Kind::DaysRead:
        path.addRoundedRect(square.adjusted(side * .05, side * .05, -side * .05, -side * .05),
                            side * .18, side * .18);
        break;
    }
    return path;
}

QString milestoneName(const activity::Milestone &milestone)
{
    using Kind = activity::Milestone::Kind;
    switch (milestone.kind) {
    case Kind::Books:
        return milestone.target == 1 ? strings::get("activity.milestone_first_book")
                                     : strings::get("activity.milestone_books");
    case Kind::Hours:
        return strings::get("activity.milestone_hours");
    case Kind::Streak:
        return strings::get("activity.milestone_streak");
    case Kind::DaysRead:
        return strings::get("activity.milestone_days_read");
    }
    return {};
}

// A badge's column: room for the shape and for the widest count below it, so
// counts never run together at a large text size.
int badgeColumn(const QVector<activity::Milestone> &milestones)
{
    const QFont caption(ui::captionFont(chrome::captionLabelText + ui::px(2)));
    int widest = 0;
    for (const auto &milestone : milestones)
        widest = qMax(widest,
                      ui::interfaceTextWidth(strings::get("activity.progress_of")
                                                     .arg(qMin(milestone.value, milestone.target))
                                                     .arg(milestone.target),
                                             caption));
    return qMax(int(ui::px(150)), widest + ui::px(16));
}

// Milestones in a grid. `named` puts the kind under each shape; a page that
// groups them by kind leaves it out. A milestone ahead shows its progress.
class Badges final : public QWidget
{
public:
    explicit Badges(QWidget *parent, const QString &name = QStringLiteral("rrActivityMilestones"))
        : QWidget(parent)
    {
        setObjectName(name);
    }

    void setMilestones(const QVector<activity::Milestone> &values, int columnCount, bool names)
    {
        milestones = values;
        columns = columnCount;
        named = names;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto ink = ui::labelInk(this);
        const auto paper = palette().color(QPalette::Window);
        const auto note = ui::noteColor(this);
        const int count = milestones.size();
        if (!count || columns <= 0)
            return;
        // A ladder wraps before its captions would touch.
        const int columns = qMax(1, qMin(this->columns, width() / badgeColumn(milestones)));
        const int rows = (count + columns - 1) / columns;
        // Wrapped rows keep a gap between them, so captions never crowd the
        // shapes below.
        const int rowGap = rows > 1 ? int(sizes::milestoneRowGap) : 0;
        const int cellWidth = width() / columns,
                  cellHeight = (height() - (rows - 1) * rowGap) / rows;
        const QFont caption(ui::captionFont(chrome::captionLabelText + ui::px(2)));
        const int captionHeight = ui::interfaceLineHeight(caption);
        const int lines = named ? 2 : 1;
        const int shape = qMax(ui::px(40),
                               qMin<int>(sizes::badgeSize,
                                         qMin(cellWidth - ui::px(16),
                                              cellHeight - lines * captionHeight - ui::px(24))));
        for (int index = 0; index < count; ++index) {
            const auto &milestone = milestones[index];
            const int column = index % columns, row = index / columns;
            const int top = row * (cellHeight + rowGap)
                    + (cellHeight - shape - ui::px(10) - lines * captionHeight) / 2;
            const QRectF area(column * cellWidth + (cellWidth - shape) / 2.0, top, shape, shape);
            const bool reached = milestone.reached();
            const auto path =
                    milestoneShape(milestone.kind,
                                   area.adjusted(sizes::badgeRule / 2.0, sizes::badgeRule / 2.0,
                                                 -sizes::badgeRule / 2.0, -sizes::badgeRule / 2.0));
            if (reached) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(ink);
            } else {
                painter.setPen(QPen(ui::dividerColor(this), sizes::badgeRule, Qt::DashLine));
                painter.setBrush(Qt::NoBrush);
            }
            painter.drawPath(path);
            // The book's spine.
            if (milestone.kind == activity::Milestone::Kind::Books) {
                const qreal x = area.center().x() - shape * .26;
                painter.setPen(QPen(reached ? paper : ui::dividerColor(this),
                                    ui::metrics::control::border));
                painter.drawLine(QPointF(x, area.top() + shape * .08),
                                 QPointF(x, area.bottom() - shape * .08));
            }
            // The number's ink sits in the middle of the shape; a book's is
            // between its spine and its right edge.
            const auto number = QString::number(milestone.target);
            // Longer numbers get smaller; a book is narrower than the others.
            const bool book = milestone.kind == activity::Milestone::Kind::Books;
            painter.setFont(ui::brandFont(number.size() > 3                   ? shape * 2 / 5
                                                  : number.size() > 2 && book ? shape * 2 / 5
                                                                              : shape / 2));
            painter.setPen(reached ? paper : note);
            const qreal shift = book ? shape * .06 : 0;
            ui::drawOpticalText(painter, area.translated(shift, 0), number, true);
            QRect label(column * cellWidth, int(area.bottom()) + ui::px(10), cellWidth,
                        captionHeight);
            painter.setFont(caption);
            painter.setPen(reached ? ink : note);
            if (named) {
                ui::drawInterfaceText(painter, label,
                                      ui::elideInterfaceText(milestoneName(milestone).toUpper(),
                                                             caption, cellWidth - ui::px(8)),
                                      Qt::AlignHCenter | Qt::AlignTop);
                label.translate(0, captionHeight);
            }
            // Reached ones show their progress too, as "1 / 1".
            ui::drawInterfaceText(painter, label,
                                  strings::get("activity.progress_of")
                                          .arg(qMin(milestone.value, milestone.target))
                                          .arg(milestone.target),
                                  Qt::AlignHCenter | Qt::AlignTop);
        }
    }

private:
    QVector<activity::Milestone> milestones;
    int columns = 4;
    bool named = true;
};

// For Records: per kind, the last milestone reached and the next one, or the
// first two while none is reached.
QVector<activity::Milestone> milestonePicks(const QVector<activity::Milestone> &all)
{
    QVector<activity::Milestone> picks;
    for (const auto kind :
         { activity::Milestone::Kind::Books, activity::Milestone::Kind::Hours,
           activity::Milestone::Kind::Streak, activity::Milestone::Kind::DaysRead }) {
        QVector<activity::Milestone> ladder;
        for (const auto &milestone : all)
            if (milestone.kind == kind)
                ladder.append(milestone);
        int next = 0;
        while (next < ladder.size() && ladder[next].reached())
            ++next;
        const int first = qBound(0, next - 1, qMax(0, int(ladder.size()) - 2));
        for (int index = first; index < qMin(int(ladder.size()), first + 2); ++index)
            picks.append(ladder[index]);
    }
    return picks;
}
} // namespace

class ActivityRecordsPage final : public ActivityPage
{
public:
    explicit ActivityRecordsPage(QWidget *parent) : ActivityPage(parent)
    {
        setObjectName("rrActivityRecords");
        hoursBand = band(this, "rrActivityHoursBand");
        hours = new HourBars(this);
        recordsBand = band(this, "rrActivityRecordsBand");
        for (int index = 0; index < 3; ++index)
            records.append(new RecordRow(this));
        milestonesBand = band(this, "rrActivityMilestonesBand");
        badges = new Badges(this);
        all = new LinkRow(this, "rrActivityAllMilestones", strings::get("activity.all_milestones"));
        connect(all, &QPushButton::clicked, this, [this] {
            if (allRequested)
                allRequested();
        });
    }

    std::function<void()> allRequested;

    void setSummary(const activity::Summary &summary, const ActivityRecords &) override
    {
        int peak = -1;
        qint64 best = 0;
        for (int index = 0; index < 24; ++index) {
            const qint64 pair = summary.hours[index] + summary.hours[(index + 1) % 24];
            if (pair > best) {
                best = pair;
                peak = index;
            }
        }
        hours->setHours(summary.hours, peak);
        hoursDetail = peak >= 0
                ? strings::get("activity.mostly")
                          .arg(QString("%1:00").arg(peak, 2, 10, QChar('0')))
                          .arg(QString("%1:00").arg((peak + 2) % 24, 2, 10, QChar('0')))
                : QString();
        fitHoursBand();
        setBand(recordsBand, strings::get("activity.records"));
        records[0]->setRecord(strings::get("activity.longest_session"),
                              // A hidden book's title stays out of the record.
                              !summary.longestSession ? QString()
                                      : summary.longestSessionTitle.isEmpty()
                                      ? day(summary.longestSessionDate)
                                      : summary.longestSessionTitle + QStringLiteral(" · ")
                                              + day(summary.longestSessionDate),
                              summary.longestSession);
        records[1]->setRecord(strings::get("activity.most_in_day"),
                              summary.bestDay ? english().toString(summary.bestDayDate,
                                                                   QStringLiteral("ddd d MMM"))
                                              + QStringLiteral(" · ")
                                              + plural("activity.book_count",
                                                       "activity.books_count", summary.bestDayBooks)
                                              : QString(),
                              summary.bestDay);
        records[2]->setRecord(strings::get("activity.longest_streak"),
                              summary.longest.days ? day(summary.longest.first)
                                              + QStringLiteral(" – ") + day(summary.longest.last)
                                                   : QString(),
                              0, QString::number(summary.longest.days),
                              summary.longest.days == 1 ? strings::get("activity.day")
                                                        : strings::get("activity.days"));
        int reached = 0;
        for (const auto &milestone : summary.milestones)
            reached += milestone.reached();
        setBand(milestonesBand, strings::get("activity.milestones"));
        all->setCaption(
                strings::get("activity.reached_of").arg(reached).arg(summary.milestones.size()));
        picks = milestonePicks(summary.milestones);
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *) override { arrange(); }

private:
    ui::ListHeader *hoursBand, *recordsBand, *milestonesBand;
    QString hoursDetail;

    // The band keeps its title. The busiest hours give way when both do not
    // fit, as in the landscape column at 150% text.
    void fitHoursBand()
    {
        const auto title = strings::get("activity.when_you_read");
        const int room = hoursBand->width() - 2 * ui::metrics::list::inset - ui::metrics::list::gap;
        const bool fits = ui::interfaceTextWidth(title.toUpper(), hoursBand->leftLabel()->font())
                        + ui::interfaceTextWidth(hoursDetail.toUpper(),
                                                 hoursBand->rightLabel()->font())
                <= room;
        setBand(hoursBand, title, fits ? hoursDetail : QString());
    }

    HourBars *hours;
    QVector<RecordRow *> records;
    Badges *badges;
    LinkRow *all;
    QVector<activity::Milestone> picks;

    int recordHeight() const
    {
        return rowHeight(sizes::sessionHeight,
                         ui::interfaceLineHeight(ui::interfaceFont(chrome::labelText)) + ui::px(4)
                                 + ui::interfaceLineHeight(ui::captionFont()));
    }

    void arrange()
    {
        const int header = ui::metrics::list::headerHeight;
        const int row = recordHeight();
        const int border = ui::metrics::control::border;
        // Landscape: the hours beside the records, and the milestones in one row.
        const int link = chapters::activityHeight;
        if (wideLayout(this)) {
            // A short landscape screen, such as a phone's, keeps the three
            // records whole: the milestones give way to their link.
            const bool badgesShown = height() - header - ui::px(220) - link >= header + 3 * row;
            milestonesBand->setVisible(badgesShown);
            badges->setVisible(badgesShown);
            const int badgeHeight = badgesShown ? int(ui::px(220)) : 0;
            const int upper = height() - (badgesShown ? header + badgeHeight : 0) - link;
            const int half = width() / 2;
            hoursBand->setGeometry(0, 0, half, header);
            fitHoursBand();
            hours->setGeometry(0, header, half, upper - header);
            recordsBand->setGeometry(half + border, 0, width() - half - border, header);
            for (int index = 0; index < records.size(); ++index)
                records[index]->setGeometry(half + border, header + index * row,
                                            width() - half - border, row);
            milestonesBand->setGeometry(0, upper, width(), header);
            badges->setGeometry(0, upper + header, width(), badgeHeight);
            badges->setMilestones(picks, qMax(1, int(picks.size())), true);
            all->setGeometry(0, height() - link, width(), link);
            update();
            return;
        }
        milestonesBand->show();
        badges->show();
        const int badgeHeight = ui::px(400);
        badges->setMilestones(picks, qMax(1, (int(picks.size()) + 1) / 2), true);
        const int chart = qMax(ui::px(150), height() - 3 * header - 3 * row - badgeHeight - link);
        int y = 0;
        hoursBand->setGeometry(0, y, width(), header);
        fitHoursBand();
        y += header;
        hours->setGeometry(0, y, width(), chart);
        y += chart;
        recordsBand->setGeometry(0, y, width(), header);
        y += header;
        for (auto *record : records) {
            record->setGeometry(0, y, width(), row);
            y += row;
        }
        milestonesBand->setGeometry(0, y, width(), header);
        y += header;
        badges->setGeometry(0, y, width(), qMax(ui::px(120), height() - y - link));
        all->setGeometry(0, height() - link, width(), link);
        update();
    }

    void paintEvent(QPaintEvent *event) override
    {
        ActivityPage::paintEvent(event);
        if (!wideLayout(this))
            return;
        QPainter painter(this);
        // The rule ends where the milestones band starts, or above the link
        // when the milestones gave way.
        paintVerticalRule(painter, this, width() / 2, 0,
                          milestonesBand->isVisibleTo(this) ? milestonesBand->y() : all->y());
    }
};

// Every milestone, grouped by kind: a band with how many are reached, then the
// ladder of shapes. Landscape puts the kinds in two columns.
class ActivityMilestones final : public ActivityPage
{
public:
    explicit ActivityMilestones(QWidget *parent) : ActivityPage(parent)
    {
        // Not "…MilestonesPage": the pager names its counter that.
        setObjectName("rrActivityMilestonesScreen");
        const char *const names[] = { "Books", "Hours", "Streak", "Days" };
        for (int kind = 0; kind < 4; ++kind) {
            bands.append(band(this, QString("rrActivity%1MilestonesBand").arg(names[kind])));
            ladders.append(new Badges(this, QString("rrActivity%1Milestones").arg(names[kind])));
            auto *phrase = new ui::InterfaceLabel(this);
            phrase->setObjectName(QString("rrActivity%1MilestonesPhrase").arg(names[kind]));
            phrase->setWordWrap(true);
            phrase->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            ui::readerStyle(phrase,
                            ui::styles::font(chapters::unitText)
                                    + "background:transparent;color:palette(placeholder-text);");
            phrases.append(phrase);
        }
        pager = new ui::PagerBar(this, "rrActivityMilestones");
        connect(pager->previous, &QPushButton::clicked, this, [this] {
            page = pager->turnTarget(-1);
            arrange();
        });
        connect(pager->next, &QPushButton::clicked, this, [this] {
            page = pager->turnTarget(1);
            arrange();
        });
    }

    void resetPage() { page = 0; }

    // The streaks let the streak line say whether the longest one is still
    // going, and if not, when it ended and what the current one is.
    void setMilestones(const QVector<activity::Milestone> &all, const activity::Streak &current,
                       const activity::Streak &longest)
    {
        currentStreak = current;
        longestStreak = longest;
        const char *const titles[] = { "activity.milestones_books", "activity.milestones_hours",
                                       "activity.milestones_streak", "activity.milestones_days" };
        for (int kind = 0; kind < 4; ++kind) {
            QVector<activity::Milestone> ladder;
            int reached = 0;
            for (const auto &milestone : all)
                if (int(milestone.kind) == kind) {
                    ladder.append(milestone);
                    reached += milestone.reached();
                }
            setBand(bands[kind], strings::get(titles[kind]),
                    strings::get("activity.reached_of").arg(reached).arg(ladder.size()));
            ladders[kind]->setMilestones(ladder, qMax(1, int(ladder.size())), false);
            counts[kind] = ladder.size();
            columnWidths[kind] = badgeColumn(ladder);
            phrases[kind]->setText(phrase(ladder));
        }
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *) override { arrange(); }

private:
    QVector<ui::ListHeader *> bands;
    QVector<Badges *> ladders;
    QVector<QLabel *> phrases;
    ui::PagerBar *pager;
    int counts[4] = { 0, 0, 0, 0 };
    int columnWidths[4] = { 1, 1, 1, 1 };
    int page = 0;

    // Where the reader stands on a ladder and how far the next milestone is:
    // "You've finished 2 books. 3 more to reach 5."
    activity::Streak currentStreak, longestStreak;

    QString phrase(const QVector<activity::Milestone> &ladder) const
    {
        if (ladder.isEmpty())
            return {};
        const int value = ladder.first().value;
        int next = -1;
        for (const auto &milestone : ladder)
            if (!milestone.reached()) {
                next = milestone.target;
                break;
            }
        using Kind = activity::Milestone::Kind;
        const auto kind = ladder.first().kind;
        const char *const keys[][3] = {
            { "activity.phrase_books_none", "activity.phrase_books_one", "activity.phrase_books" },
            { "activity.phrase_hours_none", "activity.phrase_hours_one", "activity.phrase_hours" },
            { "activity.phrase_streak_none", "activity.phrase_streak_one",
              "activity.phrase_streak" },
            { "activity.phrase_days_none", "activity.phrase_days_one", "activity.phrase_days" },
        };
        const auto *row = keys[kind == Kind::Books            ? 0
                                       : kind == Kind::Hours  ? 1
                                       : kind == Kind::Streak ? 2
                                                              : 3];
        auto standing = value <= 0 ? strings::get(row[0])
                : value == 1       ? strings::get(row[1])
                                   : strings::get(row[2]).arg(value);
        // Streaks keep to the streak itself: the longest one while it is still
        // going, otherwise when it ended and what the current one is.
        if (kind == Kind::Streak && value > 0) {
            if (currentStreak.days && currentStreak.first == longestStreak.first)
                return value == 1 ? strings::get("activity.phrase_streak_ongoing_one")
                                  : strings::get("activity.phrase_streak_ongoing").arg(value);
            const auto ended =
                    (value == 1 ? strings::get("activity.phrase_streak_ended_one")
                                : strings::get("activity.phrase_streak_ended").arg(value))
                            .arg(day(longestStreak.last));
            return ended + ' '
                    + (currentStreak.days == 0 ? strings::get("activity.phrase_current_none")
                               : currentStreak.days == 1
                               ? strings::get("activity.phrase_current_one")
                               : strings::get("activity.phrase_current").arg(currentStreak.days));
        }
        return next < 0
                ? standing + ' ' + strings::get("activity.phrase_all")
                : standing + ' ' + strings::get("activity.phrase_next").arg(next - value).arg(next);
    }

    // The height a kind needs at a comfortable shape size: its band, its
    // rows of shapes and its line.
    int kindHeight(int kind, int width) const
    {
        const int columns = qMax(1, qMin(qMax(1, counts[kind]), width / columnWidths[kind]));
        const int rows = (qMax(1, counts[kind]) + columns - 1) / columns;
        return ui::metrics::list::headerHeight + sizes::milestoneTop + rows * sizes::milestoneRow
                + (rows - 1) * sizes::milestoneRowGap + sizes::milestoneGap
                + phrases[kind]->heightForWidth(width - 2 * chrome::inset) + sizes::milestoneBottom;
    }

    void arrange()
    {
        const int header = ui::metrics::list::headerHeight;
        const bool wide = wideLayout(this);
        const int columns = wide ? 2 : 1;
        const int border = ui::metrics::control::border;
        const int columnWidth = width() / columns;
        // All four kinds when they fit, otherwise fewer to a page with the
        // pager, as on a phone.
        const auto fits = [&](int perPage) {
            for (int first = 0; first < 4; first += perPage) {
                int total = 0;
                for (int row = first; row < qMin(4, first + perPage); row += columns) {
                    int tallest = 0;
                    for (int kind = row; kind < qMin(4, qMin(first + perPage, row + columns));
                         ++kind)
                        tallest = qMax(tallest, kindHeight(kind, columnWidth));
                    total += tallest;
                }
                if (total > height() - (perPage < 4 ? int(chrome::actionHeight) : 0))
                    return false;
            }
            return true;
        };
        int perPage = 4;
        while (perPage > columns && !fits(perPage))
            perPage /= 2;
        const int pages = 4 / perPage;
        page = qBound(0, page, pages - 1);
        const int pagerHeight = pages > 1 ? int(chrome::actionHeight) : 0;
        const int rows = qMax(1, perPage / columns);
        // Each row of kinds takes the height its tallest kind needs, from the
        // top; spare height stays at the bottom.
        QVector<int> rowTops(rows + 1, 0);
        for (int row = 0; row < rows; ++row) {
            int tallest = 0;
            for (int column = 0; column < columns; ++column) {
                const int kind = page * perPage + row * columns + column;
                if (kind < 4)
                    tallest = qMax(tallest, kindHeight(kind, columnWidth));
            }
            rowTops[row + 1] = rowTops[row] + tallest;
        }
        const int room = height() - pagerHeight;
        if (rowTops[rows] > room)
            for (int row = 1; row <= rows; ++row)
                rowTops[row] = room * row / rows;
        for (int kind = 0; kind < 4; ++kind) {
            const bool shown = kind / perPage == page;
            bands[kind]->setVisible(shown);
            ladders[kind]->setVisible(shown);
            phrases[kind]->setVisible(shown);
            if (!shown)
                continue;
            const int slot = kind % perPage;
            const int column = slot % columns, row = slot / columns;
            const int left = width() * column / columns + (column ? border : 0);
            const int right = width() * (column + 1) / columns;
            const int top = rowTops[row];
            const int bottom = rowTops[row + 1];
            bands[kind]->setGeometry(left, top, right - left, header);
            const int textWidth = right - left - 2 * chrome::inset;
            const int phraseHeight = phrases[kind]->heightForWidth(textWidth);
            // Room above the shapes, between their captions and the line, and
            // before the next band.
            const int bottomGap = sizes::milestoneBottom;
            ladders[kind]->setGeometry(left, top + header + sizes::milestoneTop, right - left,
                                       bottom - top - header - sizes::milestoneTop - phraseHeight
                                               - sizes::milestoneGap - bottomGap);
            phrases[kind]->setGeometry(left + chrome::inset, bottom - phraseHeight - bottomGap,
                                       textWidth, phraseHeight);
        }
        pager->setVisible(pages > 1);
        pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
        pager->setPage(page, pages);
        update();
    }

    void paintEvent(QPaintEvent *event) override
    {
        ActivityPage::paintEvent(event);
        if (!wideLayout(this))
            return;
        QPainter painter(this);
        paintVerticalRule(painter, this, width() / 2, 0,
                          height() - (pager->isVisible() ? int(chrome::actionHeight) : 0));
    }
};

namespace {
// The open book's progress after each day it was read, a dashed line from
// today to the end at the recent pace, and the time per day for other books.
class BookChart final : public QWidget
{
public:
    // The height that leaves a readable plot below the labels at this text size.
    static int minimumHeight()
    {
        return 2 * chrome::blockTop + 2 * ui::interfaceLineHeight(ui::captionFont())
                + sizes::labelGap + ui::px(110);
    }

    explicit BookChart(QWidget *parent) : QWidget(parent) { setObjectName("rrActivityBookChart"); }

    // Percent at the end of each reading day, oldest first, and the projected
    // finish date or an invalid date.
    void setProgress(const QVector<QPair<QDate, int>> &values, const QDate &now,
                     const QDate &finish)
    {
        points = values;
        days.clear();
        today = now;
        end = finish;
        update();
    }

    void setDays(const QMap<QDate, qint64> &values, const QDate &now)
    {
        points.clear();
        days = values;
        today = now;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (!points.isEmpty())
            paintProgress(painter);
        else
            paintDays(painter);
    }

private:
    QVector<QPair<QDate, int>> points;
    QMap<QDate, qint64> days;
    QDate today, end;

    void paintProgress(QPainter &painter)
    {
        painter.setRenderHint(QPainter::Antialiasing);
        const auto ink = ui::labelInk(this);
        const QFont caption(ui::captionFont());
        const int captionHeight = ui::interfaceLineHeight(caption);
        // The percent column grows with the text, so "100%" keeps its digits.
        const int axis =
                qMax<int>(sizes::chartInset,
                          ui::interfaceTextWidth(strings::get("common.percent").arg(100), caption)
                                  + ui::px(12));
        const QRect plot(chrome::inset + axis, chrome::blockTop + captionHeight,
                         width() - 2 * chrome::inset - axis - ui::px(12),
                         height() - 2 * chrome::blockTop - 2 * captionHeight - sizes::labelGap);
        const QDate first = points.first().first;
        const QDate last = end.isValid() && end > today ? end : today;
        const qint64 span = qMax<qint64>(1, first.daysTo(last));
        const auto x = [&](const QDate &date) {
            return plot.left() + plot.width() * double(first.daysTo(date)) / double(span);
        };
        const auto y = [&](int percent) { return plot.bottom() - plot.height() * percent / 100.0; };
        for (int percent = 0; percent <= 100; percent += 25) {
            if (percent)
                painter.setPen(
                        QPen(ui::dividerColor(this), ui::metrics::control::border, Qt::DotLine));
            else
                painter.setPen(QPen(ink, ui::metrics::control::border));
            painter.drawLine(QPointF(plot.left(), y(percent)), QPointF(plot.right(), y(percent)));
            drawCaption(painter,
                        QRect(chrome::inset, int(y(percent)) - captionHeight / 2, axis - ui::px(12),
                              captionHeight),
                        strings::get("common.percent").arg(percent),
                        Qt::AlignRight | Qt::AlignVCenter, ink);
        }
        QPainterPath line;
        line.moveTo(x(first), y(0));
        int previous = 0;
        for (const auto &point : points) {
            line.lineTo(x(point.first), y(previous));
            line.lineTo(x(point.first), y(point.second));
            previous = point.second;
        }
        line.lineTo(x(today), y(previous));
        QPen solid(ink, sizes::lineWidth);
        solid.setJoinStyle(Qt::MiterJoin);
        painter.setPen(solid);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(line);
        painter.setPen(QPen(ink, ui::metrics::control::border));
        painter.drawLine(QPointF(x(today), plot.top() - ui::px(6)),
                         QPointF(x(today), plot.bottom()));
        if (end.isValid() && end > today) {
            painter.setPen(QPen(ink, ui::px(3), Qt::DashLine));
            painter.drawLine(QPointF(x(today), y(previous)), QPointF(x(end), y(100)));
            painter.setPen(QPen(ink, ui::px(3)));
            painter.setBrush(palette().color(QPalette::Window));
            painter.drawEllipse(QPointF(x(end), y(100)), sizes::dotRadius, sizes::dotRadius);
            const QFont italic(ui::brandFont(chrome::labelText, true));
            const int textHeight = ui::interfaceLineHeight(italic);
            drawText(painter,
                     QRect(plot.left(), int(y(100)) - textHeight - ui::px(6),
                           int(x(end)) - plot.left() - sizes::dotRadius - ui::px(12), textHeight),
                     strings::get("activity.at_recent_pace"), Qt::AlignRight | Qt::AlignTop, italic,
                     ink);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(QPointF(x(today), y(previous)), sizes::dotRadius, sizes::dotRadius);
        const int labelTop = plot.bottom() + sizes::labelGap;
        const auto area = [&](const QDate &date, const QString &text, Qt::Alignment flags) {
            const int textWidth = ui::interfaceTextWidth(text.toUpper(), caption) + 2;
            int left = int(x(date)) - textWidth / 2;
            if (flags & Qt::AlignLeft)
                left = int(x(date));
            if (flags & Qt::AlignRight)
                left = int(x(date)) - textWidth;
            return QRect(left, labelTop, textWidth, captionHeight);
        };
        const bool ahead = end.isValid() && end > today;
        const auto todayText = strings::get("activity.today");
        const auto todayArea = area(today, todayText, ahead ? Qt::AlignHCenter : Qt::AlignRight);
        const auto firstArea = area(first, day(first), Qt::AlignLeft);
        if (!firstArea.adjusted(0, 0, ui::px(16), 0).intersects(todayArea))
            drawCaption(painter, firstArea, day(first), Qt::AlignLeft | Qt::AlignTop, ink);
        drawCaption(painter, todayArea, todayText, Qt::AlignLeft | Qt::AlignTop, ink);
        if (ahead)
            drawCaption(painter, area(end, day(end), Qt::AlignRight), day(end),
                        Qt::AlignLeft | Qt::AlignTop, ink);
    }

    void paintDays(QPainter &painter)
    {
        const auto ink = ui::labelInk(this);
        const auto paper = palette().color(QPalette::Window);
        if (days.isEmpty())
            return;
        // The last thirty days the book was read in, up to its last day.
        const QDate last = days.lastKey();
        const QDate first = qMax(days.firstKey(), last.addDays(-29));
        const int count = int(first.daysTo(last)) + 1;
        const QFont caption(ui::captionFont());
        const int captionHeight = ui::interfaceLineHeight(caption);
        const int base = height() - chrome::blockTop - captionHeight - sizes::labelGap;
        const int room = qMax(1, base - chrome::blockTop - captionHeight / 2);
        qint64 maximum = 1;
        for (auto date = first; date <= last; date = date.addDays(1))
            maximum = qMax(maximum, days.value(date));
        // A scale of at most four lines at a round time, so the bars read as
        // time: 15 or 30 minutes, or whole hours.
        qint64 scale = 15 * minute;
        for (const qint64 candidate : { 15, 30, 60, 120, 180, 240, 360, 480 }) {
            scale = candidate * minute;
            if (maximum <= 4 * scale)
                break;
        }
        const int lines = int((maximum + scale - 1) / scale);
        maximum = lines * scale;
        const auto scaleLabel = [](qint64 milliseconds) {
            const qint64 minutes = milliseconds / minute;
            return minutes < 60 ? strings::get("stats.duration_minutes").arg(minutes)
                    : minutes % 60
                    ? strings::get("activity.duration_short").arg(minutes / 60).arg(minutes % 60)
                    : strings::get("activity.duration_hours").arg(minutes / 60);
        };
        int labelWidth = 0;
        for (int line = 1; line <= lines; ++line)
            labelWidth = qMax(labelWidth,
                              ui::interfaceTextWidth(scaleLabel(line * scale).toUpper(), caption));
        const int plotLeft = chrome::inset + labelWidth + ui::px(16);
        for (int line = 1; line <= lines; ++line) {
            const int y = base - int(room * double(line * scale) / double(maximum));
            painter.setPen(QPen(ui::dividerColor(this), ui::metrics::control::border, Qt::DotLine));
            painter.drawLine(plotLeft, y, width() - chrome::inset, y);
            drawCaption(painter,
                        QRect(chrome::inset, y - captionHeight / 2, labelWidth, captionHeight),
                        scaleLabel(line * scale), Qt::AlignRight | Qt::AlignVCenter, ink);
        }
        const int inner = width() - chrome::inset - plotLeft;
        const int step = inner / qMax(count, 7);
        const int bar = qMax(2, qMin<int>(sizes::maximumBarWidth, step - ui::px(8)));
        const int left = plotLeft + (inner - count * step) / 2;
        for (int index = 0; index < count; ++index) {
            const auto date = first.addDays(index);
            const qint64 value = days.value(date);
            const int x = left + index * step + (step - bar) / 2;
            if (value <= 0) {
                painter.fillRect(x, base - ui::metrics::control::border, bar,
                                 ui::metrics::control::border, ui::dividerColor(this));
                continue;
            }
            const int barHeight = qMax(ui::px(4), int(room * double(value) / double(maximum)));
            const QRect area(x, base - barHeight, bar, barHeight);
            painter.fillRect(area, paper);
            ui::paintEdges(painter, area,
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           ink, chapters::outline);
        }
        drawCaption(painter, QRect(plotLeft, base + sizes::labelGap, inner, captionHeight),
                    day(first), Qt::AlignLeft | Qt::AlignTop, ink);
        drawCaption(painter, QRect(plotLeft, base + sizes::labelGap, inner, captionHeight),
                    day(last), Qt::AlignRight | Qt::AlignTop, ink);
    }
};

// The book's title, author and progress above its figures.
class BookHeading final : public QWidget
{
public:
    // Its lines with room above and below; it grows with the text size.
    static int preferredHeight()
    {
        const int caption = ui::interfaceLineHeight(ui::captionFont());
        const int block = ui::interfaceLineHeight(ui::interfaceFont(ui::metrics::text::heading))
                + ui::px(8) + caption + ui::px(20) + caption;
        return qMax(int(ui::px(190)), block + 2 * ui::px(24));
    }

    explicit BookHeading(QWidget *parent) : QWidget(parent)
    {
        setObjectName("rrActivityBookHeading");
    }

    void setBook(const QString &name, const QString &by, int value, bool done)
    {
        title = name;
        author = authorCaption(by);
        percent = value;
        finished = done;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        const QFont name(ui::interfaceFont(ui::metrics::text::heading)), caption(ui::captionFont());
        const int inner = width() - 2 * chrome::inset;
        const int nameHeight = ui::interfaceLineHeight(name);
        const int captionHeight = ui::interfaceLineHeight(caption);
        int y = (height() - nameHeight - ui::px(8) - captionHeight - ui::px(20) - captionHeight)
                / 2;
        drawText(painter, QRect(chrome::inset, y, inner, nameHeight),
                 ui::elideInterfaceText(title, name, inner), Qt::AlignLeft | Qt::AlignTop, name,
                 ink);
        y += nameHeight + ui::px(8);
        drawCaption(painter, QRect(chrome::inset, y, inner, captionHeight),
                    ui::elideInterfaceText(author, caption, inner), Qt::AlignLeft | Qt::AlignTop,
                    ui::noteColor(this));
        y += captionHeight + ui::px(20);
        if (finished) {
            drawCaption(painter, QRect(chrome::inset, y, inner, captionHeight),
                        strings::get("activity.finished_mark"), Qt::AlignLeft | Qt::AlignTop, ink);
            return;
        }
        if (percent < 0)
            return;
        const int bar = qMin(inner - ui::px(120), ui::px(520));
        const QRect track(chrome::inset, y + (captionHeight - sizes::progressHeight) / 2, bar,
                          sizes::progressHeight);
        painter.fillRect(track.adjusted(0, 0, -(track.width() * (100 - percent) / 100), 0), ink);
        ui::paintEdges(painter, track,
                       ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge, ink);
        drawCaption(painter, QRect(chrome::inset + bar + ui::px(16), y, ui::px(120), captionHeight),
                    strings::get("common.percent").arg(percent), Qt::AlignLeft | Qt::AlignTop, ink);
    }

private:
    QString title, author;
    int percent = -1;
    bool finished = false;
};

// One session: when it started and how far it went, and its length.
class SessionRow final : public QWidget
{
public:
    explicit SessionRow(QWidget *parent) : QWidget(parent) { }

    static int preferredHeight()
    {
        return rowHeight(sizes::sessionHeight,
                         ui::interfaceLineHeight(ui::interfaceFont(chrome::labelText)) + ui::px(4)
                                 + ui::interfaceLineHeight(ui::captionFont()));
    }

    void setSession(const QString &when, const QString &range, qint64 milliseconds)
    {
        start = when;
        detail = range;
        time = milliseconds;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        painter.fillRect(0, height() - ui::metrics::control::border, width(),
                         ui::metrics::control::border, ui::dividerColor(this));
        const QFont name(ui::interfaceFont(chrome::labelText)), caption(ui::captionFont());
        const int nameHeight = ui::interfaceLineHeight(name);
        const int captionHeight = detail.isEmpty() ? 0 : ui::interfaceLineHeight(caption);
        const int top =
                (height() - nameHeight - (captionHeight ? ui::px(4) + captionHeight : 0)) / 2;
        const auto length = duration(time);
        const int lengthWidth = ui::interfaceTextWidth(length, name) + 2;
        drawText(painter, QRect(width() - chrome::inset - lengthWidth, 0, lengthWidth, height()),
                 length, Qt::AlignRight | Qt::AlignVCenter, name, ink);
        drawText(painter,
                 QRect(chrome::inset, top, width() - 3 * chrome::inset - lengthWidth, nameHeight),
                 start, Qt::AlignLeft | Qt::AlignTop, name, ink);
        if (captionHeight)
            drawCaption(painter,
                        QRect(chrome::inset, top + nameHeight + ui::px(4), width(), captionHeight),
                        detail, Qt::AlignLeft | Qt::AlignTop, ui::noteColor(this));
    }

private:
    QString start, detail;
    qint64 time = 0;
};
} // namespace

class ActivityBookPage final : public ActivityPage
{
public:
    explicit ActivityBookPage(QWidget *parent) : ActivityPage(parent)
    {
        setObjectName("rrActivityBookPage");
        heading = new BookHeading(this);
        timeCell = new StatsCell(this, "rrActivityBookTime", strings::get("activity.time_read"));
        sessionsCell =
                new StatsCell(this, "rrActivityBookSessions", strings::get("activity.sessions"));
        lastCell = new StatsCell(this, "rrActivityBookLast", strings::get("activity.to_finish"));
        cells = { timeCell, sessionsCell, lastCell };
        chartBand = band(this, "rrActivityChartBand");
        chart = new BookChart(this);
        sessionsBand = band(this, "rrActivitySessionsBand");
        pager = new ui::PagerBar(this, "rrActivitySessions");
        connect(pager->previous, &QPushButton::clicked, this,
                [this] { showSessions(pager->turnTarget(-1)); });
        connect(pager->next, &QPushButton::clicked, this,
                [this] { showSessions(pager->turnTarget(1)); });
    }

    void forget() { book.clear(); }

    void reset()
    {
        book.clear();
        total = {};
        sessions.clear();
        days.clear();
        extras = {};
        chart->setDays({}, {});
        page = 0;
    }

    void setBook(const QString &key, const ActivityRecords &records, const QDate &now,
                 const ActivityOpenBook &open)
    {
        // A reload of the same book keeps the page of sessions in view.
        const bool same = key == book;
        book = key;
        today = now;
        total = activity::bookTotal(records, key, now);
        sessions = activity::bookSessions(records, key, now);
        days = activity::bookDays(records, key, now);
        extras = open.book == key ? open : ActivityOpenBook{};
        const int percent = extras.percent >= 0 ? extras.percent : total.percent;
        heading->setBook(total.title, total.author, percent, total.finished);
        setDuration(timeCell, total.milliseconds,
                    total.first ? strings::get("activity.started")
                                          .arg(day(activity::localDate(total.first), true))
                                : QString());
        sessionsCell->set(QString::number(total.sessions), QString(),
                          total.sessions
                                  ? strings::get("activity.average")
                                            .arg(duration(total.milliseconds / total.sessions))
                                  : QString());
        QDate finish;
        if (!total.finished && extras.remainingMilliseconds > 0) {
            lastCell->caption->setText(strings::get("activity.to_finish"));
            const qint64 minutes = qMax<qint64>(1, (extras.remainingMilliseconds + 30000) / minute);
            // The book's own recent pace: its reading per day over the last two weeks.
            qint64 recent = 0;
            for (auto it = days.cbegin(); it != days.cend(); ++it)
                if (it.key() > today.addDays(-14))
                    recent += it.value();
            if (recent > 0)
                finish = today.addDays(qMax<qint64>(1, extras.remainingMilliseconds * 14 / recent));
            // Estimates carry "≈" in the detail, as in Stats, so a narrow cell
            // keeps the figure's unit.
            lastCell->set(minutes >= 60 ? QString::number(minutes / 60) : QString::number(minutes),
                          minutes >= 60 ? strings::get("activity.hours_unit")
                                        : strings::get("stats.minutes"),
                          finish.isValid() ? strings::get("activity.around").arg(day(finish))
                                           : strings::get("activity.estimate"));
        } else {
            lastCell->caption->setText(strings::get("activity.last_read"));
            const auto last = total.last ? activity::localDate(total.last) : QDate();
            lastCell->set(last.isValid() ? QString::number(last.day()) : QString(),
                          last.isValid() ? english().monthName(last.month(), QLocale::ShortFormat)
                                         : QString(),
                          last.isValid() && last.year() != today.year()
                                  ? QString::number(last.year())
                                  : last == today ? strings::get("activity.today")
                                                  : QString());
        }
        QVector<QPair<QDate, int>> progress;
        if (!extras.ranges.isEmpty()) {
            QMap<QDate, int> ends;
            // Sessions are newest first; the last one of each day sets its end.
            for (int index = sessions.size() - 1; index >= 0; --index)
                if (extras.ranges.contains(sessions[index].started))
                    ends[activity::localDate(sessions[index].started)] =
                            extras.ranges.value(sessions[index].started).second;
            for (auto it = ends.cbegin(); it != ends.cend(); ++it)
                progress.append({ it.key(), it.value() });
        }
        if (progress.size() >= 1) {
            setBand(chartBand, strings::get("activity.progress"));
            chart->setProgress(progress, today, finish);
        } else {
            setBand(chartBand, strings::get("activity.time_per_day"));
            chart->setDays(days, today);
        }
        setBand(sessionsBand, strings::get("activity.sessions"), QString::number(sessions.size()));
        arrange();
        showSessions(same ? page : 0);
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        arrange();
        showSessions(page);
    }

private:
    QString book;
    QDate today;
    activity::BookTotal total;
    QVector<ActivitySession> sessions;
    QMap<QDate, qint64> days;
    ActivityOpenBook extras;
    BookHeading *heading;
    StatsCell *timeCell, *sessionsCell, *lastCell;
    ui::ListHeader *chartBand, *sessionsBand;
    BookChart *chart;
    ui::PagerBar *pager;
    QVector<SessionRow *> rows;
    int page = 0, rowsTop = 0, listWidth = 0;

    int perPage() const
    {
        return qMax(1, (height() - chrome::actionHeight - rowsTop) / SessionRow::preferredHeight());
    }

    void arrange()
    {
        const int header = ui::metrics::list::headerHeight;
        const int border = ui::metrics::control::border;
        const int headingHeight = BookHeading::preferredHeight();
        // Landscape: the book, its figures and sessions on the left, the chart
        // on the right.
        listWidth = wideLayout(this) ? width() / 2 : width();
        heading->setGeometry(0, 0, listWidth, headingHeight);
        int cellsBottom = placeCells(cells, headingHeight, listWidth, columns);
        if (listWidth < width()) {
            const int right = listWidth + border, rightWidth = width() - right;
            // A short landscape screen, such as a phone's, moves the figures
            // above the chart, so the sessions keep room for two rows.
            const bool figuresRight = height() - chrome::actionHeight - cellsBottom - header
                    < 2 * SessionRow::preferredHeight();
            int chartTop = 0;
            if (figuresRight) {
                chartTop = placeCells(cells, 0, rightWidth, columns, right);
                cellsBottom = headingHeight;
            }
            chartBand->setGeometry(right, chartTop, rightWidth, header);
            chart->setGeometry(right, chartTop + header, rightWidth, height() - chartTop - header);
            sessionsBand->setGeometry(0, cellsBottom, listWidth, header);
            rowsTop = cellsBottom + header;
        } else {
            const int rest = height() - cellsBottom - 2 * header - chrome::actionHeight;
            const int chartHeight = qBound(qMax(int(ui::px(150)), BookChart::minimumHeight()),
                                           rest - 3 * SessionRow::preferredHeight(), ui::px(340));
            chartBand->setGeometry(0, cellsBottom, width(), header);
            chart->setGeometry(0, cellsBottom + header, width(), chartHeight);
            sessionsBand->setGeometry(0, cellsBottom + header + chartHeight, width(), header);
            rowsTop = cellsBottom + 2 * header + chartHeight;
        }
        pager->setGeometry(0, height() - chrome::actionHeight, listWidth, chrome::actionHeight);
        update();
    }

    void paintEvent(QPaintEvent *event) override
    {
        ActivityPage::paintEvent(event);
        if (listWidth >= width())
            return;
        QPainter painter(this);
        paintVerticalRule(painter, this, listWidth, 0, height());
    }

    void showSessions(int target)
    {
        const int count = perPage();
        const int pages = qMax(1, (int(sessions.size()) + count - 1) / count);
        page = qBound(0, target, pages - 1);
        int used = 0;
        for (int index = page * count; index < qMin(int(sessions.size()), (page + 1) * count);
             ++index) {
            if (used == rows.size())
                rows.append(new SessionRow(this));
            const auto &session = sessions[index];
            const auto date = activity::localDate(session.started);
            const auto when = date == today
                    ? strings::get("activity.today_at").arg(clock(session.started))
                    : date == today.addDays(-1)
                    ? strings::get("activity.yesterday_at").arg(clock(session.started))
                    : english().toString(date, QStringLiteral("ddd d MMM")) + QStringLiteral(", ")
                            + clock(session.started);
            QString range;
            if (extras.ranges.contains(session.started)) {
                const auto value = extras.ranges.value(session.started);
                // A session that stayed on one page shows that place once.
                range = value.first == value.second
                        ? strings::get("common.percent").arg(value.second)
                        : strings::get("activity.range")
                                  .arg(strings::get("common.percent").arg(value.first))
                                  .arg(strings::get("common.percent").arg(value.second));
            }
            auto *row = rows[used++];
            row->setSession(when, range, session.milliseconds);
            row->setGeometry(0, rowsTop + (used - 1) * SessionRow::preferredHeight(), listWidth,
                             SessionRow::preferredHeight());
            row->show();
        }
        for (int index = used; index < rows.size(); ++index)
            rows[index]->hide();
        pager->setPage(page, pages);
        prepareRows(this);
    }
};

ActivityView::ActivityView(QWidget *parent) : ui::Panel(parent)
{
    setObjectName("rrActivity");
    header = new ui::Panel(this, Qt::BottomEdge);
    header->setObjectName("rrActivityHeader");
    static_cast<ui::Panel *>(header)->setEdgeWidth(chrome::heavyRule);
    back = new ui::CellButton(ui::ControlIcon::Back, header);
    back->setObjectName("rrActivityBack");
    back->setAccessibleName(strings::get("common.back"));
    back->setRule(Qt::RightEdge);
    back->hide();
    connect(back, &QPushButton::clicked, this, [this] {
        if (detail == Detail::Book) {
            closeBook(bookKey);
            return;
        }
        detail = Detail::None;
        arrange();
    });
    title = new reader::SettingsTitle(strings::get("activity.title"), header);
    title->setObjectName("rrActivityTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    ui::readerStyle(title, ui::styles::brand(chrome::titleText) + "background:transparent;");
    // A book's page can hide the book from the lists or delete its activity.
    hideCell = new ui::CellButton(ui::ControlIcon::Hidden, header);
    hideCell->setObjectName("rrActivityHideBook");
    hideCell->setCheckable(true);
    hideCell->setRule(Qt::LeftEdge);
    hideCell->hide();
    connect(hideCell, &QPushButton::clicked, this, [this](bool hidden) {
        if (hideRequested)
            hideRequested(bookKey, hidden);
    });
    deleteCell = new ui::CellButton(ui::ControlIcon::Trash, header);
    deleteCell->setObjectName("rrActivityDeleteBook");
    deleteCell->setAccessibleName(strings::get("activity.delete_book"));
    deleteCell->setToolTip(strings::get("activity.delete_book"));
    deleteCell->setRule(Qt::LeftEdge);
    deleteCell->hide();
    connect(deleteCell, &QPushButton::clicked, this, [this] {
        if (deleteRequested)
            deleteRequested(bookKey, activity::bookTotal(records, bookKey, summary.today).title);
    });
    close = new ui::CellButton(ui::ControlIcon::Close, header);
    close->setObjectName("rrActivityClose");
    close->setAccessibleName(strings::get("activity.close"));
    close->setRule(Qt::LeftEdge);
    connect(close, &QPushButton::clicked, this, [this] {
        if (closed)
            closed();
    });
    message = new ui::InterfaceLabel(this);
    message->setObjectName("rrActivityMessage");
    message->setAlignment(Qt::AlignCenter);
    message->setWordWrap(true);
    ui::readerStyle(message, ui::styles::font(chrome::labelText) + "background:transparent;");
    overview = new ActivityOverview(this);
    calendar = new ActivityCalendar(this);
    books = new ActivityBooks(this, "rrActivityBooks", "rrActivityBook", true);
    books->chosen = [this](QDate month, const QString &key) {
        for (const auto &group : summary.months)
            if (group.month == month && group.copies.contains(key)) {
                const auto copy = group.copies.value(key).first();
                copiesTitle = copy.title.simplified();
                copiesAuthor = copy.author.simplified();
                copiesMonth = month;
                detail = Detail::Copies;
                updateCopies(true);
                arrange();
                return;
            }
        openBook(key);
    };
    books->coversRequested = [this](const QVector<activity::BookTotal> &wanted, QSize size) {
        if (coversRequested)
            coversRequested(wanted, size);
    };
    books->hiddenRequested = [this] {
        detail = Detail::Hidden;
        arrange();
    };
    hiddenPage = new ActivityBooks(this, "rrActivityHidden", "rrActivityHiddenBook", false);
    hiddenPage->chosen = [this](QDate, const QString &key) {
        openBook(key);
        bookReturn = Detail::Hidden;
    };
    hiddenPage->coversRequested = books->coversRequested;
    copiesPage = new ActivityBooks(this, "rrActivityCopies", "rrActivityCopy", false);
    copiesPage->showCopySource = true;
    copiesPage->chosen = [this](QDate, const QString &key) {
        openBook(key);
        bookReturn = Detail::Copies;
    };
    copiesPage->coversRequested = books->coversRequested;
    recordsPage = new ActivityRecordsPage(this);
    bookPage = new ActivityBookPage(this);
    milestonesPage = new ActivityMilestones(this);
    recordsPage->allRequested = [this] {
        detail = Detail::Milestones;
        arrange();
    };
    // The sections are cells in the top bar, as the Library's views are. The
    // reading menu keeps the bottom edge for its own tabs.
    const char *const names[] = { "Overview", "Calendar", "Books", "Records" };
    const char *const labels[] = { "activity.tab_overview", "activity.tab_calendar",
                                   "activity.tab_books", "activity.tab_records" };
    const ui::ControlIcon icons[] = { ui::ControlIcon::Graph, ui::ControlIcon::Calendar,
                                      ui::ControlIcon::Books, ui::ControlIcon::Crown };
    for (int index = 0; index < 4; ++index) {
        auto *tab = new ui::CellButton(icons[index], header);
        tab->setObjectName(QString("rrActivity%1Tab").arg(names[index]));
        tab->setAccessibleName(strings::get(labels[index]));
        tab->setToolTip(strings::get(labels[index]));
        tab->setCheckable(true);
        tab->setRule(Qt::LeftEdge);
        connect(tab, &QPushButton::clicked, this, [this, index] { showSection(Section(index)); });
        tabs.append(tab);
    }
    setLoading();
}

void ActivityView::reset()
{
    records = {};
    summary = {};
    openBookData = {};
    bookKey.clear();
    currentBook.clear();
    copiesMonth = {};
    copiesTitle.clear();
    copiesAuthor.clear();
    current = Section::Overview;
    detail = bookReturn = Detail::None;
    calendar->reset();
    for (auto *list : { books, hiddenPage, copiesPage })
        list->reset();
    milestonesPage->resetPage();
    bookPage->reset();
    setLoading();
}

void ActivityView::setLoading()
{
    loading = true;
    unavailable = false;
    message->setText(strings::get("activity.loading"));
    arrange();
}

void ActivityView::setUnavailable()
{
    loading = false;
    unavailable = true;
    message->setText(strings::get("activity.unavailable"));
    arrange();
}

void ActivityView::updateCopies(bool firstPage)
{
    activity::MonthBooks copies;
    copies.month = copiesMonth;
    for (const auto &month : summary.months) {
        if (month.month != copiesMonth)
            continue;
        for (const auto &group : month.copies) {
            const auto &copy = group.first();
            if (copy.title.simplified() == copiesTitle
                && copy.author.simplified() == copiesAuthor) {
                copies.books = group;
                break;
            }
        }
        break;
    }
    if (copies.books.isEmpty()) {
        if (detail == Detail::Copies)
            detail = Detail::None;
        if (bookReturn == Detail::Copies)
            bookReturn = Detail::None;
        return;
    }
    QStringList sources;
    for (const auto &copy : copies.books) {
        const QUrl identity(copy.identity);
        sources.append(identity.isLocalFile()                   ? identity.toLocalFile()
                               : copy.sourceKey.startsWith('/') ? copy.sourceKey
                                                                : copy.identity);
        copies.milliseconds += copy.milliseconds;
    }
    // Start each narrow caption at the part of its source that distinguishes
    // this copy. In sorted order only the two neighbouring sources can share
    // a longer prefix. Keep the complete source in the accessible description.
    auto ordered = sources;
    ordered.removeDuplicates();
    ordered.sort();
    QHash<QString, QString> labels;
    for (int i = 0; i < ordered.size(); ++i) {
        const auto &source = ordered[i];
        int start = 0;
        for (int other : { i - 1, i + 1 }) {
            if (other < 0 || other >= ordered.size())
                continue;
            int common = 0;
            while (common < source.size() && common < ordered[other].size()
                   && source[common] == ordered[other][common])
                ++common;
            start = qMax(start, common);
        }
        // When the source is also a prefix of another, show its filename.
        if (ordered.size() == 1 || start == source.size())
            start = source.lastIndexOf('/') + 1;
        labels.insert(source, source.mid(start));
    }
    copiesPage->copyCaptions.clear();
    copiesPage->copyDescriptions.clear();
    for (int i = 0; i < copies.books.size(); ++i) {
        const auto &copy = copies.books[i];
        const auto date =
                english().toString(activity::localDate(copy.last), QStringLiteral("d MMM yyyy"));
        const auto number = QString::number(i + 1) + QStringLiteral(" · ");
        auto caption = labels.value(sources[i]);
        if (!caption.isEmpty())
            caption += QStringLiteral(" · ");
        copiesPage->copyCaptions.insert(copy.book, number + caption + date);
        copiesPage->copyDescriptions.insert(copy.book,
                                            number + sources[i] + QStringLiteral(" · ") + date);
    }
    copiesPage->setGroups({ copies }, 0, firstPage);
}

void ActivityView::setRecords(ActivityRecords value, activity::Summary computed)
{
    records = std::move(value);
    summary = std::move(computed);
    loading = unavailable = false;
    message->setText(summary.days.isEmpty() && records.sessions.isEmpty()
                             ? strings::get("activity.empty")
                             : QString());
    overview->setSummary(summary, records);
    calendar->setSummary(summary, records);
    books->setSummary(summary, records);
    if (detail == Detail::Copies || bookReturn == Detail::Copies)
        updateCopies();
    activity::MonthBooks hidden;
    hidden.books = summary.hiddenBooks;
    for (const auto &book : hidden.books)
        hidden.milliseconds += book.milliseconds;
    hiddenPage->setGroups(hidden.books.isEmpty() ? QVector<activity::MonthBooks>()
                                                 : QVector<activity::MonthBooks>{ hidden },
                          0);
    // The last hidden book shown again leaves an empty list behind.
    if (detail == Detail::Hidden && hidden.books.isEmpty())
        detail = Detail::None;
    if (bookReturn == Detail::Hidden && hidden.books.isEmpty())
        bookReturn = Detail::None;
    recordsPage->setSummary(summary, records);
    milestonesPage->setMilestones(summary.milestones, summary.current, summary.longest);
    if (detail == Detail::Book)
        bookPage->setBook(bookKey, records, summary.today, openBookData);
    arrange();
}

void ActivityView::setOpenBook(const ActivityOpenBook &book)
{
    openBookData = book;
    if (detail == Detail::Book && bookKey == book.book)
        bookPage->setBook(bookKey, records, summary.today, openBookData);
}

void ActivityView::setCurrentBook(const QString &book)
{
    currentBook = book;
    arrange();
}

void ActivityView::setCover(const QString &book, const QImage &cover, bool missing)
{
    books->setCover(book, cover, missing);
    hiddenPage->setCover(book, cover, missing);
    copiesPage->setCover(book, cover, missing);
}

void ActivityView::showSection(Section section)
{
    current = section;
    detail = Detail::None;
    arrange();
}

void ActivityView::openBook(const QString &book)
{
    // A book opened from a list starts at its newest sessions; only a reload
    // keeps the page.
    bookPage->forget();
    bookKey = book;
    bookReturn = Detail::None;
    detail = Detail::Book;
    bookPage->setBook(bookKey, records, summary.today, openBookData);
    arrange();
    if (bookRequested)
        bookRequested(book);
}

void ActivityView::closeBook(const QString &book)
{
    if (detail != Detail::Book || book != bookKey)
        return;
    detail = bookReturn;
    bookReturn = Detail::None;
    arrange();
}

void ActivityView::resizeEvent(QResizeEvent *event)
{
    ui::Panel::resizeEvent(event);
    arrange();
}

void ActivityView::arrange()
{
    const int bar = chrome::actionHeight;
    const int cellHeight = bar - chrome::heavyRule;
    const bool sub = detail != Detail::None;
    header->setGeometry(0, 0, width(), bar);
    back->setVisible(sub);
    title->setText(detail == Detail::Book ? strings::get("activity.book_title")
                           : detail == Detail::Milestones
                           ? strings::get("activity.milestones_title")
                           : detail == Detail::Copies ? strings::get("activity.copies")
                           : detail == Detail::Hidden ? strings::get("activity.hidden_title")
                                                      : strings::get("activity.title"));
    // The section cells give way to the title on a narrow screen.
    const int titleWidth = ui::interfaceTextWidth(title->text(), ui::brandFont(chrome::titleText))
            + 2 * chrome::inset;
    const int cell = sub ? int(chrome::cellWidth)
                         : qBound(int(chrome::compactCellWidth), (width() - titleWidth) / 5,
                                  int(chrome::cellWidth));
    back->setGeometry(0, 0, cell, cellHeight);
    close->setGeometry(width() - cell, 0, cell, cellHeight);
    // A book's page puts its actions where the sections were.
    const bool ready = !loading && !unavailable && message->text().isEmpty();
    const bool book = detail == Detail::Book && ready;
    const bool hidden = records.hidden.contains(bookKey);
    hideCell->setVisible(book);
    hideCell->setChecked(hidden);
    const auto hideLabel = strings::get(hidden ? "activity.show_book" : "activity.hide_book");
    hideCell->setAccessibleName(hideLabel);
    hideCell->setToolTip(hideLabel);
    deleteCell->setVisible(book && bookKey != currentBook);
    const int actions = book ? 1 + int(deleteCell->isVisibleTo(header)) : 0;
    deleteCell->setGeometry(width() - 2 * cell, 0, cell, cellHeight);
    hideCell->setGeometry(width() - cell * (1 + actions), 0, cell, cellHeight);
    const int tabsLeft = width() - cell * (sub ? 1 + actions : 1 + int(tabs.size()));
    title->setGeometry(sub ? cell : 0, 0, tabsLeft - (sub ? cell : 0), cellHeight);
    for (int index = 0; index < tabs.size(); ++index) {
        tabs[index]->setVisible(!sub);
        tabs[index]->setGeometry(tabsLeft + index * cell, 0, cell, cellHeight);
        tabs[index]->setChecked(int(current) == index);
        tabs[index]->setEnabled(ready);
    }
    const QRect body(0, bar, width(), height() - bar);
    message->setGeometry(body.adjusted(chrome::inset, 0, -chrome::inset, 0));
    message->setVisible(!ready);
    const QVector<QWidget *> pages{ overview, calendar, books, recordsPage };
    for (int index = 0; index < pages.size(); ++index) {
        pages[index]->setVisible(ready && !sub && int(current) == index);
        pages[index]->setGeometry(body);
    }
    bookPage->setVisible(ready && detail == Detail::Book);
    bookPage->setGeometry(body);
    milestonesPage->setVisible(ready && detail == Detail::Milestones);
    milestonesPage->setGeometry(body);
    hiddenPage->setVisible(ready && detail == Detail::Hidden);
    hiddenPage->setGeometry(body);
    copiesPage->setVisible(ready && detail == Detail::Copies);
    copiesPage->setGeometry(body);
    update();
}
} // namespace kobalt
