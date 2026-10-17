#include "buttons.h"
#include "text.h"
#include "../style/private_text.h"
#include "support/strings.h"
#include "assets/font_catalog.h"
#include "../style/fonts.h"
#include "../style/styles.h"
#include "support/qt_font.h"
#include <QFontInfo>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QtMath>
#include <QStyle>
#include <QStyleOptionButton>
#include <QVariant>
#include <utility>

namespace kobalt::ui {
namespace m = metrics;
void Button::initStyleOption(QStyleOptionButton *option) const {
    QPushButton::initStyleOption(option);
    // Keep keyboard focus without drawing Nickel's focus frame on reader buttons.
    option->state &= ~QStyle::State_HasFocus;
}
void Button::mouseReleaseEvent(QMouseEvent *event) {
    // Qt repaints the released button synchronously before emitting clicked.
    // Queue that paint with the panel changes made by the click handler.
    const bool enabled = updatesEnabled();
    if (enabled)
        setUpdatesEnabled(false);
    QPointer<Button> alive(this);
    QPointer<QWidget> parent = parentWidget();
    const QRect oldGeometry = geometry();
    const bool visible = isVisible();
    QPushButton::mouseReleaseEvent(event);
    if (alive && enabled)
        alive->setUpdatesEnabled(true);
    // Hiding a button while its updates are disabled can leave its pressed
    // background behind. Queue that exposed area with the destination view.
    if (enabled && visible && parent && (!alive || !alive->isVisible()))
        parent->update(oldGeometry);
}

void Button::setPaintMode(ButtonPaintMode value) {
    if (mode == value)
        return;
    mode = value;
    update();
}

bool Button::event(QEvent *event) {
    // Qt repaints a hover-tracking widget when a pointer enters or leaves it.
    // Reader buttons draw no hover state, so that repaint would only refresh
    // unchanged pixels, for example a toolbar button when a drag starts.
    if (isHoverEvent(event))
        return true;
    return QPushButton::event(event);
}
void Button::paintEvent(QPaintEvent *) {
    if (mode == ButtonPaintMode::Native) {
        if (iconLabelGap) {
            QStyleOptionButton option;
            initStyleOption(&option);
            option.text.clear();
            option.icon = QIcon();
            QPainter painter(this);
            style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
            const bool active = isEnabled() && (isChecked() || isDown());
            painter.setPen(palette().color(!isEnabled() ? QPalette::Mid
                                           : active     ? QPalette::Window
                                                        : QPalette::WindowText));
            painter.setFont(font());
            const int labelWidth = interfaceTextWidth(text(), font());
            const int gap = text().isEmpty() ? 0 : iconLabelGap;
            if (iconAboveLabel) {
                // Capitals' height is the label's ink, so the icon and that
                // ink centre as one group, as in the bottom tabs.
                const int capital = qCeil(QFontMetricsF(font()).tightBoundingRect(QStringLiteral("H")).height());
                const int top = (height() - iconSize().height() - gap - capital) / 2;
                const QRect area((width() - iconSize().width()) / 2, top, iconSize().width(), iconSize().height());
                icon().paint(&painter, area, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled,
                             active ? QIcon::On : QIcon::Off);
                drawOpticalText(painter, QRect(0, area.bottom() + 1 + gap, width(), capital), text(), true);
                return;
            }
            const int left = (width() - iconSize().width() - gap - labelWidth) / 2;
            icon().paint(
                &painter, QRect(left, (height() - iconSize().height()) / 2, iconSize().width(), iconSize().height()),
                Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled, active ? QIcon::On : QIcon::Off);
            drawInterfaceText(painter, QRect(left + iconSize().width() + gap, 0, labelWidth, height()), text(),
                              Qt::AlignCenter);
            return;
        }
        QStyleOptionButton option;
        initStyleOption(&option);
        QPainter painter(this);
        style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        return;
    }
    QPainter painter(this);
    const bool active = isEnabled() && (isChecked() || isDown());
    const auto ink = palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid);
    const auto paper = palette().color(QPalette::Window);
    const bool toolbar = mode == ButtonPaintMode::ToolbarSquare || mode == ButtonPaintMode::ToolbarCell;
    if (toolbar) {
        painter.fillRect(rect(), paper);
        if (active) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            const int diameter = qMin(width(), height());
            painter.drawRect(mode == ButtonPaintMode::ToolbarCell
                                 ? QRectF(rect())
                                 : QRectF((width() - diameter) / 2.0, (height() - diameter) / 2.0, diameter, diameter));
        }
    } else {
        paintButtonShape(painter, rect(), paper, active ? ink : paper,
                         isEnabled() ? QColor(paper == Qt::black ? m::control::darkOutline : m::control::lightOutline)
                                     : palette().color(QPalette::Midlight));
    }
    painter.setPen(active ? paper : ink);
    if (!text().isEmpty()) {
        painter.setFont(font());
        drawOpticalText(painter, toolbar ? rect() : rect().adjusted(px(8), px(4), -px(8), -px(4)), text(), true);
    } else {
        if (!isEnabled())
            painter.setOpacity(.4);
        icon().paint(&painter,
                     QRect((width() - iconSize().width()) / 2, (height() - iconSize().height()) / 2, iconSize().width(),
                           iconSize().height()),
                     Qt::AlignCenter, QIcon::Normal, active ? QIcon::On : QIcon::Off);
    }
}

