#include "header_footer_panel.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/style/theme.h"
#include "support/qt_font.h"
#include "support/strings.h"

#include <QPainter>
#include <QResizeEvent>
#include <QtMath>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace m = ui::metrics;
namespace hf = m::headerFooter;
using ui::px;
using headerfooter::Content;
using headerfooter::Field;
using headerfooter::Layout;
using headerfooter::Preset;

namespace {
QFont valueFont() {
    QFont font = ui::chromeFont(hf::valueText);
    font.setLetterSpacing(QFont::AbsoluteSpacing, m::chrome::captionSpacing);
    return font;
}
// The text and battery of one slot, elided to `bounds` and aligned in it.
void paintValue(QPainter &painter, const QRect &bounds, const Content &content, const QColor &ink, const QColor &paper,
                Qt::Alignment align = Qt::AlignHCenter) {
    const QFont font = painter.font();
    const qreal body = qRound(font.pixelSize() * .75);
    const int icon = content.batteryIcon ? qCeil(ui::batteryWidth(body)) : 0;
    const int gap = content.batteryIcon && !content.text.isEmpty() ? int(m::reading::batteryGap) : 0;
    const int room = qMax(0, bounds.width() - icon - gap);
    QString text = content.text;
    int textWidth = text.isEmpty() ? 0 : qt::textWidth(painter.fontMetrics(), text);
    if (textWidth > room) {
        text = painter.fontMetrics().elidedText(text, Qt::ElideRight, room);
        textWidth = text.isEmpty() ? 0 : qt::textWidth(painter.fontMetrics(), text);
    }
    const int total = textWidth + gap + icon;
    const int x = align & Qt::AlignLeft    ? bounds.x()
                  : align & Qt::AlignRight ? bounds.right() + 1 - total
                                           : bounds.x() + (bounds.width() - total) / 2;
    if (!text.isEmpty())
        painter.drawText(QRect(x, bounds.y(), textWidth, bounds.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
    if (content.batteryIcon) {
        const qreal width = body * int(m::panel::batteryBodyWidth) / int(m::panel::batteryBodyHeight);
        ui::paintBattery(painter, QRectF(x + textWidth + gap, bounds.y() + (bounds.height() - body) / 2.0, width, body),
                         content.battery, content.charging, ink, paper);
    }
}
// How much of a miniature's width a field takes, as a short or long dash.
int dashWidth(Field field) {
    switch (field) {
    case Field::Nothing:
        return 0;
    case Field::BookTitle:
    case Field::Author:
    case Field::Chapter:
    case Field::ChapterAndPage:
    case Field::TitleAndPage:
        return hf::longDash;
    default:
        return hf::shortDash;
    }
}
}

class SlotMap::Cell final : public ui::Button {
public:
    Cell(QWidget *parent, int slot, bool editable) : ui::Button(parent), slot(slot), editable(editable) {
        setObjectName(QString(editable ? "rrHeaderFooterSlot%1" : "rrHeaderFooterPresetSlot%1").arg(slot));
        setFocusPolicy(Qt::NoFocus);
        setEnabled(editable);
    }
    void show(const Content &next, bool stripShown) {
        if (content == next && shown == stripShown)
            return;
        content = next;
        shown = stripShown;
        const auto value = content.empty() ? strings::get("header_footer.empty") : content.text;
        setAccessibleName(
            strings::get("header_footer.slot_value").arg(headerfooter::slotLabel(slot), value.isEmpty() ? "" : value));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const bool pressed = isDown();
        const auto ink = palette().color(pressed ? QPalette::Window : QPalette::WindowText);
        const auto paper = palette().color(pressed ? QPalette::WindowText : QPalette::Window);
        painter.fillRect(rect(), paper);
        if (!shown)
            painter.setOpacity(.45);
        painter.setFont(valueFont());
        const QRect bounds = rect().adjusted(m::list::lineInset, 0, -m::list::lineInset, 0);
        // Each column aligns its text as the strip does: left, centre, right.
        const int column = slot % 3;
        const Qt::Alignment align = column == 0 ? Qt::AlignLeft : column == 2 ? Qt::AlignRight : Qt::AlignHCenter;
        if (content.empty()) {
            if (!editable)
                return;
            QFont font = valueFont();
            font.setPixelSize(hf::emptyText);
            painter.setFont(font);
            painter.setPen(pressed ? ink : ui::noteColor(this));
            painter.drawText(bounds, align | Qt::AlignVCenter, strings::get("header_footer.empty"));
            return;
        }
        painter.setPen(ink);
        paintValue(painter, bounds, content, ink, paper, align);
    }

private:
    int slot;
    bool editable, shown = true;
    Content content;
};

SlotMap::SlotMap(QWidget *parent, bool editable) : QWidget(parent), editable(editable) {
    setObjectName(editable ? "rrHeaderFooterCustomMap" : "rrHeaderFooterPresetMap");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        cells[slot] = new Cell(this, slot, editable);
        connect(cells[slot], &QPushButton::clicked, this, [this, slot] {
            if (chosen)
                chosen(slot);
        });
    }
}

QPushButton *SlotMap::cell(int slot) const {
    return slot >= 0 && slot < headerfooter::slotCount ? cells[slot] : nullptr;
}

void SlotMap::setSlots(const Layout &next, const SlotContents &contents) {
    const bool repaint = layout != next;
    layout = next;
    for (int slot = 0; slot < headerfooter::slotCount; ++slot)
        cells[slot]->show(contents[slot],
                          headerfooter::footerSlot(slot) ? layout.footerSize != "off" : layout.headerSize != "off");
    if (repaint)
        update();
}

QSize SlotMap::sizeHint() const {
    return {hf::mapRowHeight * 4, 2 * hf::mapRowHeight + hf::mapBodyHeight + 2 * int(m::control::border)};
}

void SlotMap::resizeEvent(QResizeEvent *) {
    const int border = m::control::border;
    const int inner = width() - 2 * border;
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const int column = slot % 3;
        const int left = border + inner * column / 3, right = border + inner * (column + 1) / 3;
        const int top = headerfooter::footerSlot(slot) ? height() - border - hf::mapRowHeight : border;
        // Leave the dividers between cells, and the progress bar under the
        // footer, to the map.
        const int bar = headerfooter::footerSlot(slot) ? int(m::reading::progressFillHeight) : 0;
        cells[slot]->setGeometry(left + (column ? border : 0), top, right - left - (column ? border : 0),
                                 hf::mapRowHeight - bar);
    }
}

