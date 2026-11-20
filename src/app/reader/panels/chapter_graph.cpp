#include "reading/header_footer.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/style/theme.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "chapter_graph.h"
#include "stats_cell.h"
#include "reading/reading_progress.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include <QLocale>
#include <QPainter>
#include <QResizeEvent>
#include <algorithm>

namespace kobalt {
namespace chapters = ui::metrics::chapters;
namespace chrome = ui::metrics::chrome;

class ChapterBars : public QWidget
{
public:
    explicit ChapterBars(QWidget *parent) : QWidget(parent) { setObjectName("rrChapterBars"); }

    QVector<ChapterLink> chapters;
    ChapterSelection selection;
    int first = 0, count = 0, gap = chapters::gapWide;
    qint64 maximum = 1;
    // The tallest bar; a shorter chart shortens it by the same amount.
    int barsHeight = chapters::barsHeight;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = palette().color(QPalette::WindowText);
        const auto paper = palette().color(QPalette::Window);
        painter.fillRect(rect(), paper);
        const int visible = qMin(count, int(chapters.size()) - first);
        if (visible <= 0)
            return;
        const int chart = barsHeight;
        // Reserve a full page's columns even on the last page. A short book
        // still centers its own columns; a paged book keeps their positions.
        const int columns = qMin(count, int(chapters.size()));
        const int barWidth = qBound(1, (width() - (columns - 1) * gap) / columns,
                                    int(chapters::maximumBarWidth));
        const int used = columns * barWidth + (columns - 1) * gap;
        const int offset = (width() - used) / 2;
        auto left = [&](int index) { return offset + (index - first) * (barWidth + gap); };
        for (int i = first; i < first + visible; ++i) {
            const qint64 words = qMax<qint64>(0, chapters[i].words);
            // Keep short chapters visible without letting the minimum exceed
            // the space left for bars on a short screen.
            const int height = qMin(chart,
                                    qMax(int(chapters::minimumBarHeight),
                                         int(double(words) / double(maximum) * chart)));
            const QRect bar(left(i), chart - height, barWidth, height);
            if (selection.current >= 0 && i < selection.current) {
                painter.fillRect(bar, ink);
            } else {
                painter.fillRect(bar, paper);
                ui::paintEdges(painter, bar,
                               ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge
                                       | Qt::RightEdge,
                               ink, chapters::outline);
                if (i == selection.current && selection.pages > 0) {
                    const int inset = barWidth < chapters::narrowBarWidth
                            ? chapters::narrowFillInset
                            : chapters::fillInset;
                    const QRect inside = bar.adjusted(inset, inset, -inset, -inset);
                    if (!inside.isEmpty()) {
                        const int read = int(qint64(inside.height())
                                             * qBound(0, selection.page - 1, selection.pages)
                                             / selection.pages);
                        painter.fillRect(QRect(inside.left(), inside.bottom() - read + 1,
                                               inside.width(), read),
                                         ink);
                    }
                }
            }
        }
        painter.setFont(ui::captionFont());
        const auto metrics = painter.font();
        auto label = [&](int index) {
            const auto text = QString::number(index + 1);
            const int textWidth = ui::interfaceTextWidth(text, metrics);
            const int x = qBound(0, left(index) + (barWidth - textWidth) / 2,
                                 qMax(0, width() - textWidth));
            return qMakePair(text,
                             QRect(x, chart + chapters::numberTop, textWidth,
                                   qMax(1, this->height() - chart - chapters::numberTop)));
        };
        QRect current;
        if (selection.current >= first && selection.current < first + visible) {
            const int x = left(selection.current) + (barWidth - chapters::tickWidth) / 2;
            painter.fillRect(x, chart + chapters::tickGap, chapters::tickWidth,
                             chapters::tickHeight, ink);
            const auto number = label(selection.current);
            current = number.second;
            painter.setPen(ink);
            ui::drawInterfaceText(painter, number.second, number.first,
                                  Qt::AlignHCenter | Qt::AlignTop);
        }
        // The first and last numbers are grey and give way to the current one.
        painter.setPen(ui::noteColor(this));
        for (const int index : { first, first + visible - 1 }) {
            if (index == selection.current)
                continue;
            const auto number = label(index);
            if (!current.isNull()
                && number.second.adjusted(-chapters::tickHeight, 0, chapters::tickHeight, 0)
                           .intersects(current))
                continue;
            ui::drawInterfaceText(painter, number.second, number.first,
                                  Qt::AlignHCenter | Qt::AlignTop);
            if (visible == 1)
                break;
        }
    }
};

