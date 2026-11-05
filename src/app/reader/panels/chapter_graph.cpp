#include "ui/controls/text.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/style/theme.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "chapter_graph.h"
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
        // One scale for the whole book, so a chapter keeps its height on every
        // page. A single outlier is capped instead of flattening the others.
        QVector<qint64> counts;
        qint64 maximum = 1;
        for (const auto &chapter : chapters)
            if (chapter.words > 0) {
                counts.append(chapter.words);
                maximum = qMax(maximum, chapter.words);
            }
        std::sort(counts.begin(), counts.end());
        const double cap = counts.isEmpty()
                ? double(maximum)
                : double(counts[counts.size() / 2]) * chapters::scaleCap;
        const double scale = qMax(1.0, qMin(double(maximum), cap));
        const int chart = barsHeight;
        const int barWidth = qBound(1, (width() - (visible - 1) * gap) / visible,
                                    int(chapters::maximumBarWidth));
        const int used = visible * barWidth + (visible - 1) * gap;
        const int offset = (width() - used) / 2;
        auto left = [&](int index) { return offset + (index - first) * (barWidth + gap); };
        for (int i = first; i < first + visible; ++i) {
            const qint64 words = qMax<qint64>(0, chapters[i].words);
            const bool capped = words > scale;
            const int height = capped ? chart : qMax(2, int(double(words) * chart / scale));
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

// A caption with an optional percentage, a large figure with its unit, and one
// grey line of detail.
class StatsCell : public QWidget
{
public:
    StatsCell(QWidget *parent, const QString &prefix, const QString &title) : QWidget(parent)
    {
        setObjectName(prefix + "Cell");
        caption = new ui::InterfaceLabel(title, this);
        caption->setObjectName(prefix + "Caption");
        ui::captionLabel(caption);
        percent = new ui::InterfaceLabel(this);
        percent->setObjectName(prefix + "Percent");
        ui::captionLabel(percent);
        percent->setAlignment(Qt::AlignRight | Qt::AlignTop);
        caption->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        figure = new ui::InterfaceLabel(this);
        figure->setObjectName(prefix + "Figure");
        figure->setTextFormat(Qt::PlainText);
        ui::readerStyle(figure,
                        ui::styles::brand(chapters::figureText) + "background:transparent;");
        unit = new ui::InterfaceLabel(this);
        unit->setObjectName(prefix + "Unit");
        detail = new ui::InterfaceLabel(this);
        detail->setObjectName(prefix + "Detail");
        for (auto *label : { figure, unit, detail })
            label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        for (auto *label : { unit, detail }) {
            label->setTextFormat(Qt::PlainText);
            ui::readerStyle(label,
                            ui::styles::font(chapters::unitText)
                                    + "background:transparent;color:palette(placeholder-text);");
        }
    }

    QLabel *caption, *percent, *figure, *unit, *detail;

    // A short detail replaces the full one when only the short one fits on a line.
    void set(const QString &value, const QString &unitText, const QString &detailText,
             const QString &percentText = {}, const QString &shortDetail = {})
    {
        figure->setText(value);
        percent->setText(percentText);
        unitValue = unitText;
        detailValue = detailText;
        shortValue = shortDetail;
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *) override { arrange(); }

private:
    QString unitValue, detailValue, shortValue;

    void arrange()
    {
        const int inner = qMax(1, width() - 2 * chrome::inset);
        const QFont captionMetrics(ui::captionFont()),
                figureMetrics(ui::brandFont(chapters::figureText)),
                unitMetrics(ui::interfaceFont(chapters::unitText));
        caption->setGeometry(chrome::inset, chapters::cellPadding, inner,
                             ui::interfaceLineHeight(captionMetrics));
        percent->setGeometry(chrome::inset, chapters::cellPadding, inner,
                             ui::interfaceLineHeight(captionMetrics));
        const int figureWidth =
                qMin(inner, ui::interfaceTextWidth(figure->text(), figureMetrics) + 2);
        figure->setGeometry(chrome::inset,
                            chapters::figureBaseline - ui::interfaceLineAscent(figureMetrics),
                            figureWidth, ui::interfaceLineHeight(figureMetrics));
        const int unitLeft = chrome::inset + figureWidth + chapters::unitGap;
        // A large figure keeps every digit. A unit that does not fit beside it
        // takes two lines, the last on the figure's baseline, as in a narrow
        // cell on a phone; only then does it shorten.
        const int unitWidth = qMax(1, width() - chrome::inset - unitLeft);
        const auto unitText = twoLines(unitMetrics, unitValue, unitWidth);
        const int unitLines = unitText.count('\n') + 1;
        const int lineHeight = ui::interfaceLineHeight(unitMetrics);
        unit->setGeometry(unitLeft,
                          chapters::figureBaseline - ui::interfaceLineAscent(unitMetrics)
                                  - (unitLines - 1) * lineHeight,
                          unitWidth, unitLines * lineHeight);
        unit->setText(unitText);
        detail->setGeometry(chrome::inset, chapters::detailTop, inner,
                            ui::interfaceLineHeight(unitMetrics)
                                    + (chapters::detailLines - 1)
                                            * ui::interfaceLineHeight(unitMetrics));
        const bool shorten = !shortValue.isEmpty()
                && ui::interfaceTextWidth(detailValue, unitMetrics) > inner
                && ui::interfaceTextWidth(shortValue, unitMetrics) <= inner;
        detail->setText(twoLines(unitMetrics, shorten ? shortValue : detailValue, inner));
    }

    // A detail that does not fit breaks at the space that gives the most even
    // lines, so "≈ 12 h 45 min left" never leaves one word alone. Without such
    // a space, the first line takes what fits and the second line shortens.
    static QString twoLines(const QFont &metrics, const QString &text, int width)
    {
        if (ui::interfaceTextWidth(text, metrics) <= width)
            return text;
        int best = -1, bestWidth = width + 1, last = -1;
        for (int space = text.indexOf(' '); space > 0; space = text.indexOf(' ', space + 1)) {
            const int first = ui::interfaceTextWidth(text.left(space), metrics);
            if (first > width)
                break;
            last = space;
            const int longer = qMax(first, ui::interfaceTextWidth(text.mid(space + 1), metrics));
            if (longer < bestWidth) {
                best = space;
                bestWidth = longer;
            }
        }
        if (best < 0)
            best = last;
        if (best < 0)
            return ui::elideInterfaceText(text, metrics, width);
        return text.left(best) + '\n' + ui::elideInterfaceText(text.mid(best + 1), metrics, width);
    }
};

ChapterGraph::ChapterGraph(QWidget *parent) : Panel(parent, Qt::BottomEdge)
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
    chapterPage = new ui::InterfaceLabel(this);
    chapterPage->setObjectName("rrGraphChapterPosition");
    ui::captionLabel(chapterPage);
    chapterPage->setAlignment(Qt::AlignRight | Qt::AlignTop);
    chapterCaption->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    chapterTitle = new ui::PrivateTextLabel(this);
    chapterTitle->setObjectName("rrGraphChapterTitle");
    chapterTitle->setTextFormat(Qt::PlainText);
    ui::readerStyle(chapterTitle, ui::styles::font(chrome::labelText) + "background:transparent;");
    bars = new ChapterBars(this);
    pager = new ui::PagerBar(this, "rrGraph");
    pager->previous->setAccessibleName(strings::get("chapter_contents.previous_chapters"));
    pager->next->setAccessibleName(strings::get("chapter_contents.next_chapters"));
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        first = qMax(0, first - capacity());
        redraw(true);
        if (changed)
            changed();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        first += capacity();
        redraw(true);
        if (changed)
            changed();
    });
    chapterCell = new StatsCell(this, "rrGraphChapter", strings::get("stats.chapter"));
    chapterCell->detail->setObjectName("rrGraphNextChapter");
    bookCell = new StatsCell(this, "rrGraphBook", strings::get("stats.book"));
    sessionCell = new StatsCell(this, "rrGraphSession", strings::get("stats.session"));
    refreshCells();
    hide();
}

