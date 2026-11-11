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
// text or bars between them. Damage inside an open keyboard is its own group:
// a modal editor can share its top-level owner with the keyboard, and merging
// a key with the edited text would refresh everything between them.
inline QVector<QRect> panelDamage(const QWidget *window, const QWidget *page, const QRegion &region,
                                  const QRect &keyboard = QRect())
{
    QVector<QRect> result;
    QVector<const QWidget *> owners;
    int keyboardIndex = -1;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const auto rects = region.rects();
#else
    const auto &rects = region;
#endif
    for (const auto &rect : rects) {
        if (!keyboard.isEmpty() && keyboard.contains(rect)) {
            if (keyboardIndex >= 0) {
                result[keyboardIndex] |= rect;
            } else {
                keyboardIndex = result.size();
                owners.append(nullptr);
                result.append(rect);
            }
            continue;
        }
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
