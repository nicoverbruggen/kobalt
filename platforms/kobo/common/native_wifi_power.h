#pragma once
#include <QObject>
#include <functional>

namespace kobalt {
// Bluetooth adapter state from an asynchronous query.
enum class BluetoothPower { Off, On, Unknown };

// Native operations for the Wi-Fi power saver, behind a boundary so the
// decision can be checked without a radio.
struct NativeWifiPowerBindings
{
    std::function<bool()> wifiOn;
    // Nickel's own store sync, which needs the connection until it ends.
    std::function<bool()> nickelSyncing;
    // Answers on the GUI thread, never by waiting for the reply.
    std::function<void(QObject *context, std::function<void(BluetoothPower)> answer)> bluetooth;
    std::function<void()> turnOff;

    bool valid() const { return wifiOn && nickelSyncing && bluetooth && turnOff; }
};

// How long an opened book waits for the Bluetooth answer.
inline constexpr int nativeWifiPowerDeadlineMs = 1000;
// Turns Wi-Fi off while an opened book waits to be presented or while the
// device goes to sleep. Wi-Fi stays on while Bluetooth is on or its state is
// unknown, and during Nickel's sync. `reason` names the AllowBlocking scope of
// the off call. Calls done with whether Wi-Fi was turned off, unless the
// context was deleted first.
void saveNativeWifiPower(QObject *context, const NativeWifiPowerBindings &bindings,
                         const char *reason, std::function<void(bool)> done);
// Nickel's WirelessWorkflowManager::connectWirelessSilently(), set by the hook.
// Nickel calls it for its own background connections, for example a few
// seconds after every wake. Kobalt's KOSync resolves the function itself and
// does not pass through the hook.
extern void (*nativeConnectWirelessSilently)(void *workflow);
// Registers the reader whose `holding` decides whether Nickel's background
// connections are skipped. A later owner replaces an earlier one.
void holdNativeWifiOff(QObject *owner, std::function<bool()> holding);
// Whether Nickel's next background connection is skipped: the owner holds Wi-Fi
// off, Kobalt has no connection request of its own running, and Nickel's
// browser is not open over the reader.
bool nativeWifiHeldOff();
} // namespace kobalt

extern "C" __attribute__((visibility("default"))) void
kobalt_connect_wireless_silently(void *workflow);
