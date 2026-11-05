#include "platforms/kobo/common/native_sync_network.h"
#include "platforms/kobo/common/native_wifi_power.h"
#include "support/main_thread.h"
#include "platforms/kobo/common/window_presentation.h"
#include "platforms/kobo/common/native_ui.h"
#include "platforms/kobo/common/native_dialog_overlay.h"
#include "platform/api/reader_host_services.h"
#include "check.h"
#include <QApplication>
#include <QDialog>
#include <QPushButton>
#include <QEventLoop>
#include <QTimer>
#include <QWindow>
#include <QLabel>
#include <limits>
#include <thread>
#include <vector>

class Workflow final : public QObject
{
    Q_OBJECT
signals:
    void internetIsAccessible();
    void internetPartiallyAccessible();
    void connectingFailed();
    void connectionCancelled();
};

class MainWindowView final : public QWidget
{
    Q_OBJECT
};

class ConfirmationDialog final : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;
};

class ProgressDialog final : public QWidget
{
    Q_OBJECT
public:
    using QWidget::QWidget;
};

class WirelessStatus final : public QObject
{
    Q_OBJECT
signals:
    void wifiEnabled(bool);
    void networkConnected();
    void networkDisconnected();
    void linkQualityForConnectedNetwork(double);
};

static WirelessStatus *wirelessStatus = nullptr;
static bool radioUp = false;

extern "C" QObject *_ZN15WirelessManager14sharedInstanceEv()
{
    return wirelessStatus;
}

extern "C" bool _ZNK15WirelessManager2upEv(QObject *)
{
    return radioUp;
}

static void wifiNotifications()
{
    WirelessStatus wireless;
    wirelessStatus = &wireless;
    std::vector<kobalt::WifiStatus> states;
    {
        QObject context;
        kobalt::observeNativeWifi(&context,
                                  [&](kobalt::WifiStatus state) { states.push_back(state); });
        require(states.size() == 1 && !states.back().enabled,
                "the Wi-Fi subscription reports initial radio state");
        radioUp = true;
        emit wireless.wifiEnabled(true);
        emit wireless.networkConnected();
        require(states.back().enabled && states.back().connected && states.back().strength == -1,
                "a connection without a quality report must not invent full signal strength");
        emit wireless.linkQualityForConnectedNetwork(0.8);
        require(states.back().strength == 3,
                "native normalized link quality maps to four signal levels");
        const auto before = states.size();
        emit wireless.linkQualityForConnectedNetwork(std::numeric_limits<double>::quiet_NaN());
        require(states.size() == before, "invalid link-quality reports do not alter the indicator");
        radioUp = false;
        emit wireless.wifiEnabled(false);
        require(!states.back().enabled && !states.back().connected && states.back().strength == -1,
                "radio shutdown clears connection and signal strength");
    }
    const auto before = states.size();
    emit wireless.networkConnected();
    require(states.size() == before, "the Wi-Fi subscription ends with the reader context");
    wirelessStatus = nullptr;
}

