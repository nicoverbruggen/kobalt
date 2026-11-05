#include "font_data.h"
#include <QVector>
#include <algorithm>
#include <cmath>

namespace kobalt::ui {
namespace {
struct Point {
    double x, y;
};
// FT_Outline_EmboldenXY in floating point, followed by the half-strength
// translation that font_weight.cpp applies to book text. The translation
// cancels FreeType's constant offset, so each point moves by its shift only.
void embolden(QVector<Point> &points, const QVector<int> &ends, double strength) {
    // The orientation of the whole outline, as FT_Outline_Get_Orientation.
    double area = 0;
    int first = 0;
    for (const int last : ends) {
        Point previous = points[last];
        for (int index = first; index <= last; ++index) {
            area += (points[index].y - previous.y) * (points[index].x + previous.x);
            previous = points[index];
        }
        first = last + 1;
    }
    if (area == 0)
        return;
    const bool truetype = area < 0;
    const double half = strength / 2;
    const auto original = points;
    first = 0;
    for (const int last : ends) {
        const auto next = [first, last](int index) {
            return index < last ? index + 1 : first;
        };
        Point in{0, 0}, out{0, 0}, anchor{0, 0};
        double inLength = 0, outLength = 0, anchorLength = 0;
        // j cycles through the points; i follows only when points move; k
        // marks the first moved point.
        for (int i = last, j = first, k = -1; j != i && i != k; j = next(j)) {
            if (j != k) {
                out = {original[j].x - original[i].x, original[j].y - original[i].y};
                outLength = std::hypot(out.x, out.y);
                if (outLength == 0)
                    continue;
                out = {out.x / outLength, out.y / outLength};
            } else {
                out = anchor;
                outLength = anchorLength;
            }
            if (inLength != 0) {
                if (k < 0) {
                    k = i;
                    anchor = in;
                    anchorLength = inLength;
                }
                Point shift{0, 0};
                const double turn = in.x * out.x + in.y * out.y;
                // Shift only where the contour turns by less than about 160 degrees.
                if (turn > -0.9375) {
                    const double d = turn + 1;
                    shift = {in.y + out.y, in.x + out.x};
                    if (truetype)
                        shift.x = -shift.x;
                    else
                        shift.y = -shift.y;
                    double q = out.x * in.y - out.y * in.x;
                    if (truetype)
                        q = -q;
                    // Limit the shift where short segments would collapse.
                    const double limit = std::min(inLength, outLength);
                    const double scale = half * q <= limit * d ? half / d : limit / q;
                    shift = {shift.x * scale, shift.y * scale};
                }
                for (; i != j; i = next(i))
                    points[i] = {original[i].x + shift.x, original[i].y + shift.y};
            } else {
                i = j;
            }
            in = out;
            inLength = outLength;
        }
        first = last + 1;
    }
}
}

QPainterPath weightedOutline(const QPainterPath &path, qreal strength) {
    if (strength <= 0 || path.isEmpty())
        return path;
    QVector<Point> points;
    QVector<int> ends;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        if (element.isMoveTo() && !points.isEmpty())
            ends.append(points.size() - 1);
        points.append({element.x, element.y});
    }
    ends.append(points.size() - 1);
    embolden(points, ends, strength);
    QPainterPath result;
    result.setFillRule(path.fillRule());
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        const auto point = points[i];
        if (element.isMoveTo())
            result.moveTo(point.x, point.y);
        else if (element.isLineTo())
            result.lineTo(point.x, point.y);
        else if (element.type == QPainterPath::CurveToElement) {
            result.cubicTo(point.x, point.y, points[i + 1].x, points[i + 1].y, points[i + 2].x, points[i + 2].y);
            i += 2;
        }
    }
    return result;
}
}
