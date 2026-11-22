#include "header_footer_panel.h"
#include "app/reader/views/reading_page.h"
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
constexpr auto stripDigits = ui::TextDigits::Tabular;

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
            : halfSpaces           ? qCeil(ui::halfSpacedWidth(textFont, text, stripDigits))
                                   : ui::interfaceTextWidth(text, textFont, stripDigits);
    if (textWidth > room) {
        halfSpaces = false;
        text = ui::headerFooterText(content, textFont, room);
        textWidth = ui::interfaceTextWidth(text, textFont, stripDigits);
    }
    const int total = textWidth + gap + icon;
    const int x = align & Qt::AlignLeft ? bounds.x()
            : align & Qt::AlignRight    ? bounds.right() + 1 - total
                                        : bounds.x() + (bounds.width() - total) / 2;
    if (!text.isEmpty() && content.batteryIcon)
        ui::drawOpticalPrivateText(painter, QRect(x, bounds.y(), textWidth, bounds.height()), text,
                                   Qt::AlignLeft, ui::headerFooterTextWeight(), {}, stripDigits);
    else if (!text.isEmpty() && halfSpaces)
        ui::drawHalfSpaced(painter, x, bounds, text, ui::headerFooterTextWeight(), stripDigits);
    else if (!text.isEmpty())
        ui::drawInterfaceText(painter, QRect(x, bounds.y(), textWidth, bounds.height()), text,
                              Qt::AlignLeft | Qt::AlignVCenter, ui::headerFooterTextWeight(), {},
                              stripDigits);
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
// What the page's progress bar keeps between its edge and the strip's text.
int progressSpace()
{
    return metrics::reading::progressHeight;
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

    void setContent(Field shownField, const Content &next, headerfooter::BarEdge bar,
                    int textPixels, int strip)
    {
        if (field == shownField && content == next && progressBar == bar && pixels == textPixels
            && stripHeight == strip)
            return;
        field = shownField;
        content = next;
        progressBar = bar;
        pixels = textPixels;
        stripHeight = strip;
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
        // The strip's own font and size, so Small, Medium and Large show as on the page.
        painter.setFont(ui::headerFooterFont(pixels));
        // Header slots keep the top edge and footer slots the bottom edge,
        // inside the progress bar when it is on their edge, as on the page.
        const bool footer = headerfooter::footerSlot(slot);
        const auto barEdge = footer ? headerfooter::BarEdge::Bottom : headerfooter::BarEdge::Top;
        const int bar = progressBar == barEdge ? progressSpace() : 0;
        const int stripTop = footer ? height() - bar - stripHeight : bar;
        const int boxInset = qMax(1, int(metrics::control::border) / 2);
        const int column = slot % 3;
        const int sideGap = 2 * metrics::control::border;
        const QRect box(sideGap, stripTop + boxInset, width() - 2 * sideGap,
                        stripHeight - 2 * boxInset);
        if (pressed)
            painter.fillRect(box, paper);
        else
            paintDashes(painter, box, ui::dividerColor(this));
        const int inset = metrics::list::lineInset;
        const int textLeft = column == 0 ? int(metrics::reading::headerSide) : inset;
        const int textRight = column == 2 ? int(metrics::reading::headerSide) : inset;
        const QRect bounds(textLeft, stripTop, qMax(0, width() - textLeft - textRight),
                           stripHeight);
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
    bool editable;
    headerfooter::BarEdge progressBar = headerfooter::BarEdge::None;
    int pixels = 0, stripHeight = 0;
    Field field = Field::Nothing;
    Content content;
};

SlotMap::SlotMap(QWidget *parent, bool editable) : QWidget(parent)
{
    setObjectName(editable ? "rrHeaderFooterCustomMap" : "rrHeaderFooterPresetMap");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
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
    layout = next;
    for (int slot = 0; slot < headerfooter::slotCount; ++slot)
        setSlot(slot, contents[slot]);
    update();
}

void SlotMap::setBodyLayout(const ReadingPage::ReflowLayout &body)
{
    if (bodyLayout.margins == body.margins && bodyLayout.fontPixels == body.fontPixels
        && bodyLayout.lineSpacing == body.lineSpacing && bodyLayout.columns == body.columns
        && bodyLayout.columnSpacing == body.columnSpacing)
        return;
    bodyLayout = body;
    update();
}

void SlotMap::setProgress(qreal fraction, const QVector<double> &chapterTicks)
{
    fraction = qBound(0.0, fraction, 1.0);
    if (qFuzzyCompare(1 + progress, 1 + fraction) && ticks == chapterTicks)
        return;
    progress = fraction;
    ticks = chapterTicks;
    if (layout.progressBar)
        update();
}

void SlotMap::setSlot(int slot, const Content &content)
{
    const bool footer = headerfooter::footerSlot(slot);
    const int size = footer ? layout.footerSize : layout.headerSize;
    const bool enabled = footer ? layout.footerEnabled : layout.headerEnabled;
    cells[slot]->setVisible(enabled);
    if (!enabled)
        return;
    cells[slot]->setContent(layout.slotFields[slot], content, headerfooter::progressEdge(layout),
                            metrics::reading::textPixels(size, footer),
                            metrics::reading::stripHeight(size, layout.stripPadding));
}

void SlotMap::updateBattery(const headerfooter::Values &values)
{
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const auto field = layout.slotFields[slot];
        if (headerfooter::batteryField(field))
            setSlot(slot, headerfooter::content(field, values, layout.uppercase[slot]));
    }
}

