// Stock Nickel Wi-Fi idle correction, firmware 4 / Qt 5 only.
//
// Verified on libnickel from 4.45.23697, 4.46.23836 and 4.46.23852: WirelessWatchdog's
// shouldAttemptToKillWireless() rejects shutdown while either its 120-second
// Wi-Fi timestamp or its 900-second Bluetooth timestamp is recent, before
// checking whether either radio is up. BluetoothHeartbeat's constructor posts
// a heartbeat even when Bluetooth is off. Wi-Fi can therefore wait 15 minutes.
// Qt 6 firmware 6.0.279225 has separate WifiWatchdog and BluetoothWatchdog.
//
// After an asynchronous, confirmed Bluetooth-off answer, retry Nickel's own
// battery callback. For that predicate call only, expire the Bluetooth time
// and restore it before Nickel requests shutdown permission. Keep its Wi-Fi
// timestamp, forceWifiOn setting, permission vetoes and shutdown path intact.
// Never call radio-off here. Nickel's network activity still resets its timer.
// Ordinary Qt requests do not emit Nickel's activity event, so KOSync reports
// its own active/recent use. Defer new shutdown work while Kobalt takes input:
// Nickel's shutdown routine waits for child processes on the GUI thread.
//
// Applicability: Qt5 build, GUI thread, exact verified predicate fingerprint,
// required symbols, connected Wi-Fi, no connection setup or active sync.
// Unknown Bluetooth state and changed radio/power state keep stock behavior.
// A new radio or resume gets at least 120 seconds before this correction.
// The fingerprint gates the two ARM32 timestamp offsets below. Other builds
// pass through until inspected. This does not measure radio power or battery
// savings, change Wi-Fi policy, or correct any sleep-drain issue.
#include "wifi_idle.h"
#include "wifi_idle_log.h"
#include "sync/kosync.h"
#include "platforms/kobo/common/native_symbol.h"
#include "platforms/kobo/common/native_ui.h"
#include "platforms/kobo/common/native_sync_network.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include "support/main_thread.h"
#include <cstring>
#include <utility>