void SlotMap::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto ink = palette().color(QPalette::WindowText);
    const auto lines = palette().color(QPalette::Midlight);
    const int border = m::control::border;
    painter.fillRect(rect(), palette().color(QPalette::Window));
    ui::paintEdges(painter, rect(), ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge, ink,
                   border);
    const int inner = width() - 2 * border;
    const int headerBottom = border + hf::mapRowHeight, footerTop = height() - border - hf::mapRowHeight;
    const auto divider = ui::dividerColor(this);
    painter.fillRect(border, headerBottom, inner, border, divider);
    painter.fillRect(border, footerTop - border, inner, border, divider);
    for (int column = 1; column < 3; ++column) {
        const int x = border + inner * column / 3;
        painter.fillRect(x, border, border, hf::mapRowHeight, divider);
        painter.fillRect(x, footerTop, border, hf::mapRowHeight, divider);
    }
    // Four lines of text, the last one shorter, stand for the page.
    const int lineLeft = border + hf::mapLineInset, lineWidth = inner - 2 * hf::mapLineInset;
    const int bodyTop = headerBottom + border, bodyHeight = footerTop - border - bodyTop;
    const int first = bodyTop + (bodyHeight - 3 * hf::mapLineGap - hf::mapLineHeight) / 2;
    for (int line = 0; line < 4; ++line)
        painter.fillRect(lineLeft, first + line * hf::mapLineGap, line == 3 ? lineWidth * 3 / 4 : lineWidth,
                         hf::mapLineHeight, lines);
    if (layout.progressBar) {
        const int y = height() - border - int(m::reading::progressFillHeight);
        painter.fillRect(border, y, inner, m::reading::progressFillHeight, lines);
        painter.fillRect(border, y, inner / 30, m::reading::progressFillHeight, ink);
    }
}

