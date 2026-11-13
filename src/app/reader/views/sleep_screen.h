#pragma once

#include "reading/sleep_screen.h"

#include <QImage>
#include <QString>
#include <QRect>
#include <QVector>

class QPainter;

namespace kobalt::reader {
enum class SleepStatus { Sleeping, Charging, Charged, PoweredOff };
QString sleepStatusText(SleepStatus status);
// Keep inverted portrait; fit landscape backdrops into upright portrait without
// changing the reading page.
void portraitSleepSnapshot(sleep::Snapshot &snapshot);
// Detailed rounds up to minutes, using hours and minutes above 120 minutes.
// Approximate rounds up to quarter-hours below an hour, then whole hours.
// Negative estimates have no label; Approximate also omits zero.
QString sleepTimeLeft(qint64 milliseconds, bool approximate = false);
// The same duration without "to go", for sentences that phrase it themselves.
QString sleepDuration(qint64 milliseconds, bool approximate = false);
// Fits the cover without cropping. Unused space is always black.
QImage sleepCoverBackground(const QImage &cover, QSize size);
// The area a tile covers. It does not depend on the status, so a battery
// update can repaint and refresh only this area.
QRect sleepScreenCardRect(QSize size, bool showRemaining, sleep::Tile tile = sleep::Tile::Card);
void dimSleepPage(QImage &page, bool dark);
// Takes ownership of one background and paints in place. The caller
// releases the completed frame to the host before the device suspends.
// percent is the reading progress for tiles that draw it; -1 leaves it out.
QImage sleepScreenImage(QImage page, const QString &progress, const QString &remaining, bool dark,
                        int batteryPercent = -1, SleepStatus status = SleepStatus::Sleeping,
                        bool dimPage = true, sleep::Tile tile = sleep::Tile::Card,
                        int percent = -1);

// Draws a tile on any painter, such as a scaled gallery preview.
void paintSleepTile(QPainter &painter, QSize size, const QString &progress,
                    const QString &remaining, bool dark, int batteryPercent, SleepStatus status,
                    sleep::Tile tile, int percent);

// The chosen design over its backdrop: the cover fitted with black bars, the
// page or a blank image for designs without a cover. Tiled designs draw the
// tile above; the others draw themselves. Takes ownership like sleepScreenImage.
// When `underlay` is given, it receives what lies under the status areas.
QImage sleepDesignImage(QImage backdrop, const sleep::Snapshot &snapshot, int batteryPercent,
                        SleepStatus status, QVector<QImage> *underlay = nullptr);
// The areas a battery change repaints. They do not depend on the status.
QVector<QRect> sleepStatusAreas(QSize size, const sleep::Snapshot &snapshot);
// What lies under the status areas before the design is drawn, so a battery
// change can restore it instead of drawing the status twice.
QVector<QImage> sleepStatusUnderlay(const QImage &backdrop, const QVector<QRect> &areas);
// Restores the underlay and draws the status again. Returns the area to refresh.
QRect repaintSleepStatus(QImage &screen, const QVector<QImage> &underlay,
                         const sleep::Snapshot &snapshot, int batteryPercent, SleepStatus status);
// A design drawn small for the gallery. The screen has `size`; `cover` is the
// book's cover and `page` the current page, either of which may be null.
void paintSleepPreview(QPainter &painter, const QRectF &target, QSize size, const QImage &cover,
                       const QImage &page, const QImage &overlay, const sleep::Snapshot &snapshot);
} // namespace kobalt::reader
