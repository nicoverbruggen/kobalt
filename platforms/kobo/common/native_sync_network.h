#pragma once
#include "platform/api/sync_network.h"
#include <QObject>
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>

namespace kobalt {
// Native operations remain behind this boundary so connection sequencing can
// be checked without a radio or a native networking dialog.
struct NativeSyncNetworkBindings {
    QObject *workflow = nullptr;
    std::function<bool()> ready, disabled, connecting;
    std::function<bool()> inputPending;
    std::function<void(bool)> request;
    std::function<void()> cancel;
    // Optional. Without it, Nickel's own idle timeout turns Wi-Fi off.
    std::function<void()> turnOff;
    bool valid() const { return workflow && ready && disabled && connecting && request && cancel; }
};
class NativeSyncNetwork final : public QObject, public SyncNetwork {
    Q_OBJECT
public:
    NativeSyncNetwork();
    explicit NativeSyncNetwork(NativeSyncNetworkBindings bindings);
    // Whether this firmware lets Kobalt turn Wi-Fi off again after a sync.
    static bool canTurnOff();
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
    NativeSyncNetworkBindings bindings;
    QPointer<QObject> workflow;
    QTimer poll;
    QElapsedTimer elapsed;
    std::function<void(bool)> finished;
    bool requested = false;
    bool interactive = false;
    // A silent connection this controller started and Nickel completed.
    bool ownsConnection = false;
};
}
