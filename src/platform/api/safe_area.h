#pragma once
#include <QtGlobal>
#include <QtMath>

namespace kobalt {
// The bands of the reader's rectangle that the screen does not show in full,
// in device pixels: a camera cutout or system bar at the top and bottom, and
// rounded screen corners. Paper runs under them; text and controls do not.
// Hosts keep side cutouts outside the reader, as margins in the page colour.
struct SafeArea
{
    int top = 0, bottom = 0;
    int cornerRadius = 0;

    bool operator==(const SafeArea &other) const
    {
        return top == other.top && bottom == other.bottom && cornerRadius == other.cornerRadius;
    }

    bool operator!=(const SafeArea &other) const { return !(*this == other); }
};

// How far a rounded corner reaches in from the side at `distance` pixels
// from the top or bottom edge. Text that starts closer to the side is cut off.
inline int cornerInset(int radius, int distance)
{
    if (radius <= 0 || distance >= radius)
        return 0;
    const qreal rise = radius - qMax(0, distance);
    return qCeil(radius - qSqrt(qreal(radius) * radius - rise * rise));
}
} // namespace kobalt
