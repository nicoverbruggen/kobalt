#include "native_sync_network.h"
#include "native_network_state.h"
#include "native_dialog_overlay.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include <dlfcn.h>
#include <utility>

namespace kobalt {
namespace {
template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

NativeSyncNetworkBindings nativeBindings()
{
    const auto instance = symbol<QObject *(*)()>("_ZN23WirelessWorkflowManager14sharedInstanceEv");
    const auto wirelessInstance = symbol<QObject *(*)()>("_ZN15WirelessManager14sharedInstanceEv");
    const auto up = symbol<bool (*)(QObject *)>("_ZNK15WirelessManager2upEv");
    const auto disabled =
            symbol<bool (*)(QObject *)>("_ZN23WirelessWorkflowManager14isAirplaneModeEv");
    const auto connecting =
            symbol<bool (*)(QObject *)>("_ZNK23WirelessWorkflowManager12isConnectingEv");
    const auto accessible =
            symbol<bool (*)(QObject *)>("_ZN23WirelessWorkflowManager20isInternetAccessibleEv");
    const auto request = symbol<void (*)(QObject *, bool, bool)>(
            "_ZN23WirelessWorkflowManager15connectWirelessEbb");
    const auto silent =
            symbol<void (*)(QObject *)>("_ZN23WirelessWorkflowManager23connectWirelessSilentlyEv");
    const auto cancel = symbol<void (*)(QObject *)>("_ZN23WirelessWorkflowManager6cancelEv");
    // Present in firmware 4.46 and 6.0.279225.
    const auto off = symbol<void (*)(QObject *)>("_ZN15WirelessManager3offEv");
    if (!instance || !wirelessInstance || !up || !disabled || !connecting || !accessible || !request
        || !silent || !cancel)
        return {};
    QPointer<QObject> workflow = instance();
    QPointer<QObject> wireless = wirelessInstance();
    if (!workflow || !wireless)
        return {};
    NativeSyncNetworkBindings result;
    result.workflow = workflow;
    result.inputPending = [] { return nickel::NativeDialogOverlay::inputPending(); };
    result.disabled = [workflow, disabled] { return !workflow || disabled(workflow); };
    result.radioOn = [wireless, up] { return wireless && up(wireless); };
    result.connecting = [workflow, connecting] { return workflow && connecting(workflow); };
    result.ready = [workflow, wireless, up, accessible] {
        // up() is Nickel's cached radio state on both firmware generations.
        // An old DHCP address alone must not suppress a needed reconnect.
        if (!workflow || !wireless || !up(wireless))
            return false;
        if (accessible(workflow))
            return true;
        // A custom server can be reachable on the LAN even if Nickel's public
        // internet check fails. The HTTPS exchange decides server reachability.
        return nickel::usableLocalNetwork();
    };
    result.request = [workflow, request, silent](bool interactive) {
        if (!workflow)
            return;
        if (interactive)
            request(workflow, true, false);
        else
            silent(workflow);
    };
    result.cancel = [workflow, cancel] {
        if (workflow)
            cancel(workflow);
    };
    if (off)
        result.turnOff = [wireless, off] {
            if (!wireless)
                return;
            // On firmware 4.46, off() runs `killall dhcpcd` and waits for it on
            // this thread before it hands the radio to a worker; a Clara BW
            // measured about 87 ms. While the user reads, that breaks the
            // page-turn rule, so the guard reports it. The reader declares the
            // wait at sleep and close.
            mainthread::BlockingOperation wait("wifi-off");
            off(wireless);
        };
    return result;
}
} // namespace

NativeSyncNetwork::NativeSyncNetwork() : NativeSyncNetwork(nativeBindings()) { }

bool NativeSyncNetwork::canTurnOff()
{
    return bool(nativeBindings().turnOff);
}

NativeSyncNetwork::NativeSyncNetwork(NativeSyncNetworkBindings api)
    : bindings(std::move(api)), workflow(bindings.workflow)
{
    elapsed.invalidate();
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, this, &NativeSyncNetwork::check);
    if (!bindings.valid())
        return;
    // These InternetProvider signals are inherited by the wireless workflow on
    // both firmware generations. Polling also permits LAN-only connectivity.
    connect(workflow, SIGNAL(internetIsAccessible()), this, SLOT(check()));
    connect(workflow, SIGNAL(internetPartiallyAccessible()), this, SLOT(check()));
    connect(workflow, SIGNAL(connectingFailed()), this, SLOT(failed()));
    connect(workflow, SIGNAL(connectionCancelled()), this, SLOT(cancelled()));
    connect(workflow, &QObject::destroyed, this, [this] { finish(false); });
}

