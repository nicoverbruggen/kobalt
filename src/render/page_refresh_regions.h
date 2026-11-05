#pragma once

#include <QImage>
#include <QRegion>

namespace kobalt {

inline bool hasVisibleColor(QRgb pixel)
{
    const int red = qRed(pixel), green = qGreen(pixel), blue = qBlue(pixel);
    return qMax(red, qMax(green, blue)) - qMin(red, qMin(green, blue)) > 10;
}

// Return the page areas that need a full refresh after a page turn.
QRegion pageRefreshRegions(const QImage &image, bool colorDevice);
// Image-viewer targets use displayed size, independently of source resolution.
bool pageImageAtPoint(const QImage &image, QPoint point);
// Empty unless the page has exactly one eligible image and no body text.
QRect pageSingleImage(const QImage &image);

} // namespace kobalt