namespace kobalt::nickel {
bool (*nativeWirelessIdlePredicate)(QObject *);

WifiIdleCorrection::WifiIdleCorrection(QObject *watchdog, WifiIdleBindings api)
    : QObject(watchdog), bindings(std::move(api)), radioChangedAt(bindings.clock())
{
}

void WifiIdleCorrection::invalidate()
{
    ++generation;
}

void WifiIdleCorrection::radioChanged()
{
    invalidate();
    radioChangedAt = bindings.clock();
}

bool WifiIdleCorrection::mayCorrect() const
{
    // Nickel seeds the Wi-Fi time as already expired. Do not turn a newly
    // enabled or reconnected radio off before it has had a full idle window.
    return bindings.clock() - radioChangedAt >= 120000 && bindings.blockedByBluetooth();
}

bool WifiIdleCorrection::evaluate()
{
    if (retrying)
        return bindings.predicate(mayCorrect());
    const bool result = bindings.predicate(false);
    if (result || pending || !mayCorrect())
        return result;
    pending = true;
    const auto request = generation;
    QPointer<WifiIdleCorrection> guard(this);
    bindings.query(this, [guard, request](BluetoothPower power) {
        if (!guard)
            return;
        guard->pending = false;
        if (power != BluetoothPower::Off || request != guard->generation || !guard->mayCorrect())
            return;
        guard->retrying = true;
        const auto retry = guard->bindings.retry;
        retry();
        if (guard)
            guard->retrying = false;
    });
    return result;
}

namespace {
constexpr int wifiTimestampOffset = 8;
constexpr int bluetoothTimestampOffset = 16;
constexpr qint64 wifiIdleMs = 120000;
constexpr qint64 bluetoothIdleMs = 900000;

qint64 timestamp(QObject *watchdog, int offset)
{
    qint64 value;
    std::memcpy(&value, reinterpret_cast<const char *>(watchdog) + offset, sizeof(value));
    return value;
}

bool verifiedPredicate()
{
    // This function has no text relocations. The verified builds have the
    // same layout. Hash 208 bytes including the literal pool. Clear the Thumb bit.
    const auto address = reinterpret_cast<quintptr>(nativeWirelessIdlePredicate) & ~quintptr(1);
    if (!address)
        return false;
    const auto code = QByteArray::fromRawData(reinterpret_cast<const char *>(address), 208);
    const auto hash = QCryptographicHash::hash(code, QCryptographicHash::Sha256).toHex();
    return hash == "7089b2642cd4731ff8774cc71b3d2d407b4fa74fbd3aefc28db89ed0526aa94b"
            || hash == "60c86de90d8f587f4433d2906b62809711b71ba9c53922c050874496891dd1ca";
}

bool predicate(QObject *watchdog, bool ignoreBluetooth)
{
    if (!ignoreBluetooth)
        return nativeWirelessIdlePredicate(watchdog);
    auto *field = reinterpret_cast<char *>(watchdog) + bluetoothTimestampOffset;
    const qint64 saved = timestamp(watchdog, bluetoothTimestampOffset);
    const qint64 expired = QDateTime::currentMSecsSinceEpoch() - bluetoothIdleMs;

    struct Restore
    {
        char *field;
        qint64 value;

        ~Restore() { std::memcpy(field, &value, sizeof(value)); }
    } restore{ field, saved };

    std::memcpy(field, &expired, sizeof(expired));
    // Only the eligibility call sees the expired time. In particular, all
    // shutdown callbacks run after Restore has put the real timestamp back.
    return nativeWirelessIdlePredicate(watchdog);
}

[[maybe_unused]] WifiIdleCorrection *correction(QObject *watchdog)
{
    if (auto *existing =
                watchdog->findChild<WifiIdleCorrection *>(QString(), Qt::FindDirectChildrenOnly))
        return existing;
    const auto wireless = symbol<QObject *(*)()>("_ZN15WirelessManager14sharedInstanceEv");
    const auto up = symbol<bool (*)(QObject *)>("_ZNK15WirelessManager2upEv");
    const auto workflow = symbol<QObject *(*)()>("_ZN23WirelessWorkflowManager14sharedInstanceEv");
    const auto connecting =
            symbol<bool (*)(QObject *)>("_ZNK23WirelessWorkflowManager12isConnectingEv");
    const auto sync = symbol<QObject *(*)()>("_ZN13N3SyncManager14sharedInstanceEv");
    const auto syncing = symbol<bool (*)(QObject *)>("_ZNK15SyncManagerBase9isSyncingEv");
    const auto battery =
            symbol<void (*)(QObject *, int)>("_ZN16WirelessWatchdog14onBatteryLevelEi");
    if (!wireless || !up || !workflow || !connecting || !sync || !syncing || !battery)
        return nullptr;
    QPointer<QObject> radio = wireless(), manager = workflow(), syncManager = sync();
    if (!radio || !manager || !syncManager)
        return nullptr;
    WifiIdleBindings api;
    api.clock = [] {
        QElapsedTimer clock;
        clock.start();
        return clock.msecsSinceReference();
    };
    api.predicate = [watchdog](bool ignore) { return predicate(watchdog, ignore); };
    api.blockedByBluetooth = [watchdog, radio, manager, syncManager, up, connecting, syncing] {
        if (!radio || !manager || !syncManager || !up(radio) || connecting(manager)
            || syncing(syncManager) || NativeSyncNetwork::preparing()
            || kosync::networkInUse(wifiIdleMs) || mainthread::interactive())
            return false;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto wifi = timestamp(watchdog, wifiTimestampOffset);
        const auto bluetooth = timestamp(watchdog, bluetoothTimestampOffset);
        // A backward wall-clock change keeps the stock behavior.
        return wifi <= now - wifiIdleMs && bluetooth <= now && bluetooth > now - bluetoothIdleMs;
    };
    api.query = queryNativeBluetoothPower;
    api.retry = [watchdog, battery] {
        // This is Nickel's normal idle shutdown path. Its child-process waits
        // remain visible to the main-thread diagnostic guard.
        mainthread::blocking("nickel-wifi-idle");
        battery(watchdog, 0);
    };
    auto *fix = new WifiIdleCorrection(watchdog, std::move(api));
    QObject::connect(radio, SIGNAL(wifiEnabled(bool)), fix, SLOT(radioChanged()));
    QObject::connect(radio, SIGNAL(networkConnected()), fix, SLOT(radioChanged()));
    QObject::connect(radio, SIGNAL(networkDisconnected()), fix, SLOT(radioChanged()));
    const auto bluetooth = symbol<QObject *(*)()>("_ZN16BluetoothManager14sharedInstanceEv");
    if (bluetooth && bluetooth())
        QObject::connect(bluetooth(), SIGNAL(adapterPowered(bool)), fix, SLOT(invalidate()));
    const auto power = symbol<QObject *(*)()>("_ZN12PowerManager14sharedInstanceEv");
    if (power && power()) {
        QObject::connect(power(), SIGNAL(aboutToSuspend(PermissionRequest *)), fix,
                         SLOT(invalidate()));
        QObject::connect(power(), SIGNAL(resumed()), fix, SLOT(radioChanged()));
    }
    return fix;
}
} // namespace
} // namespace kobalt::nickel

