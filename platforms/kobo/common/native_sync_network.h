#pragma once
#include "platform/api/sync_network.h"
#include <QObject>
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>

namespace kobalt {
// Native operations remain behind this boundary so connection sequencing can
// be checked without a radio or a native networking dialog.
struct NativeSyncNetworkBindings
{
    QObject *workflow = nullptr;
    std::function<bool()> ready, disabled, connecting;
    // Optional: whether the radio is on, connected or not. Without it, only a
    // ready connection counts as on.
    std::function<bool()> radioOn;
    std::function<bool()> inputPending;
    std::function<void(bool)> request;
    std::function<void()> cancel;
    // Optional. Without it, Nickel's own idle timeout turns Wi-Fi off.
    std::function<void()> turnOff;

    bool valid() const { return workflow && ready && disabled && connecting && request && cancel; }
};

class NativeSyncNetwork final : public QObject, public SyncNetwork
{
    Q_OBJECT
public:
    NativeSyncNetwork();
    explicit NativeSyncNetwork(NativeSyncNetworkBindings bindings);
    // Whether this firmware lets Kobalt turn Wi-Fi off again after a sync.
    static bool canTurnOff();
    // Whether any controller waits for a connection. Nickel's network dialogs
    // for an interactive request can start background connections of their
    // own, which the Wi-Fi power saver must let through.
    static bool preparing();
    ~NativeSyncNetwork() override;
    bool connected() const override;
    void prepare(bool interactive, std::function<void(bool)> finished) override;
    void cancel() override;
    void release() override;

private slots:
    void check();
    void failed();
    void cancelled();

private:
    void finish(bool ready);
    bool radioOn() const;
    void setWaiting(bool value);
    NativeSyncNetworkBindings bindings;
    QPointer<QObject> workflow;
    QTimer poll;
    QElapsedTimer elapsed;
    std::function<void(bool)> finished;
    bool requested = false;
    bool interactive = false;
    bool waiting = false;
    // A silent request of this controller found the radio off. Wi-Fi is then
    // Kobalt's to turn off, whether the connection came up or not.
    bool turnedOn = false;
};
} // namespace kobalt
