#include "platforms/kobo/nickel-fixes/wifi-idle/wifi_idle.h"
#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <dlfcn.h>
#include "platforms/kobo/common/native_sync_network.h"

// Dependencies outside this adapter are inert in this focused executable.
namespace kobalt {
namespace kosync {
bool networkInUse(int)
{
    return false;
}
} // namespace kosync

bool NativeSyncNetwork::preparing()
{
    return false;
}

void queryNativeBluetoothPower(QObject *, std::function<void(BluetoothPower)> answer)
{
    answer(BluetoothPower::Unknown);
}

namespace mainthread {
void blocking(const char *) { }

bool interactive()
{
    return false;
}
} // namespace mainthread
} // namespace kobalt

using namespace kobalt;
using namespace kobalt::nickel;

static void check(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

// Independent fake watchdog: it models observed eligibility and permission
// effects, without firmware code, native memory layouts or wall-clock waits.
struct WirelessWatchdog : QObject
{
    qint64 now = 1000000, wifiActivity = now, bluetoothActivity = now;
    bool wifi = true, connecting = false, syncing = false;
    bool forceWifi = false, permission = true;
    int queries = 0, attempts = 0, shutdowns = 0;
    std::function<void(BluetoothPower)> answer;
    WifiIdleCorrection *fix;

    bool shouldAttemptToKillWireless() const;
    void onBatteryLevel(int);

    WirelessWatchdog()
    {
        WifiIdleBindings api;
        api.clock = [this] { return now; };
        api.predicate = [this](bool ignoreBluetooth) {
            return wifi && !forceWifi && now - wifiActivity >= 120000
                    && (ignoreBluetooth || now - bluetoothActivity >= 900000);
        };
        api.blockedByBluetooth = [this] {
            return wifi && !connecting && !syncing && now - wifiActivity >= 120000
                    && now - bluetoothActivity < 900000;
        };
        api.query = [this](QObject *, std::function<void(BluetoothPower)> done) {
            ++queries;
            answer = std::move(done);
        };
        api.retry = [this] { onBatteryLevel(0); };
        fix = new WifiIdleCorrection(this, std::move(api));
    }

    void reply(BluetoothPower power)
    {
        auto done = std::move(answer);
        check(bool(done), "query must be pending");
        done(power);
    }
};

bool WirelessWatchdog::shouldAttemptToKillWireless() const
{
    return wifi && !forceWifi && now - wifiActivity >= 120000 && now - bluetoothActivity >= 900000;
}

void WirelessWatchdog::onBatteryLevel(int)
{
    if (!fix->evaluate())
        return;
    ++attempts;
    if (permission) {
        ++shutdowns;
        wifi = false;
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    {
        const auto stock = reinterpret_cast<bool (*)(QObject *)>(
                dlsym(RTLD_DEFAULT, "_ZNK16WirelessWatchdog27shouldAttemptToKillWirelessEv"));
        check(stock != nullptr, "the real hook symbol must resolve in pewter");
        check(dlsym(RTLD_DEFAULT, "_ZN16WirelessWatchdog14onBatteryLevelEi") != nullptr,
              "the native battery retry symbol must resolve in pewter");
        nativeWirelessIdlePredicate = stock;
        WirelessWatchdog w;
        w.now += 120000;
        check(!kobalt_wireless_idle_predicate(&w) && w.queries == 0,
              "an unverified firmware predicate must pass through without touching native layout");
        nativeWirelessIdlePredicate = nullptr;
    }
    {
        WirelessWatchdog w;
        w.now += 119999;
        check(!w.fix->evaluate() && w.queries == 0, "recent Wi-Fi must retain its idle interval");
        ++w.now;
        check(!w.fix->evaluate() && w.queries == 1,
              "stock Bluetooth gate must query at Wi-Fi expiry");
        w.fix->evaluate();
        check(w.queries == 1, "overlapping battery updates must share the pending query");
        w.reply(BluetoothPower::Off);
        check(w.shutdowns == 1 && w.bluetoothActivity == 1000000,
              "confirmed Bluetooth-off must retry without changing the real Bluetooth history");
        w.wifi = true;
        w.wifiActivity = w.now;
        w.fix->radioChanged();
        check(!w.fix->evaluate() && w.queries == 1, "manual enable starts a fresh Wi-Fi interval");
    }
    {
        WirelessWatchdog w;
        w.wifiActivity -= 120000; // Nickel's expired startup sentinel.
        check(!w.fix->evaluate() && w.queries == 0,
              "a newly observed radio must get a complete idle window");
        w.now += 120000;
        w.fix->evaluate();
        w.fix->radioChanged();
        w.reply(BluetoothPower::Off);
        check(w.attempts == 0,
              "reconnect must invalidate an old answer and restart the idle window");
    }
    for (auto power : { BluetoothPower::On, BluetoothPower::Unknown }) {
        WirelessWatchdog w;
        w.now += 120000;
        w.fix->evaluate();
        w.reply(power);
        check(w.attempts == 0 && w.wifi, "Bluetooth on or unavailable must retain stock behavior");
    }
    for (int change = 0; change < 6; ++change) {
        WirelessWatchdog w;
        w.now += 120000;
        w.fix->evaluate();
        if (change == 0)
            w.wifiActivity = w.now; // Real network reply activity.
        if (change == 1)
            w.connecting = true; // Reconnect or DHCP.
        if (change == 2)
            w.syncing = true; // Sync started during the query.
        if (change == 3)
            w.fix->invalidate(); // Bluetooth or sleep/wake transition.
        if (change == 4)
            w.wifi = false; // Manual off during the query.
        if (change == 5)
            w.forceWifi = true;
        w.reply(BluetoothPower::Off);
        check(w.attempts == 0, "a changed state must cancel the correction");
    }
    {
        WirelessWatchdog w;
        w.now += 120000;
        w.permission = false;
        w.fix->evaluate();
        w.reply(BluetoothPower::Off);
        check(w.attempts == 1 && w.shutdowns == 0 && w.wifi,
              "the stock permission veto must still prevent shutdown");
    }
    {
        WirelessWatchdog w;
        w.now += 900000;
        check(w.fix->evaluate() && w.queries == 0,
              "an eligible stock watchdog needs no correction");
    }
    {
        auto w = std::unique_ptr<WirelessWatchdog>(new WirelessWatchdog);
        w->now += 120000;
        w->fix->evaluate();
        auto late = std::move(w->answer);
        w.reset();
        late(BluetoothPower::Off);
    }
    std::puts("Wi-Fi idle correction checks passed");
}
