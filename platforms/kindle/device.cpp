#include "device.h"
#include <QDeadlineTimer>
#include <QFile>
#include <QPointer>
#include <QDebug>
#include <QtMath>
#include <optional>
#include <utility>

namespace kobalt::kindle {
namespace {
const QString powerd = QStringLiteral("com.lab126.powerd");
std::optional<int> integer(const PropertyReply &reply, int minimum, int maximum) {
    if (!reply.ok)
        return {};
    bool ok = false;
    const int value = reply.value.toInt(&ok);
    return ok && value >= minimum && value <= maximum ? std::optional<int>(value) : std::nullopt;
}
QString text(const QMap<QString, PropertyReply> &values, const char *name) {
    const auto reply = values.value(QString::fromLatin1(name));
    return reply.ok ? QString::fromUtf8(reply.value) : QString();
}
class Light final : public LightControls {
public:
    explicit Light(std::shared_ptr<Device> device) : device(std::move(device)) {}
    ~Light() override { finishGesture(false); }
    bool initialize() override { return device->state().hasLight(); }
    bool hasWarmth() const override { return device->state().hasWarmth(); }
    int warmth() const override { return device->state().warmth; }
    int warmthMinimum() const override { return 0; }
    int warmthMaximum() const override { return 24; }
    int warmthStep() const override { return 1; }
    // Kindle's existing schedule remains owned by powerd. Manual level
    // changes must not replace that schedule or pretend to configure it.
    bool hasAutomaticWarmth() const override { return false; }
    bool automaticWarmth() const override { return false; }
    bool setAutomaticWarmth(bool) override { return false; }
    int setWarmth(int value) override { return device->setWarmth(value); }
    bool saveWarmth() override { return device->warmthVerified(); }
    bool beginGesture() override {
        if (active)
            return true;
        if (!initialize())
            return false;
        original = brightness();
        active = true;
        return true;
    }
    int setGestureLevel(int percent) override { return active ? device->setBrightness(percent) : -1; }
    void finishGesture(bool save) override {
        if (!active)
            return;
        active = false;
        if (!save)
            device->setBrightness(original);
    }
    int brightness() const override {
        return Device::levelToPercent(device->state().brightness, device->state().maximumBrightness);
    }
    int brightnessStep() const override {
        return qMax(1, qRound(100.0 / device->state().maximumBrightness));
    }
    bool gesturing() const override { return active; }
private:
    std::shared_ptr<Device> device;
    int original = 0;
    bool active = false;
};
}
Device::Device(Properties::Commands commands, QObject *parent) : QObject(parent), properties(std::move(commands)) {
    poll.setInterval(15000);
    connect(&poll, &QTimer::timeout, this, &Device::refresh);
    poll.start();
    QTimer::singleShot(0, this, &Device::refresh);
}
Device::~Device() { stop(); }
int Device::percentToLevel(int percent, int maximum) {
    return maximum > 0 ? qRound(qBound(0, percent, 100) * maximum / 100.0) : -1;
}
int Device::levelToPercent(int level, int maximum) {
    return maximum > 0 && level >= 0 && level <= maximum ? qRound(level * 100.0 / maximum) : -1;
}
DeviceState Device::decode(const QMap<QString, PropertyReply> &values) {
    DeviceState state;
    state.battery.percent = integer(values.value("battLevel"), 0, 100).value_or(-1);
    state.battery.charging = integer(values.value("isCharging"), 0, 1).value_or(0) == 1;
    state.maximumBrightness = integer(values.value("flMaxIntensity"), 1, 255).value_or(24);
    state.brightness = integer(values.value("flIntensity"), 0, state.maximumBrightness).value_or(-1);
    state.warmth = integer(values.value("currentAmberLevel"), 0, 24).value_or(-1);
    state.wifi.enabled = integer(values.value("enable"), 0, 1).value_or(0) == 1;
    const auto connection = text(values, "cmState");
    state.wifi.connected = state.wifi.enabled && (connection == "CONNECTED" || connection == "CONNECTED_LOCAL");
    const auto quality = text(values, "signalStrength");
    if (state.wifi.connected && quality.size() == 3 && quality.at(1) == '/' && quality.at(2) == '5' &&
        quality.at(0) >= '0' && quality.at(0) <= '5')
        state.wifi.strength = qRound((quality.at(0).unicode() - ushort('0')) * 4.0 / 5);
    // The stock connected-name getter accepts state 2 and reads the live list.
    state.bluetoothConnected = integer(values.value("BTstate"), 0, 255).value_or(0) == 2 &&
                               !text(values, "BTconnectedDevName").isEmpty();
    const auto power = text(values, "state");
    state.sleeping = power == "screenSaver" || power == "suspended";
    return state;
}
void Device::refresh() {
    if (stopped || reading)
        return;
    reading = true;
    sample.clear();
    const auto revision = lightRevision;
    const QList<Property> requested{
        {powerd, "battLevel"}, {powerd, "isCharging"}, {powerd, "flMaxIntensity"},
        {powerd, "flIntensity"}, {powerd, "currentAmberLevel"},
        {"com.lab126.wifid", "enable"}, {"com.lab126.wifid", "cmState", false},
        {"com.lab126.wifid", "signalStrength", false}, {"com.lab126.btfd", "BTstate"},
        {powerd, "state", false}};
    auto publish = [this, revision] {
        const auto previous = state_;
        state_ = decode(sample);
        if (lifecycle)
            state_.sleeping = previous.sleeping;
        if (revision != lightRevision) {
            state_.brightness = previous.brightness;
            state_.maximumBrightness = previous.maximumBrightness;
            state_.warmth = previous.warmth;
        } else {
            brightnessConfirmed = state_.hasLight();
            warmthConfirmed = state_.hasWarmth();
            // A failed read must not remove the capability needed to undo an
            // in-progress adjustment. Keep the last level, but require the
            // next write to reach powerd even when its value matches it.
            if (!brightnessConfirmed && previous.hasLight()) {
                state_.brightness = previous.brightness;
                state_.maximumBrightness = previous.maximumBrightness;
            }
            if (!warmthConfirmed && previous.hasWarmth())
                state_.warmth = previous.warmth;
        }
#ifndef KOBALT_KINDLE_NATIVE_TESTING
        QFile status(QStringLiteral("/sys/class/power_supply/bd71827_bat/status"));
        if (status.open(QIODevice::ReadOnly))
            state_.battery.fullyCharged = status.read(64).trimmed() == "Full";
#endif
        reading = false;
        QPointer<Device> guard(this);
        if (!initialized_) {
            initialized_ = true;
            emit initializedChanged();
            if (!guard)
                return;
        }
        if (previous.battery.percent != state_.battery.percent || previous.battery.charging != state_.battery.charging ||
            previous.battery.fullyCharged != state_.battery.fullyCharged) {
            emit batteryChanged();
            if (!guard)
                return;
        }
        if (previous.wifi.enabled != state_.wifi.enabled || previous.wifi.connected != state_.wifi.connected ||
            previous.wifi.strength != state_.wifi.strength) {
            emit wifiChanged();
            if (!guard)
                return;
        }
        if (previous.bluetoothConnected != state_.bluetoothConnected) {
            emit bluetoothChanged();
            if (!guard)
                return;
        }
        emit changed();
    };
    for (int index = 0; index < requested.size(); ++index) {
        const auto property = requested.at(index);
        properties.read(property, [this, property, last = index == requested.size() - 1, publish](PropertyReply reply) {
            sample.insert(property.name, std::move(reply));
            if (!last)
                return;
            if (integer(sample.value("BTstate"), 0, 255).value_or(0) != 2) {
                publish();
                return;
            }
            properties.read({"com.lab126.btfd", "BTconnectedDevName", false}, [this, publish](PropertyReply name) {
                sample.insert("BTconnectedDevName", std::move(name));
                publish();
            });
        });
    }
}
int Device::setLight(const QString &name, int value, int maximum) {
    if (stopped || value < 0 || value > maximum)
        return -1;
    const int current = name == "flIntensity" ? state_.brightness : state_.warmth;
    bool &confirmed = name == "flIntensity" ? brightnessConfirmed : warmthConfirmed;
    if (confirmed && value == current)
        return current;
    QDeadlineTimer limit(properties.timeout());
    const Property property{powerd, name};
    confirmed = false;
    ++lightRevision;
    if (!properties.write(property, value, int(limit.remainingTime())))
        return -1;
    const auto actual = integer(properties.readNow(property, int(limit.remainingTime())), 0, maximum);
    if (!actual) {
        refresh();
        return -1;
    }
    confirmed = true;
    (name == "flIntensity" ? state_.brightness : state_.warmth) = *actual;
    emit changed();
    return *actual;
}
int Device::setBrightness(int percent) {
    if (!state_.hasLight() || percent < 0 || percent > 100)
        return -1;
    const int level = setLight("flIntensity", percentToLevel(percent, state_.maximumBrightness), state_.maximumBrightness);
    return levelToPercent(level, state_.maximumBrightness);
}
int Device::setWarmth(int level) {
    return state_.hasWarmth() ? setLight("currentAmberLevel", level, 24) : -1;
}
void Device::stop() {
    stopped = true;
    poll.stop();
    bridgeDeadline.stop();
    bridge.disconnect(this);
    if (bridge.state() != QProcess::NotRunning) {
        bridge.closeWriteChannel();
        if (!bridge.waitForFinished(500)) {
            bridge.kill();
            bridge.waitForFinished(200);
        }
    }
    properties.stop();
}
void Device::startLifecycle(const QString &program) {
    if (lifecycle || stopped)
        return;
    lifecycle = true;
    bridgeDeadline.setSingleShot(true);
    connect(&bridgeDeadline, &QTimer::timeout, this, [this] {
        bridge.kill();
        emit lifecycleFailed(QStringLiteral("Kindle power service did not become ready"));
    });
    connect(&bridge, &QProcess::readyReadStandardOutput, this, [this] {
        bridgeOutput += bridge.readAllStandardOutput();
        if (bridgeOutput.size() > 4096) {
            bridge.kill();
            return;
        }
        while (bridgeOutput.contains('\n')) {
            const int end = bridgeOutput.indexOf('\n');
            const auto event = bridgeOutput.left(end);
            bridgeOutput.remove(0, end + 1);
            powerEvent(event);
        }
    });
    connect(&bridge, &QProcess::readyReadStandardError, this, [this] {
        qWarning().noquote() << "Kindle power bridge:" << bridge.readAllStandardError().trimmed();
    });
    connect(&bridge, &QProcess::started, this, [this] {
        const auto directory = qEnvironmentVariable("KOBALT_KINDLE_RUN_DIR");
        if (!directory.isEmpty()) {
            QFile pid(directory + QStringLiteral("/bridge.pid"));
            if (pid.open(QIODevice::WriteOnly))
                pid.write(QByteArray::number(bridge.processId()) + '\n');
        }
    });
    connect(&bridge, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        bridgeDeadline.stop();
        const auto errors = bridge.readAllStandardError().trimmed();
        if (!errors.isEmpty())
            qWarning().noquote() << "Kindle power bridge:" << errors;
        if (!stopped)
            emit lifecycleFailed(QStringLiteral("Kindle power service stopped (exit %1, %2)")
                .arg(code).arg(status == QProcess::CrashExit ? QStringLiteral("crashed") : QStringLiteral("normal exit")));
    });
    connect(&bridge, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!stopped && error == QProcess::FailedToStart) {
            bridgeDeadline.stop();
            emit lifecycleFailed(QStringLiteral("Kindle power service could not start"));
        }
    });
    bridgeDeadline.start(12000);
    bridge.start(program, {});
}
void Device::activity() {
    if (!bridgeReady || state_.sleeping || (lastActivity.isValid() && lastActivity.elapsed() < 30000))
        return;
    lastActivity.start();
    bridge.write("a\n");
}
void Device::setSleeping(bool sleeping) {
    ++powerRevision;
    const bool first = !std::exchange(powerKnown, true);
    QPointer<Device> guard(this);
    if (state_.sleeping != sleeping) {
        state_.sleeping = sleeping;
        emit sleepingChanged(sleeping);
        if (!guard)
            return;
    }
    if (first)
        emit initializedChanged();
}
void Device::setSleepFrame(QImage frame) {
    sleepFrame_ = std::move(frame);
    emit sleepFrameChanged();
}
void Device::powerEvent(const QByteArray &event) {
    qInfo().noquote() << "Kindle power:" << event;
    if (event == "ready") {
        bridgeReady = true;
        bridgeDeadline.stop();
        const auto revision = powerRevision;
        properties.read({powerd, "state", false}, [this, revision](PropertyReply reply) {
            if (revision != powerRevision)
                return;
            if (!reply.ok) {
                emit lifecycleFailed(QStringLiteral("Kindle power state is unavailable"));
                return;
            }
            if (reply.value == "screenSaver" || reply.value == "suspended")
                setSleeping(true);
            else if (reply.value == "active")
                setSleeping(false);
            else
                emit lifecycleFailed(QStringLiteral("Kindle power state is unsupported"));
        });
        activity();
    } else if (event == "goingToScreenSaver" || event == "readyToSuspend") {
        setSleeping(true);
    } else if (event == "outOfScreenSaver") {
        setSleeping(false);
        refresh();
        lastActivity.invalidate();
        activity();
    } else if (event == "usbPlugIn") {
        // The supervisor snapshots this before pausing volumd. Querying the
        // paused publisher here would time out even in USB networking mode.
        if (qEnvironmentVariable("KOBALT_KINDLE_USB_NETWORK") != QStringLiteral("1"))
            emit usbRequested();
    } else if (event == "userstoreIsLikelyToUnMount") {
        emit usbRequested();
    }
}
std::shared_ptr<Device> makeDevice() {
    Properties::Commands commands;
#ifdef KOBALT_KINDLE_NATIVE_TESTING
    // Native checks must never invoke host power or connectivity commands.
    commands.get = commands.set = QStringLiteral("/nonexistent/kobalt-kindle-device");
    if (qEnvironmentVariableIsSet("KOBALT_KINDLE_TEST_LIPC_GET"))
        commands.get = qEnvironmentVariable("KOBALT_KINDLE_TEST_LIPC_GET");
    if (qEnvironmentVariableIsSet("KOBALT_KINDLE_TEST_LIPC_SET"))
        commands.set = qEnvironmentVariable("KOBALT_KINDLE_TEST_LIPC_SET");
#endif
    return std::make_shared<Device>(std::move(commands));
}
std::unique_ptr<LightControls> makeLight(std::shared_ptr<Device> device) {
    return std::make_unique<Light>(std::move(device));
}
}