NativeSyncNetwork::~NativeSyncNetwork()
{
    cancel();
}

bool NativeSyncNetwork::connected() const
{
    return workflow && bindings.valid() && bindings.ready();
}

void NativeSyncNetwork::prepare(bool showUi, std::function<void(bool)> done)
{
    cancel();
    const bool available = connected();
    diagnostic("kosync-network-prepare",
               qt::object({ { "interactive", showUi },
                            { "ready", available },
                            { "bindings_available", bool(workflow) && bindings.valid() } }));
    if (available) {
        done(true);
        return;
    }
    if (!workflow || !bindings.valid() || (!showUi && bindings.disabled())) {
        done(false);
        return;
    }
    interactive = showUi;
    finished = std::move(done);
    setWaiting(true);
    elapsed.start();
    poll.setInterval(100);
    poll.start();
    if (!bindings.connecting()) {
        requested = true;
        // A radio that was already on is not Kobalt's, even if this request
        // connects it. An interactive request leaves Wi-Fi to the user.
        if (!showUi && !radioOn())
            turnedOn = true;
        bindings.request(showUi);
    }
}

void NativeSyncNetwork::check()
{
    if (!finished)
        return;
    if (interactive && bindings.inputPending && bindings.inputPending()) {
        elapsed.restart();
        poll.setInterval(1000);
        return;
    }
    poll.setInterval(100);
    if (connected())
        finish(true);
    else if (elapsed.elapsed() >= 10000) {
        auto done = std::move(finished);
        cancel();
        done(false);
    }
}

void NativeSyncNetwork::failed()
{
    finish(connected());
}

void NativeSyncNetwork::cancelled()
{
    finish(false);
}

void NativeSyncNetwork::finish(bool ready)
{
    diagnostic("kosync-network-finished",
               qt::object({ { "ready", ready }, { "owned", requested } }));
    poll.stop();
    requested = false;
    auto done = std::move(finished);
    finished = {};
    setWaiting(false);
    if (done)
        done(ready);
}

namespace {
int waitingControllers = 0;
}

bool NativeSyncNetwork::preparing()
{
    return waitingControllers > 0;
}

void NativeSyncNetwork::setWaiting(bool value)
{
    if (std::exchange(waiting, value) != value)
        waitingControllers += value ? 1 : -1;
}

bool NativeSyncNetwork::radioOn() const
{
    if (!workflow || !bindings.valid())
        return false;
    return bindings.radioOn ? bindings.radioOn() : bindings.ready();
}

void NativeSyncNetwork::release()
{
    if (!std::exchange(turnedOn, false))
        return;
    // A request that is still running would bring the radio back up. Nickel
    // may still be powering the radio for it, before up() reads true, so turn
    // it off whenever such a request was running.
    const bool running = requested;
    cancel();
    if (!bindings.turnOff || (!running && !radioOn()))
        return;
    diagnostic("kosync-network-released");
    bindings.turnOff();
}

void NativeSyncNetwork::cancel()
{
    poll.stop();
    finished = {};
    setWaiting(false);
    const bool owned = std::exchange(requested, false);
    if (owned && workflow && bindings.valid())
        bindings.cancel();
}
} // namespace kobalt
