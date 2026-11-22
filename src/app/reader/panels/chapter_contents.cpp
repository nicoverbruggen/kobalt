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
#include "ui/dialogs/action_menu.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include <QLineEdit>
#include <QPainter>
#include <QMouseEvent>
#include "ui/input/hold_timer.h"
#include "ui/input/input.h"
#include <algorithm>

namespace kobalt {
namespace contents = ui::metrics::contents;
namespace chrome = ui::metrics::chrome;

namespace {
class ContentsTitle final : public ui::InterfaceLabel
{
public:
    using ui::InterfaceLabel::InterfaceLabel;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        const auto letters = ui::renderPrivateText(text(), font());
        if (letters.image.isNull()) {
            ui::InterfaceLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(QPalette::WindowText));
        // Center the same cap height for every list. A descender in "Chapters"
        // extends below the baseline without moving the other letters up.
        const auto cap = ui::interfaceInkBounds(QStringLiteral("H"), font());
        const int baseline = (height() - cap.height()) / 2 - cap.top();
        painter.drawImage(QPoint(0, baseline) + letters.origin,
                          ui::tintedPrivateText(letters.image, painter.pen().color()));
    }
};

class ChapterRow final : public ui::ListRow
{
public:
    explicit ChapterRow(QWidget *parent) : ui::ListRow(parent)
    {
        timer.setInterval(ui::metrics::input::longPressMilliseconds);
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this] { trigger(); });
    }

    std::function<void()> held;

    int setBoundaryMarkers(bool start, bool end, int numberWidth, bool hasImage)
    {
        startMarker = start;
        endMarker = end;
        // A lone flag is centered in the image cell. With an image, the flags
        // end at the left edge of its tap target.
        markerRightInset = numberWidth
                + (hasImage ? int(chrome::cellWidth) : (int(chrome::cellWidth) - markerSize()) / 2);
        update();
        return start || end ? markerRightInset - numberWidth + markerWidth() : 0;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ListRow::paintEvent(event);
        if (!startMarker && !endMarker)
            return;
        QPainter painter(this);
        const int side = markerSize();
        int x = trailingLabel()->geometry().right() + 1 - markerRightInset - markerWidth();
        const auto ink = titleLabel()->palette().color(QPalette::WindowText);
        for (auto icon : { ui::ControlIcon::BookStart, ui::ControlIcon::BookEnd }) {
            if (icon == ui::ControlIcon::BookStart ? !startMarker : !endMarker)
                continue;
            ui::controlIcon(icon, ink).paint(&painter, QRect(x, (height() - side) / 2, side, side));
            x += side + chrome::selectorGap;
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        consumed = false;
        ListRow::mousePressEvent(event);
        if (property("reader_swipe_hold").toBool() && event->button() == Qt::LeftButton && isDown()) {
            origin = ui::mousePosition(event).toPoint();
            timer.start(event);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((ui::mousePosition(event).toPoint() - origin).manhattanLength() > ui::px(12))
            timer.stop();
        ListRow::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (timer.finish(event)
            && (ui::mousePosition(event).toPoint() - origin).manhattanLength() <= ui::px(12))
            trigger();
        if (consumed) {
            event->accept();
            return;
        }
        ListRow::mouseReleaseEvent(event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Leave
            || event->type() == QEvent::TouchCancel)
            timer.stop();
        return ListRow::event(event);
    }

private:
    int markerWidth() const
    {
        return (int(startMarker) + int(endMarker)) * markerSize()
                + (startMarker && endMarker ? int(chrome::selectorGap) : 0);
    }

    int markerSize() const { return titleLabel()->fontMetrics().height(); }

    bool startMarker = false, endMarker = false;
    int markerRightInset = 0;
    ui::HoldTimer timer;
    QPoint origin;
    bool consumed = false;

    void trigger()
    {
        if (held && isDown() && isVisible()) {
            consumed = true;
            setDown(false);
            held();
        }
    }
};

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

// Keep the whole cell tappable, including the space around the ring.
class ChapterImageButton final : public ui::Button
{
public:
    ChapterImageButton(ui::ListRow *row, QWidget *parent) : ui::Button(parent), row(row) { }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto colors = parentWidget()->palette();
        const auto ink = colors.color(row->isChecked() ? QPalette::Window : QPalette::WindowText);
        const auto paper = colors.color(row->isChecked() ? QPalette::WindowText : QPalette::Window);
        const int glyph = chrome::cellIcon;
        const int diameter = qMin(qMin(width(), height()) - 2 * int(ui::metrics::control::border),
                                  glyph + glyph / 2);
        const QRect circle((width() - diameter) / 2, (height() - diameter) / 2, diameter, diameter);
        QColor outline = ink;
        outline.setAlphaF(hasFocus() ? 1.0 : 0.55);
        painter.setPen(QPen(outline, int(ui::metrics::control::border)));
        if (isDown())
            painter.setBrush(ink);
        painter.drawEllipse(circle);
        ui::controlIcon(ui::ControlIcon::Image, isDown() ? paper : ink)
                .paint(&painter,
                       QRect((width() - glyph) / 2, (height() - glyph) / 2, glyph, glyph));
    }

private:
    ui::ListRow *row;
};
} // namespace

