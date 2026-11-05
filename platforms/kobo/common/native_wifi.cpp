#include "native_ui.h"
#include "native_network_state.h"
#include "platform/api/reader_host_services.h"
#include "support/diagnostics.h"
#include <QObject>
#include <QPointer>
#include <cmath>
#include <dlfcn.h>

namespace kobalt {
namespace {
class WifiObserver final : public QObject
{
    Q_OBJECT
public:
    WifiObserver(QObject *context, std::function<void(WifiStatus)> changed)
        : QObject(context), changed(std::move(changed))
    {
        const auto instance = reinterpret_cast<QObject *(*)()>(
                dlsym(RTLD_DEFAULT, "_ZN15WirelessManager14sharedInstanceEv"));
        up = reinterpret_cast<bool (*)(QObject *)>(
                dlsym(RTLD_DEFAULT, "_ZNK15WirelessManager2upEv"));
        if (instance && up)
            manager = instance();
        if (manager) {
            state.enabled = up(manager);
            state.connected = state.enabled && nickel::usableLocalNetwork();
            connect(manager, SIGNAL(wifiEnabled(bool)), this, SLOT(enabled(bool)));
            connect(manager, SIGNAL(networkConnected()), this, SLOT(connected()));
            connect(manager, SIGNAL(networkDisconnected()), this, SLOT(disconnected()));
            connect(manager, SIGNAL(linkQualityForConnectedNetwork(double)), this,
                    SLOT(quality(double)));
            connect(manager, &QObject::destroyed, this, [this] {
                state = {};
                publish();
            });
        }
        publish();
    }

private slots:

    void enabled(bool value)
    {
        state.enabled = value;
        if (!value) {
            state.connected = false;
            state.strength = -1;
        }
        publish();
    }

    void connected()
    {
        state.enabled = manager && up(manager);
        state.connected = state.enabled;
        state.strength = -1;
        publish();
    }

    void disconnected()
    {
        state.enabled = manager && up(manager);
        state.connected = false;
        state.strength = -1;
        publish();
    }

    void quality(double value)
    {
        if (!manager || !up(manager) || !std::isfinite(value))
            return;
        state.enabled = true;
        state.connected = state.connected || nickel::usableLocalNetwork();
        state.strength = state.connected ? qRound(qBound(0.0, value, 1.0) * 4) : -1;
        publish();
    }

private:
    void publish()
    {
        diagnostic("kosync-wifi-status",
                   qt::object({ { "enabled", state.enabled },
                                { "connected", state.connected },
                                { "strength", state.strength } }),
                   false);
        changed(state);
    }

    QPointer<QObject> manager;
    bool (*up)(QObject *) = nullptr;
    WifiStatus state;
    std::function<void(WifiStatus)> changed;
};
} // namespace

void observeNativeWifi(QObject *context, std::function<void(WifiStatus)> changed)
{
    new WifiObserver(context, std::move(changed));
}
} // namespace kobalt

#include "native_wifi.moc"
