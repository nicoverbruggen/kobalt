#include "header_footer_panel.h"
#include "ui/controls/icons.h"
#include "ui/controls/options.h"
#include "ui/controls/paging.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "support/qt_font.h"
#include "support/strings.h"

#include <QPainter>
#include <QResizeEvent>
#include <QtMath>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace metrics = ui::metrics;
namespace headerFooterMetrics = metrics::headerFooter;
using headerfooter::Content;
using headerfooter::Field;
using headerfooter::Layout;
using headerfooter::Preset;
using ui::px;

namespace {
QFont valueFont()
{
    QFont font = ui::headerFooterFont(headerFooterMetrics::valueText);
    font.setLetterSpacing(QFont::AbsoluteSpacing, metrics::chrome::captionSpacing);
    return font;
}

// The text and battery of one slot, elided to `bounds` and aligned in it.
// Page counts keep the strip's half-width spaces while they fit.
void paintValue(QPainter &painter, const QRect &bounds, const Content &content, const QColor &ink,
                const QColor &paper, Qt::Alignment align, bool halfSpaces)
{
    const QFont font = painter.font();
    const qreal body = qRound(font.pixelSize() * .75);
    const auto textFont = content.batteryIcon && !content.text.isEmpty()
            ? ui::batteryTextFont(font, body, ui::headerFooterTextWeight())
            : font;
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setFont(textFont);
    const int icon = content.batteryIcon ? qCeil(ui::batteryWidth(body)) : 0;
    const int gap =
            content.batteryIcon && !content.text.isEmpty() ? int(metrics::reading::batteryGap) : 0;
    const int room = qMax(0, bounds.width() - icon - gap);
    QString text = content.text;
    int textWidth = text.isEmpty() ? 0
            : halfSpaces           ? qCeil(ui::halfSpacedWidth(painter.font(), text))
                                   : ui::interfaceTextWidth(text, textFont);
    if (textWidth > room) {
        halfSpaces = false;
        text = ui::headerFooterText(content, textFont, room);
        textWidth = text.isEmpty() ? 0 : ui::interfaceTextWidth(text, textFont);
    }
    const int total = textWidth + gap + icon;
    const int x = align & Qt::AlignLeft ? bounds.x()
            : align & Qt::AlignRight    ? bounds.right() + 1 - total
                                        : bounds.x() + (bounds.width() - total) / 2;
    if (!text.isEmpty() && content.batteryIcon)
        ui::drawOpticalPrivateText(painter, QRect(x, bounds.y(), textWidth, bounds.height()), text,
                                   Qt::AlignLeft, ui::headerFooterTextWeight());
    else if (!text.isEmpty() && halfSpaces)
        ui::drawHalfSpaced(painter, x, bounds, text, ui::headerFooterTextWeight());
    else if (!text.isEmpty())
        ui::drawInterfaceText(painter, QRect(x, bounds.y(), textWidth, bounds.height()), text,
                              Qt::AlignLeft | Qt::AlignVCenter, ui::headerFooterTextWeight());
    if (content.batteryIcon) {
        const qreal width = body * int(metrics::panel::batteryBodyWidth)
                / int(metrics::panel::batteryBodyHeight);
        ui::paintBattery(painter,
                         QRectF(x + textWidth + gap, bounds.y() + (bounds.height() - body) / 2.0,
                                width, body),
                         content.battery, content.charging, ink, paper);
    }
    painter.restore();
}

// How much of a miniature's width a field takes, as a short or long dash.
int dashWidth(Field field)
{
    switch (field) {
    case Field::Nothing:
        return 0;
    case Field::BookTitle:
    case Field::Author:
    case Field::Chapter:
    case Field::ChapterAndPage:
    case Field::TitleAndPage:
        return headerFooterMetrics::longDash;
    default:
        return headerFooterMetrics::shortDash;
    }
}
} // namespace

