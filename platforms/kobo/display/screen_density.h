#pragma once
#include <QByteArray>
#include <QString>

namespace kobalt {
struct ScreenDensity
{
    int ppi = 300;
    // "product", "screen" or "default": what the value came from.
    QString source;
};

// The screen's pixels per inch. Kobo's own Qt platform plugin picks the density
// from the PRODUCT variable that the firmware exports, so Kobalt uses the same
// name first. An unknown product falls back to the screen's logical DPI when it
// looks real, and then to 300, the density of the Clara and Libra.
ScreenDensity screenDensity(const QByteArray &product, qreal logicalDpi);
// The density of this device from PRODUCT and the primary screen. Logs a
// `ui-density` event the first time and whenever the value changes.
int currentScreenDensity();
} // namespace kobalt
