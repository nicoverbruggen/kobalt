#pragma once
#include <QRegion>
#include <QVector>
#include <QWidget>

namespace kobalt {
// Groups the rectangles of one completed paint by the reader panel that shows
// them. MTK refreshes each rectangle separately, so a slider and its value
// label would otherwise appear in two screen updates. Damage inside one panel
// becomes that panel's bounding rectangle. Page rectangles and rectangles in
// different panels stay separate: merging them would refresh unchanged book
// text or bars between them.
inline QVector<QRect> panelDamage(const QWidget *window, const QWidget *page, const QRegion &region)
{
    QVector<QRect> result;
    QVector<const QWidget *> owners;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const auto rects = region.rects();
#else
    const auto &rects = region;
#endif
    for (const auto &rect : rects) {
        const QWidget *owner = window->childAt(rect.center());
        while (owner && owner->parentWidget() != window)
            owner = owner->parentWidget();
        if (owner == page)
            owner = nullptr;
        const int index = owner ? owners.indexOf(owner) : -1;
        if (index >= 0) {
            result[index] |= rect;
            continue;
        }
        owners.append(owner);
        result.append(rect);
    }
    return result;
}
} // namespace kobalt
