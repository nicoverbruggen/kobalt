#pragma once
#include "platforms/kobo/common/native_wifi_power.h"
#include <QObject>
#include <functional>

namespace kobalt {
namespace nickel {
// The callbacks keep firmware layout access separate from retry ownership.
struct WifiIdleBindings
{
    std::function<qint64()> clock;
    std::function<bool(bool ignoreBluetooth)> predicate;
    std::function<bool()> blockedByBluetooth;
    std::function<void(QObject *, std::function<void(BluetoothPower)>)> query;
    std::function<void()> retry;
};

class WifiIdleCorrection final : public QObject
{
    Q_OBJECT
public:
    WifiIdleCorrection(QObject *watchdog, WifiIdleBindings bindings);
    bool evaluate();

public slots:
    void invalidate();
    void radioChanged();

private:
    bool mayCorrect() const;
    WifiIdleBindings bindings;
    qint64 radioChangedAt;
    quint64 generation = 0;
    bool pending = false;
    bool retrying = false;
};

extern bool (*nativeWirelessIdlePredicate)(QObject *);
} // namespace nickel
} // namespace kobalt

extern "C" __attribute__((visibility("default"))) bool
kobalt_wireless_idle_predicate(QObject *watchdog);
