#include "private_functions.h"
#include "touch_rotation.h"
#include "platforms/kobo/common/orientation.h"
#include <QGuiApplication>
#include <QEventLoop>
#include <QScreen>
#include <qpa/qwindowsysteminterface.h>
#include <dlfcn.h>

namespace kobalt {
void ownReaderRotation(bool owned)
{
    nickel_qt5::ownReaderTouchRotation(owned);
    if (!owned) {
        // The borrowed sensor's native handler queues its orientation change.
        // Finish it before the reader repaints and exposes Nickel's window.
        QWindowSystemInterface::flushWindowSystemEvents(QEventLoop::ExcludeUserInputEvents);
    }
}

QOrientationSensor *nativeOrientationSensor()
{
    const auto device = reinterpret_cast<const void *(*)()>(
            dlsym(RTLD_DEFAULT, "_ZN6Device16getCurrentDeviceEv"));
    const auto capable = reinterpret_cast<bool (*)(const void *)>(
            dlsym(RTLD_DEFAULT, "_ZNK6Device20hasOrientationSensorEv"));
    if (!device || !capable || !capable(device()))
        return nullptr;
    // An unexported singleton getter allocates a 24-byte QObject that owns its
    // QOrientationSensor at offset 8. Borrow it so two sensor backends cannot
    // compete for hardware. The getter is found by its code; see private_functions.h.
    const auto instance = reinterpret_cast<QObject *(*)()>(nickel_qt5::orientationService());
    if (!instance)
        return nullptr;
    auto *sensor = reinterpret_cast<QOrientationSensor *>(reinterpret_cast<char *>(instance()) + 8);
    return sensor->inherits("QOrientationSensor") ? sensor : nullptr;
}

void turnReaderScreen(Qt::ScreenOrientation orientation)
{
    auto *screen = QGuiApplication::primaryScreen();
    if (!screen || screen->orientation() == orientation)
        return;
    // Qt 5 drops an orientation that is not in the screen's update mask, and
    // filters the current one again whenever the mask changes. Nickel narrows
    // the mask outside its own reader, and its orientation owner widens it
    // before this same call. Keep it widened while the screen is turned, and
    // give Nickel's mask back once the screen returns to an orientation it allowed.
    static Qt::ScreenOrientations nickelMask;
    static bool widened = false;
    if (!widened)
        nickelMask = screen->orientationUpdateMask();
    widened = !(nickelMask & orientation);
    screen->setOrientationUpdateMask(nickelMask | orientation);
    QWindowSystemInterface::handleScreenOrientationChange(screen, orientation);
    // Apply the geometry before starting layout, including when waking a
    // fullscreen window that Nickel temporarily returned to portrait.
    QWindowSystemInterface::flushWindowSystemEvents(QEventLoop::ExcludeUserInputEvents);
    if (!widened)
        screen->setOrientationUpdateMask(nickelMask);
}
} // namespace kobalt