// The way into Activity: a clock and "Activity" on the left, today's reading
// and the streak as a caption on the right, and a chevron, as a list row
// draws it. The Stats tab already uses the chart icon.
class ActivityRow final : public ui::Button
{
public:
    explicit ActivityRow(QWidget *parent) : ui::Button(QString(), parent)
    {
        setObjectName("rrGraphActivity");
        setAccessibleName(strings::get("activity.title"));
    }

    void setSummary(const QString &text)
    {
        if (summary == text)
            return;
        summary = text;
        setAccessibleDescription(text);
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
        const int top = (height() - icon) / 2;
        ui::controlIcon(ui::ControlIcon::Clock, ink)
                .paint(&painter, QRect(chrome::inset, top, icon, icon));
        const int chevronLeft = width() - chrome::inset - icon;
        ui::controlIcon(ui::ControlIcon::Next, ink)
                .paint(&painter, QRect(chevronLeft, top, icon, icon));
        const QFont title(ui::interfaceFont(chrome::labelText));
        const int titleLeft = chrome::inset + icon + chapters::activityIconGap;
        const auto name = strings::get("activity.title");
        const int titleWidth = ui::interfaceTextWidth(name, title) + 2;
        painter.setPen(ink);
        painter.setFont(title);
        ui::drawInterfaceText(painter, QRect(titleLeft, 0, titleWidth, height()), name,
                              Qt::AlignLeft | Qt::AlignVCenter);
        const QFont caption(ui::captionFont());
        const int captionRight = chevronLeft - chapters::activityIconGap;
        const int room = captionRight - titleLeft - titleWidth - chrome::inset;
        if (room <= 0 || summary.isEmpty())
            return;
        const auto text = ui::elideInterfaceText(summary, caption, room);
        painter.setFont(caption);
        ui::drawInterfaceText(painter, QRect(captionRight - room, 0, room, height()), text,
                              Qt::AlignRight | Qt::AlignVCenter);
    }

private:
    QString summary;
};