QSize BackButton::sizeHint() const {
    ensurePolished();
    // Include the custom arrow and leave room for fractional font advances.
    return QSize(m::navigation::labelLeft + qt::textWidth(fontMetrics(), text()) + m::navigation::labelRight +
                     contentsMargins().right() + 2,
                 Button::sizeHint().height());
}
void BackButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();
    style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
    painter.setFont(font());
    painter.setRenderHint(QPainter::Antialiasing);
    const auto ink = palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid);
    const QRect arrow(m::navigation::arrowLeft, (height() - m::navigation::arrowSize) / 2, m::navigation::arrowSize,
                      m::navigation::arrowSize);
    controlIcon(ControlIcon::Back, ink).paint(&painter, arrow);
    // The arrow's top rounds down when the height and size differ by an odd
    // number, so center on the arrow itself, not on the button.
    const qreal center = arrow.top() + arrow.height() / 2.0;
    const auto label = painter.fontMetrics().elidedText(
        text(), Qt::ElideRight,
        qMax(0, width() - m::navigation::labelLeft - m::navigation::labelRight - contentsMargins().right()));
    // Center the visible letters on the arrow.
    const auto bounds = QFontMetricsF(painter.font()).tightBoundingRect(label);
    painter.setPen(ink);
    painter.drawText(QPointF(m::navigation::labelLeft, center - bounds.center().y() + labelOffset), label);
}
TapZoneButton::TapZoneButton(tapZones::Layout layout, bool rightToLeft, QWidget *parent)
    : Button(parent), layout(layout), rightToLeft(rightToLeft) {
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    readerStyle(this, "QPushButton{border:0;padding:0;min-width:0;min-height:0;}");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(sizeHint().height());
}
QSize TapZoneButton::sizeHint() const {
    return {px(200), px(288)};
}
void TapZoneButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto paper = palette().color(QPalette::Window);
    const auto ink = palette().color(QPalette::WindowText);
    const auto gray = darkTheme(this) ? QColor("#333333") : QColor("#dddddd");
    painter.fillRect(rect(), isDown() ? gray : paper);
    const int diagramHeight = px(200);
    const int diagramWidth = qMin(px(142), width() - px(24));
    const QRectF diagram((width() - diagramWidth) / 2, px(16), diagramWidth, diagramHeight);
    painter.setPen(QPen(ink, px(2)));
    for (const auto &region : tapZones::regions(layout)) {
        const auto &r = region.bounds;
        const QRectF area(diagram.x() + r.x() * diagram.width(), diagram.y() + r.y() * diagram.height(),
                          r.width() * diagram.width(), r.height() * diagram.height());
        painter.fillRect(area, region.action == tapZones::Action::Menu ? gray : paper);
        painter.drawRect(area);
    }
    const auto glyph = [&](tapZones::Action action, qreal x, qreal y) {
        const int side = px(24);
        const QPointF center(diagram.x() + x * diagram.width(), diagram.y() + y * diagram.height());
        const QRectF area(center.x() - side / 2., center.y() - side / 2., side, side);
        if (action == tapZones::Action::Menu) {
            for (int line = -1; line <= 1; ++line)
                painter.drawLine(QPointF(area.left(), center.y() + line * px(5)),
                                 QPointF(area.right(), center.y() + line * px(5)));
        } else {
            bool next = action == tapZones::Action::Next;
            paintOpeningIcon(painter, next != rightToLeft ? ControlIcon::Next : ControlIcon::Previous, area, ink);
        }
    };
    if (layout == tapZones::Layout::OneHanded) {
        glyph(tapZones::Action::Next, .5, .2);
        glyph(tapZones::Action::Next, .5, .8);
        glyph(tapZones::Action::Menu, .5, .5);
    } else {
        for (const auto &region : tapZones::regions(layout))
            glyph(region.action, region.bounds.center().x(), region.bounds.center().y());
    }
    painter.setPen(QPen(isChecked() ? ink : gray, px(6)));
    painter.drawRect(diagram.adjusted(-px(4), -px(4), px(4), px(4)));
    painter.setPen(ink);
    painter.setFont(interfaceFont(px(25)));
    drawInterfaceText(painter, QRect(0, px(232), width(), px(40)), text(), Qt::AlignCenter);
}
QPushButton *makeButton(QWidget *parent, QBoxLayout *layout, const char *name, const QString &text,
                        std::function<void()> action) {
    auto *button = new Button(text, parent);
    button->setObjectName(name);
    button->setFocusPolicy(Qt::NoFocus);
    if (layout)
        layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, parent, std::move(action));
    return button;
}
QSize ElidingButton::sizeHint() const {
    if (!needsPrivateText(text(), font())) {
        return Button::sizeHint();
    }
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();
    const auto measured = measurePrivateTextSize(text(), font());
    const int iconWidth = icon().isNull() ? 0 : iconSize().width() + m::control::elidingIconGap;
    return style()->sizeFromContents(QStyle::CT_PushButton, &option,
                                     QSize(measured.width() + iconWidth, qMax(measured.height(), iconSize().height())),
                                     this);
}
void ElidingButton::paintEvent(QPaintEvent *) {
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    option.icon = QIcon();
    QPainter painter(this);
    style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
    auto contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
    if (!icon().isNull()) {
        const QRect target(contents.left(), contents.center().y() - iconSize().height() / 2, iconSize().width(),
                           iconSize().height());
        icon().paint(&painter, target, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
        contents.setLeft(target.right() + 1 + m::control::elidingIconGap);
    }
    painter.setPen(palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid));
    painter.setFont(font());
    drawInterfaceText(painter, contents, elideInterfaceText(text(), font(), qMax(0, contents.width())),
                      Qt::AlignLeft | Qt::AlignVCenter);
}