static void nativePrompt()
{
    MainWindowView nickel;
    nickel.setStyleSheet("QLabel { font-size: 17px; color: #135724; }");
    nickel.resize(600, 800);
    nickel.show();
    QDialog reader(&nickel);
    reader.setStyleSheet(
            "QWidget { background: black; color: white; font-family: monospace; } QLabel { font-size: 42px; }");
    reader.resize(600, 800);
    reader.show();
    auto presentation = kobalt::nickel::makeWindowPresentation(&reader, false);
    reader.hide();
    reader.show();
    QApplication::processEvents();
    require(reader.parentWidget() == &nickel,
            "the reader dialog must never be promoted into itself");
    presentation->pauseBackground();
    ConfirmationDialog prompt(&nickel, Qt::Widget);
    prompt.setGeometry(50, 100, 400, 200);
    QPushButton allow("Enable Wi-Fi", &prompt);
    QLabel explanation("Native styling", &prompt);
    prompt.ensurePolished();
    explanation.ensurePolished();
    const auto nativeFont = explanation.font();
    const auto nativePalette = explanation.palette();
    const auto nativeFlags = prompt.windowFlags();
    const auto nativePropagation = prompt.testAttribute(Qt::WA_WindowPropagation);
    Workflow workflow;
    bool connecting = false;
    int replies = 0;
    kobalt::NativeSyncNetworkBindings bindings;
    bindings.workflow = &workflow;
    bindings.ready = [] { return false; };
    bindings.disabled = [] { return true; };
    bindings.connecting = [&] { return connecting; };
    bindings.inputPending = [] { return kobalt::nickel::NativeDialogOverlay::inputPending(); };
    bindings.request = [&](bool) { prompt.show(); };
    bindings.cancel = [&] {
        connecting = false;
        prompt.hide();
        emit workflow.connectionCancelled();
    };
    kobalt::NativeSyncNetwork network(bindings);
    network.prepare(true, [&](bool) { ++replies; });
    QApplication::processEvents();
    require(explanation.font() == nativeFont && explanation.palette() == nativePalette,
            "promoting a native dialog must preserve Nickel fonts and colors instead of inheriting reader styles");
    reader.setStyleSheet(
            "QWidget { background: white; color: black; } QLabel { font-size: 31px; }");
    QApplication::processEvents();
    require(explanation.font() == nativeFont && explanation.palette() == nativePalette,
            "reader theme changes must not restyle an open native dialog");
    require(prompt.parentWidget() == &nickel && prompt.isWindow() && prompt.isVisible()
                    && prompt.windowHandle()->transientParent() == reader.windowHandle(),
            "the native Wi-Fi permission prompt must appear above the reader");
    require(prompt.updatesEnabled(),
            "the native prompt must paint outside Nickel's paused backing store");
    require(explanation.updatesEnabled() && allow.updatesEnabled(),
            "native dialog children must remain able to paint");
    require(kobalt::nickel::NativeDialogOverlay::activeFor(&reader),
            "native input must block reader shortcuts");
    QEventLoop permissionWait;
    QTimer::singleShot(11200, &permissionWait, &QEventLoop::quit);
    permissionWait.exec();
    require(replies == 0 && prompt.isVisible(),
            "a native permission prompt must not time out while the user is answering");
    network.cancel();
    QApplication::processEvents();
    require(prompt.parentWidget() == &nickel && !prompt.isVisible() && replies == 0,
            "cancelling sync restores the native prompt without completing a stale wait");
    require(prompt.windowFlags() == nativeFlags
                    && prompt.testAttribute(Qt::WA_WindowPropagation) == nativePropagation
                    && explanation.font() == nativeFont && explanation.palette() == nativePalette,
            "closing the overlay restores native window flags and leaves Nickel styling intact");
    require(!kobalt::nickel::NativeDialogOverlay::activeFor(&reader),
            "closing the prompt releases reader input");
    QDialog bluetooth(&nickel, Qt::Widget);
    bluetooth.setGeometry(20, 50, 300, 150);
    bluetooth.show();
    QApplication::processEvents();
    require(bluetooth.parentWidget() == &nickel && bluetooth.isWindow()
                    && bluetooth.windowHandle()->transientParent() == reader.windowHandle(),
            "other native modal dialogs also appear above Kobalt");
    auto *error = new ConfirmationDialog(&nickel, Qt::Widget);
    error->show();
    QApplication::processEvents();
    delete error;
    require(kobalt::nickel::NativeDialogOverlay::activeFor(&reader),
            "a nested dialog does not release another modal's guard");
    ProgressDialog progress(&nickel);
    progress.show();
    QApplication::processEvents();
    require(progress.parentWidget() == &nickel && progress.isWindow()
                    && progress.windowHandle()->transientParent() == reader.windowHandle(),
            "native progress widgets need the same presentation as confirmation dialogs");
    progress.hide();
    QDialog password(&nickel, Qt::Widget);
    QLineEdit input(&password);
    password.show();
    QApplication::processEvents();
    require(password.parentWidget() == &nickel && password.isWindow()
                    && kobalt::nickel::NativeDialogOverlay::inputPending(),
            "native password entry remains above the reader and counts as user interaction");
    password.hide();
    QDialog browser(&nickel, Qt::Widget);
    browser.setObjectName("N3Dialog");
    QWidget browserView(&browser);
    browserView.setObjectName("N3BrowserView");
    browser.show();
    QApplication::processEvents();
    require(browser.parentWidget() == &nickel,
            "the native browser keeps its separate handoff workflow");
    browser.hide();
    reader.hide();
    require(bluetooth.parentWidget() == &nickel && !bluetooth.isVisible()
                    && !kobalt::nickel::NativeDialogOverlay::activeFor(&reader),
            "sleeping or closing the reader restores all native modal ownership");
}