// On a short screen a fourth column beside the cells opens Activity instead of
// the row: the clock over a capital label, as a tab draws it.
class ActivityCell final : public ui::Button
{
public:
    explicit ActivityCell(QWidget *parent) : ui::Button(QString(), parent)
    {
        setObjectName("rrGraphActivityCell");
        setAccessibleName(strings::get("activity.title"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = ui::labelInk(this);
        painter.fillRect(rect(),
                         isDown() ? ui::themeColors(this).pressedPaper
                                  : palette().color(QPalette::Window));
        ui::paintEdges(painter, rect(), Qt::LeftEdge, palette().color(QPalette::Mid));
        const QFont label(ui::interfaceFont(chrome::tabText));
        const int capital = ui::interfaceInkBounds(QStringLiteral("H"), label).height();
        const int icon = chrome::tabIcon;
        const int top = (height() - icon - chrome::tabGap - capital) / 2;
        painter.setRenderHint(QPainter::Antialiasing);
        ui::controlIcon(ui::ControlIcon::Clock, ink)
                .paint(&painter, QRect((width() - icon) / 2, top, icon, icon));
        painter.setPen(ink);
        painter.setFont(label);
        ui::drawOpticalText(
                painter,
                QRect(ui::px(2), top + icon + chrome::tabGap, width() - ui::px(4), capital),
                strings::get("activity.title").toUpper(), true);
    }
};

// The fourth column's width: its usual width, or wider when its label needs
// it, as at a large text size.
static int activityColumn()
{
    const QFont label(ui::interfaceFont(chrome::tabText));
    return qMax(int(chapters::activityColumnWidth),
                ui::interfaceTextWidth(strings::get("activity.title").toUpper(), label)
                        + ui::px(16));
}

ChapterGraph::ChapterGraph(QWidget *parent, bool phoneLayout)
    : Panel(parent, Qt::BottomEdge), phoneLayout(phoneLayout)
{
    setObjectName("rrChapterGraph");
    message = new ui::InterfaceLabel(this);
    message->setObjectName("rrGraphMessage");
    message->setTextFormat(Qt::PlainText);
    message->setAlignment(Qt::AlignCenter);
    ui::readerStyle(message, ui::styles::font(chrome::labelText) + "background:transparent;");
    chapterCaption = new ui::InterfaceLabel(this);
    chapterCaption->setObjectName("rrGraphChapterNumber");
    ui::captionLabel(chapterCaption);
    chapterCaption->setVisible(!phoneLayout);
    // The page in the chapter follows the contents position on the caption line.
    positionSeparator = new ui::InterfaceLabel(QStringLiteral("·"), this);
    positionSeparator->setObjectName("rrGraphPositionSeparator");
    ui::captionLabel(positionSeparator);
    positionSeparator->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    chapterPage = new ui::InterfaceLabel(this);
    chapterPage->setObjectName("rrGraphChapterPosition");
    ui::captionLabel(chapterPage);
    chapterPage->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    chapterCaption->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    chapterTitle = new ui::PrivateTextLabel(this);
    chapterTitle->setObjectName("rrGraphChapterTitle");
    chapterTitle->setTextFormat(Qt::PlainText);
    ui::readerStyle(chapterTitle, ui::styles::font(chrome::labelText) + "background:transparent;");
    bars = new ChapterBars(this);
    mode = new ui::CellButton(ui::ControlIcon::ZoomOut, this);
    mode->setObjectName("rrGraphMode");
    mode->setRule(ui::Edges(Qt::LeftEdge) | Qt::BottomEdge);
    connect(mode, &QPushButton::clicked, this, [this] {
        setDense(!dense);
        if (denseChanged)
            denseChanged(dense);
    });
    wordScope = new ui::CellButton(ui::ControlIcon::Books, this);
    wordScope->hide();
    wordScope->setObjectName("rrGraphWordScope");
    wordScope->setAccessibleName(strings::get("stats.count_words_in"));
    wordScope->setToolTip(strings::get("stats.count_words_in"));
    wordScope->setRule(ui::Edges(Qt::LeftEdge) | Qt::BottomEdge);
    connect(wordScope, &QPushButton::clicked, this, [this] {
        if (wordScopeRequested)
            wordScopeRequested();
    });
    pager = new ui::PagerBar(this, "rrGraph");
    pager->previous->setAccessibleName(strings::get("chapter_contents.previous_chapters"));
    pager->next->setAccessibleName(strings::get("chapter_contents.next_chapters"));
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        anchor = qMax(0, (graphPage() - 1) * capacity());
        redraw(true);
        if (changed)
            changed();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        anchor = (graphPage() + 1) * capacity();
        redraw(true);
        if (changed)
            changed();
    });
    // The word totals label the chart: the chapter's on the left, the book's on the right.
    chapterWords = new ui::InterfaceLabel(this);
    chapterWords->setObjectName("rrGraphChapterWords");
    bookWords = new ui::InterfaceLabel(this);
    bookWords->setObjectName("rrGraphBookWords");
    for (auto *label : { chapterWords, bookWords }) {
        label->setTextFormat(Qt::PlainText);
        ui::readerStyle(label,
                        ui::styles::font(chapters::unitText)
                                + "background:transparent;color:palette(placeholder-text);");
    }
    chapterWords->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    bookWords->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    chapterCell = new StatsCell(this, "rrGraphChapter", strings::get("stats.chapter"));
    chapterCell->detail->setObjectName("rrGraphNextChapter");
    bookCell = new StatsCell(this, "rrGraphBook", strings::get("stats.book"));
    sessionCell = new StatsCell(this, "rrGraphSession", strings::get("stats.session"));
    activityRow = new ActivityRow(this);
    activityRow->hide();
    activityCell = new ActivityCell(this);
    activityCell->hide();
    for (QPushButton *entry :
         { static_cast<QPushButton *>(activityRow), static_cast<QPushButton *>(activityCell) })
        connect(entry, &QPushButton::clicked, this, [this] {
            if (activityRequested)
                activityRequested();
        });
    refreshCells();
    hide();
}