PresetCard::PresetCard(Preset preset, QWidget *parent)
    : ui::Button(parent), value(preset), miniature(headerfooter::presetLayout(preset)) {
    setObjectName("rrHeaderFooterPreset_" + headerfooter::presetValue(preset));
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(headerfooter::presetLabel(preset));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void PresetCard::setLayout(const Layout &layout) {
    if (miniature == layout)
        return;
    miniature = layout;
    update();
}

void PresetCard::setDivider(bool shown) {
    if (divider == shown)
        return;
    divider = shown;
    update();
}

QSize PresetCard::sizeHint() const {
    const int pageHeight = int(hf::miniatureWidth) * 4 / 3;
    return {int(hf::miniatureWidth), hf::miniatureTop + pageHeight + hf::nameGap + hf::nameHeight};
}

QSize PresetCard::minimumSizeHint() const {
    return {int(hf::shortDash) * 4, sizeHint().height()};
}

void PresetCard::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto ink = palette().color(QPalette::WindowText), paper = palette().color(QPalette::Window);
    painter.fillRect(rect(), isDown() ? palette().color(QPalette::Midlight) : paper);
    if (divider)
        painter.fillRect(width() - m::control::border, hf::miniatureTop, m::control::border,
                         height() - hf::miniatureTop, ui::dividerColor(this));
    const int pageWidth = qMin(int(hf::miniatureWidth), width() - 2 * hf::namePadding);
    const int pageHeight = pageWidth * 4 / 3;
    const QRect page((width() - pageWidth) / 2, hf::miniatureTop, pageWidth, pageHeight);
    const int edge = isChecked() ? int(hf::selectedBorder) : int(m::control::border);
    ui::paintEdges(painter, page, ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                   isChecked() ? ink : ui::dividerColor(this), edge);
    const int inset = pageWidth / 10;
    const int top = page.y() + edge + pageHeight / 18, bottom = page.bottom() - edge - pageHeight / 14;
    // Each slot is a dash where the strip would show it.
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const bool footer = headerfooter::footerSlot(slot);
        if ((footer ? miniature.footerSize : miniature.headerSize) == "off")
            continue;
        const int dash = qMin(dashWidth(miniature.slotFields[slot]), pageWidth / 3 - inset / 2);
        if (!dash)
            continue;
        const int column = slot % 3;
        const int x = column == 0   ? page.x() + inset
                      : column == 1 ? page.x() + (pageWidth - dash) / 2
                                    : page.right() + 1 - inset - dash;
        painter.fillRect(x, (footer ? bottom : top) - hf::dashHeight / 2, dash, hf::dashHeight, ink);
    }
    const auto lines = palette().color(QPalette::Midlight);
    const int lineTop = top + pageHeight / 9, lineGap = pageHeight / 11;
    for (int line = 0; line < 6; ++line)
        painter.fillRect(page.x() + inset, lineTop + line * lineGap,
                         line == 5 ? (pageWidth - 2 * inset) / 2 : pageWidth - 2 * inset, m::control::border, lines);
    if (miniature.progressBar)
        painter.fillRect(page.x() + edge, page.bottom() + 1 - edge - hf::dashHeight / 2, pageWidth / 12,
                         hf::dashHeight / 2, ink);
    const QFont label = ui::interfaceFont(m::text::secondary);
    painter.setFont(label);
    auto name = headerfooter::presetLabel(value);
    const int room = qMax(0, width() - 4 * hf::namePadding);
    if (ui::interfaceTextWidth(name, label) > room)
        name = ui::elideInterfaceText(name, label, room);
    const int textWidth = ui::interfaceTextWidth(name, label);
    const int nameWidth = textWidth + 2 * hf::namePadding;
    const QRect block((width() - nameWidth) / 2, page.bottom() + 1 + hf::nameGap, nameWidth, hf::nameHeight);
    if (isChecked())
        painter.fillRect(block, ink);
    painter.setPen(isChecked() ? paper : ink);
    const QRect text(block.x() + hf::namePadding, block.y(), textWidth, block.height());
    ui::drawInterfaceText(painter, text, name, Qt::AlignLeft | Qt::AlignVCenter);
}

namespace {
// The bar that takes the place of the header's action row, as in Chapters:
// a cell on the left, a serif title and an optional count on the right.
void arrangeBar(QWidget *owner, QWidget *cell, QLabel *title, QLabel *count) {
    const int barHeight = m::chrome::actionHeight - m::chrome::heavyRule;
    const int titleLeft = m::chrome::cellWidth + m::chrome::inset;
    cell->setGeometry(0, 0, m::chrome::cellWidth, barHeight);
    const int titleRight = count ? owner->width() / 2 : owner->width() - m::chrome::inset;
    title->setGeometry(titleLeft, 0, qMax(1, titleRight - titleLeft), barHeight);
    if (count)
        count->setGeometry(owner->width() / 2, 0, qMax(1, owner->width() / 2 - m::chrome::inset), barHeight);
}
QLabel *barTitle(QWidget *parent, const char *name) {
    auto *title = new QLabel(parent);
    title->setObjectName(name);
    title->setTextFormat(Qt::PlainText);
    ui::readerStyle(title, ui::styles::brand(m::chrome::titleText) + "background:transparent;");
    return title;
}
void paintBarRule(QWidget *owner) {
    QPainter painter(owner);
    painter.fillRect(0, m::chrome::actionHeight - m::chrome::heavyRule, owner->width(), m::chrome::heavyRule,
                     owner->palette().color(QPalette::WindowText));
}
}

