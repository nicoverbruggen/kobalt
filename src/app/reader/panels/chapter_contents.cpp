#include "ui/controls/text.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/style/theme.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "choice_screen.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include <QPainter>
#include <algorithm>

namespace kobalt {
namespace contents = ui::metrics::contents;
namespace chrome = ui::metrics::chrome;

namespace {
// A sibling of the title row, so unresolved headings still have an action.
class ChapterDisclosure final : public ui::Button
{
public:
    explicit ChapterDisclosure(QWidget *parent) : ui::Button(parent)
    {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto ink = palette().color(QPalette::WindowText);
        painter.fillRect(rect(),
                         isDown() ? ui::themeColors(this).pressedPaper
                                  : palette().color(QPalette::Window));
        ui::paintEdges(painter, rect(), ui::Edges(Qt::RightEdge) | Qt::BottomEdge,
                       ui::dividerColor(this));
        const int side = chrome::cellIcon;
        ui::controlIcon(isChecked() ? ui::ControlIcon::Minus : ui::ControlIcon::Plus, ink)
                .paint(&painter, QRect((width() - side) / 2, (height() - side) / 2, side, side));
    }
};
} // namespace

ChapterContents::ChapterContents(QWidget *parent) : Panel(parent)
{
    setObjectName("rrContents");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrContentsClose");
    close->setRule(Qt::RightEdge);
    close->setAccessibleName(strings::get("chapter_contents.close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (back)
            back();
    });
    title = new ui::InterfaceLabel(strings::get("chapter_contents.chapters"), this);
    title->setObjectName("rrContentsTitle");
    title->setTextFormat(Qt::PlainText);
    ui::readerStyle(title, ui::styles::brand(chrome::titleText) + "background:transparent;");
    position = new ui::InterfaceLabel(this);
    position->setObjectName("rrContentsPosition");
    position->setTextFormat(Qt::PlainText);
    position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui::readerStyle(position, ui::styles::font(chrome::countText) + "background:transparent;");
    listHeader = new ui::ListHeader(this);
    summary = listHeader->leftLabel();
    summary->setObjectName("rrContentsSummary");
    pageHeading = listHeader->rightLabel();
    pageHeading->setObjectName("rrContentsPageHeading");
    empty = new ui::InterfaceLabel(strings::get("chapter_contents.no_chapters_available"), this);
    empty->setObjectName("rrContentsEmpty");
    empty->setTextFormat(Qt::PlainText);
    empty->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui::readerStyle(empty, ui::styles::font(chrome::labelText) + "background:transparent;");
    pager = new ui::PagerBar(this, "rrContents");
    pager->previous->setAccessibleName(strings::get("chapter_contents.previous_chapters"));
    pager->next->setAccessibleName(strings::get("chapter_contents.next_chapters"));
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        if (pageCount() > 1) {
            page = pager->turnTarget(-1);
            redraw();
        }
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        if (pageCount() > 1) {
            page = pager->turnTarget(1);
            redraw();
        }
    });
    hide();
}

void ChapterContents::setPosition(const QString &text)
{
    position->setText(text);
}

int ChapterContents::pageCount() const
{
    return qMax(1, (int(visible.size()) + rows - 1) / rows);
}

bool ChapterContents::hasChildren(int index) const
{
    return index >= 0 && index + 1 < parents.size() && parents[index + 1] == index;
}

int ChapterContents::itemIndex(int row) const
{
    return row >= 0 && row < rows ? visible.value(page * rows + row, -1) : -1;
}

void ChapterContents::rebuildVisible()
{
    visible.clear();
    QVector<bool> shown(items.size(), false);
    for (int i = 0; i < items.size(); ++i) {
        const int parent = parents[i];
        shown[i] = parent < 0 || (shown[parent] && expanded[parent]);
        if (shown[i])
            visible.append(i);
    }
}