void ChapterGraph::setProgress(const QString &title, int chapterPage, int chapterPages,
                               int nextChapterPages, int page, int pages, int bookPercent,
                               bool estimated)
{
    if (this->title == title && progress.chapterPage == chapterPage
        && progress.chapterPages == chapterPages && progress.nextChapterPages == nextChapterPages
        && progress.page == page && progress.pages == pages && progress.bookPercent == bookPercent
        && progress.estimated == estimated && progressWeight == ui::interfaceWeight())
        return;
    this->title = title;
    progressWeight = ui::interfaceWeight();
    progress = { chapterPage, chapterPages, nextChapterPages, page, pages, bookPercent, estimated };
    chapterTitle->setAccessibleName(strings::get("chapter_graph.current_chapter").arg(title));
    this->chapterPage->setText(
            chapterPages > 0 ? strings::get("stats.page_of")
                                       .arg(qBound(1, chapterPage, chapterPages))
                                       .arg(headerfooter::pageTotal(chapterPages, estimated))
                             : QString());
    refreshCells();
    arrange();
}

void ChapterGraph::setChapterNumber(int number, int count)
{
    chapterNumber = number > 0 && count > 0 ? number : 0;
    chapterCaption->setText(chapterNumber
                                    ? strings::get("stats.contents_position").arg(number).arg(count)
                                    : QString());
    refreshCells();
    // The page in the chapter follows the caption, so its place depends on the caption.
    arrange();
}

void ChapterGraph::setReading(int minutes, qint64 total, qint64 chapterMilliseconds,
                              qint64 bookMilliseconds, int pageTurns, bool wholeVolume)
{
    if (this->minutes == minutes && words == total && chapterTime == chapterMilliseconds
        && bookTime == bookMilliseconds && this->pageTurns == pageTurns
        && this->wholeVolume == wholeVolume)
        return;
    this->minutes = minutes;
    this->pageTurns = pageTurns;
    this->wholeVolume = wholeVolume;
    words = total;
    chapterTime = chapterMilliseconds;
    bookTime = bookMilliseconds;
    refreshCells();
}

// The time left at the session's pace. With shortForm, hours and minutes use a
// clock format, "12:45", for cells too narrow for "12 h 45 min"; shorter times
// have no short form.
static QString duration(qint64 milliseconds, bool shortForm = false)
{
    const qint64 minutes = qMax<qint64>(1, (milliseconds + 30000) / 60000);
    if (shortForm && minutes < 60)
        return {};
    const auto time = minutes < 60 ? strings::get("stats.duration_minutes").arg(minutes)
            : shortForm            ? strings::get("stats.duration_clock")
                                  .arg(minutes / 60)
                                  .arg(minutes % 60, 2, 10, QChar('0'))
                        : strings::get("stats.duration_hours").arg(minutes / 60).arg(minutes % 60);
    return strings::get("stats.time_left").arg(time);
}