void ChapterGraph::setProgress(const QString &title, int chapterPage, int chapterPages,
                               int nextChapterPages, int page, int pages)
{
    if (this->title == title && progress.chapterPage == chapterPage
        && progress.chapterPages == chapterPages && progress.nextChapterPages == nextChapterPages
        && progress.page == page && progress.pages == pages
        && progressWeight == ui::interfaceWeight())
        return;
    this->title = title;
    progressWeight = ui::interfaceWeight();
    progress = { chapterPage, chapterPages, nextChapterPages, page, pages };
    chapterTitle->setAccessibleName(strings::get("chapter_graph.current_chapter").arg(title));
    this->chapterPage->setText(chapterPages > 0 ? strings::get("stats.page_of")
                                                          .arg(qBound(1, chapterPage, chapterPages))
                                                          .arg(chapterPages)
                                                : QString());
    refreshCells();
    arrange();
}

void ChapterGraph::setChapterNumber(int number, int count)
{
    chapterCaption->setText(number > 0 && count > 0
                                    ? strings::get("stats.contents_position").arg(number).arg(count)
                                    : QString());
}

void ChapterGraph::setReading(int minutes, qint64 total, qint64 chapterMilliseconds,
                              qint64 bookMilliseconds)
{
    if (this->minutes == minutes && words == total && chapterTime == chapterMilliseconds
        && bookTime == bookMilliseconds)
        return;
    this->minutes = minutes;
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
    const int bookValue = pageProgressPercent(p.page, p.pages);
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
    chapterCell->set(p.chapterPages > 0 ? QString::number(left) : QString(),
                     p.chapterPages > 0 ? pagesLeft(left) : QString(), next,
                     p.chapterPages > 0 ? strings::get("common.percent").arg(chapterValue)
                                        : QString(),
                     chapterTime >= 0 ? duration(chapterTime, true) : QString());
    chapterCell->percent->setAccessibleName(
            strings::get("chapter_graph.chapter_progress").arg(chapterValue));
    // The catalog is English, so group digits the English way on every device.
    const QString bookDetail = bookTime >= 0 ? duration(bookTime)
            : words >= 0
            ? strings::get("stats.words")
                      .arg(QLocale(QLocale::English, QLocale::UnitedStates).toString(words))
            : QString();
    bookCell->set(QString::number(remaining), pagesLeft(remaining), bookDetail,
                  strings::get("common.percent").arg(bookValue),
                  bookTime >= 0 ? duration(bookTime, true) : QString());
    bookCell->percent->setAccessibleName(
            strings::get("chapter_graph.book_progress").arg(bookValue));
    sessionCell->set(QString::number(qMax(0, minutes)), strings::get("stats.minutes"),
                     strings::get("stats.since_opening"));
}