StepButton::StepButton(ControlIcon icon, QWidget *parent)
    : kobalt::ui::Button(parent), normal(controlIcon(icon)), pressed(controlIcon(icon, Qt::white)) {
    // QSS minimum sizes are applied during polish, after setFixedSize(). Keep
    // both constraints identical so a label-only row cannot flatten the button.
    readerStyle(
        this,
        QString("QPushButton{min-width:%1px;max-width:%1px;min-height:%1px;max-height:%1px;padding:0;border:none;}")
            .arg(m::control::stepSize));
    setFixedSize(m::control::stepSize, m::control::stepSize);
}
bool StepButton::hitButton(const QPoint &point) const {
    return rect().contains(point);
}
void StepButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette().color(isEnabled() ? QPalette::WindowText : QPalette::Midlight), m::control::border));
    painter.setBrush(palette().color(isEnabled() && isDown() ? QPalette::WindowText : QPalette::Window));
    const qreal inset = int(m::control::border) / 2.0;
    painter.drawRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset));
    if (!isEnabled())
        painter.setOpacity(.4);
    (((isEnabled() && isDown()) != (palette().color(QPalette::Window) == Qt::black)) ? pressed : normal)
        .paint(&painter,
               rect().adjusted(m::control::stepIconInset, m::control::stepIconInset, -m::control::stepIconInset,
                               -m::control::stepIconInset),
               Qt::AlignCenter, QIcon::Normal);
}