void ChapterGraph::refreshCells()
{
    const auto &p = progress;
    const int chapterValue = pageProgressPercent(qMax(1, p.chapterPage) - 1, p.chapterPages);
    const int bookValue = p.bookPercent >= 0 ? qBound(0, p.bookPercent, 100)
                                             : pageProgressPercent(p.page, p.pages);
    const int left = qMax(0, p.chapterPages - p.chapterPage + 1),
              remaining = qMax(0, p.pages - p.page);
    const auto pagesLeft = [](int count) {
        return count == 1 ? strings::get("stats.page_left") : strings::get("stats.pages_left");
    };
    // A time at the session's pace replaces the page-count detail when there is one.
    const QString next = chapterTime >= 0 ? duration(chapterTime)
            : p.nextChapterPages > 0
            ? strings::get("chapter_graph.next_chapter")
                      .arg(p.nextChapterPages)
                      .arg(p.nextChapterPages == 1 ? strings::get("common.page")
                                                   : strings::get("common.pages"))
            : p.chapterPages > 0 ? strings::get("stats.last_chapter")
                                 : QString();
    // The catalog is English, so group digits the English way on every device.
    const auto wordTotal = [](qint64 count) {
        return count >= 0
                ? strings::get("stats.words")
                          .arg(QLocale(QLocale::English, QLocale::UnitedStates).toString(count))
                : QString();
    };
    // A chapter's total waits for the counts, and a failed count never shows
    // the page lengths that the chart falls back to.
    const qint64 chapterTotal = words >= 0 && ready && error.isEmpty() && selection.current >= 0
                    && selection.current < chapters.size()
            ? chapters[selection.current].words
            : -1;
    chapterCell->set(p.chapterPages > 0 ? QString::number(left) : QString(),
                     p.chapterPages > 0 ? pagesLeft(left) : QString(), next,
                     p.chapterPages > 0 ? strings::get("common.percent").arg(chapterValue)
                                        : QString(),
                     chapterTime >= 0 ? duration(chapterTime, true) : QString(),
                     wordsInCells() ? wordTotal(chapterTotal) : QString());
    chapterCell->percent->setAccessibleName(
            strings::get("chapter_graph.chapter_progress").arg(chapterValue));
    bookCell->set(p.pages > 0 ? QString::number(remaining) : QString(),
                  p.pages > 0 ? pagesLeft(remaining) : QString(),
                  bookTime >= 0 ? duration(bookTime) : QString(),
                  strings::get("common.percent").arg(bookValue),
                  bookTime >= 0 ? duration(bookTime, true) : QString(),
                  wordsInCells() ? wordTotal(words) : QString());
    bookCell->percent->setAccessibleName(
            strings::get("chapter_graph.book_progress").arg(bookValue));
    sessionCell->set(QString::number(qMax(0, minutes)), strings::get("stats.minutes"),
                     strings::get(pageTurns == 1 ? "stats.page_turn" : "stats.page_turns")
                             .arg(qMax(0, pageTurns)));
    chapterWordsText = chapterTotal < 0 ? QString()
            : chapterNumber
            ? strings::get("stats.chapter_words").arg(chapterNumber).arg(wordTotal(chapterTotal))
            : strings::get("stats.this_chapter_words").arg(wordTotal(chapterTotal));
    bookWordsText = words >= 0
            ? strings::get(wholeVolume ? "stats.volume_words" : "stats.main_words")
                      .arg(wordTotal(words))
            : QString();
    arrangeWords();
}

QString ChapterGraph::activitySummary() const
{
    return activityCell->accessibleDescription();
}

void ChapterGraph::setActivity(const QString &summary)
{
    activityRow->setSummary(summary);
    activityCell->setAccessibleDescription(summary);
    if (activityAvailable == bool(activityRequested))
        return;
    activityAvailable = bool(activityRequested);
    arrange();
    redraw();
}

// The row takes height only while the full chart fits above it and part of
// the reading page stays exposed, as the chart's own growth keeps it. A
// shorter screen, such as a landscape one, opens Activity from a fourth column
// beside the cells instead.
bool ChapterGraph::activityRowShown() const
{
    if (!activityAvailable)
        return false;
    if (availableHeight <= 0)
        return true;
    const int pager = paged() ? int(chrome::actionHeight) : 0;
    return availableHeight >= chartTop() + chapters::chartHeight + wordsRowHeight()
            + chapters::cellHeight + chapters::activityHeight + pager + chapters::exposedPaper;
}

int ChapterGraph::activityHeight() const
{
    return activityRowShown() ? int(chapters::activityHeight) : 0;
}

void ChapterGraph::setDense(bool value)
{
    if (dense == value)
        return;
    dense = value;
    anchor = qMax(0, selection.current);
    redraw();
}

int ChapterGraph::capacity() const
{
    if (!dense)
        return chapters::sparseBars;
    // Every chapter fits while bars stay at least the minimum width.
    const int width = qMax(1, this->width() - 2 * chrome::inset);
    const int all = qMax(1, int(chapters.size()));
    const int gap = all <= chapters::wideChapters ? chapters::gapWide
            : all <= chapters::mediumChapters     ? chapters::gapMedium
                                                  : chapters::gapNarrow;
    if ((width - (all - 1) * gap) / all >= chapters::minimumBarWidth)
        return all;
    return qMax(1,
                (width + chapters::gapNarrow) / (chapters::minimumBarWidth + chapters::gapNarrow));
}

bool ChapterGraph::paged() const
{
    return chapters.size() > capacity();
}

int ChapterGraph::graphPage() const
{
    return anchor / capacity();
}

int ChapterGraph::graphPages() const
{
    return qMax(1, (int(chapters.size()) + capacity() - 1) / capacity());
}

