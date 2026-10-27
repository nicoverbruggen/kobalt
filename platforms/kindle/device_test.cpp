#include "device.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>

using namespace kobalt::kindle;
namespace {
void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
void write(const QString &path, QByteArray data, bool executable = false) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "Cannot create device fixture");
    check(file.write(data) == data.size(), "Cannot write device fixture");
    file.close();
    if (executable)
        check(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "Cannot make fixture executable");
}
QByteArray read(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Cannot read device fixture");
    return file.readAll().trimmed();
}
bool wait(const std::function<bool()> &ready, int milliseconds = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
PropertyReply value(const char *text) { return {true, QByteArray(text)}; }
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    check(argc == 2 && QDir().mkpath(QString::fromLocal8Bit(argv[1])), "Pass an ignored test directory");
    QTemporaryDir temporary(QString::fromLocal8Bit(argv[1]) + "/device-XXXXXX");
    check(temporary.isValid(), "Cannot create device test directory");
    const auto root = temporary.path();
    auto values = QMap<QString, PropertyReply>{{"battLevel", value("70")}, {"isCharging", value("0")},
        {"flMaxIntensity", value("24")}, {"flIntensity", value("18")}, {"currentAmberLevel", value("0")},
        {"enable", value("1")}, {"cmState", value("CONNECTED")}, {"signalStrength", value("4/5")},
        {"BTstate", value("2")}, {"BTconnectedDevName", value("Headphones")}, {"state", value("active")}};
    auto state = Device::decode(values);
    check(state.battery.percent == 70 && !state.battery.charging && !state.battery.fullyCharged, "Battery state decode");
    check(state.wifi.enabled && state.wifi.connected && state.wifi.strength == 3, "Wi-Fi level conversion");
    check(state.bluetoothConnected && state.hasLight() && state.hasWarmth(), "Device capabilities decode");
    values["BTstate"] = value("1");
    check(!Device::decode(values).bluetoothConnected, "Enabled Bluetooth with a stale name is not connected");
    values["BTstate"] = value("0");
    values["cmState"] = value("NA");
    values["battLevel"] = value("101");
    state = Device::decode(values);
    check(!state.bluetoothConnected && !state.wifi.connected && state.wifi.strength == -1 && state.battery.percent == -1,
          "Unknown/offline state must not become a false connected device or valid percentage");
    values["cmState"] = value("CONNECTED_LOCAL");
    values["signalStrength"] = value("bad");
    state = Device::decode(values);
    check(state.wifi.connected && state.wifi.strength == -1, "Unknown Wi-Fi quality must remain unknown");
    values["enable"] = value("0");
    check(!Device::decode(values).wifi.connected, "Disabled Wi-Fi cannot remain connected");
    for (int level = 0; level <= 24; ++level)
        check(Device::percentToLevel(Device::levelToPercent(level, 24), 24) == level, "Brightness round-trip loses native step");
    check(Device::levelToPercent(-1, 24) == -1 && Device::percentToLevel(20, 0) == -1, "Invalid light state");

    for (auto it = values.cbegin(); it != values.cend(); ++it)
        write(root + '/' + it.key(), it.value().value);
    write(root + "/battLevel", "70");
    write(root + "/enable", "1");
    write(root + "/cmState", "CONNECTED");
    write(root + "/signalStrength", "4/5");
    auto escaped = root;
    escaped.replace('\'', "'\\''");
    const auto prefix = ("#!/bin/sh\nroot='" + escaped + "'\n").toUtf8();
    write(root + "/get", prefix + R"SH(
case "$4" in
slow) exec sleep 3 ;;
missing) exit 1 ;;
esac
if [ -f "$root/fail-$4" ]; then
    rm "$root/fail-$4"
    exec sleep 3