HeaderFooterView::HeaderFooterView(QWidget *parent) : ui::Panel(parent) {
    setObjectName("rrHeaderFooterView");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrHeaderFooterClose");
    close->setRule(Qt::RightEdge);
    close->setAccessibleName(strings::get("header_footer.close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (closed)
            closed();
    });
    title = barTitle(this, "rrHeaderFooterTitle");
    title->setText(strings::get("header_footer.title"));
    content = new QWidget(this);
    contentRows = new QVBoxLayout(content);
    contentRows->setContentsMargins(m::panel::contentMargins);
    hide();
}

void HeaderFooterView::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    paintBarRule(this);
}

void HeaderFooterView::resizeEvent(QResizeEvent *) {
    arrangeBar(this, close, title, nullptr);
    content->setGeometry(0, m::chrome::actionHeight, width(), qMax(0, height() - m::chrome::actionHeight));
}

class FieldChooser::Row final : public ui::Button {
public:
    Row(QWidget *parent, Field field) : ui::Button(parent), field(field) {
        setObjectName("rrHeaderFooterField_" + headerfooter::fieldValue(field));
        setCheckable(true);
        setAutoExclusive(true);
        setFocusPolicy(Qt::NoFocus);
        setFixedHeight(hf::chooserRowHeight);
        ui::readerStyle(
            this, QString("QPushButton{min-height:%1px;max-height:%1px;padding:0;}").arg(int(hf::chooserRowHeight)));
        setText(headerfooter::fieldLabel(field));
        setAccessibleName(text());
    }
    void setValue(const Content &next) {
        if (value == next)
            return;
        value = next;
        update();
    }
    const Field field;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const auto ink = palette().color(QPalette::WindowText), paper = palette().color(QPalette::Window);
        painter.fillRect(rect(), isDown() ? palette().color(QPalette::Midlight) : paper);
        painter.fillRect(0, height() - m::control::border, width(), m::control::border, ui::dividerColor(this));
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF radio(m::chrome::inset, (height() - hf::radioSize) / 2.0, hf::radioSize, hf::radioSize);
        const QRectF track = radio.adjusted(1, 1, -1, -1);
        painter.setPen(QPen(ink, px(2)));
        painter.setBrush(paper);
        painter.drawEllipse(track);
        if (isChecked()) {
            const qreal inset = track.width() / 4;
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawEllipse(track.adjusted(inset, inset, -inset, -inset));
        }
        const int left = m::chrome::inset + hf::radioSize + hf::radioGap;
        const int right = width() - m::chrome::inset;
        const QFont label = ui::interfaceFont(m::chrome::labelText);
        painter.setFont(label);
        painter.setPen(ink);
        auto name = text();
        int labelWidth = ui::interfaceTextWidth(name, label);
        if (labelWidth > (right - left) * 3 / 5) {
            name = ui::elideInterfaceText(name, label, (right - left) * 3 / 5);
            labelWidth = ui::interfaceTextWidth(name, label);
        }
        ui::drawInterfaceText(painter, QRect(left, 0, labelWidth, height()), name, Qt::AlignLeft | Qt::AlignVCenter);
        painter.setFont(valueFont());
        const int valueLeft = left + labelWidth + hf::radioGap;
        auto shown = value;
        if (shown.empty() && field != Field::Nothing)
            shown.text = strings::get("header_footer.unavailable");
        else if (field == Field::Nothing)
            shown.text = strings::get("header_footer.unavailable");
        // Right-align the value: measure it, then let paintValue centre it in a box that ends at the edge.
        const qreal body = qRound(painter.font().pixelSize() * .75);
        const int icon = shown.batteryIcon ? qCeil(ui::batteryWidth(body)) : 0;
        const int gap = shown.batteryIcon && !shown.text.isEmpty() ? int(m::reading::batteryGap) : 0;
        const int room = qMax(0, right - valueLeft);
        const int natural = (shown.text.isEmpty() ? 0 : qt::textWidth(painter.fontMetrics(), shown.text)) + gap + icon;
        const int box = qMin(room, natural);
        paintValue(painter, QRect(right - box, 0, box, height()), shown, ink, paper);
    }

private:
    Content value;
};

