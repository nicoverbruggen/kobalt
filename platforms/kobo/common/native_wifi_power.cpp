#include "native_wifi_power.h"
#include "browser_handoff.h"
#include "native_sync_network.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include <QApplication>
#include <QPointer>
#include <QThread>
#include <QWidget>
#include <memory>
#include <utility>

namespace kobalt {
void saveNativeWifiPower(QObject *context, const NativeWifiPowerBindings &bindings,
                         const char *reason, std::function<void(bool)> done)
{
    if (!bindings.valid() || !bindings.wifiOn() || bindings.nickelSyncing()) {
        done(false);
        return;
    }
    QPointer<QObject> guard(context);
    // The opened book waits for this answer. A Bluetooth stack that never
    // replies counts as unknown after the deadline, which keeps Wi-Fi on.
    auto answered = std::make_shared<bool>(false);
    const auto answer = [guard, bindings, reason, done, answered](BluetoothPower power) {
        if (!guard || std::exchange(*answered, true))
            return;
        // Wi-Fi or Nickel's sync may have changed while the query ran.
        if (power != BluetoothPower::Off || !bindings.wifiOn() || bindings.nickelSyncing()) {
            diagnostic("wifi-power-saver-kept", qt::object({ { "bluetooth", int(power) } }));
            done(false);
            return;
        }
        {
            // Nickel waits for child processes on this thread before it hands
            // the radio to a worker. The opened book is not presented yet, or
            // the device is going to sleep, so no tap can reach the reader
            // before the wait ends.
            mainthread::AllowBlocking allow(reason);
            mainthread::BlockingOperation wait("wifi-off");
            bindings.turnOff();
        }
        done(true);
    };
    qt::later(nativeWifiPowerDeadlineMs, context, [answer] { answer(BluetoothPower::Unknown); });
    bindings.bluetooth(context, answer);
}

void (*nativeConnectWirelessSilently)(void *);

namespace {
QPointer<QObject> holdOwner;
std::function<bool()> holdDecision;
} // namespace

void holdNativeWifiOff(QObject *owner, std::function<bool()> holding)
{
    holdOwner = owner;
    holdDecision = std::move(holding);
}

bool nativeWifiHeldOff()
{
    if (!holdOwner || !holdDecision || NativeSyncNetwork::preparing())
        return false;
    // Nickel's browser connects for the page it opens over the reader.
    if (BrowserHandoff::activeFor(qobject_cast<QWidget *>(holdOwner.data())))
        return false;
    return holdDecision();
}
} // namespace kobalt

// Nickel 4.46.23852 and 6.0.279225 call this from the routine that runs after
// every wake, before it re-enables stylus warnings and Bluetooth devices. They
// also call it for image downloads, the browser and store, and after USB.
// Nickel's store sync connects through connectWireless() instead, so it is
// not affected.
extern "C" __attribute__((visibility("default"))) void
kobalt_connect_wireless_silently(void *workflow)
{
    using namespace kobalt;
    if (qApp && QThread::currentThread() == qApp->thread() && nativeWifiHeldOff()) {
        diagnostic("wifi-power-saver",
                   qt::object({ { "moment", "nickel-connection" }, { "held", true } }));
        return;
    }
    nativeConnectWirelessSilently(workflow);
}