void ChapterContents::toggleGroup(int index)
{
    if (!collapsible || !hasChildren(index))
        return;
    expanded[index] = !expanded[index];
    rebuildVisible();
    arrange(index);
    redraw();
    for (int row = 0; row < disclosures.size(); ++row)
        if (itemIndex(row) == index)
            disclosures[row]->setFocus(Qt::OtherFocusReason);
}

void ChapterContents::showChapters(const QString &title, QVector<Choice> choices,
                                   const QString &current, ChapterGroups groups)
{
    setAccessibleDescription(title);
    items = std::move(choices);
    this->current = current;
    parents.clear();
    collapsible = groups != ChapterGroups::Off;
    expanded = QVector<bool>(items.size(), groups != ChapterGroups::Current);
    QVector<int> stack;
    int selected = -1;
    for (int i = 0; i < items.size(); ++i) {
        const int depth = qMax(0, items[i].depth);
        while (!stack.isEmpty() && qMax(0, items[stack.last()].depth) >= depth)
            stack.removeLast();
        parents.append(stack.isEmpty() ? -1 : stack.last());
        stack.append(i);
        if (!current.isEmpty() && items[i].name == current)
            selected = i;
    }
    for (int parent = selected < 0 ? -1 : parents[selected]; parent >= 0; parent = parents[parent])
        expanded[parent] = true;
    rebuildVisible();
    page = 0;
    arrange(selected);
    redraw();
    show();
    raise();
}

void ChapterContents::resizeEvent(QResizeEvent *)
{
    arrange();
    redraw();
}

void ChapterContents::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, chrome::actionHeight - chrome::heavyRule, width(), chrome::heavyRule,
                     palette().color(QPalette::WindowText));
    painter.fillRect(0, chrome::actionHeight + contents::stripHeight - ui::metrics::control::border,
                     width(), ui::metrics::control::border, ui::dividerColor(this));
}

void ChapterContents::arrange(int anchor)
{
    if (anchor < 0)
        anchor = itemIndex(0);
    const int top = chrome::actionHeight + contents::stripHeight, available = height() - top;
    // Leave room for the pager only when the list needs more than one page.
    rows = qMax(1, available / ui::metrics::list::oneLine);
    if (visible.size() > rows)
        rows = qMax(1, (available - chrome::actionHeight) / ui::metrics::list::oneLine);
    const int first = visible.indexOf(anchor);
    if (first >= 0)
        page = first / rows;
    page = qBound(0, page, pageCount() - 1);
    // The Chapters bar takes the place of the header's action row.
    const int barHeight = chrome::actionHeight - chrome::heavyRule,
              titleLeft = chrome::cellWidth + chrome::inset;
    close->setGeometry(0, 0, chrome::cellWidth, barHeight);
    title->setGeometry(titleLeft, 0, qMax(1, width() / 2 - titleLeft), barHeight);
    position->setGeometry(width() / 2, 0, qMax(1, width() / 2 - chrome::inset), barHeight);
    listHeader->setGeometry(0, chrome::actionHeight, width(), ui::metrics::list::headerHeight);
    pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
    empty->setGeometry(chrome::inset, top, qMax(1, width() - 2 * chrome::inset),
                       ui::metrics::list::oneLine);
}

