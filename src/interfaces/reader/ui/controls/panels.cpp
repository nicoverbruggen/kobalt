#include "panels.h"
#include "icons.h"
#include "../style/metrics.h"
#include "../style/styles.h"
#include "../style/fonts.h"
#include "support/strings.h"
#include <QPainter>
#include <QResizeEvent>
#include <QPainterPath>
#include <QVariant>
#include <cmath>

namespace kobalt::ui {
namespace m = metrics;
void paintBrandIcon(QPainter &painter, const QRectF &bounds, const QColor &ink) {
    painter.save();
    painter.translate(bounds.topLeft());
    painter.scale(bounds.width() / m::opening::icon.width(), bounds.height() / m::opening::icon.height());
    for (int line = 0; line < 6; ++line)
        painter.fillRect(QRectF(0, line * m::opening::iconBarPitch,
                                line == 5 ? m::opening::finalBarWidth : m::opening::icon.width(),
                                m::opening::iconBarHeight),
                         ink);
    painter.restore();
}
void BrandIcon::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const auto size = m::opening::icon.size().scaled(rect().size(), Qt::KeepAspectRatio);
    paintBrandIcon(painter,
                   QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height()),
                   palette().color(QPalette::WindowText));
}
ProgressBar::ProgressBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(m::notice::progressHeight + 2 * m::control::border);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
}
QRect ProgressBar::fillRect(int value) const {
    const int border = m::control::border;
    const QRect inside = rect().adjusted(border, border, -border, -border);
    const int filled = qMax(0, inside.width()) * value / 100;
    return QRect(layoutDirection() == Qt::RightToLeft ? inside.right() - filled + 1 : inside.left(), inside.top(),
                 filled, qMax(0, inside.height()));
}
void ProgressBar::setValue(int value) {
    const int next = qBound(0, value, 100);
    if (percent == next) {
        return;
    }
    const QRegion changed = QRegion(fillRect(percent)).xored(QRegion(fillRect(next)));
    percent = next;
    if (!changed.isEmpty()) {
        update(changed);
    }
}
void ProgressBar::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const int border = m::control::border;
    painter.fillRect(rect(), palette().color(QPalette::WindowText));
    painter.fillRect(rect().adjusted(border, border, -border, -border), palette().color(QPalette::Window));
    painter.fillRect(fillRect(percent), palette().color(QPalette::WindowText));
}
LightSwipeNotice::LightSwipeNotice(QWidget *parent)
    : Panel(parent, Edges(Qt::LeftEdge) | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge) {
    setEdgeWidth(m::chrome::heavyRule);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    percentage = new QLabel(this);
    percentage->setObjectName("rrLightSwipeValue");
    caption = new QLabel(this);
    caption->setObjectName("rrLightSwipeCaption");
    readerStyle(percentage, styles::font(m::lightSwipe::percentText));
    readerStyle(caption, styles::font(m::lightSwipe::captionText) + "color:#666666;");
    auto font = interfaceFont(m::lightSwipe::captionText);
    font.setLetterSpacing(QFont::AbsoluteSpacing, px(2));
    caption->setFont(font);
    setLevel(false, 0);
}
void LightSwipeNotice::setLevel(bool warmth, int value) {
    const int next = qBound(0, value, 100);
    const bool changed = warm != warmth || level != next;
    warm = warmth;
    level = next;
    percentage->setText(QString::number(level) + '%');
    const auto style =
        styles::font(m::lightSwipe::captionText) + (darkTheme(this) ? "color:#aaaaaa;" : "color:#666666;");
    if (caption->property("reader_style").toString() != style)
        readerStyle(caption, style);
    caption->setText((warm ? strings::get("light_panel.warmth") : strings::get("light_panel.brightness")).toUpper());
    setAccessibleName((warm ? strings::get("reader.warmth") : strings::get("reader.brightness")).arg(level));
    if (changed)
        update();
}
void LightSwipeNotice::centerIn(const QSize &viewport) {
    const int cardWidth = qMin(int(m::lightSwipe::width), qMax(1, viewport.width() - 2 * int(m::spacing::section)));
    const int cardHeight = qMin(int(m::lightSwipe::height), viewport.height());
    setGeometry((viewport.width() - cardWidth) / 2, (viewport.height() - cardHeight) / 2, cardWidth, cardHeight);
}
void LightSwipeNotice::resizeEvent(QResizeEvent *event) {
    Panel::resizeEvent(event);
    const int x = m::lightSwipe::inset + m::lightSwipe::iconWidth + m::lightSwipe::gap;
    const int top =
        (height() - int(m::lightSwipe::percentText + m::lightSwipe::captionGap + m::lightSwipe::captionText)) / 2;
    percentage->setGeometry(x, top, qMax(0, width() - x - int(m::lightSwipe::inset)), m::lightSwipe::percentText);
    caption->setGeometry(x, top + m::lightSwipe::percentText + m::lightSwipe::captionGap,
                         qMax(0, width() - x - int(m::lightSwipe::inset)), m::lightSwipe::captionText);
}
void LightSwipeNotice::paintEvent(QPaintEvent *event) {
    Panel::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal side = warm ? 64. : 72.;
    const qreal scale = qMin(qreal(m::lightSwipe::iconWidth) / side, qreal(m::lightSwipe::iconHeight) / side);
    painter.translate(m::lightSwipe::inset + (m::lightSwipe::iconWidth - side * scale) / 2.,
                      (height() - side * scale) / 2.);
    painter.scale(scale, scale);
    if (!warm)
        painter.translate(4, 6);
    const QColor ink = palette().color(QPalette::WindowText);
    painter.setPen(QPen(ink, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    auto line = [&](qreal x, qreal y, qreal dx, qreal dy) {
        painter.drawLine(QPointF(x, y), QPointF(x + dx, y + dy));
    };
    if (warm) {
        painter.drawEllipse(QPointF(32, 32), 12, 12);
        for (int i = 0; i < 8; ++i) {
            const qreal angle = i * 3.141592653589793 / 4, x = 32 + 19 * std::cos(angle), y = 32 + 19 * std::sin(angle);
            if (level == 0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(ink);
                painter.drawEllipse(QPointF(x, y), 1.25, 1.25);
            } else {
                painter.setPen(QPen(ink, level >= 80 ? 3.5 : 2.5, Qt::SolidLine, Qt::RoundCap));
                painter.setBrush(Qt::NoBrush);
                line(x, y, 8. * level / 100 * std::cos(angle), 8. * level / 100 * std::sin(angle));
            }
        }
    } else {
        QPainterPath bulb;
        bulb.moveTo(23, 43);
        bulb.cubicTo(23, 36, 14, 33, 14, 23);
        bulb.arcTo(QRectF(14, 5, 36, 36), 180, -180);
        bulb.cubicTo(50, 33, 41, 36, 41, 43);
        bulb.closeSubpath();
        painter.drawPath(bulb);
        painter.save();
        painter.setClipPath(bulb);
        painter.fillRect(QRectF(12, 43 - 38. * level / 100, 40, 38. * level / 100), ink);
        painter.restore();
        line(24, 48, 16, 0);
        line(25, 53, 14, 0);
        line(28, 58, 8, 0);
        if (level >= 30) {
            line(8, 25, -5, 0);
            line(56, 25, 5, 0);
        }
        if (level >= 50) {
            line(8, 7, 3, 3);
            line(56, 7, -3, 3);
        }
        if (level >= 80)
            line(32, -4, 0, 3);
    }
}

BluetoothIndicator::BluetoothIndicator(QWidget *parent) : QWidget(parent) {
    setFixedSize(m::panel::bluetoothIconSize, m::panel::statusHeight);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAccessibleName(strings::get("components.bluetooth_enabled"));
    hide();
}
void BluetoothIndicator::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const int side = m::panel::bluetoothIconSize;
    paintOpeningIcon(painter, ControlIcon::Bluetooth, QRectF(0, (height() - side) / 2., side, side),
                     palette().color(QPalette::WindowText));
}

BatteryIndicator::BatteryIndicator(QWidget *parent) : QLabel(parent) {
    setFixedSize(m::panel::batteryWidth, m::panel::statusHeight);
    setContentsMargins(0, 0, m::panel::batteryTextInset, 0);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setStatus(-1, false);
}
void BatteryIndicator::setStatus(int value, bool pluggedIn) {
    value = value >= 0 && value <= 100 ? value : -1;
    if (percent == value && charging == pluggedIn)
        return;
    percent = value;
    charging = pluggedIn;
    setText(percent < 0 ? strings::get("components.battery_unknown") : strings::get("common.percent").arg(percent));
    setAccessibleName(percent < 0 ? strings::get("components.battery_level_unavailable")
                                  : strings::get("components.battery_percent")
                                        .arg(percent)
                                        .arg(charging ? strings::get("components.charging") : ""));
    update();
}
void paintBattery(QPainter &painter, const QRectF &body, int percent, bool charging, const QColor &ink,
                  const QColor &paper) {
    // The menu's battery is the reference size; smaller ones scale its parts.
    const qreal scale = body.height() / int(m::panel::batteryBodyHeight);
    const auto part = [scale](int pixels) {
        return px(pixels) * scale;
    };
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ink, m::panel::batteryStroke));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(body);
    painter.fillRect(QRectF(body.right() + part(2), body.center().y() - part(4), part(3), part(8)), ink);
    auto fill = body.adjusted(part(4), part(4), -part(4), -part(4));
    fill.setWidth(qRound(fill.width() * qMax(0, percent) / 100.0));
    painter.fillRect(fill, ink);
    if (charging) {
        const auto center = body.center();
        const QPolygonF bolt(QVector<QPointF>{center + QPointF(2, -9) * scale, center + QPointF(-5, 1) * scale,
                                              center + QPointF(0, 1) * scale, center + QPointF(-2, 9) * scale,
                                              center + QPointF(5, -1) * scale, center + QPointF(0, -1) * scale});
        painter.setPen(QPen(paper, 2 * scale));
        painter.setBrush(ink);
        painter.drawPolygon(bolt);
    }
    painter.restore();
}
qreal batteryWidth(qreal bodyHeight) {
    const qreal scale = bodyHeight / int(m::panel::batteryBodyHeight);
    return (int(m::panel::batteryBodyWidth) + px(5)) * scale;
}
void BatteryIndicator::paintEvent(QPaintEvent *event) {
    QLabel::paintEvent(event);
    QPainter painter(this);
    const QRectF body(width() - m::panel::batteryIconSize + 1, (height() - m::panel::batteryBodyHeight) / 2.0,
                      m::panel::batteryBodyWidth, m::panel::batteryBodyHeight);
    paintBattery(painter, body, percent, charging, palette().color(QPalette::WindowText),
                 palette().color(QPalette::Window));
}
Panel::Panel(QWidget *parent, Edges separators) : QFrame(parent), separators(separators) {
    setAttribute(Qt::WA_OpaquePaintEvent);
}
void Panel::paintEvent(QPaintEvent *event) {
    // Opaque widgets skip Qt's background fill. Paint our paper in the same
    // pass as the controls, without repainting the book underneath.
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(fill));
    }
    QFrame::paintEvent(event);
    QPainter painter(this);
    paintEdges(painter, rect(), separators, palette().color(softEdges ? QPalette::Mid : QPalette::WindowText),
               edgeWidth > 0 ? edgeWidth : int(m::control::border));
    paintEdges(painter, rect(), dividers, dividerColor(this), m::control::border);
}
}
