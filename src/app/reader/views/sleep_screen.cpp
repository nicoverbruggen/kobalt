#include "sleep_screen.h"
#include "reading/remaining_time.h"

#include "ui/controls/panels.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "support/strings.h"

#include <QPainter>
#include <QPolygonF>
#include <QtMath>

#include <cmath>

namespace kobalt::reader {
void portraitSleepSnapshot(sleep::Snapshot &snapshot)
{
    // Portrait reading keeps its top edge. Landscape still uses upright portrait.
    if (snapshot.orientation != Qt::InvertedPortraitOrientation)
        snapshot.orientation = Qt::PortraitOrientation;
    if (snapshot.image.width() <= snapshot.image.height())
        return;
    const QSize size(snapshot.image.height(), snapshot.image.width());
    if (snapshot.mode == sleep::Mode::Cover) {
        const QImage cover = snapshot.coverArea.isEmpty() ? snapshot.image
                                                          : snapshot.image.copy(snapshot.coverArea);
        snapshot.image = sleep::coverBackground(cover, size);
        snapshot.coverArea = sleep::coverArea(cover.size(), size);
        return;
    }
    QImage portrait(size, QImage::Format_RGB32);
    portrait.fill(snapshot.dark ? Qt::black : Qt::white);
    if (snapshot.mode != sleep::Mode::Plain) {
        QPainter painter(&portrait);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(sleep::coverArea(snapshot.image.size(), size), snapshot.image);
    }
    snapshot.image = std::move(portrait);
}

namespace {
namespace sleepMetrics = ui::metrics::sleep;

struct Content
{
    SleepStatus status = SleepStatus::Sleeping;
    QString progress, remaining;
    int percent = -1, batteryPercent = -1;

    bool showsBattery() const
    {
        return status == SleepStatus::Charging || status == SleepStatus::Charged;
    }
};

// The battery percentage followed by its icon, as one line.
struct Battery
{
    QString text;
    QFont font;
    qreal body = 0;
    int textWidth = 0, iconWidth = 0, gap = 0;

    int width() const { return textWidth + gap + iconWidth; }
};

QFont detailFont()
{
    return ui::interfaceFont(sleepMetrics::detailText);
}

Battery sleepBattery(int percent)
{
    Battery battery;
    const bool valid = percent >= 0 && percent <= 100;
    battery.text = valid ? strings::get("common.percent").arg(percent)
                         : strings::get("components.battery_unknown");
    battery.font = ui::chromeFont(sleepMetrics::batteryText);
    battery.body = qRound(detailFont().pixelSize() * .75);
    battery.textWidth = ui::interfaceTextWidth(battery.text, battery.font);
    battery.iconWidth = qCeil(ui::batteryWidth(battery.body));
    battery.gap = sleepMetrics::batteryGap;
    return battery;
}

// Paints the line from left, centered in a line of the given height.
void paintSleepBattery(QPainter &painter, const Battery &battery, int left, int top, int height,
                       const Content &content, const QColor &ink, const QColor &paper)
{
    const int iconLeft = left + battery.textWidth + battery.gap;
    const bool valid = content.batteryPercent >= 0 && content.batteryPercent <= 100;
    ui::paintBattery(painter,
                     QRectF(iconLeft, top + (height - battery.body) / 2.0,
                            battery.body * int(ui::metrics::panel::batteryBodyWidth)
                                    / int(ui::metrics::panel::batteryBodyHeight),
                            battery.body),
                     valid ? content.batteryPercent : -1, content.status == SleepStatus::Charging,
                     ink, paper);
    const QFont previous = painter.font();
    painter.setFont(battery.font);
    ui::drawInterfaceText(painter, QRect(left, top, battery.textWidth, height), battery.text);
    painter.setFont(previous);
}

void paintCard(QPainter &painter, const QRect &card, const Content &content, const QColor &ink,
               const QColor &paper, int screenWidth)
{
    const int padding = qMin(int(sleepMetrics::padding), screenWidth / 16);
    const QFont title = ui::brandFont(sleepMetrics::titleText);
    const QFont detail = detailFont();
    const int titleHeight = ui::interfaceLineHeight(title);
    const int detailHeight = ui::interfaceLineHeight(detail);
    painter.fillRect(card, paper);
    painter.setPen(QPen(ink, int(sleepMetrics::border)));
    painter.drawRect(card.adjusted(0, 0, -1, -1));
    const int left = card.left() + padding;
    const int width = card.width() - 2 * padding;
    int top = card.top() + padding;
    painter.setFont(title);
    int batterySpace = 0;
    if (content.showsBattery()) {
        const auto battery = sleepBattery(content.batteryPercent);
        paintSleepBattery(painter, battery, left + width - battery.width(), top, titleHeight,
                          content, ink, paper);
        batterySpace = battery.width() + battery.gap;
    }
    const int titleWidth = qMax(0, width - batterySpace);
    ui::drawInterfaceText(
            painter, QRect(left, top, titleWidth, titleHeight),
            ui::elideInterfaceText(sleepStatusText(content.status), title, titleWidth));
    top += titleHeight + int(sleepMetrics::gap);
    painter.setFont(detail);
    ui::drawInterfaceText(painter, QRect(left, top, width, detailHeight),
                          ui::elideInterfaceText(content.progress, detail, width));
    if (!content.remaining.isEmpty()) {
        top += detailHeight + int(sleepMetrics::lineGap);
        ui::drawInterfaceText(painter, QRect(left, top, width, detailHeight),
                              ui::elideInterfaceText(content.remaining, detail, width));
    }
}

// The ribbon's lines from its top edge. The height never depends on the
// status, so a battery update stays inside the same area.
struct RibbonLines
{
    QFont caption = ui::captionFont(sleepMetrics::captionText),
          figure = ui::brandFont(sleepMetrics::ribbonPercentText),
          detail = ui::interfaceFont(sleepMetrics::captionText),
          battery = ui::chromeFont(sleepMetrics::batteryText);
    int captionHeight = ui::interfaceLineHeight(caption),
        figureHeight = ui::interfaceLineHeight(figure),
        detailHeight = ui::interfaceLineHeight(detail),
        batteryHeight = ui::interfaceLineHeight(battery);