// The power saver turns Wi-Fi off only while it is on, Nickel is not
// syncing and Bluetooth is known to be off, and it checks again after the
// asynchronous Bluetooth answer.
static void wifiPowerSaver()
{
    using kobalt::BluetoothPower;
    bool wifi = true, syncing = false;
    int queries = 0, turnedOff = 0, violations = 0;
    std::function<void(BluetoothPower)> answer;
    kobalt::NativeWifiPowerBindings bindings;
    bindings.wifiOn = [&] { return wifi; };
    bindings.nickelSyncing = [&] { return syncing; };
    bindings.bluetooth = [&](QObject *, std::function<void(BluetoothPower)> reply) {
        ++queries;
        answer = std::move(reply);
    };
    bindings.turnOff = [&] {
        require(!kobalt::mainthread::interactive(), "Wi-Fi off runs inside a declared wait");
        ++turnedOff;
        wifi = false;
    };
    kobalt::mainthread::setViolationHandler([&](const char *) { ++violations; });
    kobalt::mainthread::Interaction reader([] { return true; });
    QObject context;
    int replies = 0;
    bool result = true;
    const auto save = [&] {
        kobalt::saveNativeWifiPower(&context, bindings, "wifi-off before reader input",
                                    [&](bool value) {
                                        ++replies;
                                        result = value;
                                    });
    };
    wifi = false;
    save();
    require(replies == 1 && !result && queries == 0,
            "Wi-Fi that is already off needs no Bluetooth query");
    wifi = true;
    syncing = true;
    save();
    require(replies == 2 && !result && queries == 0, "Nickel's sync keeps Wi-Fi on");
    syncing = false;
    for (const auto power : { BluetoothPower::On, BluetoothPower::Unknown }) {
        save();
        answer(power);
        require(!result && turnedOff == 0, "Bluetooth that is on or unknown keeps Wi-Fi on");
    }
    save();
    syncing = true;
    answer(BluetoothPower::Off);
    require(!result && turnedOff == 0, "a sync that starts during the query keeps Wi-Fi on");
    syncing = false;
    save();
    require(replies == 5 && queries == 4, "the saver waits for the Bluetooth answer");
    answer(BluetoothPower::Off);
    require(replies == 6 && result && turnedOff == 1 && !wifi,
            "Bluetooth off turns Wi-Fi off once");
    require(violations == 0, "the declared wait is not reported as a violation");
    // A Bluetooth stack that never answers must not hold the opening.
    wifi = true;
    save();
    QEventLoop loop;
    QTimer::singleShot(kobalt::nativeWifiPowerDeadlineMs + 500, &loop, &QEventLoop::quit);
    loop.exec();
    require(replies == 7 && !result && turnedOff == 1,
            "an unanswered Bluetooth query keeps Wi-Fi on after the deadline");
    answer(BluetoothPower::Off);
    require(replies == 7 && turnedOff == 1, "a late Bluetooth answer is ignored");
    kobalt::mainthread::setViolationHandler({});
}

static int nickelConnections = 0;

static void countNickelConnection(void *)
{
    ++nickelConnections;
}

// Nickel's own background connections, such as the one after every wake, pass
// through the hook. The reader's hold skips them, except while Kobalt waits
// for a connection of its own or Nickel's browser is open over the reader.
static void wifiHold()
{
    kobalt::nativeConnectWirelessSilently = countNickelConnection;
    kobalt_connect_wireless_silently(nullptr);
    require(nickelConnections == 1, "without a reader Nickel connects as usual");
    bool holding = true;
    auto *reader = new QWidget;
    kobalt::holdNativeWifiOff(reader, [&] { return holding; });
    kobalt_connect_wireless_silently(nullptr);
    require(nickelConnections == 1 && kobalt::nativeWifiHeldOff(),
            "the reader's hold skips Nickel's connection");
    holding = false;
    kobalt_connect_wireless_silently(nullptr);
    require(nickelConnections == 2, "a reader that does not hold Wi-Fi off lets Nickel connect");
    holding = true;
    Workflow workflow;
    bool connecting = false;
    kobalt::NativeSyncNetworkBindings bindings;
    bindings.workflow = &workflow;
    bindings.ready = [] { return false; };
    bindings.disabled = [] { return false; };
    bindings.connecting = [&] { return connecting; };
    bindings.request = [&](bool) { connecting = true; };
    bindings.cancel = [&] { connecting = false; };
    {
        kobalt::NativeSyncNetwork network(bindings);
        network.prepare(true, [](bool) { });
        require(kobalt::NativeSyncNetwork::preparing(), "a running request counts as preparing");
        kobalt_connect_wireless_silently(nullptr);
        require(nickelConnections == 3,
                "Nickel's dialogs may connect while Kobalt waits for a connection");
        network.cancel();
        require(!kobalt::NativeSyncNetwork::preparing(), "a cancelled request no longer counts");
        network.prepare(false, [](bool) { });
    }
    require(!kobalt::NativeSyncNetwork::preparing(), "a destroyed controller no longer counts");
    kobalt_connect_wireless_silently(nullptr);
    require(nickelConnections == 3, "the hold returns when Kobalt's request ends");
    reader->setProperty("kobalt_native_browser", true);
    kobalt_connect_wireless_silently(nullptr);
    require(nickelConnections == 4, "Nickel's browser over the reader may connect");
    reader->setProperty("kobalt_native_browser", false);
    std::thread worker([] { kobalt_connect_wireless_silently(nullptr); });
    worker.join();
    require(nickelConnections == 5,
            "a call from another thread is passed on without asking the reader");
    delete reader;
    kobalt_connect_wireless_silently(nullptr);
    require(nickelConnections == 6 && !kobalt::nativeWifiHeldOff(),
            "a deleted reader ends the hold");
}

