#include "sleep_screen.h"

#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "support/strings.h"

#include <QFontMetrics>
#include <QPainter>
#include <QtMath>

namespace kobalt::reader {
QString sleepStatusText(SleepStatus status) {
    switch (status) {
    case SleepStatus::Charging:
        return strings::get("sleep.charging");
    case SleepStatus::Charged:
        return strings::get("sleep.charged");
    case SleepStatus::Sleeping:
        return strings::get("sleep.sleeping");
    }
    return {};
}
QString sleepTimeLeft(qint64 milliseconds) {
    if (milliseconds < 0) {
        return {};
    }
    const qint64 minutes = milliseconds / 60000 + (milliseconds % 60000 != 0);
    const auto time = minutes <= 120 ? strings::get("stats.duration_minutes").arg(minutes)
                                     : strings::get("stats.duration_hours").arg(minutes / 60).arg(minutes % 60);
    return strings::get("sleep.time_left").arg(time);
}
QImage sleepCoverBackground(const QImage &cover, QSize size, bool dark) {
    if (cover.isNull() || size.isEmpty() || qint64(size.width()) * size.height() > 4 * 1024 * 1024) {
        return {};
    }
    QImage background(size, QImage::Format_RGB32);
    if (background.isNull()) {
        return {};
    }
    background.fill(dark ? Qt::black : Qt::white);
    const auto fitted = cover.size().scaled(size, Qt::KeepAspectRatio);
    QPainter painter(&background);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRect(QPoint((size.width() - fitted.width()) / 2, (size.height() - fitted.height()) / 2), fitted),
                      cover);
    painter.end();
    return background;
}
QRect sleepScreenCardRect(QSize size, bool showRemaining) {
    namespace m = ui::metrics::sleep;
    const int inset = qMin(int(m::inset), size.width() / 12);
    const int padding = qMin(int(m::padding), size.width() / 16);
    const int titleHeight = QFontMetrics(ui::brandFont(m::titleText)).height();
    const int detailHeight = QFontMetrics(ui::interfaceFont(m::detailText)).height();
    const int height =
        padding * 2 + titleHeight + int(m::gap) + detailHeight + (showRemaining ? detailHeight + int(m::lineGap) : 0);
    return {inset, qMax(inset, size.height() - height - int(m::bottom)), qMin(int(m::width), size.width() - 2 * inset),
            height};
}
QImage sleepScreenImage(QImage page, const QString &progress, const QString &remaining, bool dark, int batteryPercent,
                        SleepStatus status, bool dimPage) {
    if (page.isNull()) {
        return {};
    }
    namespace m = ui::metrics::sleep;
    QPainter painter(&page);
    if (dimPage) {
        painter.fillRect(page.rect(), QColor(0, 0, 0, m::dimAlpha));
    }
    const int padding = qMin(int(m::padding), page.width() / 16);
    const QFont title = ui::brandFont(m::titleText);
    const QFont detail = ui::interfaceFont(m::detailText);
    const QFont batteryFont = ui::chromeFont(m::batteryText);
    const int titleHeight = QFontMetrics(title).height();
    const int detailHeight = QFontMetrics(detail).height();
    const QRect card = sleepScreenCardRect(page.size(), !remaining.isEmpty());
    const QColor paper(dark ? m::darkPaper : m::lightPaper);
    const QColor ink(dark ? Qt::white : Qt::black);
    painter.fillRect(card, paper);
    painter.setPen(QPen(ink, int(m::border)));
    painter.drawRect(card.adjusted(0, 0, -1, -1));
    const int left = card.left() + padding;
    const int width = card.width() - 2 * padding;
    int top = card.top() + padding;
    painter.setFont(title);
    const bool validBattery = batteryPercent >= 0 && batteryPercent <= 100;
    int batterySpace = 0;
    if (status != SleepStatus::Sleeping) {
        const qreal body = qRound(detail.pixelSize() * .75);
        const int iconWidth = qCeil(ui::batteryWidth(body));
        const int gap = m::batteryGap;
        const auto percentage = validBattery ? strings::get("common.percent").arg(batteryPercent)
                                             : strings::get("components.battery_unknown");
        const int textWidth = ui::interfaceTextWidth(percentage, batteryFont);
        const int right = left + width;
        const int batteryLeft = right - iconWidth;
        ui::paintBattery(
            painter,
            QRectF(batteryLeft, top + (titleHeight - body) / 2.0,
                   body * int(ui::metrics::panel::batteryBodyWidth) / int(ui::metrics::panel::batteryBodyHeight), body),
            validBattery ? batteryPercent : -1, status == SleepStatus::Charging, ink, paper);
        painter.setFont(batteryFont);
        ui::drawInterfaceText(painter, QRect(batteryLeft - gap - textWidth, top, textWidth, titleHeight), percentage);
        batterySpace = iconWidth + textWidth + 2 * gap;
        painter.setFont(title);
    }
    const int titleWidth = qMax(0, width - batterySpace);
    ui::drawInterfaceText(painter, QRect(left, top, titleWidth, titleHeight),
                          ui::elideInterfaceText(sleepStatusText(status), title, titleWidth));
    top += titleHeight + int(m::gap);
    painter.setFont(detail);
    ui::drawInterfaceText(painter, QRect(left, top, width, detailHeight),
                          ui::elideInterfaceText(progress, detail, width));
    if (!remaining.isEmpty()) {
        top += detailHeight + int(m::lineGap);
        ui::drawInterfaceText(painter, QRect(left, top, width, detailHeight),
                              ui::elideInterfaceText(remaining, detail, width));
    }
    painter.end();
    return page;
}
}