void ChapterContents::redraw()
{
    page = qBound(0, page, pageCount() - 1);
    empty->setVisible(items.isEmpty());
    summary->setText(items.size() == 1
                             ? strings::get("chapter_contents.summary_one")
                             : strings::get("chapter_contents.summary").arg(items.size()));
    int lastPage = -1;
    for (const auto &item : items)
        lastPage = qMax(lastPage, item.page);
    const bool pagesKnown = lastPage >= 0;
    listHeader->setCaptions(summary->text(),
                            pagesKnown ? strings::get("chapter_contents.page") : QString());
    pager->setPage(page, pageCount(), {}, true);
    const int used = chrome::actionHeight + contents::stripHeight
            + qMin(rows, qMax(0, int(visible.size()) - page * rows))
                    * int(ui::metrics::list::oneLine);
    pager->setBlankArea({ chrome::inset, used, width() - 2 * chrome::inset, pager->y() - used });
    while (buttons.size() < qMin(rows, int(visible.size()))) {
        const int row = buttons.size();
        auto *button = new ui::ListRow(this);
        auto *disclosure = new ChapterDisclosure(this);
        buttons.append(button);
        disclosures.append(disclosure);
        labels.append(button->titleLabel());
        connect(button, &QPushButton::clicked, this, [this, row] {
            const int index = itemIndex(row);
            if (index >= 0 && !items[index].value.isEmpty() && selected)
                selected(items[index].value);
        });
        connect(disclosure, &QPushButton::clicked, this,
                [this, row] { toggleGroup(itemIndex(row)); });
    }
    int rootDepth = items.isEmpty() ? 0 : qMax(0, items.first().depth);
    for (const auto &item : items)
        rootDepth = qMin(rootDepth, qMax(0, item.depth));
    // Every row reserves the widest page caption, so titles end in one column.
    const int pageWidth = pagesKnown
            ? ui::interfaceTextWidth(QString::number(lastPage + 1), ui::captionFont())
            : 0;
    const int digits = qMax(2, int(QString::number(items.size()).size()));
    const bool grouped = collapsible
            && std::any_of(parents.cbegin(), parents.cend(),
                           [](int parent) { return parent >= 0; });
    const int cellWidth = grouped ? int(chrome::cellWidth) : 0;
    for (int row = 0; row < buttons.size(); ++row) {
        const int index = itemIndex(row);
        auto *button = buttons[row];
        auto *disclosure = disclosures[row];
        auto *label = labels[row];
        button->setVisible(index >= 0);
        disclosure->setVisible(collapsible && hasChildren(index));
        button->setObjectName({});
        disclosure->setObjectName({});
        if (index < 0)
            continue;
        const auto &item = items[index];
        const bool active = item.name == current;
        button->setObjectName(item.name);
        label->setObjectName(item.name + "Title");
        button->setAccessibleDescription(
                item.value.isEmpty()
                        ? strings::get("chapter_contents.chapter_destination_unavailable")
                        : active ? strings::get("chapter_contents.current_chapter")
                                 : QString());
        button->setEnabled(!item.value.isEmpty());
        button->setCheckable(true);
        button->setChecked(active);
        const int rowLeft = hasChildren(index) ? cellWidth : 0;
        button->setContentsMargins(cellWidth - rowLeft, 0, 0, 0);
        button->setGeometry(rowLeft,
                            chrome::actionHeight + contents::stripHeight
                                    + row * ui::metrics::list::oneLine,
                            qMax(1, width() - rowLeft), ui::metrics::list::oneLine);
        disclosure->setGeometry(0, button->y(), cellWidth, button->height());
        disclosure->setObjectName(item.name + "Toggle");
        disclosure->setChecked(expanded[index]);
        disclosure->setAccessibleName(strings::get(expanded[index]
                                                           ? "chapter_contents.collapse_group"
                                                           : "chapter_contents.expand_group")
                                              .arg(item.label));
        disclosure->setAccessibleDescription(strings::get(
                expanded[index] ? "chapter_contents.expanded" : "chapter_contents.collapsed"));
        auto *itemRow = static_cast<ui::ListRow *>(button);
        itemRow->setTitle(item.label);
        label->setObjectName(item.name + "Title");
        itemRow->setLeading(QString("%1").arg(index + 1, digits, 10, QChar('0')),
                            qBound(0, item.depth - rootDepth, 6) * contents::indent);
        itemRow->leadingLabel()->setObjectName(item.name + "Number");
        itemRow->setAccessory(ui::ListRow::Caption{
                item.page >= 0 ? QString::number(item.page + 1) : QString(), pageWidth });
        itemRow->trailingLabel()->setObjectName(item.name + "Page");
        button->setAccessibleName(
                strings::get("common.numbered_title").arg(index + 1).arg(item.label));
    }
    if (changed && isVisible())
        changed();
}
} // namespace kobalt