DropdownButton::DropdownButton(QWidget *parent) : Button(parent) {
    readerStyle(this, QString("QPushButton{min-height:%1px;max-height:%1px;}")
                          .arg(m::choices::fontButtonHeight - 2 * m::control::padding));
}
FontButton::FontButton(QWidget *parent) : DropdownButton(parent) {
    setProperty("opens_upward", true);
}
void FontButton::setFace(const QString &face) {
    const auto next = fontSample(face);
    const auto label = face.isEmpty() ? strings::get("controls.publisher_default") : fontDisplayName(face);
    if (sample == next && text() == label)
        return;
    sample = next;
    setText(label);
    setAccessibleName(text());
    update();
}
QString FontButton::resolvedFace() const {
    return needsPrivateText(QStringLiteral("Hox"), sample) ? sample.family() : QFontInfo(sample).family();
}
void DropdownButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto paper = palette().color(QPalette::Window);
    const auto ink = palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid);
    painter.fillRect(rect(), isEnabled() && isDown() ? palette().color(QPalette::Midlight) : paper);
    paintEdges(painter, rect(), Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
               palette().color(isEnabled() ? QPalette::WindowText : QPalette::Midlight));
    painter.setPen(ink);
    painter.setFont(labelFont());
    const QRect label(m::choices::fontButtonLeft, 0, qMax(0, width() - m::choices::fontButtonWidthReserve), height());
    if (opticalLabel())
        drawOpticalText(painter, label, text(), false, sampleAppearance());
    else
        drawInterfaceText(painter, label, elideInterfaceText(text(), painter.font(), label.width()),
                          Qt::AlignLeft | Qt::AlignVCenter);
    const int size = m::choices::fontArrowSize;
    const auto arrow = forward                             ? ControlIcon::Next
                       : property("opens_upward").toBool() ? ControlIcon::Up
                                                           : ControlIcon::Down;
    controlIcon(arrow, ink)
        .paint(&painter, QRect(width() - m::choices::fontArrowRight - size / 2, (height() - size) / 2, size, size));
}

CaptionButton::CaptionButton(ControlIcon icon, QWidget *parent) : CaptionButton(Presentation::Caption, parent) {
    glyph = icon;
}
CaptionButton::CaptionButton(Presentation presentation, QWidget *parent)
    : Button(parent), mode(presentation), chevron(presentation == Presentation::ListAction),
      iconVisible(presentation == Presentation::Caption) {}
