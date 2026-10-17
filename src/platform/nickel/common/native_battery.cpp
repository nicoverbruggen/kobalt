#include "platform/nickel/common/native_ui.h"
#include <QObject>
#include <dlfcn.h>
#include <utility>

namespace kobalt {
namespace {
class BatteryNotifications final : public QObject {
    Q_OBJECT
public:
    BatteryNotifications(QObject *context, std::function<void()> changed)
        : QObject(context), changed(std::move(changed)) {
        const auto hardware =
            reinterpret_cast<QObject *(*)()>(dlsym(RTLD_DEFAULT, "_ZN15HardwareFactory14sharedInstanceEv"));
        const auto plug = reinterpret_cast<QObject *(*)()>(dlsym(RTLD_DEFAULT, "_ZN11PlugManager14sharedInstanceEv"));
        // Queue notifications until Nickel has finished updating its state.
        // These signals also run while the native charging screen is visible.
        if (auto *object = hardware ? hardware() : nullptr) {
            connect(object, SIGNAL(battery_level(int)), this, SLOT(notify()), Qt::QueuedConnection);
            connect(object, SIGNAL(battery_changed()), this, SLOT(notify()), Qt::QueuedConnection);
        }
        if (auto *object = plug ? plug() : nullptr) {
            connect(object, SIGNAL(plugged()), this, SLOT(notify()), Qt::QueuedConnection);
            connect(object, SIGNAL(unplugged()), this, SLOT(notify()), Qt::QueuedConnection);
            connect(object, SIGNAL(ac_plugged()), this, SLOT(notify()), Qt::QueuedConnection);
            connect(object, SIGNAL(ac_unplugged()), this, SLOT(notify()), Qt::QueuedConnection);
        }
    }
private slots:
    void notify() {
        changed();
    }

private:
    std::function<void()> changed;
};
}
void observeNativeBattery(QObject *context, std::function<void()> changed) {
    if (context && changed) {
        new BatteryNotifications(context, std::move(changed));
    }
}
}
#include "native_battery.moc"
