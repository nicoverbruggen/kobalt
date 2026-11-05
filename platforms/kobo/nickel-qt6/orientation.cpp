#include "private_functions.h"
#include "platforms/kobo/common/orientation.h"
#include <QGuiApplication>
#include <QEventLoop>
#include <QScreen>
#include <qpa/qwindowsysteminterface.h>
#include <dlfcn.h>

namespace kobalt {
// Kobo's Qt6 evdev handler follows QScreen::orientationChanged, including on
// Clara devices. It needs no separate touch override while Kobalt rotates.
void ownReaderRotation(bool owned)
{
    if (!owned) {
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
    // An unexported singleton owns its QOrientationSensor at offset 8. Borrow it
    // so two sensor backends cannot compete for hardware. The getter is found
    // by its code; see private_functions.h.
    const auto instance = reinterpret_cast<QObject *(*)()>(nickel_qt6::orientationService());
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
    QWindowSystemInterface::handleScreenOrientationChange(screen, orientation);
    // Apply the geometry before starting layout, including when waking a
    // fullscreen window that Nickel temporarily returned to portrait.
    QWindowSystemInterface::flushWindowSystemEvents(QEventLoop::ExcludeUserInputEvents);
}

} // namespace kobalt
