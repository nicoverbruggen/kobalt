#pragma once
#include <QObject>
#include <functional>

namespace kobalt {
// Bluetooth adapter state from an asynchronous query.
enum class BluetoothPower { Off, On, Unknown };
// Native operations for the Wi-Fi power saver, behind a boundary so the
// decision can be checked without a radio.
struct NativeWifiPowerBindings {
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
// Turns Wi-Fi off while an opened book waits to be presented. Wi-Fi stays on
// while Bluetooth is on or its state is unknown, and during Nickel's sync.
// Calls done with whether Wi-Fi was turned off.
void saveNativeWifiPower(QObject *context, const NativeWifiPowerBindings &bindings, std::function<void(bool)> done);
}
