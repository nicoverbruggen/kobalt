#include "rows.h"
#include "support/qt_font.h"
#include "../style/metrics.h"
#include "../style/styles.h"
#include "../style/fonts.h"
#include "../style/private_text.h"
#include "support/strings.h"
#include <QPainter>
#include <QResizeEvent>
#include "text.h"
#include <QVariant>

namespace kobalt::ui {
namespace m = metrics;
ListRow::ListRow(QWidget *parent) : Button(parent) {
    readerStyle(this, "QPushButton{border:none;padding:0;min-height:0;background:transparent;}");
    title = new PrivateTextLabel(this);
    detail = new PrivateTextLabel(this);
    caption = new PrivateTextLabel(this);
    leading = new PrivateTextLabel(this);
    trailing = new PrivateTextLabel(this);
    for (auto *label : {title, detail, caption, leading, trailing}) {
        label->setTextFormat(Qt::PlainText);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setStyleSheet("background:transparent;border:none;");
    }
    readerStyle(title, styles::font(m::chrome::labelText) + "background:transparent;border:none;");
    readerStyle(detail, styles::font(m::chrome::descriptionText) + "background:transparent;border:none;");
    ui::captionLabel(caption);
    ui::captionLabel(leading);
    ui::captionLabel(trailing);
    int index = 0;
    for (auto *label : {title, detail, caption, leading, trailing}) {
        labelStyles[index++] = label->property("reader_style").toString();
        label->installEventFilter(this);
    }
    trailing->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(this, &QPushButton::toggled, this, [this] {
        syncLabelStyles();
    });
    observeParent();
    syncLabelStyles();
    arrange();
}
void ListRow::setTitle(const QString &text) {
    if (titleText == text)
        return;
    titleText = text;
    setText(text);
    setAccessibleName(text);
    arrange();
}
void ListRow::setDetail(const QString &text) {
    if (detailText == text)
        return;
    detailText = text;
    arrange();
}
void ListRow::setCaption(const QString &text) {
    if (captionText == text)
        return;
    captionText = text;
    arrange();
}
void ListRow::setLeading(const QString &text, int indent) {
    indent = qMax(0, indent);
    if (leadingText == text && indentation == indent)
        return;
    leadingText = text;
    indentation = indent;
    arrange();
}
void ListRow::setAccessory(Accessory value) {
    if (accessory == value)
        return;
    accessory = std::move(value);
    arrange();
}
void ListRow::setTitlePixels(int pixels) {
    const QString style = styles::font(pixels) + "background:transparent;border:none;";
    if (labelStyles[0] == style)
        return;
    labelStyles[0] = style;
    syncLabelStyles();
}
QPushButton *ListRow::addAction(const QString &name, const QString &accessibleName) {
    if (!action) {
        auto *cell = new CellButton(ControlIcon::Close, this);
        cell->setRule(Edges(Qt::LeftEdge) | Qt::BottomEdge);
        action = cell;
    }
    action->setObjectName(name);
    action->setAccessibleName(accessibleName);
    arrange();
    return action;
}
void ListRow::setActionRule(bool value) {
    if (action)
        static_cast<CellButton *>(action)->setRule(value ? Edges(Qt::LeftEdge) | Qt::BottomEdge
                                                         : Edges(Qt::BottomEdge));
}
QSize ListRow::sizeHint() const {
    const int lines = 1 + !detailText.isEmpty() + !captionText.isEmpty();
    return {0, lines == 3 ? int(m::list::threeLines) : lines == 2 ? int(m::list::twoLines) : int(m::list::oneLine)};
}
void ListRow::resizeEvent(QResizeEvent *) {
    arrange();
}
void ListRow::changeEvent(QEvent *event) {
    Button::changeEvent(event);
    if (!title)
        return;
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::EnabledChange)
        syncLabelStyles();
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        arrange();
}
void ListRow::checkStateSet() {
    Button::checkStateSet();
    syncLabelStyles();
}
void ListRow::nextCheckState() {
    // Qt skips checkStateSet during a click, including when signals are blocked.
    QPointer<ListRow> alive(this);
    Button::nextCheckState();
    if (alive)
        alive->syncLabelStyles();
}
void ListRow::observeParent() {
    if (observedParent == parentWidget())
        return;
    if (observedParent)
        observedParent->removeEventFilter(this);
    observedParent = parentWidget();
    if (observedParent)
        observedParent->installEventFilter(this);
}
bool ListRow::event(QEvent *event) {
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
bool ListRow::eventFilter(QObject *object, QEvent *event) {
    if (object == observedParent &&
        (event->type() == QEvent::PaletteChange || event->type() == QEvent::DynamicPropertyChange))
        syncLabelStyles();
    if (!synchronizing &&
        (object == title || object == detail || object == caption || object == leading || object == trailing) &&
        (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))
        arrange();
    return Button::eventFilter(object, event);
}
void ListRow::syncLabelStyles() {
    if (!title || synchronizing)
        return;
    synchronizing = true;
    const auto colors = parentWidget() ? parentWidget()->palette() : palette();
    const bool current = isChecked();
    const QColor ink = colors.color(current ? QPalette::Window : QPalette::WindowText);
    int index = 0;
    bool changed = false;
    for (auto *label : {title, detail, caption, leading, trailing}) {
        const QColor color = !isEnabled() ? QColor("#999999") : current || label == title ? ink : noteColor(this);
        const QString style = labelStyles[index++] + "color:" + color.name() + ';';
        if (label->property("reader_style").toString() != style) {
            readerStyle(label, style);
            changed = true;
        }
    }
    synchronizing = false;
    if (changed)
        arrange();
}
void ListRow::arrange() {
    if (!title || arranging || synchronizing)
        return;
    arranging = true;
    const int end = width() - (action ? int(m::chrome::cellWidth) : 0) - m::list::inset;
    const int lead = leadingText.isEmpty() ? 0 : int(m::list::leadingWidth);
    const auto *trailingCaption = accessory.caption();
    const int trail = trailingCaption ? qMax(trailingCaption->reservedWidth, trailing->textWidth(trailingCaption->text))
                      : accessory.kind() == Accessory::Kind::None ? 0
                                                                  : int(m::chrome::cellIcon);
    const int start = m::list::inset + lead + indentation;
    const int textWidth = qMax(0, end - start - trail - (trail ? int(m::contents::pageGap) : 0));
    const int lines = 1 + !detailText.isEmpty() + !captionText.isEmpty();
    const int top = lines == 1 ? 0 : int(m::list::lineInset);
    const int lineHeight = qMax(1, (height() - 2 * top - m::control::border) / lines);
    title->setGeometry(start, top, textWidth, lineHeight);
    title->setText(title->elidedText(titleText, textWidth));
    int line = 1;
    detail->setVisible(!detailText.isEmpty());
    caption->setVisible(!captionText.isEmpty());
    for (const auto &item : {qMakePair(detail, detailText), qMakePair(caption, captionText)}) {
        if (item.second.isEmpty())
            continue;
        item.first->setGeometry(start, top + line++ * lineHeight, qMax(0, end - start), lineHeight);
        item.first->setText(item.first->elidedText(item.second, item.first->width()));
    }
    leading->setVisible(!leadingText.isEmpty());
    leading->setText(leadingText);
    leading->setGeometry(m::list::inset, 0, lead, height() - m::control::border);
    trailing->setVisible(trailingCaption != nullptr);
    trailing->setText(trailingCaption ? trailingCaption->text : QString());
    trailing->setGeometry(qMax(start, end - trail), top, qMax(0, qMin(trail, end - start)), lineHeight);
    if (action) {
        const QPoint origin = action->parentWidget() == this ? QPoint() : pos();
        action->setGeometry(origin.x() + width() - m::chrome::cellWidth, origin.y(), m::chrome::cellWidth, height());
    }
    arranging = false;
    updateGeometry();
    update();
}
void ListRow::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto colors = parentWidget() ? parentWidget()->palette() : palette();
    const bool current = isChecked();
    const QColor ink = colors.color(current ? QPalette::Window : QPalette::WindowText);
    QColor paper = colors.color(current ? QPalette::WindowText : QPalette::Window);
    if (!current && isEnabled() && isDown())
        paper = darkTheme(this) ? QColor("#333333") : QColor("#dddddd");
    painter.fillRect(rect(), paper);
    const int end = width() - (action ? int(m::chrome::cellWidth) : 0) - m::list::inset;
    if (accessory.kind() == Accessory::Kind::Chevron || accessory.kind() == Accessory::Kind::Check) {
        painter.setRenderHint(QPainter::Antialiasing);
        const int side = m::chrome::cellIcon;
        const QRectF area(end - side, (height() - side) / 2.0, side, side);
        if (accessory.kind() == Accessory::Kind::Chevron)
            controlIcon(ControlIcon::Next, ink).paint(&painter, area.toRect());
        else {
            painter.setPen(QPen(ink, px(3), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            QPolygonF check;
            check << area.topLeft() + QPointF(side * .18, side * .5) << area.topLeft() + QPointF(side * .42, side * .75)
                  << area.topLeft() + QPointF(side * .82, side * .22);
            painter.drawPolyline(check);
        }
    }
    painter.fillRect(0, height() - m::control::border, width(), m::control::border, dividerColor(this));
}
ListHeader::ListHeader(QWidget *parent) : QWidget(parent) {
    left = new QLabel(this);
    right = new QLabel(this);
    ui::captionLabel(left);
    ui::captionLabel(right);
    right->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
}
void ListHeader::setCaptions(const QString &a, const QString &b) {
    left->setText(a);
    right->setText(b);
    QResizeEvent event(size(), size());
    resizeEvent(&event);
}
QSize ListHeader::sizeHint() const {
    return {0, int(m::list::headerHeight)};
}
void ListHeader::setTrailingInset(int pixels) {
    trailingInset = qMax(0, pixels);
    QResizeEvent event(size(), size());
    resizeEvent(&event);
}
void ListHeader::resizeEvent(QResizeEvent *) {
    const int available = qMax(0, width() - 2 * m::list::inset - trailingInset);
    const int side =
        right->text().isEmpty() ? 0 : qMin(available, qt::textWidth(QFontMetrics(right->font()), right->text()));
    left->setGeometry(m::list::inset, 0, qMax(0, available - side - (side ? int(m::list::gap) : 0)),
                      height() - m::control::border);
    right->setGeometry(width() - m::list::inset - trailingInset - side, 0, side, height() - m::control::border);
}
void ListHeader::setShaded(bool value) {
    if (shaded == value)
        return;
    shaded = value;
    update();
}
void ListHeader::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    // Styles can replace Midlight when a list creates headers after theme replay.
    const auto background =
        shaded ? (darkTheme(this) ? QColor("#333333") : QColor("#dddddd")) : palette().color(QPalette::Window);
    painter.fillRect(rect(), background);
    painter.fillRect(0, height() - m::control::border, width(), m::control::border, dividerColor(this));
}
QHBoxLayout *horizontalRow() {
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(m::control::itemGap);
    return line;
}
QHBoxLayout *labeledRow(QWidget *parent, QVBoxLayout *rows, const QString &label) {
    auto *line = horizontalRow();
    rows->addLayout(line);
    auto *caption = new QLabel(label, parent);
    caption->setFixedWidth(m::control::captionWidth);
    caption->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    readerStyle(caption, styles::font(m::text::secondary));
    line->addWidget(caption);
    return line;
}
SegmentedRow::SegmentedRow(QWidget *parent, QVBoxLayout *rows, const char *name, const QString &label) {
    auto *line = labeledRow(parent, rows, label);
    frame = new QFrame(parent);
    frame->setObjectName(name);
    line->addWidget(frame, 1);
    layout = new QHBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    group = new QButtonGroup(frame);
}
QPushButton *SegmentedRow::add(const QString &name, const QString &text) {
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
}
