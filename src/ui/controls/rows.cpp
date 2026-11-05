#include "ui/controls/text.h"
#include "rows.h"
#include "support/qt_font.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "support/strings.h"
#include <QPainter>
#include <QResizeEvent>
#include <QGridLayout>
#include "text.h"
#include <QVariant>
#include <utility>

namespace kobalt::ui {
namespace {
class RowTitle final : public PrivateTextLabel
{
public:
    using PrivateTextLabel::PrivateTextLabel;
    ControlIcon icon{};
    bool hasIcon = false;
    bool optical = false;

    int iconInset() const
    {
        return hasIcon ? fontMetrics().height() + qMax(0, int(metrics::chrome::selectorGap) - px(5))
                       : 0;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (!optical) {
            PrivateTextLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        painter.setFont(font());
        const auto ink = palette().color(QPalette::WindowText);
        painter.setPen(ink);
        const int side = fontMetrics().height();
        const int center = alignment().testFlag(Qt::AlignBottom)
                ? height() - interfaceLineHeight(font()) / 2
                : height() / 2;
        if (hasIcon)
            controlIcon(icon, ink).paint(&painter, QRect(0, center - side / 2, side, side));
        const int start = iconInset();
        drawOpticalText(painter, QRect(start, center - side / 2, qMax(0, width() - start), side),
                        text());
    }
};
} // namespace

ListRow::ListRow(QWidget *parent) : Button(parent)
{
    readerStyle(this, "QPushButton{border:none;padding:0;min-height:0;background:transparent;}");
    title = new RowTitle(this);
    detail = new PrivateTextLabel(this);
    caption = new PrivateTextLabel(this);
    leading = new PrivateTextLabel(this);
    trailing = new PrivateTextLabel(this);
    for (auto *label : { title, detail, caption, leading, trailing }) {
        label->setTextFormat(Qt::PlainText);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setStyleSheet("background:transparent;border:none;");
    }
    readerStyle(title,
                styles::font(metrics::chrome::labelText) + "background:transparent;border:none;");
    readerStyle(detail,
                styles::font(metrics::chrome::descriptionText)
                        + "background:transparent;border:none;");
    ui::captionLabel(caption);
    ui::captionLabel(leading);
    ui::captionLabel(trailing);
    int index = 0;
    for (auto *label : { title, detail, caption, leading, trailing }) {
        labelStyles[index++] = label->property("reader_style").toString();
        label->installEventFilter(this);
    }
    trailing->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(this, &QPushButton::toggled, this, [this] { syncLabelStyles(); });
    observeParent();
    syncLabelStyles();
    arrange();
}

void ListRow::setTitle(const QString &text)
{
    if (titleText == text)
        return;
    titleText = text;
    setText(text);
    setAccessibleName(text);
    arrange();
}

void ListRow::setTitleIcon(ControlIcon icon, bool visible, bool alignWithoutIcon)
{
    auto *label = static_cast<RowTitle *>(title);
    const bool optical = visible || alignWithoutIcon;
    if (label->icon == icon && label->hasIcon == visible && label->optical == optical)
        return;
    label->icon = icon;
    label->hasIcon = visible;
    label->optical = optical;
    title->update();
    arrange();
    update();
}

void ListRow::setDetailLines(int lines)
{
    lines = qBound(1, lines, 3);
    if (detailLines == lines)
        return;
    detailLines = lines;
    detail->setMaximumLines(lines);
    detail->setWordWrap(lines > 1);
    detail->setAlignment(lines > 1 ? Qt::AlignLeft | Qt::AlignTop
                                   : Qt::AlignLeft | Qt::AlignVCenter);
    arrange();
    updateGeometry();
}

void ListRow::setDetail(const QString &text)
{
    if (detailText == text)
        return;
    detailText = text;
    if (detailText.isEmpty())
        detail->clear();
    arrange();
}

void ListRow::setDetailHighlight(const QString &style, QColor color, int lineSpacing)
{
    detail->setHighlight(style, color, lineSpacing);
    detail->setMaximumLines(detailLines);
    detail->setWordWrap(detailLines > 1 || !style.isEmpty() || lineSpacing > 0);
}

void ListRow::setCaption(const QString &text)
{
    if (captionText == text)
        return;
    captionText = text;
    arrange();
}

void ListRow::setLeading(const QString &text, int indent)
{
    indent = qMax(0, indent);
    if (leadingText == text && indentation == indent)
        return;
    leadingText = text;
    indentation = indent;
    arrange();
}

void ListRow::setLeadingIcon(ControlIcon icon)
{
    if (hasLeadingIcon && leadingIcon == icon)
        return;
    leadingIcon = icon;
    hasLeadingIcon = true;
    setIconConfiguration({ icon, IconRendition::Action });
    arrange();
    update();
}

void ListRow::setAccessory(Accessory value)
{
    if (accessory == value)
        return;
    accessory = std::move(value);
    arrange();
}

void ListRow::setTitlePixels(int pixels)
{
    const QString style = styles::font(pixels) + "background:transparent;border:none;";
    if (labelStyles[0] == style)
        return;
    labelStyles[0] = style;
    syncLabelStyles();
}

void ListRow::setTitleGap(int pixels)
{
    pixels = qMax(0, pixels);
    if (titleGap == pixels)
        return;
    titleGap = pixels;
    arrange();
}

void ListRow::setLabelFont(PrivateTextLabel *label, int index, const QFont &font)
{
    auto family = font.family();
    family.replace('\\', "\\\\");
    family.replace('"', "\\\"");
    const auto style =
            QString("font-family:\"%1\";font-size:%2px;font-weight:400;font-style:normal;background:transparent;border:none;")
                    .arg(family)
                    .arg(font.pixelSize());
    if (labelStyles[index] == style) {
        return;
    }
    label->setFont(font);
    labelStyles[index] = style;
    syncLabelStyles();
}

void ListRow::setDetailFont(const QFont &font)
{
    setLabelFont(detail, 1, font);
}

void ListRow::setMetadataFont(const QFont &font)
{
    setLabelFont(caption, 2, font);
    setLabelFont(leading, 3, font);
    setLabelFont(trailing, 4, font);
}

QPushButton *ListRow::addAction(const QString &name, const QString &accessibleName)
{
    if (!action) {
        auto *cell = new CellButton(ControlIcon::Close, this);
        cell->setGrayPress(true);
        cell->setRule(Edges(Qt::LeftEdge) | Qt::BottomEdge);
        action = cell;
    }
    action->setObjectName(name);
    action->setAccessibleName(accessibleName);
    arrange();
    return action;
}

void ListRow::setActionIcon(ControlIcon icon)
{
    if (action) {
        static_cast<CellButton *>(action)->setGlyph(icon);
    }
}

QSize ListRow::sizeHint() const
{
    const int lines = 1 + (detailText.isEmpty() ? 0 : detailLines) + !captionText.isEmpty();
    if (lines > 3)
        return { 0,
                 int(metrics::list::threeLines) + (lines - 3) * detail->fontMetrics().height()
                         + titleGap };
    return { 0,
             (lines == 3           ? int(metrics::list::threeLines)
                      : lines == 2 ? int(metrics::list::twoLines)
                                   : int(metrics::list::oneLine))
                     + titleGap };
}

void ListRow::resizeEvent(QResizeEvent *)
{
    arrange();
}

void ListRow::moveEvent(QMoveEvent *)
{
    // Sibling actions stay enabled when their row cannot be opened.
    if (action && action->parentWidget() != this)
        action->move(x() + width() - metrics::chrome::cellWidth, y());
}

void ListRow::changeEvent(QEvent *event)
{
    Button::changeEvent(event);
    if (!title)
        return;
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::EnabledChange)
        syncLabelStyles();
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        arrange();
}

void ListRow::checkStateSet()
{
    Button::checkStateSet();
    syncLabelStyles();
}

void ListRow::nextCheckState()
{
    // Qt skips checkStateSet during a click, including when signals are blocked.
    QPointer<ListRow> alive(this);
    Button::nextCheckState();
    if (alive)
        alive->syncLabelStyles();
}

void ListRow::observeParent()
{
    if (observedParent == parentWidget())
        return;
    if (observedParent)
        observedParent->removeEventFilter(this);
    observedParent = parentWidget();
    if (observedParent)
        observedParent->installEventFilter(this);
}

bool ListRow::event(QEvent *event)
{
    QPointer<ListRow> alive(this);
    const bool result = Button::event(event);
    if (!alive)
        return result;
    if (title && event->type() == QEvent::ParentChange) {
        observeParent();
        syncLabelStyles();
        arrange();
    }
    if (title && event->type() == QEvent::DynamicPropertyChange)
        syncLabelStyles();
    return result;
}

bool ListRow::eventFilter(QObject *object, QEvent *event)
{
    if (object == observedParent
        && (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::DynamicPropertyChange))
        syncLabelStyles();
    if (!synchronizing
        && (object == title || object == detail || object == caption || object == leading
            || object == trailing)) {
        if (event->type() == QEvent::PaletteChange)
            syncLabelStyles();
        else if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            arrange();
    }
    return Button::eventFilter(object, event);
}

void ListRow::syncLabelStyles()
{
    if (!title || synchronizing)
        return;
    synchronizing = true;
    const auto colors = parentWidget() ? parentWidget()->palette() : palette();
    const bool current = isChecked();
    const QColor ink = colors.color(current ? QPalette::Window : QPalette::WindowText);
    int index = 0;
    bool changed = false;
    for (auto *label : { title, detail, caption, leading, trailing }) {
        const QColor color = !isEnabled()   ? themeColors(this).disabledInk
                : current || label == title ? ink
                                            : noteColor(this);
        const QString style = labelStyles[index++] + "color:" + color.name() + ';';
        if (label->property("reader_style").toString() != style) {
            readerStyle(label, style);
            changed = true;
        }
        // Theme propagation can reset the palette without changing the
        // stylesheet. Private text reads this role directly.
        const QColor textColor(color.name());
        if (label->palette().color(QPalette::WindowText) != textColor) {
            auto colors = label->palette();
            colors.setColor(QPalette::WindowText, textColor);
            label->setPalette(colors);
            changed = true;
        }
    }
    synchronizing = false;
    if (changed)
        arrange();
}

void ListRow::arrange()
{
    if (!title || arranging || synchronizing)
        return;
    arranging = true;
    const int end = width() - (action ? int(metrics::chrome::cellWidth) : 0) - metrics::list::inset;
    const int lead = hasLeadingIcon ? int(metrics::control::iconSize + metrics::chrome::selectorGap)
            : leadingText.isEmpty() ? 0
                                    : int(metrics::list::leadingWidth);
    const auto *trailingCaption = accessory.caption();
    const int trail = trailingCaption
            ? qMax(trailingCaption->reservedWidth, trailing->textWidth(trailingCaption->text))
            : accessory.kind() == Accessory::Kind::None ? 0
                                                        : int(metrics::chrome::cellIcon);
    const int start = metrics::list::inset + lead + indentation;
    const int textWidth =
            qMax(0, end - start - trail - (trail ? int(metrics::contents::pageGap) : 0));
    const int lines = 1 + (detailText.isEmpty() ? 0 : detailLines) + !captionText.isEmpty();
    const int top = lines == 1 ? 0 : int(metrics::list::lineInset);
    const int lineHeight =
            qMax(1, (height() - 2 * top - titleGap - metrics::control::border) / lines);
    const int titleInset = static_cast<RowTitle *>(title)->iconInset();
    title->setGeometry(start, top, textWidth, lineHeight);
    title->setText(title->elidedText(titleText, qMax(0, textWidth - titleInset)));
    int line = 1;
    detail->setVisible(!detailText.isEmpty());
    caption->setVisible(!captionText.isEmpty());
    for (const auto &item : { qMakePair(detail, detailText), qMakePair(caption, captionText) }) {
        if (item.second.isEmpty())
            continue;
        const int count = item.first == detail ? detailLines : 1;
        item.first->setGeometry(start, top + titleGap + line * lineHeight, qMax(0, end - start),
                                count * lineHeight);
        line += count;
        item.first->setText(count > 1 ? item.second
                                      : item.first->elidedText(item.second, item.first->width()));
    }
    leading->setVisible(!hasLeadingIcon && !leadingText.isEmpty());
    leading->setText(leadingText);
    leading->setGeometry(metrics::list::inset, 0, lead, height() - metrics::control::border);
    trailing->setVisible(trailingCaption != nullptr);
    trailing->setText(trailingCaption ? trailingCaption->text : QString());
    trailing->setGeometry(qMax(start, end - trail), top, qMax(0, qMin(trail, end - start)),
                          lineHeight);
    if (action) {
        const QPoint origin = action->parentWidget() == this ? QPoint() : pos();
        action->setGeometry(origin.x() + width() - metrics::chrome::cellWidth, origin.y(),
                            metrics::chrome::cellWidth, height());
    }
    arranging = false;
    updateGeometry();
    update();
}

void ListRow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto colors = parentWidget() ? parentWidget()->palette() : palette();
    const bool current = isChecked();
    const QColor ink = colors.color(current ? QPalette::Window : QPalette::WindowText);
    QColor paper = colors.color(current ? QPalette::WindowText : QPalette::Window);
    if (!current && isEnabled() && isDown())
        paper = themeColors(this).pressedPaper;
    painter.fillRect(rect(), paper);
    if (hasLeadingIcon) {
        const int side = metrics::control::iconSize;
        painter.save();
        if (!isEnabled())
            painter.setOpacity(.4);
        icon().paint(&painter, QRect(metrics::list::inset, (height() - side) / 2, side, side),
                     Qt::AlignCenter, QIcon::Normal, current ? QIcon::On : QIcon::Off);
        painter.restore();
    }
    const int end = width() - (action ? int(metrics::chrome::cellWidth) : 0) - metrics::list::inset;
    if (accessory.kind() == Accessory::Kind::Chevron
        || accessory.kind() == Accessory::Kind::Check) {
        painter.setRenderHint(QPainter::Antialiasing);
        const int side = metrics::chrome::cellIcon;
        const QRectF area(end - side, (height() - side) / 2.0, side, side);
        if (accessory.kind() == Accessory::Kind::Chevron)
            controlIcon(ControlIcon::Next, ink).paint(&painter, area.toRect());
        else {
            painter.setPen(QPen(ink, px(3), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            QPolygonF check;
            check << area.topLeft() + QPointF(side * .18, side * .5)
                  << area.topLeft() + QPointF(side * .42, side * .75)
                  << area.topLeft() + QPointF(side * .82, side * .22);
            painter.drawPolyline(check);
        }
    }
    painter.fillRect(0, height() - metrics::control::border, width(), metrics::control::border,
                     dividerColor(this));
}

ListHeader::ListHeader(QWidget *parent) : QWidget(parent)
{
    left = new ui::InterfaceLabel(this);
    right = new ui::InterfaceLabel(this);
    ui::captionLabel(left);
    ui::captionLabel(right);
    right->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

void ListHeader::setCaptions(const QString &a, const QString &b)
{
    left->setText(a);
    right->setText(b);
    QResizeEvent event(size(), size());
    resizeEvent(&event);
}

QSize ListHeader::sizeHint() const
{
    return { 0, int(metrics::list::headerHeight) };
}

void ListHeader::setTrailingInset(int pixels)
{
    trailingInset = qMax(0, pixels);
    QResizeEvent event(size(), size());
    resizeEvent(&event);
}

void ListHeader::resizeEvent(QResizeEvent *)
{
    const int available = qMax(0, width() - 2 * metrics::list::inset - trailingInset);
    const int side = right->text().isEmpty()
            ? 0
            : qMin(available, interfaceTextWidth(right->text(), right->font()));
    left->setGeometry(metrics::list::inset, 0,
                      qMax(0, available - side - (side ? int(metrics::list::gap) : 0)),
                      height() - metrics::control::border);
    right->setGeometry(width() - metrics::list::inset - trailingInset - side, 0, side,
                       height() - metrics::control::border);
}

void ListHeader::setShaded(bool value)
{
    if (shaded == value)
        return;
    shaded = value;
    update();
}

void ListHeader::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    // Styles can replace Midlight when a list creates headers after theme replay.
    const auto background = shaded ? sectionColor(this) : palette().color(QPalette::Window);
    painter.fillRect(rect(), background);
    painter.fillRect(0, height() - metrics::control::border, width(), metrics::control::border,
                     dividerColor(this));
}

QHBoxLayout *horizontalRow()
{
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(metrics::control::itemGap);
    return line;
}

namespace {
// QWidgetItem measures a widget through its layout when one is installed.
// Keep that path consistent with ControlRow's width-dependent height.
class ControlGrid final : public QGridLayout
{
public:
    explicit ControlGrid(ControlRow *row) : QGridLayout(row), row(row) { }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override { return row->heightForWidth(width); }

    void setGeometry(const QRect &bounds) override
    {
        row->fitWidth(bounds.width());
        QGridLayout::setGeometry(bounds);
    }

private:
    ControlRow *row;
};
} // namespace

ControlRow::ControlRow(QWidget *parent) : QWidget(parent)
{
    auto *grid = new ControlGrid(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(metrics::control::itemGap);
    captionArea = new QWidget(this);
    controlArea = new QWidget(this);
    controlLayout = horizontalRow();
    controlArea->setLayout(controlLayout);
    auto policy = sizePolicy();
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    arrange(width());
}

void ControlRow::setMarkerBand(int height)
{
    markerBand = height;
    arrange(width());
}

int ControlRow::heightForWidth(int width) const
{
    const bool compact = stackedAt(width);
    const int captionWidth = compact ? width : int(metrics::control::captionWidth);
    const auto measure = [](QLayout *layout, int width) {
        return !layout                        ? 0
                : layout->hasHeightForWidth() ? layout->totalHeightForWidth(qMax(1, width))
                                              : layout->sizeHint().height();
    };
    const auto *captionLayout = captionArea->layout();
    const int captionHeight = measure(captionArea->layout(), captionWidth)
            - (captionLayout ? captionLayout->contentsMargins().top() : 0)
            + (compact ? 0 : markerBand);
    const int controlsHeight = measure(
            controlLayout, compact ? width : width - captionWidth - metrics::control::itemGap);
    return compact ? captionHeight + metrics::control::itemGap + controlsHeight
                   : qMax(captionHeight, controlsHeight);
}

QSize ControlRow::sizeHint() const
{
    const int width = metrics::control::captionWidth + px(480) + metrics::control::itemGap;
    return { width, heightForWidth(width) };
}

QSize ControlRow::minimumSizeHint() const
{
    return { 0, heightForWidth(width()) };
}

bool ControlRow::stackedAt(int width)
{
    return width < metrics::control::captionWidth + px(480) + metrics::control::itemGap;
}

void ControlRow::fitWidth(int width)
{
    if (stackedAt(width) != stacked)
        arrange(width);
}

void ControlRow::resizeEvent(QResizeEvent *)
{
    arrange(width());
}

void ControlRow::arrange(int width)
{
    const bool wasStacked = std::exchange(stacked, stackedAt(width));
    setProperty("caption_stacked", stacked);
    if (stacked != wasStacked && stackedChanged)
        stackedChanged(stacked);
    captionArea->setMinimumWidth(stacked ? 0 : int(metrics::control::captionWidth));
    captionArea->setMaximumWidth(stacked ? QWIDGETSIZE_MAX : int(metrics::control::captionWidth));
    if (auto *rows = captionArea->layout()) {
        auto margins = rows->contentsMargins();
        margins.setTop(stacked ? 0 : markerBand);
        rows->setContentsMargins(margins);
    }
    auto *grid = static_cast<QGridLayout *>(layout());
    grid->removeWidget(captionArea);
    grid->removeWidget(controlArea);
    grid->addWidget(captionArea, 0, 0, 1, stacked ? 2 : 1);
    grid->addWidget(controlArea, stacked ? 1 : 0, stacked ? 0 : 1, 1, stacked ? 2 : 1);
    grid->setColumnStretch(0, stacked ? 1 : 0);
    grid->setColumnStretch(1, stacked ? 0 : 1);
    updateGeometry();
}

ControlRow *controlRowOf(QWidget *control)
{
    for (auto *widget = control; widget; widget = widget->parentWidget())
        if (auto *row = dynamic_cast<ControlRow *>(widget))
            return row;
    Q_ASSERT(false);
    return nullptr;
}

QHBoxLayout *labeledRow(QWidget *parent, QVBoxLayout *rows, const QString &label,
                        const QString &detail, const QString &detailName)
{
    auto *row = new ControlRow(parent);
    rows->addWidget(row);
    auto *labels = new QVBoxLayout(row->caption());
    labels->setContentsMargins(0, 0, metrics::control::captionRight, 0);
    labels->setSpacing(px(2));
    auto *caption = new ui::InterfaceLabel(label, row->caption());
    caption->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    caption->setWordWrap(true);
    readerStyle(caption, styles::font(metrics::text::secondary));
    labels->addWidget(caption);
    if (!detail.isEmpty()) {
        auto *value = new InterfaceLabel(detail, row->caption());
        value->setObjectName(detailName);
        value->setWordWrap(true);
        readerStyle(value,
                    styles::font(metrics::control::captionValueText)
                            + "color:palette(placeholder-text);");
        labels->addWidget(value);
    }
    return row->controls();
}

SegmentedRow::SegmentedRow(QWidget *parent, QVBoxLayout *rows, const char *name,
                           const QString &label, const QString &detail)
{
    auto *line = labeledRow(parent, rows, label, detail, QString(name) + "Detail");
    frame = new QFrame(parent);
    frame->setObjectName(name);
    line->addWidget(frame, 1);
    layout = new QHBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    group = new QButtonGroup(frame);
}

QPushButton *SegmentedRow::add(const QString &name, const QString &text)
{
    auto *button = new Button(text, frame);
    button->setObjectName(name);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setProperty("segment_first", group->buttons().isEmpty());
    // QSS heights exclude padding. Every segment has the same 72px hit area.
    readerStyle(button, styles::segment());
    group->addButton(button);
    layout->addWidget(button, 1);
    return button;
}
} // namespace kobalt::ui