fi
cat "$root/$4"
)SH", true);
    write(root + "/set", prefix + R"SH(
[ "$5" != 99 ] || exit 1
printf '%s\n' "$5" > "$root/$4"
)SH", true);
    Properties::Commands commands{root + "/get", root + "/set", 500};
    {
        Properties properties(commands);
        bool failed = false, next = false;
        QElapsedTimer elapsed;
        elapsed.start();
        properties.read({"test", "slow"}, [&](PropertyReply result) { failed = !result.ok; });
        properties.read({"test", "battLevel"}, [&](PropertyReply result) { next = result.ok && result.value == "70"; });
        check(wait([&] { return failed && next; }) && elapsed.elapsed() < 1500, "Timed-out property blocked the query queue");
        properties.stop();
    }
    auto device = std::make_shared<Device>(commands);
    check(wait([&] { return device->initialized(); }), "Initial device sample never completed");
    check(device->state().battery.percent == 70 && device->state().brightness == 18, "Initial device readback");
    auto light = makeLight(device);
    check(light->initialize() && light->hasWarmth() && !light->hasAutomaticBrightness() && !light->hasAutomaticWarmth(),
          "Standard PW5 exposed unsupported automatic controls");
    check(light->beginGesture() && light->setGestureLevel(light->brightness() - light->brightnessStep()) == 71 &&
          read(root + "/flIntensity") == "17", "Brightness button step cannot leave its quantized level");
    light->finishGesture(false);
    check(light->beginGesture() && light->setGestureLevel(50) == 50 && read(root + "/flIntensity") == "12",
          "Brightness adjustment did not reach the native level");
    light->finishGesture(false);
    check(read(root + "/flIntensity") == "18" && light->brightness() == 75, "Canceled brightness did not restore its original level");
    check(light->beginGesture() && light->setGestureLevel(25) == 25, "Brightness commit setup");
    light->finishGesture(true);
    check(read(root + "/flIntensity") == "6", "Committed brightness was rolled back");
    check(light->setWarmth(12) == 12 && light->saveWarmth() && read(root + "/currentAmberLevel") == "12",
          "Warmth readback or save failed");
    check(light->setWarmth(25) == -1 && device->setBrightness(101) == -1, "Invalid light input was accepted");
    check(light->beginGesture(), "Cannot begin readback-failure gesture");
    write(root + "/fail-flIntensity", "1");
    check(light->setGestureLevel(50) == -1 && read(root + "/flIntensity") == "12", "Missing readback did not report failure");
    light->finishGesture(false);
    check(read(root + "/flIntensity") == "6", "Unconfirmed write prevented cancellation rollback");
    check(light->beginGesture() && light->setGestureLevel(50) == 50, "Poll-failure gesture setup");
    write(root + "/fail-flIntensity", "1");
    bool sampled = false;
    auto connection = QObject::connect(device.get(), &Device::changed, [&] { sampled = true; });
    device->refresh();
    check(wait([&] { return sampled; }), "Failed-property poll never completed");
    QObject::disconnect(connection);
    light->finishGesture(false);
    check(read(root + "/flIntensity") == "6", "Failed status poll prevented cancellation rollback");
    write(root + "/fail-currentAmberLevel", "1");
    check(light->setWarmth(18) == -1 && !light->saveWarmth(), "Unconfirmed warmth was reported saved");
    check(light->setWarmth(12) == 12 && light->saveWarmth(), "Warmth rollback after failed readback");
    light.reset();
    device->stop();
    device.reset();
    write(root + "/state", "screenSaver");
    write(root + "/bridge", "#!/bin/sh\nprintf 'ready\\n'\nwhile IFS= read -r line; do :; done\n", true);
    device = std::make_shared<Device>(commands);
    bool lifecycleFailed = false;
    QObject::connect(device.get(), &Device::lifecycleFailed, [&](const QString &) { lifecycleFailed = true; });
    device->startLifecycle(root + "/bridge");
    check(!device->initialized() && wait([&] { return device->initialized(); }) && device->state().sleeping,
          "Initial sleep before event subscription was lost");
    check(!lifecycleFailed, "Healthy lifecycle bridge reported failure");
    device->stop();
    device.reset();
    write(root + "/bridge", "#!/bin/sh\nprintf 'ready\\ngoingToScreenSaver\\noutOfScreenSaver\\n'\nwhile IFS= read -r line; do :; done\n", true);
    device = std::make_shared<Device>(commands);
    int transitions = 0, usbRequests = 0;
    QObject::connect(device.get(), &Device::sleepingChanged, [&](bool) { ++transitions; });
    QObject::connect(device.get(), &Device::usbRequested, [&] { ++usbRequests; });
    device->startLifecycle(root + "/bridge");
    check(wait([&] { return device->initialized() && transitions == 2; }) && !device->state().sleeping,
          "Stale initial snapshot overwrote a newer wake event");
    qputenv("KOBALT_KINDLE_USB_NETWORK", "1");
    device->simulatePowerEvent("usbPlugIn");
    check(usbRequests == 0, "USB networking incorrectly closed the app");
    qputenv("KOBALT_KINDLE_USB_NETWORK", "0");
    device->simulatePowerEvent("usbPlugIn");
    check(usbRequests == 1, "USB storage request did not release the app");
    qunsetenv("KOBALT_KINDLE_USB_NETWORK");
    device->stop();
    device.reset();
    std::puts("Kindle device parsing, bounded IPC, light readback, commit and cancellation passed.");
    return 0;
}