namespace {
// The page's progress bar: its fill and the gap under it.
int progressSpace()
{
    return metrics::reading::progressFillHeight + metrics::reading::progressBottom;
}

// A slot's box: its strip's line of text and the padding around it.
int slotBoxHeight(int size, bool footer)
{
    const QFont font = ui::headerFooterFont(metrics::reading::textPixels(size, footer));
    return ui::interfaceLineHeight(font) + 2 * headerFooterMetrics::slotPadding;
}

// A dashed outline on whole pixels, drawn as filled dashes so every density
// keeps the same crisp border width.
void paintDashes(QPainter &painter, const QRect &box, const QColor &color)
{
    const int border = metrics::control::border, dash = headerFooterMetrics::slotDash;
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
} // namespace

class SlotMap::Cell final : public ui::Button
{
public:
    Cell(QWidget *parent, int slot, bool editable)
        : ui::Button(parent), slot(slot), editable(editable)
    {
        setObjectName(QString(editable ? "rrHeaderFooterSlot%1" : "rrHeaderFooterPresetSlot%1")
                              .arg(slot));
        setFocusPolicy(Qt::NoFocus);
        setEnabled(editable);
    }

    void show(Field shownField, const Content &next, bool stripShown, bool bar, int textPixels)
    {
        if (field == shownField && content == next && shown == stripShown && progressBar == bar
            && pixels == textPixels)
            return;
        field = shownField;
        content = next;
        shown = stripShown;
        progressBar = bar;
        pixels = textPixels;
        const auto value = content.empty() ? strings::get("header_footer.empty") : content.text;
        setAccessibleName(
                strings::get("header_footer.slot_value")
                        .arg(headerfooter::slotLabel(slot), value.isEmpty() ? "" : value));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool pressed = isDown();
        const auto ink = palette().color(pressed ? QPalette::Window : QPalette::WindowText);
        const auto paper = palette().color(pressed ? QPalette::WindowText : QPalette::Window);
        // The page shows through around the slot boxes; a held slot fills its box.
        if (!shown)
            painter.setOpacity(.45);
        // The strip's own font and size, so Small, Medium and Large show as on the page.
        painter.setFont(ui::headerFooterFont(pixels));
        // Header slots keep the top edge and footer slots the bottom edge,
        // above the progress bar when there is one, as on the page.
        const bool footer = headerfooter::footerSlot(slot);
        const int line = painter.fontMetrics().height();
        const int edge =
                headerFooterMetrics::mapEdgeInset + (footer && progressBar ? progressSpace() : 0);
        const int boxHeight = line + 2 * headerFooterMetrics::slotPadding;
        // Boxes keep half a gap from each other and a wider margin from the page's sides.
        const int column = slot % 3;
        const int left = column == 0 ? int(headerFooterMetrics::mapSideInset)
                                     : headerFooterMetrics::slotGap / 2;
        const int right = column == 2 ? int(headerFooterMetrics::mapSideInset)
                                      : headerFooterMetrics::slotGap / 2;
        const QRect box(left, footer ? height() - edge - boxHeight : edge,
                        qMax(0, width() - left - right), boxHeight);
        if (pressed)
            painter.fillRect(box, paper);
        else
            paintDashes(painter, box, ui::dividerColor(this));
        const QRect bounds =
                box.adjusted(metrics::list::lineInset - headerFooterMetrics::slotGap / 2,
                             headerFooterMetrics::slotPadding,
                             -(metrics::list::lineInset - headerFooterMetrics::slotGap / 2),
                             -headerFooterMetrics::slotPadding);
        // Each column aligns its text as the strip does: left, centre, right.
        const Qt::Alignment align = column == 0 ? Qt::AlignLeft
                : column == 2                   ? Qt::AlignRight
                                                : Qt::AlignHCenter;
        if (content.empty()) {
            if (!editable)
                return;
            QFont font = valueFont();
            font.setPixelSize(headerFooterMetrics::emptyText);
            painter.setFont(font);
            painter.setPen(pressed ? ink : ui::noteColor(this));
            ui::drawInterfaceText(painter, bounds, strings::get("header_footer.empty"),
                                  align | Qt::AlignVCenter);
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

SlotMap::SlotMap(QWidget *parent, bool editable) : QWidget(parent), editable(editable)
{
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

QPushButton *SlotMap::cell(int slot) const
{
    return slot >= 0 && slot < headerfooter::slotCount ? cells[slot] : nullptr;
}

void SlotMap::setSlots(const Layout &next, const SlotContents &contents)
{
    const bool repaint = layout != next;
    layout = next;
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const bool footer = headerfooter::footerSlot(slot);
        const auto &size = footer ? layout.footerSize : layout.headerSize;
        cells[slot]->show(layout.slotFields[slot], contents[slot],
                          footer ? layout.footerEnabled : layout.headerEnabled, layout.progressBar,
                          metrics::reading::textPixels(size, footer));
    }
    if (repaint)
        update();
}

QSize SlotMap::sizeHint() const
{
    // Room for the largest strips and the bar, whatever the layout shows, so
    // the sheet keeps its height when a preset or size changes. Smaller
    // strips leave more page between them, as they do on the reader.
    const int strips = 2 * headerFooterMetrics::mapEdgeInset
            + slotBoxHeight(headerfooter::maximumSize, false)
            + slotBoxHeight(headerfooter::maximumSize, true) + progressSpace();
    // Two lines with a line gap above and below them. The sheet and its size
    // sliders fit on one page on a Clara BW only with this short space.
    const int body = 3 * headerFooterMetrics::mapLineGap + headerFooterMetrics::mapLineHeight;
    return { headerFooterMetrics::mapRowHeight * 4,
             qMax(2 * int(headerFooterMetrics::mapRowHeight), strips + body)
                     + 2 * int(metrics::control::border) };
}

void SlotMap::resizeEvent(QResizeEvent *)
{
    const int border = metrics::control::border;
    const int inner = width() - 2 * border;
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const int column = slot % 3;
        const int left = border + inner * column / 3, right = border + inner * (column + 1) / 3;
        const bool footer = headerfooter::footerSlot(slot);
        const int rowHeight = headerFooterMetrics::mapEdgeInset
                + slotBoxHeight(headerfooter::maximumSize, footer) + (footer ? progressSpace() : 0);
        const int top = footer ? height() - border - rowHeight : border;
        cells[slot]->setGeometry(left, top, right - left, rowHeight);
    }
}

void SlotMap::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto ink = palette().color(QPalette::WindowText);
    const auto lines = palette().color(QPalette::Midlight);
    const int border = metrics::control::border;
    painter.fillRect(rect(), palette().color(QPalette::Window));
    ui::paintEdges(painter, rect(),
                   ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge, ink,
                   border);
    const int inner = width() - 2 * border;
    // Two lines of text, the last one shorter, stand for the page between
    // the header and footer slots.
    const int lineLeft = border + headerFooterMetrics::mapLineInset,
              lineWidth = inner - 2 * headerFooterMetrics::mapLineInset;
    const int headerBottom =
            border + headerFooterMetrics::mapEdgeInset + slotBoxHeight(layout.headerSize, false);
    const int footerTop = height() - border - headerFooterMetrics::mapEdgeInset
            - (layout.progressBar ? progressSpace() : 0) - slotBoxHeight(layout.footerSize, true);
    const int first = (headerBottom + footerTop - headerFooterMetrics::mapLineGap
                       - headerFooterMetrics::mapLineHeight)
            / 2;
    for (int line = 0; line < 2; ++line) {
        painter.fillRect(lineLeft, first + line * headerFooterMetrics::mapLineGap,
                         line == 1 ? lineWidth * 3 / 4 : lineWidth,
                         headerFooterMetrics::mapLineHeight, lines);
    }
    if (layout.progressBar) {
        // Where the page draws it: above the bottom edge, across the width.
        const int y = height() - border - progressSpace();
        painter.fillRect(border, y, inner, metrics::reading::progressFillHeight, lines);
        painter.fillRect(border, y, inner / 30, metrics::reading::progressFillHeight, ink);
    }
}

PresetCard::PresetCard(Preset preset, QWidget *parent)
    : ui::Button(parent), value(preset), miniature(headerfooter::presetLayout(preset))
{
    setObjectName("rrHeaderFooterPreset_" + headerfooter::presetValue(preset));
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(headerfooter::presetLabel(preset));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void PresetCard::setLayout(const Layout &layout)
{
    if (miniature == layout)
        return;
    miniature = layout;
    update();
}

void PresetCard::setDivider(bool shown)
{
    if (divider == shown)
        return;
    divider = shown;
    update();
}

void PresetCard::setModified(bool value)
{
    if (modified == value)
        return;
    modified = value;
    setAccessibleName(modified ? strings::get("header_footer.preset.modified_name")
                                         .arg(headerfooter::presetLabel(this->value))
                               : headerfooter::presetLabel(this->value));
    update();
}

QSize PresetCard::sizeHint() const
{
    const int pageHeight = int(headerFooterMetrics::miniatureWidth) * 4 / 3;
    return { int(headerFooterMetrics::miniatureWidth),
             headerFooterMetrics::miniatureTop + pageHeight + headerFooterMetrics::nameGap
                     + headerFooterMetrics::nameHeight };
}

QSize PresetCard::minimumSizeHint() const
{
    return { int(headerFooterMetrics::shortDash) * 4, sizeHint().height() };
}

void PresetCard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const int namePadding = qMin(int(headerFooterMetrics::namePadding), qMax(1, width() / 16));
    const auto ink = palette().color(QPalette::WindowText),
               paper = palette().color(QPalette::Window);
    // A held card already looks selected, so the press shows where the choice lands.
    const bool selected = isChecked() || isDown();
    painter.fillRect(rect(), paper);
    if (divider)
        painter.fillRect(width() - metrics::control::border, headerFooterMetrics::miniatureTop,
                         metrics::control::border, height() - headerFooterMetrics::miniatureTop,
                         ui::dividerColor(this));
    // The six-inch sheet can shorten the cards to fit its controls. Keep the
    // name visible by shrinking the page in both dimensions.
    const int pageRoom =
            qMax(0,
                 height() - headerFooterMetrics::miniatureTop - headerFooterMetrics::nameGap
                         - headerFooterMetrics::nameHeight);
    const int pageWidth =
            qMax(0,
                 qMin(qMin(int(headerFooterMetrics::miniatureWidth), width() - 2 * namePadding),
                      pageRoom * 3 / 4));
    const int pageHeight = pageWidth * 4 / 3;
    const QRect page((width() - pageWidth) / 2, headerFooterMetrics::miniatureTop, pageWidth,
                     pageHeight);
    const int edge =
            selected ? int(headerFooterMetrics::selectedBorder) : int(metrics::control::border);
    ui::paintEdges(painter, page,
                   ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                   selected ? ink : ui::dividerColor(this), edge);
    const int inset = pageWidth / 10;
    const int top = page.y() + edge + pageHeight / 18,
              bottom = page.bottom() - edge - pageHeight / 14;
    // Each slot is a dash where the strip would show it.
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const bool footer = headerfooter::footerSlot(slot);
        if (!(footer ? miniature.footerEnabled : miniature.headerEnabled))
            continue;
        const int dash = qMin(dashWidth(miniature.slotFields[slot]), pageWidth / 3 - inset / 2);
        if (!dash)
            continue;
        const int column = slot % 3;
        const int x = column == 0 ? page.x() + inset
                : column == 1     ? page.x() + (pageWidth - dash) / 2
                                  : page.right() + 1 - inset - dash;
        painter.fillRect(x, (footer ? bottom : top) - headerFooterMetrics::dashHeight / 2, dash,
                         headerFooterMetrics::dashHeight, ink);
    }
    const auto lines = palette().color(QPalette::Midlight);
    const int lineTop = top + pageHeight / 9, lineGap = pageHeight / 11;
    for (int line = 0; line < 6; ++line)
        painter.fillRect(page.x() + inset, lineTop + line * lineGap,
                         line == 5 ? (pageWidth - 2 * inset) / 2 : pageWidth - 2 * inset,
                         metrics::control::border, lines);
    if (miniature.progressBar)
        painter.fillRect(page.x() + edge,
                         page.bottom() + 1 - edge - headerFooterMetrics::dashHeight / 2,
                         pageWidth / 12, headerFooterMetrics::dashHeight / 2, ink);
    const QFont label = ui::interfaceFont(metrics::text::secondary);
    painter.setFont(label);
    auto name = modified
            ? strings::get("header_footer.preset.modified").arg(headerfooter::presetLabel(value))
            : headerfooter::presetLabel(value);
    const int room = qMax(0, width() - 2 * namePadding);
    if (ui::interfaceTextWidth(name, label) > room)
        name = ui::elideInterfaceText(name, label, room);
    const int textWidth = ui::interfaceTextWidth(name, label);
    const int nameWidth = textWidth + 2 * namePadding;
    const QRect block((width() - nameWidth) / 2, page.bottom() + 1 + headerFooterMetrics::nameGap,
                      nameWidth, headerFooterMetrics::nameHeight);
    if (selected)
        painter.fillRect(block, ink);
    painter.setPen(selected ? paper : ink);
    const QRect text(block.x() + namePadding, block.y(), textWidth, block.height());
    ui::drawInterfaceText(painter, text, name, Qt::AlignLeft | Qt::AlignVCenter);
}

namespace {
// The bar that takes the place of the header's action row, as in Chapters:
// a cell on the left, a serif title and an optional count on the right.
void arrangeBar(QWidget *owner, QWidget *cell, QLabel *title, QLabel *count)
{
    const int barHeight = metrics::chrome::actionHeight - metrics::chrome::heavyRule;
    const int titleLeft = metrics::chrome::cellWidth + metrics::chrome::inset;
    cell->setGeometry(0, 0, metrics::chrome::cellWidth, barHeight);
    const int titleRight = count ? owner->width() / 2 : owner->width() - metrics::chrome::inset;
    title->setGeometry(titleLeft, 0, qMax(1, titleRight - titleLeft), barHeight);
    if (count)
        count->setGeometry(owner->width() / 2, 0,
                           qMax(1, owner->width() / 2 - metrics::chrome::inset), barHeight);
}

QLabel *barTitle(QWidget *parent, const char *name)
{
    auto *title = new ui::InterfaceLabel(parent);
    title->setObjectName(name);
    title->setTextFormat(Qt::PlainText);
    ui::readerStyle(title,
                    ui::styles::brand(metrics::chrome::titleText) + "background:transparent;");
    return title;
}

void paintBarRule(QWidget *owner, int height)
{
    QPainter painter(owner);
    painter.fillRect(0, height - metrics::chrome::heavyRule, owner->width(),
                     metrics::chrome::heavyRule, owner->palette().color(QPalette::WindowText));
}
} // namespace

HeaderFooterView::HeaderFooterView(QWidget *parent) : ui::Panel(parent, Qt::TopEdge)
{
    setObjectName("rrHeaderFooterView");
    // The same heavy rule, bold title and close cell as the Font sheet.
    setEdgeWidth(metrics::chrome::heavyRule);
    title = new ui::InterfaceLabel(strings::get("header_footer.customize_title"), this);
    title->setObjectName("rrHeaderFooterTitle");
    title->setWordWrap(true);
    ui::readerStyle(title, ui::styles::font(metrics::text::body, true) + "background:transparent;");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrHeaderFooterClose");
    close->setRule(Qt::LeftEdge);
    close->setAccessibleName(strings::get("header_footer.close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (closed)
            closed();
    });
    clip = new QWidget(this);
    clip->setObjectName("rrHeaderFooterClip");
    content = new QWidget(clip);
    pager = new ui::PagerBar(this, "rrHeaderFooter");
    connect(pager->previous, &QPushButton::clicked, this,
            [this] { showPage(pager->turnTarget(-1)); });
    connect(pager->next, &QPushButton::clicked, this, [this] { showPage(pager->turnTarget(1)); });
    contentRows = new QVBoxLayout(content);
    // Room under the title's divider and below the last row.
    QMargins margins = metrics::panel::contentMargins;
    margins.setBottom(metrics::spacing::section);
    contentRows->setContentsMargins(margins);
    hide();
}

int HeaderFooterView::headingHeight(int width) const
{
    const int cell = qMin(int(metrics::choices::pickerHeader), width / 4);
    return qMax(int(metrics::choices::pickerHeader),
                title->heightForWidth(qMax(1, width - cell - metrics::chrome::inset))
                        + 2 * int(metrics::control::border));
}

int HeaderFooterView::preferredHeight(int width) const
{
    const int rows = contentRows->hasHeightForWidth() ? contentRows->totalHeightForWidth(width)
                                                      : contentRows->totalSizeHint().height();
    return headingHeight(width) + rows;
}

void HeaderFooterView::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    // A divider separates the title row from the presets.
    QPainter painter(this);
    painter.fillRect(0, headingHeight(width()) - metrics::control::border, width(),
                     metrics::control::border, ui::dividerColor(this));
}