    int height() const
    {
        return int(sleepMetrics::ribbonTop) + captionHeight + int(sleepMetrics::ribbonGap)
                + int(sleepMetrics::border) + int(sleepMetrics::ribbonGap) / 2 + figureHeight
                + int(sleepMetrics::ribbonGap) / 2 + detailHeight + int(sleepMetrics::ribbonGap)
                + batteryHeight + int(sleepMetrics::ribbonGap) + int(sleepMetrics::ribbonNotch);
    }
};

// A bookmark hanging from the top edge, with a notch cut into its end.
void paintBookmark(QPainter &painter, const QRect &ribbon, const Content &content,
                   const QColor &fill, const QColor &ink)
{
    const qreal outline = int(sleepMetrics::ribbonOutline);
    const qreal left = ribbon.left(), right = ribbon.left() + ribbon.width(), top = ribbon.top();
    const qreal bottom = ribbon.top() + ribbon.height(), middle = (left + right) / 2,
                half = (right - left) / 2;
    const qreal notch = qMin<qreal>(int(sleepMetrics::ribbonNotch), ribbon.height() / 4);
    // The edges stay unsmoothed so painting the ribbon again gives the same pixels.
    const qreal rise = outline * std::hypot(half, notch) / half;
    const qreal slope = notch / half;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPolygon(QPolygonF(QVector<QPointF>{ { left, top },
                                                    { right, top },
                                                    { right, bottom },
                                                    { middle, bottom - notch },
                                                    { left, bottom } }));
    painter.setBrush(fill);
    painter.drawPolygon(
            QPolygonF(QVector<QPointF>{ { left + outline, top },
                                        { right - outline, top },
                                        { right - outline, bottom - outline * slope - rise },
                                        { middle, bottom - notch - rise },
                                        { left + outline, bottom - outline * slope - rise } }));
    painter.restore();
    const RibbonLines lines;
    const int padding = qMin(int(sleepMetrics::ribbonPadding), ribbon.width() / 8);
    const int textLeft = ribbon.left() + padding;
    const int width = ribbon.width() - 2 * padding;
    const auto centered = [&](const QString &text, const QFont &font, int y, int height) {
        painter.setFont(font);
        ui::drawInterfaceText(painter, QRect(textLeft, y, width, height),
                              ui::elideInterfaceText(text, font, width),
                              Qt::AlignHCenter | Qt::AlignVCenter);
    };
    painter.setPen(QPen(ink, int(sleepMetrics::border)));
    int y = ribbon.top() + int(sleepMetrics::ribbonTop);
    centered(sleepStatusText(content.status).toUpper(), lines.caption, y, lines.captionHeight);
    y += lines.captionHeight + int(sleepMetrics::ribbonGap);
    const int rule = qMin(int(sleepMetrics::ribbonRule), width);
    painter.fillRect(
            QRect(ribbon.left() + (ribbon.width() - rule) / 2, y, rule, int(sleepMetrics::border)),
            ink);
    y += int(sleepMetrics::border) + int(sleepMetrics::ribbonGap) / 2;
    if (content.percent >= 0)
        centered(strings::get("common.percent").arg(qMin(content.percent, 100)), lines.figure, y,
                 lines.figureHeight);
    else
        centered(content.progress, lines.detail, y, lines.figureHeight);
    y += lines.figureHeight + int(sleepMetrics::ribbonGap) / 2;
    if (!content.remaining.isEmpty())
        centered(content.remaining, lines.detail, y, lines.detailHeight);
    y += lines.detailHeight + int(sleepMetrics::ribbonGap);
    if (content.showsBattery()) {
        const auto battery = sleepBattery(content.batteryPercent);
        paintSleepBattery(painter, battery, ribbon.left() + (ribbon.width() - battery.width()) / 2,
                          y, lines.batteryHeight, content, ink, fill);
    }
}
} // namespace

QString sleepStatusText(SleepStatus status)
{
    switch (status) {
    case SleepStatus::PoweredOff:
        return strings::get("sleep.powered_off");
    case SleepStatus::Charging:
        return strings::get("sleep.charging");
    case SleepStatus::Charged:
        return strings::get("sleep.charged");
    case SleepStatus::Sleeping:
        return strings::get("sleep.sleeping");
    }
    return {};
}

QString sleepDuration(qint64 milliseconds, bool approximate)
{
    if (approximate)
        return remainingTime::approximateDuration(milliseconds);
    if (milliseconds < 0) {
        return {};
    }
    const qint64 minutes = milliseconds / 60000 + (milliseconds % 60000 != 0);
    return minutes <= 120
            ? strings::get("stats.duration_minutes").arg(minutes)
            : strings::get("stats.duration_hours").arg(minutes / 60).arg(minutes % 60);
}

QString sleepTimeLeft(qint64 milliseconds, bool approximate)
{
    const auto duration = sleepDuration(milliseconds, approximate);
    return duration.isEmpty() ? QString() : strings::get("sleep.time_left").arg(duration);
}

QImage sleepCoverBackground(const QImage &cover, QSize size)
{
    return sleep::coverBackground(cover, size);
}

QRect sleepScreenCardRect(QSize size, bool showRemaining, sleep::Tile tile)
{
    switch (tile) {
    case sleep::Tile::None:
        return {};
    case sleep::Tile::Bookmark: {
        const int width = qMin(int(sleepMetrics::ribbonWidth), size.width() / 3);
        const int right = qMin(int(sleepMetrics::ribbonRight), size.width() / 10);
        return { qMax(0, size.width() - right - width), 0, width,
                 qMin(size.height(), RibbonLines().height()) };
    }
    case sleep::Tile::Card:
        break;
    }
    const int inset = qMin(int(sleepMetrics::inset), size.width() / 12);
    const int padding = qMin(int(sleepMetrics::padding), size.width() / 16);
    const int titleHeight = ui::interfaceLineHeight(ui::brandFont(sleepMetrics::titleText));
    const int detailHeight = ui::interfaceLineHeight(detailFont());
    const int height = padding * 2 + titleHeight + int(sleepMetrics::gap) + detailHeight
            + (showRemaining ? detailHeight + int(sleepMetrics::lineGap) : 0);
    return { inset, qMax(inset, size.height() - height - int(sleepMetrics::bottom)),
             qMin(int(sleepMetrics::width), size.width() - 2 * inset), height };
}

void dimSleepPage(QImage &page, bool dark)
{
    QPainter painter(&page);
    painter.fillRect(page.rect(), ui::themeColors(dark).sleepDim);
}

void paintSleepTile(QPainter &painter, QSize size, const QString &progress,
                    const QString &remaining, bool dark, int batteryPercent, SleepStatus status,
                    sleep::Tile tile, int percent)
{
    if (tile == sleep::Tile::None)
        return;
    const bool poweredOff = status == SleepStatus::PoweredOff;
    const QColor paper =
            poweredOff ? ui::themeColors(true).paper : ui::themeColors(dark).sleepPaper;
    const QColor ink = ui::themeColors(poweredOff || dark).ink;
    const Content content{ status, progress, remaining, percent, batteryPercent };
    const QRect area = sleepScreenCardRect(size, !remaining.isEmpty(), tile);
    switch (tile) {
    case sleep::Tile::Card:
        paintCard(painter, area, content, ink, paper, size.width());
        break;
    case sleep::Tile::None:
        break;
    case sleep::Tile::Bookmark:
        // Keep a contrasting outline over either a page or a cover.
        paintBookmark(painter, area, content, paper, ink);
        break;
    }
}

QImage sleepScreenImage(QImage page, const QString &progress, const QString &remaining, bool dark,
                        int batteryPercent, SleepStatus status, bool dimPage, sleep::Tile tile,
                        int percent)
{
    if (page.isNull())
        return page;
    if (dimPage)
        dimSleepPage(page, dark);
    if (tile == sleep::Tile::None)
        return page;
    QPainter painter(&page);
    paintSleepTile(painter, page.size(), progress, remaining, dark, batteryPercent, status, tile,
                   percent);
    painter.end();
    return page;
}
} // namespace kobalt::reader
