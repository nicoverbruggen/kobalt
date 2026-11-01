#include "buttons.h"
#include "text.h"
#include "../style/private_text.h"
#include "support/strings.h"
#include "assets/font_catalog.h"
#include "../style/fonts.h"
#include "../style/styles.h"
#include "support/qt_font.h"
#include <QFontInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QtMath>
#include <QStyle>
#include <QStyleOptionButton>
#include <QVariant>
#include <QStringList>
#include <utility>
#include <limits>

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
        if (iconLabelGap || contentAlignment == Qt::AlignLeft || (!icon().isNull() && !text().isEmpty())) {
            QStyleOptionButton option;
            initStyleOption(&option);
            option.text.clear();
            option.icon = QIcon();
            QPainter painter(this);
            style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
            const bool primary = property("reader_primary_action").toBool();
            const bool active = isEnabled() && (primary ? !isDown() : isChecked() || isDown());
            const auto colors = primary ? readerPalette(palette(), darkTheme(this)) : palette();
            painter.setPen(colors.color(!isEnabled() ? QPalette::Mid
                                           : active     ? QPalette::Window
                                                        : QPalette::WindowText));
            painter.setFont(font());
            const int labelWidth = interfaceTextWidth(text(), font());
            const QSize glyph = icon().isNull() ? QSize(0, 0) : iconSize();
            const int gap = text().isEmpty() || icon().isNull() ? 0 : (iconLabelGap ? iconLabelGap : px(12));
            if (iconAboveLabel) {
                // Capitals' height is the label's ink, so the icon and that
                // ink centre as one group, as in the bottom tabs.
                const int capital = interfaceInkBounds(QStringLiteral("H"), font()).height();
                const int top = (height() - iconSize().height() - gap - capital) / 2;
                const QRect area((width() - iconSize().width()) / 2, top, iconSize().width(), iconSize().height());
                icon().paint(&painter, area, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled,
                             active ? QIcon::On : QIcon::Off);
                drawOpticalText(painter, QRect(0, area.bottom() + 1 + gap, width(), capital), text(), true);
                return;
            }
            const int left = contentAlignment == Qt::AlignLeft
                ? qMax(int(px(24)), style()->subElementRect(QStyle::SE_PushButtonContents, &option, this).left())
                : (width() - glyph.width() - gap - labelWidth) / 2;
            icon().paint(
                &painter, QRect(left, (height() - glyph.height()) / 2, glyph.width(), glyph.height()),
                Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled, active ? QIcon::On : QIcon::Off);
            drawOpticalText(painter, QRect(left + glyph.width() + gap, 0, labelWidth, height()), text(), true);
            return;
        }
        QStyleOptionButton option;
        initStyleOption(&option);
        QPainter painter(this);
        if (needsPrivateText(text(), font())) {
            option.text.clear();
            style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
            painter.setFont(font());
            // Qt5's stylesheet palette can keep the normal text color after
            // a segment is selected or pressed. Match its painted background.
            const bool primary = property("reader_primary_action").toBool();
            const bool segment = property("segment_first").isValid();
            const bool inverted = segment ? isChecked() || isDown() : primary && !isDown();
            const auto colors = primary || segment ? readerPalette(palette(), darkTheme(this)) : palette();
            painter.setPen(colors.color(!isEnabled() ? QPalette::Mid
                                        : inverted ? QPalette::Window : QPalette::WindowText));
            drawOpticalText(painter, rect(), text(), true);
        } else {
            style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        }
        return;
    }
    QPainter painter(this);
    const bool active = isEnabled() && (isChecked() || isDown());
    const auto ink = palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid);
    const auto paper = fill == QPalette::Midlight ? sectionColor(this) : palette().color(fill);
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
                         isEnabled() ? themeColors(paper == Qt::black).outline
                                     : palette().color(QPalette::Midlight));
    }
    painter.setPen(active ? paper : ink);
    if (!text().isEmpty()) {
        painter.setFont(font());
        drawOpticalText(painter, toolbar ? rect() : rect().adjusted(px(8), px(4), -px(8), -px(4)), text(),
                        contentAlignment != Qt::AlignLeft);
    } else {
        if (!isEnabled())
            painter.setOpacity(.4);
        // Toolbar cells keep their size; large text can enlarge their icons.
        const QSize glyph = toolbar ? iconSize() * detail::iconScale : iconSize();
        icon().paint(&painter,
                     QRect((width() - glyph.width()) / 2, (height() - glyph.height()) / 2, glyph.width(),
                           glyph.height()),
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
    const int available = qMax(0, width() - m::navigation::labelLeft - m::navigation::labelRight - contentsMargins().right());
    const auto label = elideInterfaceText(text(), painter.font(), available);
    painter.setPen(ink);
    drawOpticalText(painter, QRectF(m::navigation::labelLeft, center - height() / 2.0 + labelOffset, available, height()), label);

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
    const auto gray = themeColors(this).pressedPaper;
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
            .arg(int(m::control::stepSize)));
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
    const QRect label(m::choices::fontButtonLeft, 0,
                      qMax(0, width() - int(m::choices::fontButtonWidthReserve)), height());
    // Profile labels center their visible ink, not the font's full line box.
    if (opticalLabel())
        drawOpticalText(painter, label, text(), false, sampleAppearance());
    else if (forward)
        drawOpticalText(painter, label, text());
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
void CaptionButton::setTypefacePreview(const QString &face) {
    if (previewFace == face && previewDensity == density())
        return;
    previewFace = face;
    previewDensity = density();
    if (!face.isEmpty()) {
        previewFont = fontSample(face);
        previewFont.setPixelSize(
            qRound(previewFont.pixelSize() * qreal(m::chrome::cellIcon) / m::choices::sampleInkHeight));
    }
    update();
}
QSize CaptionButton::sizeHint() const {
    if (!description.isEmpty()) {
        const auto size = Button::sizeHint();
        return {size.width(), heightForWidth(size.width())};
    }
    if (mode != Presentation::CompactAction) {
        return Button::sizeHint();
    }
    const int gap = text().isEmpty() ? 0 : px(16);
    return {interfaceTextWidth(text(), interfaceFont(m::chrome::labelText)) + int(m::chrome::cellIcon) + gap + px(32) +
                int(m::control::padding),
            m::scrubber::actionHeight};
}
void CaptionButton::setDescription(const QString &text) {
    description = text;
    setAccessibleDescription(text);
    updateGeometry();
    update();
}
int CaptionButton::heightForWidth(int width) const {
    if (description.isEmpty())
        return Button::heightForWidth(width);
    namespace c = m::chrome;
    const int iconSpace = mode == Presentation::ListAction && iconVisible ? int(c::cellIcon) + px(20) : 0;
    const int available = qMax(1, width - c::inset - c::actionRight - iconSpace);
    const auto label = font(), noteFont = captionFont();
    const int titleHeight = note.isEmpty() ? interfaceLineHeight(label)
        : interfaceLineHeight(label) / 2 + px(40) + interfaceLineHeight(noteFont) / 2;
    return qMax(int(c::listActionHeight), titleHeight + px(40) +
        wrappedInterfaceTextHeight(description, interfaceFont(c::descriptionText), available));
}
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
    if (mode == Presentation::ToolbarAction || mode == Presentation::CompactAction) {
        QPainter painter(this);
        const auto ink = palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid);
        const auto paper = palette().color(QPalette::Window);
        const bool pressed = isEnabled() && isDown();
        painter.fillRect(rect(), pressed ? ink : paper);
        paintEdges(painter, rect(), rule, dividerColor(this));
        painter.setPen(pressed ? paper : ink);
        if (isIconAboveLabel()) {
            painter.setFont(font());
            const int iconSize = m::chrome::cellIcon, gap = px(8);
            const int labelHeight = interfaceLineHeight(font());
            const int top = (height() - iconSize - gap - labelHeight) / 2;
            icon().paint(&painter, QRect((width() - iconSize) / 2, top, iconSize, iconSize), Qt::AlignCenter,
                         isEnabled() ? QIcon::Normal : QIcon::Disabled, pressed ? QIcon::On : QIcon::Off);
            const QRect labelArea(px(16), top + iconSize + gap, qMax(0, width() - 2 * px(16)), labelHeight);
            drawInterfaceText(painter, labelArea, elideInterfaceText(text(), font(), labelArea.width()), Qt::AlignCenter);
            return;
        }
        painter.setFont(interfaceFont(m::chrome::labelText));
        const int iconSize = m::chrome::cellIcon, gap = text().isEmpty() ? 0 : px(16);
        const int inset = mode == Presentation::CompactAction ? px(16) : int(m::chrome::inset);
        const int available = qMax(0, width() - 2 * inset - iconSize - gap);
        const auto label = elideInterfaceText(text(), painter.font(), available);
        const int labelWidth = interfaceTextWidth(label, painter.font());
        const int left = (width() - iconSize - gap - labelWidth) / 2;
        icon().paint(&painter, QRect(left, (height() - iconSize) / 2, iconSize, iconSize), Qt::AlignCenter,
                     isEnabled() ? QIcon::Normal : QIcon::Disabled, pressed ? QIcon::On : QIcon::Off);
        const QRect labelArea(left + iconSize + gap, 0, labelWidth, height());
        if (mode == Presentation::CompactAction) {
            drawOpticalPrivateText(painter, labelArea, label, Qt::AlignHCenter);
        } else {
            drawInterfaceText(painter, labelArea, label, Qt::AlignCenter);
        }
        return;
    }
    if (mode == Presentation::ListAction) {
        namespace c = m::chrome;
        QPainter painter(this);
        painter.fillRect(rect(), isEnabled() && isDown() ? themeColors(this).pressedPaper : palette().color(QPalette::Window));
        paintEdges(painter, rect(), rule, dividerColor(this));
        if (!isEnabled())
            painter.setOpacity(.45);
        painter.setPen(palette().color(QPalette::WindowText));
        QFont label = font();
        painter.setFont(label);
        const bool actionIcon = iconVisible;
        const int left = c::inset + (actionIcon ? c::cellIcon + px(20) : 0);
        // Balance the visible icon-to-border and icon-to-label gaps, including the glyph bearings.
        if (actionIcon)
            icon().paint(&painter,
                         QRect(c::inset - px(7), (height() - c::cellIcon) / 2 + px(1), c::cellIcon, c::cellIcon));
        const int previewWidth = previewFace.isEmpty() ? 0 : int(px(88));
        const int textWidth = qMax(0, width() - left - c::actionRight - (previewWidth ? previewWidth + px(20) : 0));
        // The label and its note are one block, centred on its letters. Their
        // line boxes keep the same centre-to-centre distance as before.
        const QFont noteFont = valueCaption ? interfaceFont(c::descriptionText) : captionFont();
        const int labelHeight = interfaceLineHeight(label), noteHeight = interfaceLineHeight(noteFont);
        const int pitch = px(40);
        const int titleHeight = note.isEmpty() ? labelHeight : labelHeight / 2 + pitch + noteHeight / 2;
        const auto descriptionFont = interfaceFont(c::descriptionText);
        const int descriptionHeight = description.isEmpty() ? 0
            : wrappedInterfaceTextHeight(description, descriptionFont, textWidth);
        const int blockHeight = titleHeight + (description.isEmpty() ? 0 : px(12) + descriptionHeight);
        const int top = (height() - blockHeight) / 2 - opticalCenterShift(label, note.isEmpty() ? label : noteFont);
        drawInterfaceText(painter, QRect(left, top, textWidth, labelHeight),
                          elideInterfaceText(text(), painter.font(), textWidth), Qt::AlignVCenter | Qt::AlignLeft);
        if (chevron) {
            const int size = px(28);
            controlIcon(ControlIcon::Next, palette().color(QPalette::WindowText))
                .paint(&painter, QRect(width() - c::chevronRight - size / 2, (height() - size) / 2, size, size));
        }
        if (!description.isEmpty()) {
            painter.setFont(descriptionFont);
            painter.setPen(noteColor(this));
            drawWrappedInterfaceText(painter,
                QRect(left, top + titleHeight + px(12), textWidth, descriptionHeight), description);
        }
        if (previewWidth) {
            painter.setFont(previewFont);
            drawOpticalText(painter, QRect(width() - c::actionRight - previewWidth, 0, previewWidth, height()),
                            QStringLiteral("Aa"), true, {});
        }
        if (!note.isEmpty()) {
            painter.setFont(noteFont);
            painter.setPen(noteColor(this));
            drawInterfaceText(painter,
                              QRect(left, top + labelHeight / 2 + pitch - noteHeight / 2, textWidth, noteHeight),
                              elideInterfaceText(note, painter.font(), textWidth), Qt::AlignLeft | Qt::AlignVCenter);
        }
        return;
    }
    namespace c = m::chrome;
    QPainter painter(this);
    painter.fillRect(rect(), isEnabled() && isDown() ? themeColors(this).pressedPaper : palette().color(QPalette::Window));
    paintEdges(painter, rect(), rule, dividerColor(this));
    const auto ink = palette().color(QPalette::WindowText);
    controlIcon(glyph, ink).paint(&painter, QRect(c::inset, (height() - c::cellIcon) / 2, c::cellIcon, c::cellIcon));
    const int left = c::inset + c::cellIcon + px(24), right = chevron ? int(c::actionRight) : int(c::inset);
    const int available = qMax(0, width() - left - right);
    const auto labelFont = interfaceFont(c::labelText), noteFont = captionFont(px(20));
    const int labelHeight = interfaceLineHeight(labelFont),
              noteHeight = note.isEmpty() ? 0 : interfaceLineHeight(noteFont);
    const int gap = note.isEmpty() ? 0 : px(2), top = (height() - labelHeight - gap - noteHeight) / 2;
    painter.setPen(ink);
    painter.setFont(labelFont);
    drawInterfaceText(painter, QRect(left, top, available, labelHeight),
                      elideInterfaceText(text(), painter.font(), available), Qt::AlignVCenter);
    if (!note.isEmpty()) {
        painter.setFont(noteFont);
        painter.setPen(noteColor(this));
        drawInterfaceText(painter, QRect(left, top + labelHeight + gap, available, noteHeight),
                          elideInterfaceText(note, painter.font(), available), Qt::AlignVCenter);
    }
    if (chevron) {
        const int size = px(28);
        controlIcon(ControlIcon::Next, ink)
            .paint(&painter, QRect(width() - c::chevronRight - size / 2, (height() - size) / 2, size, size));
    }
}
void DismissButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto ink = palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid);
    const auto paper = palette().color(QPalette::Window);
    const bool pressed = isEnabled() && isDown();
    const int side = px(44);
    const QPoint center = contentsRect().center();
    painter.fillRect(contentsRect(), pressed ? ink : paper);
    painter.fillRect(QRect(contentsRect().left(), contentsRect().top(), m::control::border, contentsRect().height()),
                     palette().color(QPalette::Mid));
    const QRect bounds(center.x() - side / 2, center.y() - side / 2, side, side);
    controlIcon(ControlIcon::Close, pressed ? paper : ink).paint(&painter, bounds);
}
namespace {
QStringList cellLabelLines(const QString &text, const QFont &font, int available, int maximumLines) {
    if (maximumLines == 1 || interfaceTextWidth(text, font) <= available)
        return {elideInterfaceText(text, font, available)};
    int split = -1, bestWidth = std::numeric_limits<int>::max();
    for (int i = 1; i < text.size() - 1; ++i) {
        if (!text.at(i).isSpace())
            continue;
        const int width = qMax(interfaceTextWidth(text.left(i).trimmed(), font),
                               interfaceTextWidth(text.mid(i + 1).trimmed(), font));
        if (width < bestWidth) {
            bestWidth = width;
            split = i;
        }
    }
    if (split < 0)
        return {elideInterfaceText(text, font, available)};
    return {elideInterfaceText(text.left(split).trimmed(), font, available),
            elideInterfaceText(text.mid(split + 1).trimmed(), font, available)};
}
}
CellButton::CellButton(ControlIcon icon, QWidget *parent, int iconPixels)
    : Button(parent), glyph(icon), iconPixels(iconPixels) {
    setFocusPolicy(Qt::NoFocus);
}
QSize CellButton::sizeHint() const {
    if (text().isEmpty())
        return {m::chrome::cellWidth + (dropdownIndicator ? px(8) + int(m::chrome::selectorIcon) : 0),
                m::chrome::actionHeight};
    // Paint with our own font; the reader's button style sets a larger size.
    QFont label = interfaceFont(m::chrome::countText);
    label.setCapitalization(QFont::AllUppercase);
    return {m::chrome::inset + trailingInset + interfaceTextWidth(text().toUpper(), label) + m::chrome::selectorGap + iconPixels,
            m::chrome::actionHeight};
}
int CellButton::widthForLabel(int maximumWidth) const {
    if (text().isEmpty())
        return qMin(maximumWidth, sizeHint().width());
    QFont label = interfaceFont(m::chrome::countText);
    label.setCapitalization(QFont::AllUppercase);
    const int reserve = m::chrome::inset + trailingInset + m::chrome::selectorGap + iconPixels;
    const auto lines = cellLabelLines(text().toUpper(), label, qMax(0, maximumWidth - reserve), maximumLabelLines);
    int width = 0;
    for (const auto &line : lines)
        width = qMax(width, interfaceTextWidth(line, label));
    return qMin(maximumWidth, reserve + width);
}
void CellButton::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const bool pressed = isEnabled() && isDown();
    const bool active = isEnabled() && (isChecked() || (pressed && !grayPress));
    const auto paper = palette().color(inverted ? QPalette::WindowText : QPalette::Window);
    const auto ink = palette().color(inverted ? QPalette::Window : QPalette::WindowText);
    if (circular) {
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal inset = int(m::control::border) / 2.0;
        if (active) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawEllipse(QRectF(rect()).adjusted(inset, inset, -inset, -inset));
        }
        if (!isEnabled()) painter.setOpacity(.45);
        controlIcon(glyph, active ? paper : ink).paint(&painter,
            QRect((width() - iconPixels) / 2, (height() - iconPixels) / 2, iconPixels, iconPixels));
        return;
    }
    painter.fillRect(rect(), active ? ink : pressed && grayPress ? themeColors(this).pressedPaper : paper);
    paintEdges(painter, rect(), rule, palette().color(QPalette::Mid));
    if (!isEnabled())
        painter.setOpacity(.45);
    const auto color = active ? paper : ink;
    if (text().isEmpty()) {
        const int indicator = dropdownIndicator ? int(m::chrome::selectorIcon) : 0;
        const int gap = dropdownIndicator ? px(8) : 0;
        const int left = (width() - iconPixels - gap - indicator) / 2;
        controlIcon(glyph, color)
            .paint(&painter, QRect(left, (height() - iconPixels) / 2, iconPixels, iconPixels));
        if (dropdownIndicator)
            controlIcon(ControlIcon::Down, color)
                .paint(&painter, QRect(left + iconPixels + gap, (height() - indicator) / 2, indicator, indicator));
        return;
    }
    QFont label = interfaceFont(m::chrome::countText);
    label.setCapitalization(QFont::AllUppercase);
    painter.setFont(label);
    painter.setPen(color);
    const int available = qMax(0, width() - m::chrome::inset - trailingInset - m::chrome::selectorGap - iconPixels);
    const auto lines = cellLabelLines(text().toUpper(), label, available, maximumLabelLines);
    const int lineHeight = interfaceLineHeight(label);
    const int top = (height() - int(lines.size()) * lineHeight) / 2;
    int labelWidth = 0;
    for (int i = 0; i < lines.size(); ++i) {
        drawOpticalText(painter, QRect(m::chrome::inset, top + i * lineHeight, available, lineHeight), lines[i]);
        labelWidth = qMax(labelWidth, interfaceTextWidth(lines[i], label));
    }
    const int x = m::chrome::inset + labelWidth + m::chrome::selectorGap;
    controlIcon(glyph, color).paint(&painter, QRect(x, (height() - iconPixels) / 2, iconPixels, iconPixels));
}

}