// Checks the Kobo connection controller, Wi-Fi release, signal levels, the power saver, the hold on Nickel's background connections and native prompts above the reader, with fake native calls.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    wifiNotifications();
    wifiPowerSaver();
    wifiHold();
    nativePrompt();
    Workflow workflow;
    bool ready = true, disabled = false, connecting = false, interactive = false;
    int requests = 0, cancelled = 0, replies = 0;
    bool result = false;
    kobalt::NativeSyncNetworkBindings bindings;
    bindings.workflow = &workflow;
    bindings.ready = [&] { return ready; };
    bindings.disabled = [&] { return disabled; };
    bindings.connecting = [&] { return connecting; };
    bindings.request = [&](bool showUi) {
        ++requests;
        interactive = showUi;
        connecting = true;
    };
    bindings.cancel = [&] {
        ++cancelled;
        connecting = false;
        emit workflow.connectionCancelled();
    };
    auto done = [&](bool connected) {
        ++replies;
        result = connected;
    };
    kobalt::NativeSyncNetwork network(bindings);
    network.prepare(false, done);
    require(replies == 1 && result && requests == 0,
            "reuse an available connection without touching the radio");
    ready = false;
    disabled = true;
    network.prepare(false, done);
    require(replies == 2 && !result && requests == 0,
            "automatic reconnect respects explicitly disabled Wi-Fi");
    network.prepare(true, done);
    require(requests == 1 && interactive && replies == 2,
            "manual sync starts the native interactive workflow and waits");
    ready = true;
    connecting = false;
    emit workflow.internetPartiallyAccessible();
    require(replies == 3 && result,
            "a usable connection can proceed when internet access is partial");
    emit workflow.internetIsAccessible();
    require(replies == 3, "duplicate readiness notifications do not start another exchange");
    ready = false;
    disabled = false;
    network.prepare(false, done);
    require(requests == 2 && !interactive, "automatic opening uses silent reconnect");
    connecting = false;
    emit workflow.connectingFailed();
    require(replies == 4 && !result, "native connection failure completes the wait once");
    network.prepare(true, done);
    network.cancel();
    require(cancelled == 1 && replies == 4,
            "cancel stops an owned native workflow without completing a stale wait");
    ready = true;
    emit workflow.internetIsAccessible();
    require(replies == 4, "late connection success after cancellation is ignored");
    network.prepare(false, done);
    require(replies == 5 && result, "a fresh attempt works after cancellation");
    ready = false;
    connecting = true;
    const int before = requests;
    network.prepare(true, done);
    require(requests == before, "join an existing workflow instead of starting another one");
    network.cancel();
    require(cancelled == 1,
            "cancelling this waiter does not cancel another native caller's workflow");
    connecting = false;
    {
        kobalt::NativeSyncNetwork pending(bindings);
        pending.prepare(false, done);
    }
    require(cancelled == 2 && replies == 5,
            "destruction releases an owned wait and suppresses its callback");
    {
        // Only a silent connection this controller started is turned off again.
        const int savedReplies = replies, savedRequests = requests;
        int turnedOff = 0;
        auto releasing = bindings;
        releasing.turnOff = [&] {
            ++turnedOff;
            ready = false;
        };
        kobalt::NativeSyncNetwork owned(releasing);
        ready = true;
        owned.prepare(false, done);
        owned.release();
        require(turnedOff == 0, "a connection that was already up stays on");
        ready = false;
        owned.prepare(false, done);
        ready = true;
        connecting = false;
        emit workflow.internetIsAccessible();
        require(result, "a silent background connection completes");
        owned.release();
        require(turnedOff == 1 && !ready, "release turns off the Wi-Fi this controller turned on");
        owned.release();
        require(turnedOff == 1, "release turns Wi-Fi off once");
        owned.prepare(true, done);
        ready = true;
        connecting = false;
        emit workflow.internetIsAccessible();
        owned.release();
        require(turnedOff == 1, "a connection the user asked for stays on");
        ready = false;
        kobalt::NativeSyncNetwork without(bindings);
        without.prepare(false, done);
        ready = true;
        connecting = false;
        emit workflow.internetIsAccessible();
        without.release();
        require(result, "without the native turn-off call, release leaves Wi-Fi to Nickel");
        ready = false;
        connecting = false;
        replies = savedReplies;
        requests = savedRequests;
    }
    {
        // With the radio state known, Wi-Fi is Kobalt's to turn off only when
        // its silent request found the radio off, whether the connection
        // came up or not.
        const int savedReplies = replies, savedRequests = requests, savedCancelled = cancelled;
        bool radio = true;
        int turnedOff = 0;
        auto releasing = bindings;
        releasing.radioOn = [&] { return radio; };
        releasing.turnOff = [&] {
            ++turnedOff;
            radio = ready = false;
        };
        kobalt::NativeSyncNetwork radioNetwork(releasing);
        ready = false;
        connecting = false;
        radioNetwork.prepare(false, done);
        ready = true;
        connecting = false;
        emit workflow.internetIsAccessible();
        require(result, "a silent request can connect a radio that is already on");
        radioNetwork.release();
        require(turnedOff == 0,
                "a radio that was already on stays on when Kobalt's request connects it");
        radio = ready = false;
        radioNetwork.prepare(false, done);
        radio = true;
        radioNetwork.cancel();
        radioNetwork.release();
        require(turnedOff == 1 && !radio,
                "a cancelled silent request turns off the radio it turned on");
        radioNetwork.prepare(false, done);
        radio = true;
        connecting = false;
        emit workflow.connectingFailed();
        require(!result, "a silent request can fail with the radio on");
        radioNetwork.release();
        require(turnedOff == 2 && !radio,
                "a failed silent request turns off the radio it turned on");
        const int beforeRelease = cancelled;
        radioNetwork.prepare(false, done);
        radio = true;
        radioNetwork.release();
        require(turnedOff == 3 && cancelled == beforeRelease + 1,
                "release stops a silent request that is still running before it turns the radio off");
        // Nickel can still be powering the radio for a running request.
        radioNetwork.prepare(false, done);
        radioNetwork.release();
        require(turnedOff == 4,
                "release turns off a radio that a running request may still bring up");
        radioNetwork.prepare(true, done);
        radio = ready = true;
        connecting = false;
        emit workflow.internetIsAccessible();
        radioNetwork.release();
        require(turnedOff == 4 && radio, "an interactive request leaves the radio on");
        ready = connecting = false;
        replies = savedReplies;
        requests = savedRequests;
        cancelled = savedCancelled;
    }
    kobalt::NativeSyncNetwork unavailable(kobalt::NativeSyncNetworkBindings{});
    unavailable.prepare(true, done);
    require(replies == 6 && !result, "unsupported native APIs fail without a connection attempt");
    QEventLoop deadline;
    network.prepare(false, [&](bool connected) {
        done(connected);
        deadline.quit();
    });
    QTimer::singleShot(12000, &deadline, &QEventLoop::quit);
    deadline.exec();
    require(replies == 7 && !result && cancelled == 3,
            "a stalled native workflow is cancelled at the bounded deadline");
    require(!connecting, "the deadline leaves no owned connection workflow running");
    network.prepare(false, done);
    ready = true;
    connecting = false;
    emit workflow.connectingFailed();
    require(replies == 8 && result,
            "a failed native internet probe does not reject an available LAN connection");
    ready = false;
    network.prepare(true, done);
    ready = true;
    connecting = false;
    emit workflow.connectionCancelled();
    require(replies == 9 && !result,
            "explicit native cancellation wins even if an address became available");
}

#include "native_sync_network.moc"
