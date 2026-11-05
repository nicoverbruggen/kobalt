#include "platforms/kobo/common/lifecycle_test.h"
#ifndef KOBALT_READER_HARDWARE
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QWindow>
#include <array>
#include <dlfcn.h>

namespace kobalt::nickel {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
}
void sendTestPageButton(const QJsonObject &command, bool blocked) {
    const int key = command.value("button").toString() == "upper" ? Qt::Key_F23 : Qt::Key_F24;
    const bool repeat = command.value("repeat").toBool();
    // ReadingSettings uses a 44-byte Settings base with its derived vtable.
    // Use its setter so Nickel's settings cache sees the test preference.
    std::array<void *, 11> settings{};
    const auto device = symbol<const void *(*)()>("_ZN6Device16getCurrentDeviceEv");
    const auto construct = symbol<void (*)(void *, const void *, bool)>("_ZN8SettingsC2ERK6Deviceb");
    const auto destroy = symbol<void (*)(void *)>("_ZN8SettingsD2Ev");
    const auto vtable = symbol<void **>("_ZTV15ReadingSettings");
    const auto get = symbol<bool (*)(void *)>("_ZN15ReadingSettings21invertPageTurnButtonsEv");
    const auto set = symbol<void (*)(void *, bool)>("_ZN15ReadingSettings24setInvertPageTurnButtonsEb");
    const bool override =
        command.contains("invert") && !blocked && device && construct && destroy && vtable && get && set;
    bool saved = false;
    if (override) {
        construct(settings.data(), device(), false);
        settings[0] = vtable + 2;
        saved = get(settings.data());
        set(settings.data(), command.value("invert").toBool());
    }
    // Enter Qt through its focused window, including Nickel's input filters.
    if (auto *target = QGuiApplication::focusWindow()) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, {}, repeat);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, {}, repeat);
        QCoreApplication::sendEvent(target, &press);
        QCoreApplication::sendEvent(target, &release);
    }
    if (override) {
        set(settings.data(), saved);
        destroy(settings.data());
    }
}
}
#endif