void ChapterGraph::updateChartScale()
{
    QVector<qint64> lengths;
    for (const auto &chapter : chapters)
        if (chapter.words > 0)
            lengths.append(chapter.words);
    std::sort(lengths.begin(), lengths.end());
    // All pages use the same book-wide distribution. Uneven books benefit
    // most from the extra resolution, without taking all the exposed paper.
    bars->maximum = lengths.isEmpty() ? 1 : lengths.last();
    chapterSpread = lengths.isEmpty()
            ? 1.0
            : qBound(1.0, double(lengths.last()) / double(lengths[lengths.size() / 2]), 3.0);
}

void ChapterGraph::setData(QVector<ChapterLink> chapters, ChapterSelection selection, bool ready,
                           const QString &error)
{
    const bool sameChapters = chapters.size() == this->chapters.size()
            && std::equal(chapters.cbegin(), chapters.cend(), this->chapters.cbegin(),
                          [](const ChapterLink &left, const ChapterLink &right) {
                              return left.title == right.title && left.position == right.position
                                      && left.page == right.page && left.depth == right.depth
                                      && left.words == right.words;
                          });
    if (sameChapters && selection.current == this->selection.current
        && selection.next == this->selection.next && selection.page == this->selection.page
        && selection.pages == this->selection.pages && ready == this->ready && error == this->error)
        return;
    const bool moved = selection.current != this->selection.current
            || chapters.size() != this->chapters.size();
    this->chapters = std::move(chapters);
    if (!sameChapters)
        updateChartScale();
    this->selection = selection;
    this->ready = ready;
    this->error = error;
    // The chapter's word total comes with the counts.
    refreshCells();
    if (moved)
        anchor = qMax(0, selection.current);
    redraw();
}

void ChapterGraph::open()
{
    anchor = qMax(0, selection.current);
    redraw();
    show();
    raise();
}

void ChapterGraph::resizeEvent(QResizeEvent *event)
{
    Panel::resizeEvent(event);
    arrange();
    redraw();
}

int ChapterGraph::chartTop() const
{
    return topPadding + chapters::rowHeight;
}

bool ChapterGraph::inlinePager() const
{
    const int minimumChart = chapters::chartHeight - chapters::barsHeight + 2;
    // A separate pager would push the chapter row under the top toolbar on
    // a landscape phone. Its wide row has room beside the chapter instead.
    return paged() && width() >= ui::px(1200) && availableHeight > 0
            && availableHeight < chartTop() + minimumChart + chapters::cellHeight + activityHeight()
                    + chrome::actionHeight;
}

bool ChapterGraph::wordsInCells() const
{
    const int minimumChart = chapters::chartHeight - chapters::barsHeight + 2;
    // A short screen has no height for the word row, which would push the
    // chapter row under the top toolbar. The totals take the free second
    // detail line of the chapter and book cells instead.
    return availableHeight > 0
            && availableHeight < chartTop() + minimumChart + wordsRowHeight() + chapters::cellHeight
                    + activityHeight() + pagerHeight();
}

int ChapterGraph::wordsRowHeight() const
{
    return qMax(int(chapters::wordsHeight),
                ui::interfaceLineHeight(ui::interfaceFont(chapters::unitText))
                        + 2 * int(chapters::wordsPadding));
}

int ChapterGraph::wordsHeight() const
{
    return wordsInCells() ? 0 : wordsRowHeight();
}

int ChapterGraph::pagerHeight() const
{
    return paged() && !inlinePager() ? int(chrome::actionHeight) : 0;
}

int ChapterGraph::chartHeight() const
{
    const int base = chapters::chartHeight;
    if (availableHeight <= 0)
        return base;
    const int room = availableHeight - topPadding - chapters::rowHeight - wordsHeight()
            - chapters::cellHeight - activityHeight() - pagerHeight();
    // A larger physical short edge earns some extra bar height. Wide phones
    // and six-inch screens in landscape keep their existing chart size.
    const int shortEdge =
            qMin(width(), parentWidget() ? parentWidget()->height() : availableHeight);
    const int growth = qBound(0, (shortEdge - int(chapters::growthShortEdge)) / 2,
                              int(chapters::maximumGrowth));
    const int extra =
            qMin(int(growth * chapterSpread / 3), qMax(0, room - base - int(chapters::exposedPaper)));
    // Leave the text and cells intact when only a very short chart fits.
    const int least = base - chapters::barsHeight + 2;
    return qBound(least, room, base + extra);
}