void HeaderFooterView::resizeEvent(QResizeEvent *)
{
    const int header = headingHeight(width());
    const int cell = qMin(int(metrics::choices::pickerHeader), width() / 4);
    title->setGeometry(metrics::chrome::inset, 1, qMax(0, width() - cell - metrics::chrome::inset),
                       header - 2);
    // The cell stops above the divider, which runs the full width.
    close->setGeometry(width() - cell, metrics::chrome::heavyRule, cell,
                       header - metrics::chrome::heavyRule - int(metrics::control::border));
    // Lay out at the natural height before splitting on complete row boundaries.
    // Squeezing this layout makes the slot targets overlap in landscape.
    const int natural = preferredHeight(width()) - header;
    const bool overflow = natural > height() - header;
    const int available =
            qMax(1, height() - header - (overflow ? int(metrics::chrome::actionHeight) : 0));
    content->resize(width(), natural);
    contentRows->setGeometry(content->rect());
    pages.clear();
    int start = 0;
    for (int index = 0; overflow && index < contentRows->count(); ++index) {
        auto *item = contentRows->itemAt(index);
        if (item->isEmpty())
            continue;
        const auto bounds = item->geometry();
        if (bounds.bottom() + 1 - start > available && bounds.top() > start) {
            pages.append({ start, bounds.top() });
            start = bounds.top();
        }
    }
    pages.append({ start, natural });
    pager->setGeometry(0, height() - metrics::chrome::actionHeight, width(),
                       metrics::chrome::actionHeight);
    clip->setGeometry(0, header, width(), available);
    showPage(currentPage);
}