FieldChooser::FieldChooser(QWidget *parent) : ui::Panel(parent) {
    setObjectName("rrHeaderFooterChooser");
    // The same back cell as Acknowledgments: it leaves without a new choice.
    auto *back = new ui::CellButton(ui::ControlIcon::Back, this, m::chrome::backIcon);
    back->setRule(Qt::RightEdge);
    back->setObjectName("rrHeaderFooterBack");
    back->setAccessibleName(strings::get("common.back"));
    finish = back;
    connect(finish, &QPushButton::clicked, this, [this] {
        if (done)
            done();
    });
    title = barTitle(this, "rrHeaderFooterChooserTitle");
    position = new QLabel(this);
    position->setObjectName("rrHeaderFooterChooserPosition");
    position->setTextFormat(Qt::PlainText);
    position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui::readerStyle(position, ui::styles::font(m::chrome::countText) + "background:transparent;");
    heading = new ui::ListHeader(this);
    heading->setObjectName("rrHeaderFooterChooserHeading");
    heading->setCaptions(strings::get("header_footer.fields"), strings::get("header_footer.shows"));
    for (const auto field : headerfooter::fields) {
        auto *row = new Row(this, field);
        rows.append(row);
        connect(row, &QPushButton::clicked, this, [this, field] {
            if (selected)
                selected(field);
        });
    }
    // Rows that do not fit go to further pages, with the same pager as Chapters.
    pager = new ui::PagerBar(this, "rrHeaderFooterFields");
    pager->previous->setAccessibleName(strings::get("header_footer.previous_fields"));
    pager->next->setAccessibleName(strings::get("header_footer.next_fields"));
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        page = pager->turnTarget(-1);
        arrange();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        page = pager->turnTarget(1);
        arrange();
    });
    // The slot's own case, in the same segmented row as the other settings.
    options = new ui::Panel(this, Qt::TopEdge);
    options->setSoftEdges(true);
    auto *optionRows = new QVBoxLayout(options);
    optionRows->setContentsMargins(m::panel::contentMargins);
    ui::SegmentedRow uppercase(options, optionRows, "rrHeaderFooterUppercase", strings::get("header_footer.uppercase"));
    for (const bool on : {false, true}) {
        auto *button = uppercase.add(QString("rrHeaderFooterUppercase_") + (on ? "on" : "off"),
                                     strings::get(on ? "common.on" : "common.off"));
        uppercaseButtons[on] = button;
        connect(button, &QPushButton::clicked, this, [this, on] {
            if (uppercaseChanged)
                uppercaseChanged(on);
        });
    }
    hide();
}

void FieldChooser::open(int slot, Field current, const headerfooter::Values &values, bool uppercase) {
    currentSlot = slot;
    title->setText(headerfooter::slotLabel(slot));
    position->setText(strings::get("header_footer.slot_position").arg(slot + 1).arg(headerfooter::slotCount));
    for (auto *row : rows) {
        row->setValue(headerfooter::content(row->field, values, uppercase));
        row->setChecked(row->field == current);
    }
    uppercaseButtons[uppercase]->setChecked(true);
    // Open on the page that holds the slot's field.
    page = 0;
    for (int index = 0; index < rows.size(); ++index)
        if (rows[index]->field == current)
            page = index / qMax(1, perPage());
    arrange();
}

int FieldChooser::perPage() const {
    const int listTop = m::chrome::actionHeight + m::list::headerHeight;
    const int available = height() - listTop - options->sizeHint().height();
    int count = qMax(1, available / int(hf::chooserRowHeight));
    // Leave room for the pager only when the list needs more than one page.
    if (rows.size() > count)
        count = qMax(1, (available - m::chrome::actionHeight) / int(hf::chooserRowHeight));
    return count;
}

void FieldChooser::arrange() {
    const int count = perPage(), pages = (int(rows.size()) + count - 1) / count;
    page = qBound(0, page, pages - 1);
    pager->setPage(page, pages);
    const int listTop = m::chrome::actionHeight + m::list::headerHeight;
    for (int index = 0; index < rows.size(); ++index) {
        const int row = index - page * count;
        rows[index]->setVisible(row >= 0 && row < count);
        rows[index]->setGeometry(0, listTop + row * int(hf::chooserRowHeight), width(), hf::chooserRowHeight);
    }
}

void FieldChooser::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    paintBarRule(this);
}

void FieldChooser::resizeEvent(QResizeEvent *) {
    arrangeBar(this, finish, title, position);
    heading->setGeometry(0, m::chrome::actionHeight, width(), m::list::headerHeight);
    // The pager keeps the bottom, as in Chapters; the slot's case sits above it.
    const int optionsHeight = options->sizeHint().height();
    pager->setGeometry(0, height() - m::chrome::actionHeight, width(), m::chrome::actionHeight);
    arrange();
    const int bottom = pager->isVisible() ? pager->y() : height();
    options->setGeometry(0, bottom - optionsHeight, width(), optionsHeight);
}
}
