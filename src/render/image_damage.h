#pragma once
#include <QImage>
#include <QRect>
#include <cstring>

namespace kobalt::ui {
// One rectangle bounds the changed pixels, so a text page does not turn into
// hundreds of separate panel submissions. Row padding is not display content.
inline QRect imageDamage(const QImage &before, const QImage &after) {
    if (before.cacheKey() == after.cacheKey())
        return {};
    const QRect all = before.rect().united(after.rect());
    if (before.size() != after.size() || before.format() != after.format() || before.depth() != 32 ||
        before.devicePixelRatio() != after.devicePixelRatio())
        return all;
    int left = after.width(), right = -1, top = after.height(), bottom = -1;
    for (int y = 0; y < after.height(); ++y) {
        const auto *a = reinterpret_cast<const quint32 *>(before.constScanLine(y));
        const auto *b = reinterpret_cast<const quint32 *>(after.constScanLine(y));
        if (std::memcmp(a, b, size_t(after.width()) * sizeof(quint32)) == 0)
            continue;
        int x = 0;
        // Only changes outside the bounds found so far can enlarge them.
        while (x < left && a[x] == b[x])
            ++x;
        left = qMin(left, x);
        x = after.width() - 1;
        while (x > right && a[x] == b[x])
            --x;
        right = qMax(right, x);
        top = qMin(top, y);
        bottom = y;
    }
    return right < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
}