CaptionButton *makeCaptionButton(QWidget *parent, QBoxLayout *layout, const char *name, const QString &text,
                                 std::function<void()> action) {
    auto *button = new CaptionButton(CaptionButton::Presentation::ListAction, parent);
    button->setText(text);
    button->setObjectName(name);
    button->setFocusPolicy(Qt::NoFocus);
    if (layout)
        layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, parent, std::move(action));
    return button;
}
void CaptionButton::paintEvent(QPaintEvent *) {
    if (mode == Presentation::ListAction) {
        namespace c = m::chrome;
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(isDown() ? QPalette::Midlight : QPalette::Window));
        paintEdges(painter, rect(), rule, dividerColor(this));
        painter.setPen(palette().color(QPalette::WindowText));
        QFont label = font();
        painter.setFont(label);
        const bool actionIcon = iconVisible;
        const int left = c::inset + (actionIcon ? c::cellIcon + px(20) : 0);
        // Balance the visible icon-to-border and icon-to-label gaps, including the glyph bearings.
        if (actionIcon)
            icon().paint(&painter,
                         QRect(c::inset - px(7), (height() - c::cellIcon) / 2 + px(1), c::cellIcon, c::cellIcon));
        const int textWidth = width() - left - c::actionRight;
        // The label and its note are one block, centred on its letters. Their
        // line boxes keep the same centre-to-centre distance as before.
        const QFont noteFont = captionFont();
        const int labelHeight = QFontMetrics(label).height(), noteHeight = QFontMetrics(noteFont).height();
        const int pitch = px(40);
        const int blockHeight = note.isEmpty() ? labelHeight : labelHeight / 2 + pitch + noteHeight / 2;
        const int top = (height() - blockHeight) / 2 - opticalCenterShift(label, note.isEmpty() ? label : noteFont);
        painter.drawText(QRect(left, top, textWidth, labelHeight), Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(text(), Qt::ElideRight, textWidth));
        painter.setPen(QPen(palette().color(QPalette::WindowText), px(2)));
        const int x = width() - c::chevronRight, y = height() / 2;
        if (chevron) {
            painter.drawLine(x - px(8), y - px(10), x + px(2), y);
            painter.drawLine(x + px(2), y, x - px(8), y + px(10));
        }
        if (!note.isEmpty()) {
            painter.setFont(noteFont);
            painter.setPen(noteColor(this));
            painter.drawText(QRect(c::inset, top + labelHeight / 2 + pitch - noteHeight / 2, textWidth, noteHeight),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(note, Qt::ElideRight, textWidth));
        }
        return;
    }
    namespace c = m::chrome;
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(isDown() ? QPalette::Midlight : QPalette::Window));
    paintEdges(painter, rect(), rule, dividerColor(this));
    const auto ink = palette().color(QPalette::WindowText);
    controlIcon(glyph, ink).paint(&painter, QRect(c::inset, (height() - c::cellIcon) / 2, c::cellIcon, c::cellIcon));
    const int left = c::inset + c::cellIcon + px(24), right = chevron ? int(c::actionRight) : int(c::inset);
    const int available = qMax(0, width() - left - right);
    const auto labelFont = interfaceFont(c::labelText), noteFont = captionFont(px(20));
    const int labelHeight = QFontMetrics(labelFont).height(),
              noteHeight = note.isEmpty() ? 0 : QFontMetrics(noteFont).height();
    const int gap = note.isEmpty() ? 0 : px(2), top = (height() - labelHeight - gap - noteHeight) / 2;
    painter.setPen(ink);
    painter.setFont(labelFont);
    painter.drawText(QRect(left, top, available, labelHeight), Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(text(), Qt::ElideRight, available));
    if (!note.isEmpty()) {
        painter.setFont(noteFont);
        painter.setPen(noteColor(this));
        painter.drawText(QRect(left, top + labelHeight + gap, available, noteHeight), Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(note, Qt::ElideRight, available));
    }
    if (chevron) {
        // The same chevron as list rows.
        const int x = width() - c::chevronRight, y = height() / 2;
        painter.setPen(QPen(ink, px(2)));
        painter.drawLine(x - px(8), y - px(10), x + px(2), y);
        painter.drawLine(x + px(2), y, x - px(8), y + px(10));
    }
}
CellButton::CellButton(ControlIcon icon, QWidget *parent, int iconPixels)
    : Button(parent), glyph(icon), iconPixels(iconPixels) {
    setFocusPolicy(Qt::NoFocus);
}
QSize CellButton::sizeHint() const {
    if (text().isEmpty())
        return {m::chrome::cellWidth, m::chrome::actionHeight};
    // Paint with our own font; the reader's button style sets a larger size.
    QFont label = interfaceFont(m::chrome::countText);
    label.setCapitalization(QFont::AllUppercase);
    return {2 * m::chrome::inset + qt::textWidth(QFontMetrics(label), text()) + m::chrome::selectorGap + iconPixels,
            m::chrome::actionHeight};
}
void CellButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const bool active = isEnabled() && (isDown() || isChecked());
    const auto paper = palette().color(QPalette::Window), ink = palette().color(QPalette::WindowText);
    painter.fillRect(rect(), active ? ink : paper);
    paintEdges(painter, rect(), rule, palette().color(QPalette::Mid));
    if (!isEnabled())
        painter.setOpacity(.45);
    const auto color = active ? paper : ink;
    if (text().isEmpty()) {
        controlIcon(glyph, color)
            .paint(&painter, QRect((width() - iconPixels) / 2, (height() - iconPixels) / 2, iconPixels, iconPixels));
        return;
    }
    QFont label = interfaceFont(m::chrome::countText);
    label.setCapitalization(QFont::AllUppercase);
    painter.setFont(label);
    painter.setPen(color);
    const int available = qMax(0, width() - 2 * m::chrome::inset - m::chrome::selectorGap - iconPixels);
    const auto shown = painter.fontMetrics().elidedText(text(), Qt::ElideRight, available);
    painter.drawText(QRect(m::chrome::inset, 0, available, height()), Qt::AlignLeft | Qt::AlignVCenter, shown);
    const int x =
        m::chrome::inset + qMin(available, qt::textWidth(painter.fontMetrics(), shown)) + m::chrome::selectorGap;
    controlIcon(glyph, color).paint(&painter, QRect(x, (height() - iconPixels) / 2, iconPixels, iconPixels));
}

}
