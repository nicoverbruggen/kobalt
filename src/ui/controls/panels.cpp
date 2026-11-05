#include "ui/controls/text.h"
#include "panels.h"
#include "icons.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "support/strings.h"
#include <QPainter>
#include <QResizeEvent>
#include <QPainterPath>
#include <QVariant>
#include <cmath>

namespace kobalt::ui {
void paintBrandIcon(QPainter &painter, const QRectF &bounds, const QColor &ink)
{
    painter.save();
    painter.translate(bounds.topLeft());
    painter.scale(bounds.width() / metrics::opening::icon.width(),
                  bounds.height() / metrics::opening::icon.height());
    for (int line = 0; line < 6; ++line)
        painter.fillRect(
                QRectF(0, line * metrics::opening::iconBarPitch,
                       line == 5 ? metrics::opening::finalBarWidth : metrics::opening::icon.width(),
                       metrics::opening::iconBarHeight),
                ink);
    painter.restore();
}

void BrandIcon::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto size = metrics::opening::icon.size().scaled(rect().size(), Qt::KeepAspectRatio);
    paintBrandIcon(painter,
                   QRectF((width() - size.width()) / 2, (height() - size.height()) / 2,
                          size.width(), size.height()),
                   palette().color(QPalette::WindowText));
}

ProgressBar::ProgressBar(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(metrics::notice::progressHeight + 2 * metrics::control::border);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

QRect ProgressBar::fillRect(int value) const
{
    const int border = metrics::control::border;
    const QRect inside = rect().adjusted(border, border, -border, -border);
    const int filled = qMax(0, inside.width()) * value / 100;
    return QRect(layoutDirection() == Qt::RightToLeft ? inside.right() - filled + 1 : inside.left(),
                 inside.top(), filled, qMax(0, inside.height()));
}

void ProgressBar::setValue(int value)
{
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

void ProgressBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const int border = metrics::control::border;
    painter.fillRect(rect(), palette().color(QPalette::WindowText));
    painter.fillRect(rect().adjusted(border, border, -border, -border),
                     palette().color(QPalette::Window));
    painter.fillRect(fillRect(percent), palette().color(QPalette::WindowText));
}

LightSwipeNotice::LightSwipeNotice(QWidget *parent)
    : Panel(parent, Edges(Qt::LeftEdge) | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge)
{
    setEdgeWidth(metrics::chrome::heavyRule);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    percentage = new ui::InterfaceLabel(this);
    percentage->setObjectName("rrLightSwipeValue");
    caption = new ui::InterfaceLabel(this);
    caption->setObjectName("rrLightSwipeCaption");
    readerStyle(percentage, styles::font(metrics::lightSwipe::percentText));
    readerStyle(caption,
                styles::font(metrics::lightSwipe::captionText) + "color:@swipe-caption-ink@;");
    auto font = interfaceFont(metrics::lightSwipe::captionText);
    font.setLetterSpacing(QFont::AbsoluteSpacing, px(2));
    caption->setFont(font);
    setLevel(false, 0);
}

void LightSwipeNotice::setLevel(bool warmth, int value)
{
    const int next = qBound(0, value, 100);
    const bool changed = warm != warmth || level != next;
    warm = warmth;
    level = next;
    percentage->setText(QString::number(level) + '%');
    caption->setText(
            (warm ? strings::get("light_panel.warmth") : strings::get("light_panel.brightness"))
                    .toUpper());
    setAccessibleName(
            (warm ? strings::get("reader.warmth") : strings::get("reader.brightness")).arg(level));
    if (changed)
        update();
}

void LightSwipeNotice::centerIn(const QSize &viewport)
{
    const int cardWidth = qMin(int(metrics::lightSwipe::width),
                               qMax(1, viewport.width() - 2 * int(metrics::spacing::section)));
    const int cardHeight = qMin(int(metrics::lightSwipe::height), viewport.height());
    setGeometry((viewport.width() - cardWidth) / 2, (viewport.height() - cardHeight) / 2, cardWidth,
                cardHeight);
}

void LightSwipeNotice::resizeEvent(QResizeEvent *event)
{
    Panel::resizeEvent(event);
    const int x =
            metrics::lightSwipe::inset + metrics::lightSwipe::iconWidth + metrics::lightSwipe::gap;
    const int top = (height()
                     - int(metrics::lightSwipe::percentText + metrics::lightSwipe::captionGap
                           + metrics::lightSwipe::captionText))
            / 2;
    percentage->setGeometry(x, top, qMax(0, width() - x - int(metrics::lightSwipe::inset)),
                            metrics::lightSwipe::percentText);
    caption->setGeometry(x,
                         top + metrics::lightSwipe::percentText + metrics::lightSwipe::captionGap,
                         qMax(0, width() - x - int(metrics::lightSwipe::inset)),
                         metrics::lightSwipe::captionText);
}

void LightSwipeNotice::paintEvent(QPaintEvent *event)
{
    Panel::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal side = warm ? 64. : 72.;
    const qreal scale = qMin(qreal(metrics::lightSwipe::iconWidth) / side,
                             qreal(metrics::lightSwipe::iconHeight) / side);
    painter.translate(metrics::lightSwipe::inset
                              + (metrics::lightSwipe::iconWidth - side * scale) / 2.,
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
            const qreal angle = i * 3.141592653589793 / 4, x = 32 + 19 * std::cos(angle),
                        y = 32 + 19 * std::sin(angle);
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

BluetoothIndicator::BluetoothIndicator(QWidget *parent) : QWidget(parent)
{
    setFixedSize(metrics::panel::bluetoothIconSize, metrics::panel::statusHeight);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAccessibleName(strings::get("components.bluetooth_enabled"));
    hide();
}

WifiIndicator::WifiIndicator(QWidget *parent) : QWidget(parent)
{
    const int width = metrics::panel::wifiIconWidth;
    const int reference = metrics::panel::bluetoothIconSize;
    // Equal width parity keeps their centers on the same pixel grid.
    setFixedSize(width + (width - reference) % 2, metrics::panel::statusHeight);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
}

bool WifiIndicator::setStatus(bool radio, bool online, int bars)
{
    online = radio && online;
    bars = online && bars >= 0 ? qBound(0, bars, 4) : -1;
    if (enabled == radio && connected == online && strength == bars)
        return false;
    enabled = radio;
    connected = online;
    strength = bars;
    setAccessibleName(online && bars >= 0
                              ? strings::get("components.wifi_signal").arg(bars)
                              : strings::get(!radio            ? "components.wifi_off"
                                                     : !online ? "components.wifi_disconnected"
                                                               : "components.wifi_connected"));
    update();
    return true;
}

void WifiIndicator::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const int side = metrics::panel::bluetoothIconSize;
    const ControlIcon levels[] = { ControlIcon::WifiNone, ControlIcon::WifiLow,
                                   ControlIcon::WifiMedium, ControlIcon::WifiHigh };
    // Map Nickel's 0..4 reports to Phosphor's four signal silhouettes.
    const auto icon = !connected ? ControlIcon::WifiDisconnected
            : strength < 0       ? ControlIcon::WifiNone
                                 : levels[(strength * 3 + 2) / 4];
    paintWifiStatusIcon(painter, icon, QRectF(0, (height() - side) / 2. - px(1), width(), side),
                        palette().color(QPalette::WindowText));
}

void BluetoothIndicator::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const int side = metrics::panel::bluetoothIconSize;
    paintOpeningIcon(painter, ControlIcon::Bluetooth, QRectF(0, (height() - side) / 2., side, side),
                     palette().color(QPalette::WindowText));
}

BatteryIndicator::BatteryIndicator(QWidget *parent) : InterfaceLabel(parent)
{
    setFixedSize(metrics::panel::batteryWidth, metrics::panel::statusHeight);
    setContentsMargins(0, 0, metrics::panel::batteryTextInset, 0);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setStatus(-1, false);
}

void BatteryIndicator::setStatus(int value, bool pluggedIn)
{
    value = value >= 0 && value <= 100 ? value : -1;
    if (percent == value && charging == pluggedIn)
        return;
    percent = value;
    charging = pluggedIn;
    setText(percent < 0 ? strings::get("components.battery_unknown")
                        : strings::get("common.percent").arg(percent));
    setAccessibleName(percent < 0
                              ? strings::get("components.battery_level_unavailable")
                              : strings::get("components.battery_percent")
                                        .arg(percent)
                                        .arg(charging ? strings::get("components.charging") : ""));
    update();
}

AnnotationModeNotice::AnnotationModeNotice(QWidget *parent)
    : Panel(parent, Edges(Qt::LeftEdge) | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge)
{
    setEdgeWidth(metrics::chrome::heavyRule);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    state = new InterfaceLabel(this);
    state->setObjectName("rrAnnotationModeState");
    caption = new InterfaceLabel(strings::get("annotations.mode").toUpper(), this);
    readerStyle(state, styles::font(metrics::lightSwipe::percentText));
    readerStyle(caption,
                styles::font(metrics::chrome::descriptionText)
                        + "color:palette(placeholder-text);");
    setModeEnabled(false);
}

void AnnotationModeNotice::setModeEnabled(bool enabled)
{
    modeEnabled = enabled;
    state->setText(strings::get(enabled ? "common.on" : "common.off"));
    setAccessibleName(strings::get("annotations.mode") + ": " + state->text());
    update();
}

void AnnotationModeNotice::centerIn(const QSize &viewport)
{
    const int w = qMin(int(metrics::lightSwipe::width),
                       qMax(1, viewport.width() - 2 * int(metrics::spacing::section)));
    const int h = qMin(int(metrics::lightSwipe::height), viewport.height());
    setGeometry((viewport.width() - w) / 2, (viewport.height() - h) / 2, w, h);
}

void AnnotationModeNotice::resizeEvent(QResizeEvent *event)
{
    Panel::resizeEvent(event);
    const int x =
            metrics::lightSwipe::inset + metrics::lightSwipe::iconWidth + metrics::lightSwipe::gap;
    const int top = (height()
                     - int(metrics::lightSwipe::percentText + metrics::lightSwipe::captionGap
                           + metrics::lightSwipe::captionText))
            / 2;
    const int w = qMax(0, width() - x - int(metrics::lightSwipe::inset));
    state->setGeometry(x, top, w, metrics::lightSwipe::percentText);
    caption->setGeometry(x,
                         top + metrics::lightSwipe::percentText + metrics::lightSwipe::captionGap,
                         w, metrics::lightSwipe::captionText);
}

void AnnotationModeNotice::paintEvent(QPaintEvent *event)
{
    Panel::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int side = metrics::lightSwipe::iconWidth;
    const QRect icon(metrics::lightSwipe::inset, (height() - side) / 2, side, side);
    const auto ink = palette().color(QPalette::WindowText);
    paintOpeningIcon(painter, ControlIcon::Annotate, icon, ink);
    if (!modeEnabled) {
        const auto slash = QLineF(icon.topLeft() + QPoint(px(8), px(8)),
                                  icon.bottomRight() - QPoint(px(8), px(8)));
        painter.setPen(
                QPen(palette().color(QPalette::Window), px(14), Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(slash);
        painter.setPen(QPen(ink, px(6), Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(slash);
    }
}

void paintBattery(QPainter &painter, const QRectF &body, int percent, bool charging,
                  const QColor &ink, const QColor &paper)
{
    // The menu's battery is the reference size; smaller ones scale its parts.
    const qreal scale = body.height() / int(metrics::panel::batteryBodyHeight);
    const auto part = [scale](int pixels) { return px(pixels) * scale; };
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ink, metrics::panel::batteryStroke));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(body);
    painter.fillRect(QRectF(body.right() + part(2), body.center().y() - part(4), part(3), part(8)),
                     ink);
    auto fill = body.adjusted(part(4), part(4), -part(4), -part(4));
    fill.setWidth(qRound(fill.width() * qMax(0, percent) / 100.0));
    painter.fillRect(fill, ink);
    if (charging)
        paintBatteryBolt(painter, body.center(), scale, ink, paper);
    painter.restore();
}

void paintBatteryBolt(QPainter &painter, const QPointF &center, qreal scale, const QColor &ink,
                      const QColor &paper)
{
    const QPolygonF bolt(
            QVector<QPointF>{ center + QPointF(2, -9) * scale, center + QPointF(-5, 1) * scale,
                              center + QPointF(0, 1) * scale, center + QPointF(-2, 9) * scale,
                              center + QPointF(5, -1) * scale, center + QPointF(0, -1) * scale });
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(paper, 2 * scale));
    painter.setBrush(ink);
    painter.drawPolygon(bolt);
    painter.restore();
}

qreal batteryWidth(qreal bodyHeight)
{
    const qreal scale = bodyHeight / int(metrics::panel::batteryBodyHeight);
    return (int(metrics::panel::batteryBodyWidth) + px(5)) * scale;
}

QFont batteryTextFont(QFont font, qreal bodyHeight, int weight)
{
    const auto sample = renderPrivateText(QStringLiteral("100%"), font, weight).image;
    int first = sample.height(), last = -1;
    for (int y = 0; y < sample.height(); ++y) {
        const auto *pixels = reinterpret_cast<const QRgb *>(sample.constScanLine(y));
        for (int x = 0; x < sample.width(); ++x)
            if (qAlpha(pixels[x]) > 128) {
                first = qMin(first, y);
                last = y;
                break;
            }
    }
    if (last >= first)
        font.setPixelSize(qMax(1, qRound(font.pixelSize() * bodyHeight / (last - first + 1))));
    return font;
}

void BatteryIndicator::paintEvent(QPaintEvent *event)
{
    const bool privateText = needsPrivateText(text(), font());
    if (!privateText)
        InterfaceLabel::paintEvent(event);
    QPainter painter(this);
    if (privateText) {
        painter.setFont(font());
        painter.setPen(palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid));
        // Align the visible digits with the icon, rather than the padded font line.
        drawOpticalPrivateText(painter, contentsRect(), text(), alignment());
    }
    const QRectF body(width() - metrics::panel::batteryIconSize + 1,
                      (height() - metrics::panel::batteryBodyHeight) / 2.0,
                      metrics::panel::batteryBodyWidth, metrics::panel::batteryBodyHeight);
    paintBattery(painter, body, percent, charging, palette().color(QPalette::WindowText),
                 palette().color(QPalette::Window));
}

Panel::Panel(QWidget *parent, Edges separators) : QFrame(parent), separators(separators)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void Panel::paintEvent(QPaintEvent *event)
{
    // Opaque widgets skip Qt's background fill. Paint our paper in the same
    // pass as the controls, without repainting the book underneath.
    {
        QPainter painter(this);
        painter.fillRect(rect(),
                         fill == QPalette::Midlight ? sectionColor(this) : palette().color(fill));
    }
    QFrame::paintEvent(event);
    QPainter painter(this);
    paintEdges(painter, rect(), separators,
               palette().color(softEdges ? QPalette::Mid : QPalette::WindowText),
               edgeWidth > 0 ? edgeWidth : int(metrics::control::border));
    paintEdges(painter, rect(), dividers, dividerColor(this), metrics::control::border);
}
} // namespace kobalt::ui