void ChapterGraph::paintEvent(QPaintEvent *event)
{
    Panel::paintEvent(event);
    QPainter painter(this);
    const int border = ui::metrics::control::border;
    painter.fillRect(0, chartTop() - border, width(), border, ui::dividerColor(this));
    const int cells = chartTop() + chartHeight() + wordsHeight() + pagerHeight();
    painter.fillRect(0, cells - border, width(), border, ui::dividerColor(this));
    const int cellsWidth = this->cellsWidth();
    for (int column = 1; column < 3; ++column)
        painter.fillRect(cellsWidth * column / 3, cells, border, chapters::cellHeight,
                         palette().color(QPalette::Mid));
}

void ChapterGraph::arrangeWords()
{
    const QFont metrics(ui::interfaceFont(chapters::unitText));
    const int inner = qMax(1, width() - 2 * chrome::inset);
    const int top = chartTop() + chartHeight();
    // The book's total keeps its full text up to half the row; the chapter's
    // takes the rest and shortens first.
    const int gap = chrome::inset;
    const int bookWidth =
            qMin((inner - gap) / 2, ui::interfaceTextWidth(bookWordsText, metrics) + 2);
    const int chapterWidth = qMax(1, inner - bookWidth - gap);
    chapterWords->setVisible(!wordsInCells());
    bookWords->setVisible(!wordsInCells());
    chapterWords->setGeometry(chrome::inset, top, chapterWidth, wordsRowHeight());
    bookWords->setGeometry(width() - chrome::inset - bookWidth, top, bookWidth, wordsRowHeight());
    chapterWords->setText(ui::elideInterfaceText(chapterWordsText, metrics, chapterWidth));
    bookWords->setText(ui::elideInterfaceText(bookWordsText, metrics, bookWidth));
}

void ChapterGraph::setWordScopeAvailable(bool available)
{
    if (available == !wordScope->isHidden())
        return;
    wordScope->setVisible(available);
    arrange();
}