void HeaderFooterView::showPage(int page)
{
    if (pages.isEmpty())
        return;
    currentPage = qBound(0, page, int(pages.size()) - 1);
    const auto range = pages[currentPage];
    clip->setFixedHeight(
            qMax(1,
                 qMin(height() - headingHeight(width())
                              - (pages.size() > 1 ? int(metrics::chrome::actionHeight) : 0),
                      range.second - range.first)));
    content->move(0, -range.first);
    pager->setPage(currentPage, pages.size());
    pager->setBlankArea({ metrics::chrome::inset, clip->geometry().bottom() + 1,
                          width() - 2 * metrics::chrome::inset,
                          qMax(0, pager->y() - clip->geometry().bottom() - 1) });
}

FieldChooser::FieldChooser(QWidget *parent) : ui::Panel(parent)
{
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
    look.indicatorSize = QSize(metrics::chrome::checkSize, metrics::chrome::checkSize);
    look.labelPixels = metrics::chrome::labelText;
    look.minimumRowHeight = metrics::chrome::actionHeight - metrics::chrome::heavyRule;
    look.horizontalInset = metrics::chrome::inset;
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
    list->setMinimumRowHeight(headerFooterMetrics::fieldRowHeight);
    // Only the described fields need the room for a second line.
    list->setUniformRows(false);
    for (const auto field : headerfooter::choosableFields)
        list->add("rrHeaderFooterField_" + headerfooter::fieldValue(field),
                  headerfooter::fieldLabel(field), headerfooter::fieldDescription(field));
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

void FieldChooser::open(int slot, Field current, const headerfooter::Values &values, bool uppercase,
                        int textPixels)
{
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
        if (textPixels > 0) {
            style.sampleFont = ui::headerFooterFont(textPixels);
            style.sampleWeight = ui::interfaceWeight() + ui::headerFooterAddedWeight();
        }
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

QVector<int> FieldChooser::pageStarts() const
{
    // Fill each page with the rows that fit, at their own heights. Leave room
    // for the pager only when the list needs more than one page.
    const int listTop = barHeight() + metrics::list::headerHeight;
    const auto starts = [this](int available) {
        QVector<int> result{ 0 };
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
    return single.size() > 1 ? starts(height() - listTop - metrics::chrome::actionHeight) : single;
}

void FieldChooser::arrange()
{
    const auto starts = pageStarts();
    const int pages = starts.size();
    if (openRow >= 0
        && height()
                > barHeight() + metrics::list::headerHeight + headerFooterMetrics::fieldRowHeight) {
        page = 0;
        while (page + 1 < pages && starts[page + 1] <= openRow)
            ++page;
        openRow = -1;
    }
    page = qBound(0, page, pages - 1);
    pager->setPage(page, pages);
    const int listTop = barHeight() + metrics::list::headerHeight;
    const int first = starts[page];
    const int count = (page + 1 < pages ? starts[page + 1] : int(list->options().size())) - first;
    list->showRows(first, count);
    list->setGeometry(0, listTop, width(), list->heightForWidth(width()));
    const int used = list->geometry().bottom() + 1;
    pager->setBlankArea({ metrics::chrome::inset, used, width() - 2 * metrics::chrome::inset,
                          pager->y() - used });
}

void FieldChooser::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    paintBarRule(this, barHeight());
    if (barHeight() > metrics::chrome::actionHeight) {
        QPainter painter(this);
        painter.fillRect(0, int(metrics::chrome::actionHeight) - int(metrics::control::border),
                         width(), metrics::control::border, ui::dividerColor(this));
    }
}

int FieldChooser::barHeight() const
{
    const int box =
            ui::interfaceTextWidth(uppercase->text(), ui::interfaceFont(metrics::chrome::labelText))
            + metrics::settings::toggleGap + metrics::chrome::checkSize
            + 2 * metrics::chrome::inset;
    const bool stacked = ui::interfaceTextWidth(title->text(), title->font()) + box
                    + metrics::chrome::cellWidth + metrics::chrome::inset
            > width();
    return int(metrics::chrome::actionHeight) * (stacked ? 2 : 1);
}

void FieldChooser::resizeEvent(QResizeEvent *)
{
    for (auto *row : list->options()) {
        auto look = row->presentation();
        look.valueBelowLabel = width() < qRound(ui::px(850) * ui::textScale());
        row->setPresentation(look);
    }
    // The checkbox takes what its label and box need; the title has the rest.
    const int barHeight = metrics::chrome::actionHeight - metrics::chrome::heavyRule;
    const int boxWidth =
            ui::interfaceTextWidth(uppercase->text(), ui::interfaceFont(metrics::chrome::labelText))
            + metrics::settings::toggleGap + metrics::chrome::checkSize
            + 2 * metrics::chrome::inset;
    const bool stacked = this->barHeight() > metrics::chrome::actionHeight;
    uppercase->setGeometry(width() - boxWidth, stacked ? int(metrics::chrome::actionHeight) : 0,
                           boxWidth, barHeight);
    arrangeBar(this, finish, title, nullptr);
    if (!stacked)
        title->setGeometry(title->x(), 0, qMax(1, uppercase->x() - title->x()), barHeight);
    heading->setGeometry(0, this->barHeight(), width(), metrics::list::headerHeight);
    // The pager keeps the bottom, as in Chapters.
    pager->setGeometry(0, height() - metrics::chrome::actionHeight, width(),
                       metrics::chrome::actionHeight);
    arrange();
}
} // namespace kobalt::reader
