#include "native_wifi_power.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include <QPointer>
#include <memory>
#include <utility>

namespace kobalt {
void saveNativeWifiPower(QObject *context, const NativeWifiPowerBindings &bindings, const char *reason,
                         std::function<void(bool)> done) {
    if (!bindings.valid() || !bindings.wifiOn() || bindings.nickelSyncing()) {
        done(false);
        return;
    }
    QPointer<QObject> guard(context);
    // The opened book waits for this answer. A Bluetooth stack that never
    // replies counts as unknown after the deadline, which keeps Wi-Fi on.
    auto answered = std::make_shared<bool>(false);
    const auto answer = [guard, bindings, reason, done, answered](BluetoothPower power) {
        if (!guard || std::exchange(*answered, true)) return;
        // Wi-Fi or Nickel's sync may have changed while the query ran.
        if (power != BluetoothPower::Off || !bindings.wifiOn() || bindings.nickelSyncing()) {
            diagnostic("wifi-power-saver-kept", qt::object({{"bluetooth", int(power)}}));
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
}