void ChapterGraph::arrange()
{
    const int inner = qMax(1, width() - 2 * chrome::inset);
    const int rowInner = qMax(1, inner - (wordScope->isHidden() ? 1 : 2) * int(chrome::cellWidth));
    // A change of height or paging can move the word totals in or out of the cells.
    if (wordsInCells() != cellWords) {
        cellWords = !cellWords;
        refreshCells();
    }
    const int chart = chartHeight();
    const int requiredHeight = topPadding + chapters::rowHeight + chart + wordsHeight()
            + chapters::cellHeight + activityHeight() + pagerHeight();
    if (height() != requiredHeight) {
        const int bottom = geometry().bottom();
        resize(width(), requiredHeight);
        if (isVisible())
            move(x(), bottom - height() + 1);
    }
    const QFont captionMetrics(ui::captionFont()),
            titleMetrics(ui::interfaceFont(chrome::labelText));
    const int captionHeight = ui::interfaceLineHeight(captionMetrics);
    const int titleHeight = ui::interfaceLineHeight(titleMetrics);
    const int headingTop = topPadding
            + (chapters::rowHeight - captionHeight - chapters::headingGap - titleHeight) / 2;
    const int pagerWidth = inlinePager() ? 3 * int(chrome::cellWidth) : 0;
    const int titleWidth = qMax(
            1, rowInner - int(chrome::inset) - (pagerWidth ? pagerWidth + int(chrome::inset) : 0));
    wordScope->setGeometry(width() - 2 * chrome::cellWidth, topPadding, chrome::cellWidth,
                           chapters::rowHeight);
    mode->setGeometry(width() - chrome::cellWidth, topPadding, chrome::cellWidth,
                      chapters::rowHeight);
    // "Contents · 8 of 23 · Page 1 of 65". The page keeps its full text; a
    // long contents position gives way first.
    const bool showChapterCount = !phoneLayout && !chapterCaption->text().isEmpty();
    const bool both = showChapterCount && !chapterPage->text().isEmpty();
    const int pageWidth =
            qMin(titleWidth, ui::interfaceTextWidth(chapterPage->text(), captionMetrics) + 2);
    const int separatorWidth =
            both ? ui::interfaceTextWidth(QStringLiteral(" · "), captionMetrics) : 0;
    const int captionWidth = !showChapterCount ? 0
            : chapterPage->text().isEmpty()
            ? titleWidth
            : qBound(0, ui::interfaceTextWidth(chapterCaption->text(), captionMetrics) + 2,
                     titleWidth - pageWidth - separatorWidth);
    chapterCaption->setGeometry(chrome::inset, headingTop, captionWidth, captionHeight);
    positionSeparator->setVisible(both);
    positionSeparator->setGeometry(chrome::inset + captionWidth, headingTop, separatorWidth,
                                   captionHeight);
    chapterPage->setGeometry(chrome::inset + captionWidth + separatorWidth, headingTop, pageWidth,
                             captionHeight);
    chapterTitle->setGeometry(chrome::inset, headingTop + captionHeight + chapters::headingGap,
                              titleWidth, titleHeight);
    chapterTitle->setText(ui::elideInterfaceText(title, chapterTitle->font(), titleWidth));
    const int top = chartTop();
    bars->barsHeight = chapters::barsHeight + chart - chapters::chartHeight;
    bars->setGeometry(chrome::inset, top + chapters::barsTop, inner,
                      chart - chapters::barsTop - ui::metrics::control::border);
    message->setGeometry(chrome::inset, top, inner, chart);
    pager->setFixedHeight(inlinePager() ? chapters::rowHeight - ui::metrics::control::border
                                        : int(chrome::actionHeight));
    if (inlinePager())
        pager->setGeometry(chrome::inset + titleWidth + chrome::inset, topPadding, pagerWidth,
                           chapters::rowHeight);
    else
        pager->setGeometry(0, top + chart + wordsHeight() - ui::metrics::control::border, width(),
                           chrome::actionHeight);
    arrangeWords();
    const int cells = top + chart + wordsHeight() + pagerHeight();
    const bool row = activityRowShown();
    activityRow->setVisible(row);
    activityRow->setGeometry(0, cells + chapters::cellHeight, width(), chapters::activityHeight);
    activityCell->setVisible(activityAvailable && !row);
    const int cellsWidth = this->cellsWidth();
    for (int column = 0; column < 3; ++column) {
        auto *cell = QVector<StatsCell *>{ chapterCell, bookCell, sessionCell }[column];
        const int left = cellsWidth * column / 3 + (column ? ui::metrics::control::border : 0);
        cell->setGeometry(left, cells, cellsWidth * (column + 1) / 3 - left, chapters::cellHeight);
    }
    activityCell->setGeometry(cellsWidth, cells, width() - cellsWidth, chapters::cellHeight);
}

int ChapterGraph::cellsWidth() const
{
    return activityAvailable && !activityRowShown() ? width() - activityColumn() : width();
}

void ChapterGraph::redraw(bool paging)
{
    mode->setGlyph(dense ? ui::ControlIcon::ZoomIn : ui::ControlIcon::ZoomOut);
    const auto action = strings::get(dense ? "chapter_graph.show_wider_bars"
                                           : "chapter_graph.show_more_chapters");
    mode->setAccessibleName(action);
    mode->setToolTip(action);
    anchor = qBound(0, anchor, qMax(0, int(chapters.size()) - 1));
    const int first = graphPage() * capacity();
    const bool available = ready && error.isEmpty() && !chapters.isEmpty();
    bars->setVisible(available);
    message->setVisible(!available);
    message->setText(!error.isEmpty()
                             ? strings::get("chapter_graph.chapter_lengths_are_unavailable")
                             : !ready ? strings::get("stats.counting")
                                      : strings::get("chapter_graph.no_chapter_lengths_available"));
    arrange();
    pager->setPage(graphPage(), available ? graphPages() : 1);
    const int columns = qMin(capacity(), int(chapters.size()));
    bars->chapters = chapters;
    bars->selection = selection;
    bars->first = first;
    bars->count = capacity();
    bars->gap = !dense                            ? chapters::sparseGap
            : paged()                             ? chapters::gapNarrow
            : columns <= chapters::wideChapters   ? chapters::gapWide
            : columns <= chapters::mediumChapters ? chapters::gapMedium
                                                  : chapters::gapNarrow;
    bars->setAccessibleName(strings::get("chapter_graph.chapters_to_of")
                                    .arg(first + 1)
                                    .arg(qMin(first + capacity(), int(chapters.size())))
                                    .arg(chapters.size()));
    bars->update();
    if (!paging)
        update();
}
} // namespace kobalt
