#pragma once
#include <QObject>
#include <QVariant>

namespace kindle {
// The overlay marks its generation only after painting. An unrelated pending
// flush must not freeze the display before that image reaches the framebuffer.
inline quint64 requestSleepFrame(QObject &window) {
    auto generation = window.property("kobalt.sleepRequested").toULongLong() + 1;
    if (!generation)
        ++generation;
    window.setProperty("kobalt.displaySleeping", true);
    window.setProperty("kobalt.sleepRequested", generation);
    return generation;
}
inline void sleepFramePainted(QObject &window, quint64 generation) {
    if (window.property("kobalt.displaySleeping").toBool() &&
        window.property("kobalt.sleepRequested").toULongLong() == generation)
        window.setProperty("kobalt.sleepPainted", generation);
}
inline bool displayFlushAllowed(const QObject &window, const QObject *sleepingSurface = nullptr) {
    if (sleepingSurface && sleepingSurface != &window && sleepingSurface->property("kobalt.displaySleeping").toBool())
        return false;
    if (!window.property("kobalt.displaySleeping").toBool())
        return true;
    const auto generation = window.property("kobalt.sleepRequested").toULongLong();
    return generation && window.property("kobalt.sleepPainted").toULongLong() == generation &&
        window.property("kobalt.sleepPresented").toULongLong() != generation;
}
inline void sleepFramePresented(QObject &window, quint64 generation) {
    if (generation && window.property("kobalt.sleepRequested").toULongLong() == generation &&
        window.property("kobalt.displaySleeping").toBool() && displayFlushAllowed(window))
        window.setProperty("kobalt.sleepPresented", generation);
}
inline void wakeDisplay(QObject &window) {
    window.setProperty("kobalt.displaySleeping", false);
    // Invalidate a late paint from the previous sleep cycle.
    auto generation = window.property("kobalt.sleepRequested").toULongLong() + 1;
    if (!generation)
        ++generation;
    window.setProperty("kobalt.sleepRequested", generation);
}
}