extern "C" bool kobalt_wireless_idle_predicate(QObject *watchdog)
{
    using namespace kobalt::nickel;
    if (!nativeWirelessIdlePredicate)
        return false;
    static const bool verified = verifiedPredicate();
    auto *app = QCoreApplication::instance();
    if (!watchdog || !app || QThread::currentThread() != app->thread()
        || watchdog->thread() != app->thread())
        return nativeWirelessIdlePredicate(watchdog);
#ifdef KOBALT_WIFI_IDLE_DIAGNOSTICS
    startWifiIdleLog(watchdog, verified, [watchdog] {
        QJsonObject fields;
        if (verified) {
            fields[QStringLiteral("wifi_idle_ms")] = double(
                    QDateTime::currentMSecsSinceEpoch() - timestamp(watchdog, wifiTimestampOffset));
            fields[QStringLiteral("bluetooth_idle_ms")] =
                    double(QDateTime::currentMSecsSinceEpoch()
                           - timestamp(watchdog, bluetoothTimestampOffset));
        }
        fields[QStringLiteral("kobalt_interactive")] = kobalt::mainthread::interactive();
        fields[QStringLiteral("kosync_network_in_use")] = kobalt::kosync::networkInUse(wifiIdleMs);
        fields[QStringLiteral("connection_preparing")] = kobalt::NativeSyncNetwork::preparing();
        const auto sync = kobalt::symbol<QObject *(*)()>("_ZN13N3SyncManager14sharedInstanceEv");
        const auto syncing =
                kobalt::symbol<bool (*)(QObject *)>("_ZNK15SyncManagerBase9isSyncingEv");
        fields[QStringLiteral("native_syncing")] =
                sync && syncing && sync() ? int(syncing(sync())) : -1;
        const auto workflow =
                kobalt::symbol<QObject *(*)()>("_ZN23WirelessWorkflowManager14sharedInstanceEv");
        const auto connecting = kobalt::symbol<bool (*)(QObject *)>(
                "_ZNK23WirelessWorkflowManager12isConnectingEv");
        fields[QStringLiteral("native_connecting")] =
                workflow && connecting && workflow() ? int(connecting(workflow())) : -1;
        const auto instance =
                kobalt::symbol<QObject *(*)()>("_ZN15WirelessManager14sharedInstanceEv");
        const auto up = kobalt::symbol<bool (*)(QObject *)>("_ZNK15WirelessManager2upEv");
        fields[QStringLiteral("native_up")] =
                instance && up && instance() ? int(up(instance())) : -1;
        return fields;
    });
#endif
    if (!verified)
        return nativeWirelessIdlePredicate(watchdog);
#ifdef KOBALT_WIFI_IDLE_FIX_DISABLED
    return nativeWirelessIdlePredicate(watchdog);
#else
    auto *fix = correction(watchdog);
    return fix ? fix->evaluate() : nativeWirelessIdlePredicate(watchdog);
#endif
}
