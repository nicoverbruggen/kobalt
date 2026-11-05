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

namespace kobalt {
namespace contents = ui::metrics::contents;
namespace chrome = ui::metrics::chrome;
ChapterContents::ChapterContents(QWidget *parent) : Panel(parent) {
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
void ChapterContents::setPosition(const QString &text) {
    position->setText(text);
}
int ChapterContents::pageCount() const {
    return qMax(1, (int(items.size()) + rows - 1) / rows);
}
void ChapterContents::showChapters(const QString &title, QVector<Choice> choices, const QString &current) {
    setAccessibleDescription(title);
    items = std::move(choices);
    this->current = current;
    page = 0;
    arrange();
    for (int i = 0; i < items.size(); ++i)
        if (!current.isEmpty() && items[i].name == current)
            page = i / rows;
    redraw();
    show();
    raise();
}
void ChapterContents::resizeEvent(QResizeEvent *) {
    arrange();
    redraw();
}
void ChapterContents::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, chrome::actionHeight - chrome::heavyRule, width(), chrome::heavyRule, palette().color(QPalette::WindowText));
    painter.fillRect(0, chrome::actionHeight + contents::stripHeight - ui::metrics::control::border, width(),
                     ui::metrics::control::border, ui::dividerColor(this));
}
void ChapterContents::arrange() {
    const int first = page * rows, top = chrome::actionHeight + contents::stripHeight, available = height() - top;
    // Leave room for the pager only when the list needs more than one page.
    rows = qMax(1, available / ui::metrics::list::oneLine);
    if (items.size() > rows)
        rows = qMax(1, (available - chrome::actionHeight) / ui::metrics::list::oneLine);
    page = first / rows;
    // The Chapters bar takes the place of the header's action row.
    const int barHeight = chrome::actionHeight - chrome::heavyRule, titleLeft = chrome::cellWidth + chrome::inset;
    close->setGeometry(0, 0, chrome::cellWidth, barHeight);
    title->setGeometry(titleLeft, 0, qMax(1, width() / 2 - titleLeft), barHeight);
    position->setGeometry(width() / 2, 0, qMax(1, width() / 2 - chrome::inset), barHeight);
    listHeader->setGeometry(0, chrome::actionHeight, width(), ui::metrics::list::headerHeight);
    pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
    empty->setGeometry(chrome::inset, top, qMax(1, width() - 2 * chrome::inset), ui::metrics::list::oneLine);
}
void ChapterContents::redraw() {
    page = qBound(0, page, pageCount() - 1);
    empty->setVisible(items.isEmpty());
    summary->setText(items.size() == 1 ? strings::get("chapter_contents.summary_one")
                                       : strings::get("chapter_contents.summary").arg(items.size()));
    int lastPage = -1;
    for (const auto &item : items)
        lastPage = qMax(lastPage, item.page);
    const bool pagesKnown = lastPage >= 0;
    listHeader->setCaptions(summary->text(), pagesKnown ? strings::get("chapter_contents.page") : QString());
    pager->setPage(page, pageCount(), {}, true);
    const int used = chrome::actionHeight + contents::stripHeight + qMin(rows, qMax(0, int(items.size()) - page * rows)) * int(ui::metrics::list::oneLine);
    pager->setBlankArea({chrome::inset, used, width() - 2 * chrome::inset, pager->y() - used});
    while (buttons.size() < qMin(rows, int(items.size()))) {
        const int row = buttons.size();
        auto *button = new ui::ListRow(this);
        buttons.append(button);
        labels.append(button->titleLabel());
        connect(button, &QPushButton::clicked, this, [this, row] {
            const int index = page * rows + row;
            if (index < items.size() && !items[index].value.isEmpty() && selected)
                selected(items[index].value);
        });
    }
    int rootDepth = items.isEmpty() ? 0 : qMax(0, items.first().depth);
    for (const auto &item : items)
        rootDepth = qMin(rootDepth, qMax(0, item.depth));
    // Every row reserves the widest page caption, so titles end in one column.
    const int pageWidth = pagesKnown ? ui::interfaceTextWidth(QString::number(lastPage + 1), ui::captionFont()) : 0;
    const int digits = qMax(2, int(QString::number(items.size()).size()));
    for (int row = 0; row < buttons.size(); ++row) {
        const int index = page * rows + row;
        auto *button = buttons[row];
        auto *label = labels[row];
        button->setVisible(row < rows && index < items.size());
        if (row >= rows || index >= items.size())
            continue;
        const auto &item = items[index];
        const bool active = item.name == current;
        button->setObjectName(item.name);
        label->setObjectName(item.name + "Title");
        button->setAccessibleDescription(item.value.isEmpty()
                                             ? strings::get("chapter_contents.chapter_destination_unavailable")
                                         : active ? strings::get("chapter_contents.current_chapter")
                                                  : QString());
        button->setEnabled(!item.value.isEmpty());
        button->setCheckable(true);
        button->setChecked(active);
        button->setGeometry(0, chrome::actionHeight + contents::stripHeight + row * ui::metrics::list::oneLine, width(),
                            ui::metrics::list::oneLine);
        auto *itemRow = static_cast<ui::ListRow *>(button);
        itemRow->setTitle(item.label);
        label->setObjectName(item.name + "Title");
        itemRow->setLeading(QString("%1").arg(index + 1, digits, 10, QChar('0')),
                            qBound(0, item.depth - rootDepth, 6) * contents::indent);
        itemRow->leadingLabel()->setObjectName(item.name + "Number");
        itemRow->setAccessory(
            ui::ListRow::Caption{item.page >= 0 ? QString::number(item.page + 1) : QString(), pageWidth});
        itemRow->trailingLabel()->setObjectName(item.name + "Page");
        button->setAccessibleName(strings::get("common.numbered_title").arg(index + 1).arg(item.label));
    }
    if (changed && isVisible())
        changed();
}
}
