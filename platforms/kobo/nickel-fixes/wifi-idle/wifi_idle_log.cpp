// Physical Wi-Fi A/B diagnostics. Both packages use this same observer.
// It reads radio state only. It sends no network traffic, changes no radio
// setting and holds no wake lock. File reads, JSON and writes use the existing
// bounded diagnostic worker. Normal packages do not create its polling timer.
#include "wifi_idle_log.h"
#ifdef KOBALT_WIFI_IDLE_DIAGNOSTICS
#  include "build_info.h"
#  include "platforms/kobo/common/native_symbol.h"
#  include "platforms/kobo/common/native_ui.h"
#  include "platforms/kobo/common/native_wifi_power.h"
#  include "support/diagnostics.h"
#  include "support/main_thread.h"
#  include <QDateTime>
#  include <QDir>
#  include <QFile>
#  include <QFileInfo>
#  include <QJsonArray>
#  include <QJsonDocument>
#  include <QPointer>
#  include <QTimer>
#  include <atomic>
#  include <memory>
#  include <time.h>
#  include <unistd.h>

namespace kobalt::nickel {
void (*nativeWifiIdleOff)(QObject *);

namespace {
std::atomic<unsigned> offRequests{ 0 }, offReturns{ 0 };

qint64 bootMilliseconds()
{
    timespec value{};
    return clock_gettime(CLOCK_BOOTTIME, &value) == 0
            ? qint64(value.tv_sec) * 1000 + value.tv_nsec / 1000000
            : -1;
}

QByteArray read(const QString &path, int limit, bool *ok = nullptr)
{
    QFile file(path);
    const bool opened = file.open(QIODevice::ReadOnly);
    if (ok)
        *ok = opened;
    return opened ? file.read(limit) : QByteArray();
}

void append(QJsonObject fields)
{
    mainthread::blocking("wifi-idle-log");
    const QString directory = QStringLiteral(KOBALT_CONFIG_DIR "/diagnostics");
    if (!QDir().mkpath(directory))
        return;
    const QString path = directory + QStringLiteral("/wifi-idle.jsonl");
    if (QFileInfo(path).size() >= 1024 * 1024) {
        QFile::remove(path + QStringLiteral(".previous"));
        if (!QFile::rename(path, path + QStringLiteral(".previous")))
            return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    file.write(QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n');
    file.flush();
}
} // namespace

QJsonObject collectWifiIdleHardware(const QString &root, QJsonObject fields)
{
    fields[QStringLiteral("sample_boot_ms")] = double(bootMilliseconds());
    fields[QStringLiteral("utc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    bool modulesAvailable = false;
    const auto modules = read(root + QStringLiteral("/proc/modules"), 65536, &modulesAvailable);
    QJsonArray names;
    const auto configuredModule = qgetenv("WIFI_MODULE");
    bool configuredLoaded = false;
    for (const auto &line : modules.split('\n')) {
        const auto name = line.left(line.indexOf(' '));
        if (!name.isEmpty()) {
            names.append(QString::fromLatin1(name));
            configuredLoaded = configuredLoaded || name == configuredModule;
        }
    }
    fields[QStringLiteral("modules_available")] = modulesAvailable;
    fields[QStringLiteral("modules")] = names;
    fields[QStringLiteral("configured_wifi_module")] = QString::fromLatin1(configuredModule);
    fields[QStringLiteral("wifi_driver_loaded")] =
            modulesAvailable && !configuredModule.isEmpty() ? int(configuredLoaded) : -1;
    const QString interface = root + QStringLiteral("/sys/class/net/wlan0");
    fields[QStringLiteral("interface_exists")] = QFileInfo(interface).exists();
    fields[QStringLiteral("operstate")] =
            QString::fromLatin1(read(interface + "/operstate", 64).trimmed());
    fields[QStringLiteral("carrier")] =
            QString::fromLatin1(read(interface + "/carrier", 64).trimmed());
    fields[QStringLiteral("interface_flags")] =
            QString::fromLatin1(read(interface + "/flags", 64).trimmed());
    fields[QStringLiteral("runtime_power_status")] =
            QString::fromLatin1(read(interface + "/device/power/runtime_status", 64).trimmed());
    QJsonArray rfkill;
    const QDir radios(root + QStringLiteral("/sys/class/rfkill"));
    const auto entries = radios.entryList(QStringList() << QStringLiteral("rfkill*"),
                                          QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : entries.mid(0, 16)) {
        const auto path = radios.filePath(entry);
        QJsonObject radio;
        for (const auto &field : { "type", "state", "soft", "hard" })
            radio[QString::fromLatin1(field)] =
                    QString::fromLatin1(read(path + "/" + field, 64).trimmed());
        rfkill.append(radio);
    }
    fields[QStringLiteral("rfkill")] = rfkill;
    const auto module = QFileInfo(interface + "/device/driver/module").symLinkTarget();
    fields[QStringLiteral("interface_driver_module")] = QFileInfo(module).fileName();
    const auto version = read(root + QStringLiteral("/mnt/onboard/.kobo/version"), 1024).split(',');
    if (version.size() > 2)
        fields[QStringLiteral("firmware")] = QString::fromLatin1(version.at(2));
    return fields;
}

void startWifiIdleLog(QObject *owner, bool verified, std::function<QJsonObject()> state)
{
    static QPointer<QObject> context;
    if (context)
        return;
    context = new QObject(owner);
    auto *timer = new QTimer(context);
    timer->setInterval(2000);
    const QString run = QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzz")
            + QStringLiteral("-") + QString::number(getpid());
    const auto bluetooth = std::make_shared<int>(int(BluetoothPower::Unknown));
    const auto pending = std::make_shared<std::atomic_bool>(false);
    QObject::connect(timer, &QTimer::timeout, context, [state, verified, run, bluetooth, pending] {
        if (!diagnosticsActive() || pending->exchange(true))
            return;
        auto fields = state();
        fields[QStringLiteral("run")] = run;
        fields[QStringLiteral("source_commit")] = QString::fromLatin1(kobaltCommit);
#  ifdef KOBALT_WIFI_IDLE_FIX_DISABLED
        fields[QStringLiteral("fix_enabled")] = false;
#  else
        fields[QStringLiteral("fix_enabled")] = true;
#  endif
        fields[QStringLiteral("predicate_verified")] = verified;
        fields[QStringLiteral("observed_boot_ms")] = double(bootMilliseconds());
        fields[QStringLiteral("bluetooth_power")] = *bluetooth == int(BluetoothPower::Off)
                ? QStringLiteral("off")
                : *bluetooth == int(BluetoothPower::On) ? QStringLiteral("on")
                                                        : QStringLiteral("unknown");
        fields[QStringLiteral("off_requests")] = int(offRequests.load());
        fields[QStringLiteral("off_returns")] = int(offReturns.load());
        if (!sampleDiagnostics([fields, pending] {
                append(collectWifiIdleHardware(QString(), fields));
                pending->store(false);
            }))
            pending->store(false);
        queryNativeBluetoothPower(context,
                                  [bluetooth](BluetoothPower power) { *bluetooth = int(power); });
    });
    timer->start();
}
} // namespace kobalt::nickel

extern "C" void kobalt_wifi_idle_off(QObject *manager)
{
    using namespace kobalt::nickel;
    ++offRequests;
    if (nativeWifiIdleOff)
        nativeWifiIdleOff(manager);
    ++offReturns;
}

#else
namespace kobalt::nickel {
void startWifiIdleLog(QObject *, bool, std::function<QJsonObject()>) { }
} // namespace kobalt::nickel
#endif