void ChapterGraph::setDense(bool value)
{
    if (dense == value)
        return;
    dense = value;
    first = qMax(0, selection.current) / capacity() * capacity();
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
    return first / capacity();
}

int ChapterGraph::graphPages() const
{
    return qMax(1, (int(chapters.size()) + capacity() - 1) / capacity());
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
    this->selection = selection;
    this->ready = ready;
    this->error = error;
    arrange();
    if (moved)
        first = qMax(0, selection.current) / capacity() * capacity();
    redraw();
}

void ChapterGraph::open()
{
    first = qMax(0, selection.current) / capacity() * capacity();
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

int ChapterGraph::chartHeight() const
{
    if (availableHeight <= 0)
        return chapters::chartHeight;
    const int room = availableHeight - topPadding - chapters::rowHeight - chapters::cellHeight
            - (paged() ? int(chrome::actionHeight) : 0);
    // Keep bars at least a third of their design height.
    const int least = chapters::chartHeight - chapters::barsHeight * 2 / 3;
    return qBound(least, room, int(chapters::chartHeight));
}

void ChapterGraph::paintEvent(QPaintEvent *event)
{
    Panel::paintEvent(event);
    QPainter painter(this);
    const int border = ui::metrics::control::border;
    painter.fillRect(0, chartTop() - border, width(), border, ui::dividerColor(this));
    const int cells = chartTop() + chartHeight() + (paged() ? chrome::actionHeight : 0);
    painter.fillRect(0, cells - border, width(), border, ui::dividerColor(this));
    for (int column = 1; column < 3; ++column)
        painter.fillRect(width() * column / 3, cells, border, height() - cells,
                         palette().color(QPalette::Mid));
}

void ChapterGraph::arrange()
{
    const int inner = qMax(1, width() - 2 * chrome::inset);
    const int chart = chartHeight();
    const int requiredHeight = topPadding + chapters::rowHeight + chart + chapters::cellHeight
            + (paged() ? chrome::actionHeight : 0);
    if (height() != requiredHeight)
        resize(width(), requiredHeight);
    const QFont captionMetrics(ui::captionFont()),
            titleMetrics(ui::interfaceFont(chrome::labelText));
    chapterCaption->setGeometry(chrome::inset, topPadding + chapters::captionTop, inner / 2,
                                ui::interfaceLineHeight(captionMetrics));
    chapterPage->setGeometry(chrome::inset + inner / 2, topPadding + chapters::captionTop,
                             inner - inner / 2, ui::interfaceLineHeight(captionMetrics));
    // The title uses the whole row and elides only when it is longer than that.
    chapterTitle->setGeometry(chrome::inset, topPadding + chapters::titleTop, inner,
                              ui::interfaceLineHeight(titleMetrics));
    chapterTitle->setText(ui::elideInterfaceText(title, chapterTitle->font(), inner));
    const int top = chartTop();
    bars->barsHeight = chapters::barsHeight - (chapters::chartHeight - chart);
    bars->setGeometry(chrome::inset, top + chapters::barsTop, inner,
                      chart - chapters::barsTop - ui::metrics::control::border);
    message->setGeometry(chrome::inset, top, inner, chart);
    pager->setGeometry(0, top + chart - ui::metrics::control::border, width(),
                       chrome::actionHeight);
    const int cells = top + chart + (paged() ? chrome::actionHeight : 0);
    for (int column = 0; column < 3; ++column) {
        auto *cell = QVector<StatsCell *>{ chapterCell, bookCell, sessionCell }[column];
        const int left = width() * column / 3 + (column ? ui::metrics::control::border : 0);
        cell->setGeometry(left, cells, width() * (column + 1) / 3 - left, chapters::cellHeight);
    }
}

void ChapterGraph::redraw(bool paging)
{
    first = qBound(0, first / capacity(), graphPages() - 1) * capacity();
    const bool available = ready && error.isEmpty() && !chapters.isEmpty();
    bars->setVisible(available);
    message->setVisible(!available);
    message->setText(!error.isEmpty()
                             ? strings::get("chapter_graph.chapter_lengths_are_unavailable")
                             : !ready ? strings::get("stats.counting")
                                      : strings::get("chapter_graph.no_chapter_lengths_available"));
    arrange();
    pager->setPage(graphPage(), available ? graphPages() : 1);
    const int visible = qMin(capacity(), int(chapters.size()) - first);
    bars->chapters = chapters;
    bars->selection = selection;
    bars->first = first;
    bars->count = capacity();
    bars->gap = !dense                            ? chapters::sparseGap
            : visible <= chapters::wideChapters   ? chapters::gapWide
            : visible <= chapters::mediumChapters ? chapters::gapMedium
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
