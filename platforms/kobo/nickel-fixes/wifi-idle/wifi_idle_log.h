#pragma once
#include <QObject>
#include <QJsonObject>
#include <functional>

namespace kobalt {
namespace nickel {
// Only diagnostic builds sample hardware or write the dedicated log.
void startWifiIdleLog(QObject *owner, bool verified, std::function<QJsonObject()> state);
#ifdef KOBALT_WIFI_IDLE_DIAGNOSTICS
QJsonObject collectWifiIdleHardware(const QString &root, QJsonObject fields);
extern void (*nativeWifiIdleOff)(QObject *);
#endif
} // namespace nickel
} // namespace kobalt

#ifdef KOBALT_WIFI_IDLE_DIAGNOSTICS
extern "C" __attribute__((visibility("default"))) void kobalt_wifi_idle_off(QObject *manager);
#endif
