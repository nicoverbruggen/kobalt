#include "platforms/kobo/common/native_ui.h"
#include "platforms/kobo/common/native_symbol.h"
#include <QObject>
#include <dlfcn.h>
#include <utility>

namespace kobalt {
namespace {
class BatteryNotifications final : public QObject
{
    Q_OBJECT
public:
    BatteryNotifications(QObject *context, std::function<void()> changed)
        : QObject(context), changed(std::move(changed))
    {
        const auto hardware = reinterpret_cast<QObject *(*)()>(
                dlsym(RTLD_DEFAULT, "_ZN15HardwareFactory14sharedInstanceEv"));
        const auto plug = reinterpret_cast<QObject *(*)()>(
                dlsym(RTLD_DEFAULT, "_ZN11PlugManager14sharedInstanceEv"));
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

    void notify() { changed(); }

private:
    std::function<void()> changed;
};
} // namespace

NativeBatteryStatus nativeReaderBattery()
{
    static const auto hardware = symbol<void *(*)()>("_ZN15HardwareFactory14sharedInstanceEv");
    static const auto charging = symbol<bool (*)(void *)>("_ZN17HardwareInterface10isChargingEv");
    void *device = hardware && charging ? hardware() : nullptr;
    if (!device)
        return {};
    const auto vtable = *static_cast<void ***>(device);
    if (!vtable)
        return {};

    using Level = int (*)(const void *);
    using Charge = int (*)(void *);

    struct Implementation
    {
        Level level;
        Charge charge;
    };

    static const Implementation known[] = { { symbol<Level>("_ZNK3MTK15getBatteryLevelEv"),
                                              symbol<Charge>("_ZN3MTK13chargingStateEv") },
                                            { symbol<Level>("_ZNK6iMX50815getBatteryLevelEv"),
                                              symbol<Charge>("_ZN6iMX50813chargingStateEv") },
                                            { symbol<Level>("_ZNK9AllWinner15getBatteryLevelEv"),
                                              symbol<Charge>("_ZN9AllWinner13chargingStateEv") } };
    // Slots used by N3BatteryStatusLabel::updateBatteryLevelNow. The base
    // implementation always returns 40, so call only a known override.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    static constexpr int offsets[] = { 0x84, 0x90 }; // Firmware 4.38.23697 and 4.46.23836.
#else
    static constexpr int offsets[] = { 0x9c }; // Firmware 5.18.
#endif
    for (const auto offset : offsets) {
        const auto level = reinterpret_cast<Level>(vtable[offset / sizeof(void *)]);
        if (!level)
            continue;
        for (const auto &implementation : known) {
            if (level != implementation.level)
                continue;
            const int percent = level(device);
            // Nickel uses 0 for battery power, 1 for charging and 2 for charged.
            const int state = implementation.charge ? implementation.charge(device)
                                                    : (charging(device) ? 1 : 0);
            return { percent >= 0 && percent <= 100 ? percent : -1, state == 1 || state == 2,
                     state == 2 };
        }
    }
    return {};
}

void observeNativeBattery(QObject *context, std::function<void()> changed)
{
    if (context && changed) {
        new BatteryNotifications(context, std::move(changed));
    }
}
} // namespace kobalt

#include "native_battery.moc"
