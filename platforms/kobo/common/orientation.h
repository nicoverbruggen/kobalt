#pragma once
#include <QOrientationSensor>

namespace kobalt {
// Firmware integrations must validate the native ABI before borrowing a sensor.
QOrientationSensor *nativeOrientationSensor();
// Releasing ownership also applies any change queued by the native sensor owner.
void ownReaderRotation(bool owned);
void turnReaderScreen(Qt::ScreenOrientation orientation);
} // namespace kobalt