QSize SlotMap::sizeHint() const
{
    // Room for the largest strips and the bar, whatever the layout shows, so
    // the sheet keeps its height when a preset or size changes. Smaller
    // strips leave more page between them, as they do on the reader.
    const int strips = 2
                    * metrics::reading::stripHeight(headerfooter::maximumSize,
                                                    headerfooter::maximumExtraHeight)
            + progressSpace();
    const int body = headerFooterMetrics::minimumBodyHeight;
    return { headerFooterMetrics::mapRowHeight * 4,
             qMax(2 * int(headerFooterMetrics::mapRowHeight), strips + body)
                     + 2 * int(metrics::control::border) };
}

void SlotMap::resizeEvent(QResizeEvent *)
{
    for (int slot = 0; slot < headerfooter::slotCount; ++slot) {
        const int column = slot % 3;
        const int left = width() * column / 3, right = width() * (column + 1) / 3;
        const bool footer = headerfooter::footerSlot(slot);
        // Either row leaves room for the bar, which can be on either edge.
        const int rowHeight = qMin(height() / 2,
                                   metrics::reading::stripHeight(headerfooter::maximumSize,
                                                                 headerfooter::maximumExtraHeight)
                                           + progressSpace() + metrics::spacing::row);
        const int top = footer ? height() - rowHeight : 0;
        cells[slot]->setGeometry(left, top, right - left, rowHeight);
    }
}