ChapterContents::ChapterContents(QWidget *parent, QWidget *overlayHost)
    : Panel(parent), modalHost(overlayHost ? overlayHost : this)
{
    setObjectName("rrContents");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrContentsClose");
    close->setRule(Qt::LeftEdge);
    close->setAccessibleName(strings::get("chapter_contents.close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (back)
            back();
    });
    title = new ContentsTitle(strings::get("chapter_contents.chapters"), this);
    title->setObjectName("rrContentsTitle");
    title->setTextFormat(Qt::PlainText);
    ui::readerStyle(title, ui::styles::brand(chrome::titleText) + "background:transparent;");
    moreButton = new ui::CellButton(ui::ControlIcon::More, this);
    moreButton->setObjectName("rrContentsMore");
    moreButton->setRule(Qt::LeftEdge);
    moreButton->setAccessibleName(strings::get("chapter_contents.more"));
    moreButton->hide();
    connect(moreButton, &QPushButton::clicked, this, &ChapterContents::openListMenu);
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

void ChapterContents::setListMode(ContentsList active, const QVector<ContentsList> &available)
{
    const auto caption = [](ContentsList mode) {
        if (mode == ContentsList::PrintedPages)
            return strings::get("chapter_contents.printed_pages");
        if (mode == ContentsList::Landmarks)
            return strings::get("chapter_contents.landmarks");
        return strings::get("chapter_contents.chapters");
    };
    listMode = active;
    title->setText(caption(active));
    closeListMenu();
    availableLists = available;
    moreButton->show();
}

void ChapterContents::openListMenu()
{
    closeListMenu();
    QVector<ui::ActionMenuItem> actions;
    const auto add = [&](ContentsList mode, const char *label, const char *name,
                         ui::ControlIcon icon) {
        if (mode != listMode && availableLists.contains(mode))
            actions.append(ui::actionItem(strings::get(label), name, icon, [this, mode] {
                if (switchList)
                    switchList(mode);
            }));
    };
    add(ContentsList::Chapters, "chapter_contents.chapters", "rrContentsShowChapters",
        ui::ControlIcon::Chapters);
    add(ContentsList::PrintedPages, "chapter_contents.go_to_printed_page", "rrContentsShowPrinted",
        ui::ControlIcon::Scroll);
    if (!availableLists.contains(ContentsList::PrintedPages)) {
        auto unavailable = ui::actionItem(strings::get("chapter_contents.go_to_printed_page"),
                                          "rrContentsShowPrinted", ui::ControlIcon::Scroll, {});
        unavailable.disabledReason = strings::get("chapter_contents.no_printed_pages");
        actions.append(unavailable);
    }
    add(ContentsList::Landmarks, "chapter_contents.view_landmarks", "rrContentsShowLandmarks",
        ui::ControlIcon::Flag);
    if (actions.isEmpty())
        return;
    listMenu = new ui::ActionMenu(
            modalHost, { {}, strings::get("chapter_contents.additional_actions"), {}, {} },
            actions);
    listMenu->setObjectName("rrContentsMenu");
    listMenu->setDimmed(true);
    listMenu->dismissed = [this] { closeListMenu(); };
    listMenu->fitCard();
    listMenu->show();
    listMenu->raise();
    listMenu->setFocus();
}

void ChapterContents::showPrintedPageInput(const QHash<QString, QString> &pages,
                                           bool hasOtherLabels, std::function<void(QString)> jump,
                                           std::function<void()> other)
{
    closeListMenu();
    auto *dialog = new ui::ChoiceModal(modalHost,
                                       { strings::get("chapter_contents.enter_printed_page"),
                                         strings::get("chapter_contents.go_to_printed_page"),
                                         {} },
                                       { strings::get("common.cancel"), "rrPrintedPageCancel" },
                                       { strings::get("search_panel.go"), "rrPrintedPageGo" });
    listMenu = dialog;
    dialog->dismissed = [this] { closeListMenu(); };
    auto *number = dialog->addPageKeypad("rrPrintedPage");
    if (hasOtherLabels) {
        auto *button = ui::makeModalButton(
                dialog->card(),
                { strings::get("chapter_contents.other_labels"), "rrPrintedOtherLabels" }, false);
        dialog->card()->rows()->insertWidget(2, button);
        connect(button, &QPushButton::clicked, dialog, [this, other] {
            closeListMenu();
            other();
        });
    }
    connect(dialog->secondaryButton(), &QPushButton::clicked, dialog, dialog->dismissed);
    connect(number, &QLineEdit::textChanged, dialog, [dialog, pages](const QString &value) {
        dialog->primaryButton()->setEnabled(pages.contains(value));
    });
    const auto go = [this, number, pages, jump] {
        const auto found = pages.constFind(number->text());
        if (found != pages.constEnd()) {
            closeListMenu();
            jump(found.value());
        }
    };
    connect(number, &QLineEdit::returnPressed, dialog, go);
    connect(dialog->primaryButton(), &QPushButton::clicked, dialog, go);
    dialog->show();
    dialog->raise();
    number->setFocus();
}

void ChapterContents::closeListMenu()
{
    if (listMenu) {
        listMenu->hide();
        listMenu->deleteLater();
        listMenu = nullptr;
        moreButton->setFocus();
    }
}

void ChapterContents::hideEvent(QHideEvent *event)
{
    closeListMenu();
    Panel::hideEvent(event);
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

void ChapterContents::setBoundaries(int start, int end, int firstPage, int lastPage)
{
    if (start == startRow && end == endRow && firstPage == firstMainPage
        && lastPage == lastMainPage)
        return;
    startRow = start;
    endRow = end;
    firstMainPage = firstPage;
    lastMainPage = lastPage;
    redraw();
}

void ChapterContents::setPages(const QVector<int> &pages)
{
    if ((listMode != ContentsList::Chapters) || pages.size() != items.size())
        return;
    bool changed = false;
    for (int i = 0; i < items.size(); ++i) {
        changed = changed || items[i].page != pages[i];
        items[i].page = pages[i];
    }
    if (changed)
        redraw();
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
    // The Chapters bar takes the place of the header's action row. The close
    // cell sits on the right, as in Text Presets.
    const int barHeight = chrome::actionHeight - chrome::heavyRule,
              closeLeft = width() - chrome::cellWidth;
    close->setGeometry(closeLeft, 0, chrome::cellWidth, barHeight);
    const int actionsLeft = closeLeft - (moreButton->isHidden() ? 0 : int(chrome::cellWidth));
    moreButton->setGeometry(actionsLeft, 0, chrome::cellWidth, barHeight);
    const int middle = actionsLeft / 2;
    title->setGeometry(chrome::inset, 0, qMax(1, middle - chrome::inset), barHeight);
    position->setGeometry(middle, 0, qMax(1, actionsLeft - chrome::inset - middle), barHeight);
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
    if (listMode != ContentsList::Chapters)
        summary->setText((listMode == ContentsList::Landmarks
                                  ? strings::get("chapter_contents.landmark_count")
                                  : strings::get("chapter_contents.printed_count"))
                                 .arg(items.size()));
    int lastPage = -1;
    for (const auto &item : items)
        lastPage = qMax(lastPage, item.page);
    QVector<QString> printed(rows);
    bool pagesKnown = lastPage >= 0;
    if (listMode == ContentsList::Chapters && printedLabel)
        for (int row = 0; row < rows; ++row) {
            const int index = itemIndex(row);
            if (index >= 0) {
                printed[row] = printedLabel(index);
                pagesKnown |= !printed[row].isEmpty();
            }
        }
    listHeader->setCaptions(summary->text(),
                            pagesKnown ? strings::get("chapter_contents.page") : QString());
    pager->setPage(page, pageCount(), {}, true);
    const int used = chrome::actionHeight + contents::stripHeight
            + qMin(rows, qMax(0, int(visible.size()) - page * rows))
                    * int(ui::metrics::list::oneLine);
    pager->setBlankArea({ chrome::inset, used, width() - 2 * chrome::inset, pager->y() - used });
    while (buttons.size() < qMin(rows, int(visible.size()))) {
        const int row = buttons.size();
        auto *button = new ChapterRow(this);
        button->held = [this, row] {
            const int index = itemIndex(row);
            if (index >= 0 && !items[index].value.isEmpty() && boundaryRequested)
                boundaryRequested(index);
        };
        auto *disclosure = new ChapterDisclosure(this);
        buttons.append(button);
        disclosures.append(disclosure);
        auto *preview = new ChapterImageButton(button, this);
        previews.append(preview);
        connect(preview, &QPushButton::clicked, this, [this, row] {
            const int index = itemIndex(row);
            if (index >= 0 && previewImage)
                previewImage(items[index].labelImage);
        });
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
    int pageWidth = pagesKnown
            ? ui::interfaceTextWidth(QString::number(lastPage + 1), ui::captionFont())
            : 0;
    if (pagesKnown && pageLabel && (firstMainPage > 0 || lastMainPage < lastPage))
        for (int row = 0; row < buttons.size(); ++row) {
            const int index = itemIndex(row);
            if (index >= 0)
                pageWidth = qMax(
                        pageWidth,
                        ui::interfaceTextWidth(pageLabel(items[index].page), ui::captionFont()));
        }
    for (const auto &label : printed)
        pageWidth = qMax(pageWidth, ui::interfaceTextWidth(label, ui::captionFont()));
    const int digits = qMax(2, int(QString::number(items.size()).size()));
    const int numberWidth = ui::interfaceTextWidth(QString(digits, QChar('0')), ui::captionFont());
    const bool grouped = collapsible
            && std::any_of(parents.cbegin(), parents.cend(),
                           [](int parent) { return parent >= 0; });
    const int cellWidth = grouped ? int(chrome::cellWidth) : 0;
    const bool chapter = listMode == ContentsList::Chapters;
    const int numberColumn = pagesKnown ? pageWidth : numberWidth;
    for (int row = 0; row < buttons.size(); ++row) {
        const int index = itemIndex(row);
        auto *button = buttons[row];
        auto *disclosure = disclosures[row];
        auto *label = labels[row];
        const bool hasImage = index >= 0 && !items[index].labelImage.isEmpty();
        previews[row]->setVisible(hasImage);
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
        auto *itemRow = static_cast<ChapterRow *>(button);
        itemRow->setProperty("reader_swipe_hold", chapter);
        const int flagSpace = itemRow->setBoundaryMarkers(
                chapter && index == startRow, chapter && index == endRow, numberColumn, hasImage);
        const int imageSpace = hasImage ? int(chrome::cellWidth) : 0;
        const int accessoryWidth = numberColumn + qMax(flagSpace, imageSpace);
        if (hasImage) {
            const int left = width() - ui::metrics::list::inset - numberColumn - imageSpace;
            previews[row]->setGeometry(left, button->y(), chrome::cellWidth, button->height());
            previews[row]->setObjectName(item.name + "Image");
            previews[row]->setAccessibleName(
                    strings::get("chapter_contents.preview_image").arg(item.label));
        }
        disclosure->setGeometry(0, button->y(), cellWidth, button->height());
        disclosure->setObjectName(item.name + "Toggle");
        disclosure->setChecked(expanded[index]);
        disclosure->setAccessibleName(strings::get(expanded[index]
                                                           ? "chapter_contents.collapse_group"
                                                           : "chapter_contents.expand_group")
                                              .arg(item.label));
        disclosure->setAccessibleDescription(strings::get(
                expanded[index] ? "chapter_contents.expanded" : "chapter_contents.collapsed"));
        QString title = item.label;
        if (chapter && (index == startRow || index == endRow)) {
            const auto marker =
                    strings::get(index == startRow && index == endRow ? "chapter_contents.both"
                                         : index == startRow          ? "chapter_contents.start"
                                                                      : "chapter_contents.end");
            title = marker + QStringLiteral(" · ") + title;
        }
        itemRow->setTitle(item.label);
        label->setObjectName(item.name + "Title");
        const auto number = QString("%1").arg(index + 1, digits, 10, QChar('0'));
        const int indent = qBound(0, item.depth - rootDepth, 6) * contents::indent;
        if (listMode != ContentsList::Chapters) {
            itemRow->setLeading({}, 0);
            itemRow->setAccessory(ui::ListRow::Caption{ {}, 0 });
        } else if (pagesKnown) {
            itemRow->setLeading(number, indent);
            itemRow->leadingLabel()->setObjectName(item.name + "Number");
            itemRow->setAccessory(ui::ListRow::Caption{
                    !printed.value(row).isEmpty() ? printed[row]
                            : item.page >= 0
                            ? (pageLabel ? pageLabel(item.page) : QString::number(item.page + 1))
                            : QString(),
                    accessoryWidth });
            itemRow->trailingLabel()->setObjectName(item.name + "Page");
        } else {
            // Without page numbers, the chapter number takes their place at
            // the right edge.
            itemRow->setLeading(QString(), indent);
            itemRow->leadingLabel()->setObjectName({});
            itemRow->setAccessory(ui::ListRow::Caption{ number, accessoryWidth });
            itemRow->trailingLabel()->setObjectName(item.name + "Number");
        }
        button->setAccessibleName(
                (listMode != ContentsList::Chapters)
                        ? item.label
                        : strings::get("common.numbered_title").arg(index + 1).arg(title));
    }
    if (changed && isVisible())
        changed();
}
} // namespace kobalt
