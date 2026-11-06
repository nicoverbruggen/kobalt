#pragma once

#include "reading/sleep_screen.h"

#include <QImage>
#include <QString>
#include <QRect>

namespace kobalt::reader {
enum class SleepStatus { Sleeping, Charging, Charged, PoweredOff };
QString sleepStatusText(SleepStatus status);
// Detailed rounds up to minutes, using hours and minutes above 120 minutes.
// Approximate rounds up to quarter-hours below an hour, then whole hours.
// Negative estimates have no label; Approximate also omits zero.
QString sleepTimeLeft(qint64 milliseconds, bool approximate = false);
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
} // namespace kobalt::reader