void SlotMap::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto ink = palette().color(QPalette::WindowText);
    const int border = metrics::control::border;
    painter.fillRect(rect(), palette().color(QPalette::Window));
    const auto bar = headerfooter::progressEdge(layout);
    const int topBar = bar == headerfooter::BarEdge::Top ? progressSpace() : 0;
    const int bottomBar = bar == headerfooter::BarEdge::Bottom ? progressSpace() : 0;
    const int headerStrip = layout.headerEnabled
            ? metrics::reading::stripHeight(layout.headerSize, layout.stripPadding)
            : 0;
    const int footerStrip = layout.footerEnabled
            ? metrics::reading::stripHeight(layout.footerSize, layout.stripPadding)
            : 0;
    const int header = topBar + (layout.header() ? headerStrip : 0);
    const int footer = bottomBar + (layout.footer() ? footerStrip : 0);
    const auto bandPaper = ui::themeColors(this).overviewPaper;
    painter.fillRect(0, 0, width(), header, bandPaper);
    painter.fillRect(0, height() - footer, width(), footer, bandPaper);
    ui::paintEdges(painter, rect(), ui::Edges(Qt::TopEdge) | Qt::BottomEdge, ink, border);
    painter.setPen(QPen(ui::dividerColor(this), border, Qt::DotLine, Qt::FlatCap));
    if (header)
        painter.drawLine(0, header - border / 2, width(), header - border / 2);
    if (footer)
        painter.drawLine(0, height() - footer + border / 2, width(),
                         height() - footer + border / 2);

    // Enabled strips reserve space for their buttons even when all three slots are empty.
    const int topBand = topBar + headerStrip, bottomBand = bottomBar + footerStrip;
    // Compress the page margins when the short preview would otherwise lose its text.
    const int marginRoom =
            qMax(0, height() - topBand - bottomBand - headerFooterMetrics::minimumBodyHeight);
    const auto &margins = bodyLayout.margins;
    const int marginSum = margins.top() + margins.bottom();
    const int topMargin =
            marginSum > marginRoom ? marginRoom * margins.top() / marginSum : margins.top();
    const int bottomMargin = marginSum > marginRoom ? marginRoom - topMargin : margins.bottom();
    const int top = topBand + topMargin, bottom = height() - bottomBand - bottomMargin;
    const int columns = qBound(1, bodyLayout.columns, 2);
    const int gap = bodyLayout.columnSpacing;
    const int span =
            (width() - bodyLayout.margins.left() - bodyLayout.margins.right() - (columns - 1) * gap)
            / columns;
    const int pitch = qMax(1, bodyLayout.lineHeight() / 2);
    const int thickness = qMax(border, bodyLayout.fontPixels / 10);
    const int rows = qBound(1, (bottom - top + pitch / 2) / pitch, 6);
    if (span > 0 && bottom - top >= thickness) {
        const auto lines = ui::sectionColor(this);
        const int step = (bottom - top) / rows;
        for (int column = 0; column < columns; ++column) {
            const int left = bodyLayout.margins.left() + column * (span + gap);
            for (int row = 0; row < rows; ++row)
                painter.fillRect(left, top + row * step + (step - thickness) / 2,
                                 row == rows - 1 ? span / 2 : span, thickness, lines);
        }
    }
    if (bar != headerfooter::BarEdge::None) {
        // Where the page draws it, inside the top or bottom edge.
        const bool atTop = bar == headerfooter::BarEdge::Top;
        const int edge = atTop ? border + int(metrics::reading::progressRuleEdge)
                               : height() - border - int(metrics::reading::progressRuleEdge);
        paintProgressBar(painter, 0, width(), edge, atTop, progress,
                         layout.progressTicks ? ticks : QVector<double>{},
                         { ink, ui::themeColors(this).outline, bandPaper });
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
    const auto lines = ui::sectionColor(this);
    const int lineTop = top + pageHeight / 9, lineGap = pageHeight / 11;
    for (int line = 0; line < 6; ++line)
        painter.fillRect(page.x() + inset, lineTop + line * lineGap,
                         line == 5 ? (pageWidth - 2 * inset) / 2 : pageWidth - 2 * inset,
                         metrics::control::border, lines);
    if (miniature.progressBar)
        painter.fillRect(page.x() + edge,
                         miniature.progressBarTop
                                 ? page.y() + edge
                                 : page.bottom() + 1 - edge - headerFooterMetrics::dashHeight / 2,
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
// a serif title and the close cell on the right.
void arrangeBar(QWidget *owner, QWidget *cell, QLabel *title)
{
    const int barHeight = metrics::chrome::actionHeight - metrics::chrome::heavyRule;
    const int cellLeft = owner->width() - metrics::chrome::cellWidth;
    cell->setGeometry(cellLeft, 0, metrics::chrome::cellWidth, barHeight);
    title->setGeometry(metrics::chrome::inset, 0,
                       qMax(1, cellLeft - 2 * int(metrics::chrome::inset)), barHeight);
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

HeaderFooterView::HeaderFooterView(QWidget *parent) : ui::Panel(parent)
{
    setObjectName("rrHeaderFooterView");
    heading = new ui::Panel(this, Qt::BottomEdge);
    heading->setEdgeWidth(metrics::chrome::heavyRule);
    title = barTitle(heading, "rrHeaderFooterTitle");
    title->setText(strings::get("header_footer.customize_title"));
    title->setWordWrap(true);
    fontButton = new ui::CellButton(ui::ControlIcon::Typography, heading);
    fontButton->setObjectName("rrStripFontPicker");
    fontButton->setAccessibleName(strings::get("settings.header_footer_font"));
    fontButton->setRule(Qt::LeftEdge);
    close = new ui::CellButton(ui::ControlIcon::Close, heading);
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
    // Match the visible top inset, including the space above the preset miniatures.
    QMargins margins = metrics::panel::contentMargins;
    margins.setBottom(margins.top() + headerFooterMetrics::miniatureTop);
    contentRows->setContentsMargins(margins);
    hide();
}

int HeaderFooterView::headingHeight(int width) const
{
    const int titleWidth =
            qMax(1, width - 2 * metrics::chrome::cellWidth - 2 * metrics::chrome::inset);
    return qMax(int(metrics::chrome::actionHeight),
                title->heightForWidth(titleWidth) + metrics::chrome::heavyRule);
}

int HeaderFooterView::preferredHeight(int width) const
{
    const int rows = contentRows->hasHeightForWidth() ? contentRows->totalHeightForWidth(width)
                                                      : contentRows->totalSizeHint().height();
    return headingHeight(width) + rows;
}

void HeaderFooterView::resizeEvent(QResizeEvent *)
{
    const int header = headingHeight(width());
    const int cell = metrics::chrome::cellWidth;
    const int controlsHeight = header - metrics::chrome::heavyRule;
    heading->setGeometry(0, 0, width(), header);
    title->setGeometry(metrics::chrome::inset, 0,
                       qMax(1, width() - 2 * cell - 2 * metrics::chrome::inset), controlsHeight);
    fontButton->setGeometry(width() - 2 * cell, 0, cell, controlsHeight);
    close->setGeometry(width() - cell, 0, cell, controlsHeight);
    // Lay out at the natural height before splitting on complete row boundaries.
    // Squeezing this layout makes the slot targets overlap in landscape.
    const int natural = preferredHeight(width()) - header;
    const bool overflow = natural > height() - header;
    const int available =
            qMax(1, height() - header - (overflow ? int(metrics::chrome::actionHeight) : 0));
    content->resize(width(), qMax(natural, available));
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
    pages.append({ start, content->height() });
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
    close->setRule(Qt::LeftEdge);
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

namespace {
void setFieldValue(ui::DescribedOption *row, Field field, const headerfooter::Values &values,
                   bool uppercase, int textPixels)
{
    auto shown = headerfooter::content(field, values, uppercase);
    // Before the first turn the page turn time shows a typical value.
    if (field == Field::PageTurnTime && values.pageTurnMilliseconds < 0) {
        auto sample = values;
        sample.pageTurnMilliseconds = headerfooter::samplePageTurnMilliseconds;
        shown = headerfooter::content(field, sample, uppercase);
    }
    // Each row shows what the slot would: text, a battery, or both.
    if (shown.text.isEmpty() && !shown.batteryIcon)
        shown.text = strings::get("header_footer.unavailable");
    ui::DescribedOption::ValueStyle style;
    style.battery = shown.batteryIcon;
    style.percent = shown.battery;
    style.charging = shown.charging;
    style.halfSpaces = headerfooter::pageCount(field);
    style.digits = stripDigits;
    if (textPixels > 0) {
        style.sampleFont = ui::headerFooterFont(textPixels);
        style.sampleWeight = ui::interfaceWeight() + ui::headerFooterAddedWeight();
    }
    row->setValue(shown.text, style);
}
} // namespace

void FieldChooser::updateBattery(const headerfooter::Values &values, int textPixels)
{
    const auto &rows = list->options();
    for (int index = 0; index < rows.size(); ++index) {
        const auto field = headerfooter::choosableFields[index];
        if (headerfooter::batteryField(field))
            setFieldValue(rows[index], field, values, uppercase->isChecked(), textPixels);
    }
}

void FieldChooser::open(int slot, Field current, const headerfooter::Values &values, bool uppercase,
                        int textPixels)
{
    currentSlot = slot;
    title->setText(strings::get("header_footer.customize_slot").arg(headerfooter::slotLabel(slot)));
    const auto &rows = list->options();
    for (int index = 0; index < rows.size(); ++index) {
        const auto field = headerfooter::choosableFields[index];
        list->setLeftOut(index, headerfooter::debugField(field) && !debug && field != current);
        setFieldValue(rows[index], field, values, uppercase, textPixels);
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
    uppercase->setGeometry(width() - (stacked ? 0 : int(metrics::chrome::cellWidth)) - boxWidth,
                           stacked ? int(metrics::chrome::actionHeight) : 0, boxWidth, barHeight);
    arrangeBar(this, finish, title);
    if (!stacked)
        title->setGeometry(title->x(), 0, qMax(1, uppercase->x() - title->x()), barHeight);
    heading->setGeometry(0, this->barHeight(), width(), metrics::list::headerHeight);
    // The pager keeps the bottom, as in Chapters.
    pager->setGeometry(0, height() - metrics::chrome::actionHeight, width(),
                       metrics::chrome::actionHeight);
    arrange();
}
} // namespace kobalt::reader
