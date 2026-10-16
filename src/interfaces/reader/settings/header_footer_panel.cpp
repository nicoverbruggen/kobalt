#include "header_footer_panel.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/options.h"
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
// Page counts keep the strip's half-width spaces while they fit.
void paintValue(QPainter &painter, const QRect &bounds, const Content &content, const QColor &ink, const QColor &paper,
                Qt::Alignment align, bool halfSpaces) {
    const QFont font = painter.font();
    const qreal body = qRound(font.pixelSize() * .75);
    const int icon = content.batteryIcon ? qCeil(ui::batteryWidth(body)) : 0;
    const int gap = content.batteryIcon && !content.text.isEmpty() ? int(m::reading::batteryGap) : 0;
    const int room = qMax(0, bounds.width() - icon - gap);
    QString text = content.text;
    int textWidth = text.isEmpty() ? 0
                    : halfSpaces   ? qCeil(ui::halfSpacedWidth(painter.fontMetrics(), text))
                                   : qt::textWidth(painter.fontMetrics(), text);
    if (textWidth > room) {
        halfSpaces = false;
        text = painter.fontMetrics().elidedText(text, Qt::ElideRight, room);
        textWidth = text.isEmpty() ? 0 : qt::textWidth(painter.fontMetrics(), text);
    }
    const int total = textWidth + gap + icon;
    const int x = align & Qt::AlignLeft    ? bounds.x()
                  : align & Qt::AlignRight ? bounds.right() + 1 - total
                                           : bounds.x() + (bounds.width() - total) / 2;
    if (!text.isEmpty() && halfSpaces)
        ui::drawHalfSpaced(painter, x, bounds, text);
    else if (!text.isEmpty())
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

namespace {
// The page's progress bar: its fill and the gap under it.
int progressSpace() {
    return m::reading::progressFillHeight + m::reading::progressBottom;
}
// A slot's box: its strip's line of text and the padding around it.
int slotBoxHeight(const QString &size, bool footer) {
    const QFont font = ui::chromeFont(m::reading::textPixels(m::reading::textSize(size), footer));
    return QFontMetrics(font).height() + 2 * hf::slotPadding;
}
// A dashed outline on whole pixels, drawn as filled dashes so every density
// keeps the same crisp border width.
void paintDashes(QPainter &painter, const QRect &box, const QColor &color) {
    const int border = m::control::border, dash = hf::slotDash;
    for (int x = box.left(); x <= box.right(); x += 2 * dash) {
        const int length = qMin(dash, box.right() + 1 - x);
        painter.fillRect(x, box.top(), length, border, color);
        painter.fillRect(x, box.bottom() + 1 - border, length, border, color);
    }
    for (int y = box.top(); y <= box.bottom(); y += 2 * dash) {
        const int length = qMin(dash, box.bottom() + 1 - y);
        painter.fillRect(box.left(), y, border, length, color);
        painter.fillRect(box.right() + 1 - border, y, border, length, color);
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
    void show(Field shownField, const Content &next, bool stripShown, bool bar, int textPixels) {
        if (field == shownField && content == next && shown == stripShown && progressBar == bar && pixels == textPixels)
            return;
        field = shownField;
        content = next;
        shown = stripShown;
        progressBar = bar;
        pixels = textPixels;
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
        // The page shows through around the slot boxes; a held slot fills its box.
        if (!shown)
            painter.setOpacity(.45);
        // The strip's own font and size, so Small, Medium and Large show as on the page.
        painter.setFont(ui::chromeFont(pixels));
        // Header slots keep the top edge and footer slots the bottom edge,
        // above the progress bar when there is one, as on the page.
        const bool footer = headerfooter::footerSlot(slot);
        const int line = painter.fontMetrics().height();
        const int edge = hf::mapEdgeInset + (footer && progressBar ? progressSpace() : 0);
        const int boxHeight = line + 2 * hf::slotPadding;
        // Boxes keep half a gap from each other and a wider margin from the page's sides.
        const int column = slot % 3;
        const int left = column == 0 ? int(hf::mapSideInset) : hf::slotGap / 2;
        const int right = column == 2 ? int(hf::mapSideInset) : hf::slotGap / 2;
        const QRect box(left, footer ? height() - edge - boxHeight : edge, qMax(0, width() - left - right), boxHeight);
        if (pressed)
            painter.fillRect(box, paper);
        else
            paintDashes(painter, box, ui::dividerColor(this));
        const QRect bounds = box.adjusted(m::list::lineInset - hf::slotGap / 2, hf::slotPadding,
                                          -(m::list::lineInset - hf::slotGap / 2), -hf::slotPadding);
        // Each column aligns its text as the strip does: left, centre, right.
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
        paintValue(painter, bounds, content, ink, paper, align, headerfooter::pageCount(field));
    }

private:
    int slot;
    bool editable, shown = true, progressBar = false;
    int pixels = 0;
    Field field = Field::Nothing;
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
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const bool footer = headerfooter::footerSlot(slot);
        const auto &size = footer ? layout.footerSize : layout.headerSize;
        cells[slot]->show(layout.slotFields[slot], contents[slot], size != "off", layout.progressBar,
                          m::reading::textPixels(m::reading::textSize(size), footer));
    }
    if (repaint)
        update();
}

QSize SlotMap::sizeHint() const {
    // Room for the largest strips and the bar, whatever the layout shows, so
    // the sheet keeps its height when a preset or size changes. Smaller
    // strips leave more page between them, as they do on the reader.
    const int strips = 2 * hf::mapEdgeInset + slotBoxHeight(QStringLiteral("large"), false) +
                       slotBoxHeight(QStringLiteral("large"), true) + progressSpace();
    // Two lines with a line gap above and below them. A third line would
    // push the sheet past the screen on a Clara BW.
    const int body = 3 * hf::mapLineGap + hf::mapLineHeight;
    return {hf::mapRowHeight * 4, qMax(2 * int(hf::mapRowHeight), strips + body) + 2 * int(m::control::border)};
}

void SlotMap::resizeEvent(QResizeEvent *) {
    const int border = m::control::border;
    const int inner = width() - 2 * border;
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const int column = slot % 3;
        const int left = border + inner * column / 3, right = border + inner * (column + 1) / 3;
        const int top = headerfooter::footerSlot(slot) ? height() - border - hf::mapRowHeight : border;
        cells[slot]->setGeometry(left, top, right - left, hf::mapRowHeight);
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
    // Two lines of text, the last one shorter, stand for the page between
    // the header and footer slots.
    const int lineLeft = border + hf::mapLineInset, lineWidth = inner - 2 * hf::mapLineInset;
    const int headerBottom = border + hf::mapEdgeInset + slotBoxHeight(layout.headerSize, false);
    const int footerTop = height() - border - hf::mapEdgeInset - (layout.progressBar ? progressSpace() : 0) -
                          slotBoxHeight(layout.footerSize, true);
    const int first = (headerBottom + footerTop - hf::mapLineGap - hf::mapLineHeight) / 2;
    for (int line = 0; line < 2; ++line) {
        painter.fillRect(lineLeft, first + line * hf::mapLineGap, line == 1 ? lineWidth * 3 / 4 : lineWidth,
                         hf::mapLineHeight, lines);
    }
    if (layout.progressBar) {
        // Where the page draws it: above the bottom edge, across the width.
        const int y = height() - border - progressSpace();
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

void PresetCard::setModified(bool value) {
    if (modified == value)
        return;
    modified = value;
    setAccessibleName(
        modified ? strings::get("header_footer.preset.modified_name").arg(headerfooter::presetLabel(this->value))
                 : headerfooter::presetLabel(this->value));
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
    // A held card already looks selected, so the press shows where the choice lands.
    const bool selected = isChecked() || isDown();
    painter.fillRect(rect(), paper);
    if (divider)
        painter.fillRect(width() - m::control::border, hf::miniatureTop, m::control::border,
                         height() - hf::miniatureTop, ui::dividerColor(this));
    const int pageWidth = qMin(int(hf::miniatureWidth), width() - 2 * hf::namePadding);
    const int pageHeight = pageWidth * 4 / 3;
    const QRect page((width() - pageWidth) / 2, hf::miniatureTop, pageWidth, pageHeight);
    const int edge = selected ? int(hf::selectedBorder) : int(m::control::border);
    ui::paintEdges(painter, page, ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                   selected ? ink : ui::dividerColor(this), edge);
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
    auto name = modified ? strings::get("header_footer.preset.modified").arg(headerfooter::presetLabel(value))
                         : headerfooter::presetLabel(value);
    const int room = qMax(0, width() - 4 * hf::namePadding);
    if (ui::interfaceTextWidth(name, label) > room)
        name = ui::elideInterfaceText(name, label, room);
    const int textWidth = ui::interfaceTextWidth(name, label);
    const int nameWidth = textWidth + 2 * hf::namePadding;
    const QRect block((width() - nameWidth) / 2, page.bottom() + 1 + hf::nameGap, nameWidth, hf::nameHeight);
    if (selected)
        painter.fillRect(block, ink);
    painter.setPen(selected ? paper : ink);
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

HeaderFooterView::HeaderFooterView(QWidget *parent) : ui::Panel(parent, Qt::TopEdge) {
    setObjectName("rrHeaderFooterView");
    // The same heavy rule, bold title and close cell as the Font sheet.
    setEdgeWidth(m::chrome::heavyRule);
    title = new QLabel(strings::get("header_footer.customize_title"), this);
    title->setObjectName("rrHeaderFooterTitle");
    ui::readerStyle(title, ui::styles::font(m::text::body, true) + "background:transparent;");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrHeaderFooterClose");
    close->setRule(Qt::LeftEdge);
    close->setAccessibleName(strings::get("header_footer.close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (closed)
            closed();
    });
    content = new QWidget(this);
    contentRows = new QVBoxLayout(content);
    // Room under the title's divider and below the last row.
    QMargins margins = m::panel::contentMargins;
    margins.setBottom(2 * m::spacing::section);
    contentRows->setContentsMargins(margins);
    hide();
}

int HeaderFooterView::preferredHeight(int width) const {
    const int rows = contentRows->hasHeightForWidth() ? contentRows->totalHeightForWidth(width)
                                                      : contentRows->totalSizeHint().height();
    return m::choices::pickerHeader + rows;
}

void HeaderFooterView::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    // A divider separates the title row from the presets.
    QPainter painter(this);
    painter.fillRect(0, m::choices::pickerHeader - m::control::border, width(), m::control::border,
                     ui::dividerColor(this));
}

void HeaderFooterView::resizeEvent(QResizeEvent *) {
    const int header = m::choices::pickerHeader;
    const int cell = qMin(int(m::choices::pickerHeader), width() / 4);
    title->setGeometry(m::chrome::inset, 1, qMax(0, width() - cell - m::chrome::inset), header - 2);
    // The cell stops above the divider, which runs the full width.
    close->setGeometry(width() - cell, m::chrome::heavyRule, cell,
                       header - m::chrome::heavyRule - int(m::control::border));
    // The rows keep their own height; a sheet stretched to meet the toolbar
    // leaves the extra room below the last row.
    const int rows = preferredHeight(width()) - header;
    content->setGeometry(0, header, width(), qMax(0, qMin(rows, height() - header)));
}

FieldChooser::FieldChooser(QWidget *parent) : ui::Panel(parent) {
    setObjectName("rrHeaderFooterChooser");
    // The same close cell as Chapters: it leaves without a new choice.
    auto *close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setRule(Qt::RightEdge);
    close->setObjectName("rrHeaderFooterFieldsClose");
    close->setAccessibleName(strings::get("header_footer.close_fields"));
    finish = close;
    connect(finish, &QPushButton::clicked, this, [this] {
        if (done)
            done();
    });
    title = barTitle(this, "rrHeaderFooterChooserTitle");
    // The slot's own case, as a settings checkbox on the right of the bar.
    uppercase = new ui::DescribedOption(strings::get("header_footer.uppercase"), {}, this);
    uppercase->setObjectName("rrHeaderFooterUppercase");
    auto look = uppercase->presentation();
    look.indicator = ui::DescribedOption::Indicator::Square;
    // The same size as the checkboxes in Settings and the Light panel.
    look.indicatorSize = QSize(m::chrome::checkSize, m::chrome::checkSize);
    look.labelPixels = m::chrome::labelText;
    look.minimumRowHeight = m::chrome::actionHeight - m::chrome::heavyRule;
    look.horizontalInset = m::chrome::inset;
    uppercase->setPresentation(look);
    connect(uppercase, &QCheckBox::clicked, this, [this](bool on) {
        if (uppercaseChanged)
            uppercaseChanged(on);
    });
    heading = new ui::ListHeader(this);
    heading->setObjectName("rrHeaderFooterChooserHeading");
    heading->setCaptions(strings::get("header_footer.fields"), strings::get("header_footer.shows"));
    // A pick-one list: the slot's field has the filled radio, and each row
    // shows on its right what the field would show now.
    list = new ui::RadioList(this);
    list->setObjectName("rrHeaderFooterFields");
    list->setMinimumRowHeight(hf::fieldRowHeight);
    // Only the described fields need the room for a second line.
    list->setUniformRows(false);
    for (const auto field : headerfooter::choosableFields)
        list->add("rrHeaderFooterField_" + headerfooter::fieldValue(field), headerfooter::fieldLabel(field),
                  headerfooter::fieldDescription(field));
    list->chosen = [this](int index) {
        if (selected)
            selected(headerfooter::choosableFields[index]);
    };
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
    hide();
}

void FieldChooser::open(int slot, Field current, const headerfooter::Values &values, bool uppercase) {
    currentSlot = slot;
    title->setText(strings::get("header_footer.customize_slot").arg(headerfooter::slotLabel(slot)));
    const auto &rows = list->options();
    for (int index = 0; index < rows.size(); ++index) {
        const auto field = headerfooter::choosableFields[index];
        auto shown = headerfooter::content(field, values, uppercase);
        // Each row shows what the slot would: text, a battery, or both.
        if (shown.text.isEmpty() && !shown.batteryIcon)
            shown.text = strings::get("header_footer.unavailable");
        ui::DescribedOption::ValueStyle style;
        style.battery = shown.batteryIcon;
        style.percent = shown.battery;
        style.charging = shown.charging;
        style.halfSpaces = headerfooter::pageCount(field);
        rows[index]->setValue(shown.text, style);
        if (field == current)
            list->setCurrent(index);
    }
    this->uppercase->setChecked(uppercase);
    // Open on the page that holds the slot's field. The chooser may not have
    // its final size yet, so the page is chosen once it has room for rows.
    page = 0;
    openRow = 0;
    for (int index = 0; index < rows.size(); ++index)
        if (headerfooter::choosableFields[index] == current)
            openRow = index;
    arrange();
}

QVector<int> FieldChooser::pageStarts() const {
    // Fill each page with the rows that fit, at their own heights. Leave room
    // for the pager only when the list needs more than one page.
    const int listTop = m::chrome::actionHeight + m::list::headerHeight;
    const auto starts = [this](int available) {
        QVector<int> result{0};
        int used = 0;
        for (int index = 0; index < list->options().size(); ++index) {
            const int row = list->rowHeight(index, width());
            if (used > 0 && used + row > available) {
                result.append(index);
                used = 0;
            }
            used += row;
        }
        return result;
    };
    const auto single = starts(height() - listTop);
    return single.size() > 1 ? starts(height() - listTop - m::chrome::actionHeight) : single;
}

void FieldChooser::arrange() {
    const auto starts = pageStarts();
    const int pages = starts.size();
    if (openRow >= 0 && height() > m::chrome::actionHeight + m::list::headerHeight + hf::fieldRowHeight) {
        page = 0;
        while (page + 1 < pages && starts[page + 1] <= openRow)
            ++page;
        openRow = -1;
    }
    page = qBound(0, page, pages - 1);
    pager->setPage(page, pages);
    const int listTop = m::chrome::actionHeight + m::list::headerHeight;
    const int first = starts[page];
    const int count = (page + 1 < pages ? starts[page + 1] : int(list->options().size())) - first;
    list->showRows(first, count);
    list->setGeometry(0, listTop, width(), list->heightForWidth(width()));
}

void FieldChooser::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    paintBarRule(this);
}

void FieldChooser::resizeEvent(QResizeEvent *) {
    // The checkbox takes what its label and box need; the title has the rest.
    const int barHeight = m::chrome::actionHeight - m::chrome::heavyRule;
    const int boxWidth = ui::interfaceTextWidth(uppercase->text(), ui::interfaceFont(m::chrome::labelText)) +
                         m::settings::toggleGap + m::chrome::checkSize + 2 * m::chrome::inset;
    uppercase->setGeometry(width() - boxWidth, 0, boxWidth, barHeight);
    arrangeBar(this, finish, title, nullptr);
    title->setGeometry(title->x(), 0, qMax(1, uppercase->x() - title->x()), barHeight);
    heading->setGeometry(0, m::chrome::actionHeight, width(), m::list::headerHeight);
    // The pager keeps the bottom, as in Chapters.
    pager->setGeometry(0, height() - m::chrome::actionHeight, width(), m::chrome::actionHeight);
    arrange();
}
}
