#include "support/qt_compat.h"
#include "platforms/kobo/common/publication_source.h"
#include "platforms/kobo/common/native_light.h"
#include "support/diagnostics.h"
#include <QApplication>
#include <QMetaType>
#include <QVariant>
#include <QThread>
#ifndef KOBALT_READER_HARDWARE
#include <QTimer>
#include <QTouchEvent>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <unistd.h>
#include <cerrno>
#endif
#include <dlfcn.h>
#include <new>
#include <cstddef>
#include <utility>
#include <array>
#include <QSettings>

namespace kobalt {
#ifndef KOBALT_READER_HARDWARE
int (*lightTestIoctl)(int, unsigned long, void *) = nullptr;
static bool simulateLightDevice = false;
static bool observedLightDevice = false;
static int lightDeviceError = 0;
#endif
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
struct Api {
    const void *(*device)() = symbol<decltype(device)>("_ZN6Device16getCurrentDeviceEv");
    bool (*supports)(const void *) = symbol<decltype(supports)>("_ZNK6Device13supportsLightEv");
    void (*constructLight)(void *, QObject *) = symbol<decltype(constructLight)>("_ZN10FrontLightC1EP7QObject");
    int (*brightness)(const void *) = symbol<decltype(brightness)>("_ZNK10FrontLight10brightnessEv");
    int (*maximum)(const void *) = symbol<decltype(maximum)>("_ZNK10FrontLight13maxBrightnessEv");
    void (*setBrightness)(const void *, int,
                          int) = symbol<decltype(setBrightness)>("_ZNK10FrontLight13setBrightnessEii");
    void (*cancelBrightnessChanging)(void *) =
        symbol<decltype(cancelBrightnessChanging)>("_ZN10FrontLight24cancelBrightnessChangingEv");
    void (*constructGesture)(void *, QObject *, void *) =
        symbol<decltype(constructGesture)>("_ZN21BrightnessEventFilterC1EP7QObjectP23BrightnessEventDelegate");
    void (*finishGesture)(void *) = symbol<decltype(finishGesture)>("_ZN21BrightnessEventFilter13finishGestureEv");
    // Firmware 4.46.23836 requires a 32-byte FrontLight, including its
    // timer, and a 104-byte BrightnessEventFilter. Qt5 exposes no metatype
    // size check for these classes.
    static constexpr size_t lightBytes = 32, gestureBytes = 104;
    bool archiveAvailable = native::publicationSource().available();
    bool gestureValid() const {
        return archiveAvailable && device && supports && constructLight && brightness && maximum && setBrightness &&
               cancelBrightnessChanging && constructGesture && finishGesture;
    }
    bool valid() const {
        return gestureValid();
    }
    bool (*sensor)(const void *) = symbol<decltype(sensor)>("_ZNK6Device21hasAmbientLightSensorEv");
    bool (*ambientEnabled)(void *) =
        symbol<decltype(ambientEnabled)>("_ZN13PowerSettings28getAmbientLightSensorEnabledEv");
    void (*setAmbientEnabled)(void *, bool) =
        symbol<decltype(setAmbientEnabled)>("_ZN13PowerSettings28setAmbientLightSensorEnabledEb");
    bool (*natural)(const void *) = symbol<decltype(natural)>("_ZNK6Device15hasNaturalLightEv");
    int (*temperature)(const void *) = symbol<decltype(temperature)>("_ZNK10FrontLight11temperatureEv");
    int (*minTemperature)(const void *) = symbol<decltype(minTemperature)>("_ZNK10FrontLight14minTemperatureEv");
    int (*maxTemperature)(const void *) = symbol<decltype(maxTemperature)>("_ZNK10FrontLight14maxTemperatureEv");
    int (*intervals)(const void *) = symbol<decltype(intervals)>("_ZNK10FrontLight23numTemperatureIntervalsEv");
    void (*setTemperature)(void *, int, bool,
                           int) = symbol<decltype(setTemperature)>("_ZN10FrontLight14setTemperatureEibi");
    void (*constructSettings)(void *, const void *,
                              bool) = symbol<decltype(constructSettings)>("_ZN8SettingsC2ERK6Deviceb");
    void (*destroySettings)(void *) = symbol<decltype(destroySettings)>("_ZN8SettingsD2Ev");
    void **powerVtable = symbol<void **>("_ZTV13PowerSettings");
    bool (*automatic)(void *) = symbol<decltype(automatic)>("_ZN13PowerSettings16autoColorEnabledEv");
    void (*setAutomatic)(void *, bool) = symbol<decltype(setAutomatic)>("_ZN13PowerSettings19setAutoColorEnabledEb");
    QTime (*bedtime)(void *) = symbol<decltype(bedtime)>("_ZN13PowerSettings7bedTimeEv");
    void (*setBedtime)(void *, QTime) = symbol<decltype(setBedtime)>("_ZN13PowerSettings10setBedTimeE5QTime");
    void (*saveTemperature)(void *, int) = symbol<decltype(saveTemperature)>("_ZN13PowerSettings15setColorSettingEi");
    bool automaticBrightnessValid() const {
        return valid() && sensor && ambientEnabled && setAmbientEnabled && constructSettings && destroySettings &&
               powerVtable;
    }
    bool warmthValid() const {
        return valid() && natural && temperature && minTemperature && maxTemperature && intervals && setTemperature &&
               constructSettings && destroySettings && powerVtable && automatic && setAutomatic && bedtime &&
               setBedtime && saveTemperature;
    }
};
const Api &api() {
    static const Api value;
    return value;
}
// PowerSettings uses a 28-byte Settings base and its derived vtable.
// Keep it short-lived so changes use Nickel's cache, persistence and
// SettingEvent delivery to BedtimeManager.
struct PowerSettings {
    std::array<void *, 7> storage{};
    PowerSettings() {
        api().constructSettings(storage.data(), api().device(), false);
        storage[0] = api().powerVtable + 2;
    }
    ~PowerSettings() {
        api().destroySettings(storage.data());
    }
    void *get() {
        return storage.data();
    }
};
bool stored(const QString &key, const QVariant &expected) {
    QSettings file("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf", QSettings::IniFormat);
    file.sync();
    const auto value = file.value("PowerOptions/" + key);
    // QSettings reads booleans and integers from an INI file as strings.
    return file.status() == QSettings::NoError && value.isValid() && value.toString() == expected.toString();
}

}
NativeLight::NativeLight(QWidget *anchor) : QObject(anchor), anchor(anchor) {
#ifndef KOBALT_READER_HARDWARE
    if (!QFile::exists("/instance/enable-brightness-test"))
        return;
    auto *test = new QTimer(this);
    connect(test, &QTimer::timeout, this, [this] {
        QFile input("/instance/brightness-test.json");
        if (!input.open(QIODevice::ReadOnly))
            return;
        const auto request = QJsonDocument::fromJson(input.read(4096)).object();
        input.close();
        input.remove();
        QJsonObject result = kobalt::qt::object({{"id", request.value("id")},
                                                 {"light_bytes", int(Api::lightBytes)},
                                                 {"gesture_bytes", int(Api::gestureBytes)}});
        observedLightDevice = false;
        lightDeviceError = 0;
        if (request.value("action").toString() == "native-settings") {
            const bool ready = initialize() && hasWarmth();
            result["ready"] = ready;
            if (ready) {
                // Simulate another native settings caller. Do not call our
                // setters or touch the panel: it must observe native changes.
                {
                    PowerSettings settings;
                    if (request.contains("automatic"))
                        api().setAutomatic(settings.get(), request.value("automatic").toBool());
                    if (request.contains("bedtime"))
                        api().setBedtime(settings.get(),
                                         QTime::fromString(request.value("bedtime").toString(), "HH:mm"));
                }
                result["automatic"] = automaticWarmth();
                result["bedtime"] = bedtime().toString("HH:mm");
                result["temperature"] = api().temperature(frontLight);
                result["warmth"] = warmth();
                result["minimum"] = warmthMinimum();
                result["maximum"] = warmthMaximum();
                result["step"] = warmthStep();
            }
            QSaveFile output("/instance/artifacts/brightness-test.json");
            if (output.open(QIODevice::WriteOnly)) {
                output.write(QJsonDocument(result).toJson());
                output.commit();
            }
            return;
        }
        if (request.value("action").toString() == "cancel-touch") {
            if (this->anchor && this->anchor->window()) {
                const auto target = request.value("target").toString();
                const char *name = target == "warmth" ? "rrWarmth" : target == "brightness" ? "rrBrightness" : "rrPage";
                if (auto *page = this->anchor->window()->findChild<QWidget *>(name)) {
                    QTouchEvent cancel(QEvent::TouchCancel);
                    QApplication::sendEvent(page, &cancel);
                }
            }
            result["final"] = brightness();
            QSaveFile output("/instance/artifacts/brightness-test.json");
            if (output.open(QIODevice::WriteOnly)) {
                output.write(QJsonDocument(result).toJson());
                output.commit();
            }
            return;
        }
        simulateLightDevice = request.value("simulate_device").toBool(false);
        const bool started = beginGesture();
        result["started"] = started;
        if (started) {
            result["before"] = brightness();
            result["after"] = setGestureLevel(request.value("percent").toInt());
            finishGesture(!request.value("cancel").toBool());
            result["final"] = brightness();
        }
        result["observed_ntx_light"] = observedLightDevice;
        result["device_errno"] = lightDeviceError;
        result["simulated_device"] = simulateLightDevice;
        QSaveFile output("/instance/artifacts/brightness-test.json");
        if (output.open(QIODevice::WriteOnly)) {
            output.write(QJsonDocument(result).toJson());
            output.commit();
        }
    });
    test->start(100);
#endif
}
NativeLight::~NativeLight() {
    finishGesture(false);
    delete frontLight.data();
}
bool NativeLight::available() {
    return api().valid() && api().supports(api().device());
}
bool NativeLight::initialize() {
    if (!qApp || QThread::currentThread() != qApp->thread() || !available())
        return false;
    if (!frontLight) {
        void *storage = ::operator new(Api::lightBytes);
        api().constructLight(storage, this);
        frontLight = static_cast<QObject *>(storage);
    }
    return api().maximum(frontLight) == 100;
}
bool NativeLight::beginGesture() {
    if (gesturing())
        return true;
    if (!initialize())
        return false;
    const int level = brightness();
    if (level < 0 || level > 100)
        return false;
    gestureStart = level;
    gestureAutomatic = automaticBrightness();
    reportedGestureFailure = false;
    return true;
}
bool NativeLight::hasAutomaticBrightness() const {
    return frontLight && api().automaticBrightnessValid() && api().sensor(api().device());
}
bool NativeLight::automaticBrightness() const {
    if (!hasAutomaticBrightness()) {
        return false;
    }
    PowerSettings settings;
    return api().ambientEnabled(settings.get());
}
bool NativeLight::setAutomaticBrightness(bool enabled) {
    if (!hasAutomaticBrightness()) {
        return false;
    }
    {
        PowerSettings settings;
        api().setAmbientEnabled(settings.get(), enabled);
    }
    const bool saved = automaticBrightness() == enabled && stored("AmbientLightSensorEnabled", enabled);
    diagnostic(saved ? "automatic-brightness-saved" : "automatic-brightness-save-failed",
               kobalt::qt::object({{"enabled", enabled}}));
    return saved;
}
bool NativeLight::hasWarmth() const {
    return frontLight && api().warmthValid() && api().natural(api().device()) && warmthMaximum() > warmthMinimum();
}
int NativeLight::warmth() const {
    // Nickel stores color temperature: a lower value produces warmer light.
    return frontLight && api().warmthValid() ? warmthMinimum() + warmthMaximum() - api().temperature(frontLight) : -1;
}
int NativeLight::warmthMinimum() const {
    return frontLight && api().warmthValid() ? api().minTemperature(frontLight) : 0;
}
int NativeLight::warmthMaximum() const {
    return frontLight && api().warmthValid() ? api().maxTemperature(frontLight) : 0;
}
int NativeLight::warmthStep() const {
    if (!hasWarmth())
        return 1;
    const int intervals = api().intervals(frontLight);
    // Continuous hardware gets one-percent steps; stepped hardware keeps its intervals.
    return qMax(1, (warmthMaximum() - warmthMinimum()) / (intervals > 0 ? intervals : 100));
}
bool NativeLight::automaticWarmth() const {
    if (!hasWarmth())
        return false;
    PowerSettings settings;
    return api().automatic(settings.get());
}
QTime NativeLight::bedtime() const {
    if (!hasWarmth())
        return {};
    PowerSettings settings;
    return api().bedtime(settings.get());
}
int NativeLight::setWarmth(int value) {
    if (!hasWarmth())
        return -1;
    // Nickel disables automatic warmth when the user moves this slider.
    if (automaticWarmth() && !setAutomaticWarmth(false))
        return -1;
    const int temperature = warmthMinimum() + warmthMaximum() - qBound(warmthMinimum(), value, warmthMaximum());
    api().setTemperature(frontLight, temperature, true, 0);
    return warmth();
}
bool NativeLight::saveWarmth() {
    if (!hasWarmth())
        return false;
    const int value = api().temperature(frontLight);
    {
        PowerSettings settings;
        api().saveTemperature(settings.get(), value);
    }
    const bool saved = stored("ColorSetting", value);
    diagnostic(saved ? "warmth-saved" : "warmth-save-failed", kobalt::qt::object({{"value", value}}));
    return saved;
}
bool NativeLight::setAutomaticWarmth(bool enabled) {
    if (!hasWarmth())
        return false;
    {
        PowerSettings settings;
        api().setAutomatic(settings.get(), enabled);
    }
    const bool saved = automaticWarmth() == enabled && stored("AutoColorEnabled", enabled);
    diagnostic(saved ? "warmth-schedule-saved" : "warmth-schedule-save-failed",
               kobalt::qt::object({{"automatic", enabled}}));
    return saved;
}
int NativeLight::brightness() const {
    return frontLight ? api().brightness(frontLight) : -1;
}
int NativeLight::setGestureLevel(int percent) {
    if (!gesturing() || !frontLight)
        return -1;
    if (automaticBrightness() && !setAutomaticBrightness(false)) {
        return -1;
    }
    // An automatic brightness animation writes the shared channels after a
    // direct setBrightness call. Stop it before applying manual input, as
    // Nickel's brightness gesture does through FRONT_LIGHT_CUSTOM_EVENT.
    api().cancelBrightnessChanging(frontLight);
    const int next = qBound(0, percent, 100);
    if (next != brightness())
        api().setBrightness(frontLight, next, 0);
    const int actual = brightness();
    if (actual != next && !reportedGestureFailure) {
        diagnostic("brightness-gesture-device-failed", kobalt::qt::object({{"requested", next}, {"actual", actual}}), kobalt::DiagnosticSeverity::Error);
        reportedGestureFailure = true;
    }
    return actual;
}
void NativeLight::finishGesture(bool save) {
    if (!gesturing() || !frontLight)
        return;
    const int original = std::exchange(gestureStart, -1);
    if (!save) {
        api().setBrightness(frontLight, original, 0);
        if (gestureAutomatic && !automaticBrightness()) {
            setAutomaticBrightness(true);
        }
        return;
    }
    if (brightness() == original)
        return;
    // The native gesture owns its FrontLight and timer. finishGesture saves
    // the current shared light level, including zero, without using the
    // optional delegate or constructing a view. Destroy it synchronously so
    // its two-second notification timer cannot run after this call.
    // Nickel's analytics starting level retains the constructor's -1 sentinel.
    void *storage = ::operator new(Api::gestureBytes);
    api().constructGesture(storage, nullptr, nullptr);
    api().finishGesture(storage);
    delete static_cast<QObject *>(storage);
    diagnostic("brightness-gesture-save-requested", kobalt::qt::object({{"percent", brightness()}}));
}
}

#ifndef KOBALT_READER_HARDWARE
extern "C" __attribute__((visibility("default"))) int kobalt_light_test_ioctl(int fd, unsigned long request,
                                                                              void *argument) {
    using namespace kobalt;
    if (!lightTestIoctl) {
        errno = ENOSYS;
        return -1;
    }
    const int result = lightTestIoctl(fd, request, argument);
    const int error = errno;
    if (request == 0xf1) {
        char path[256]{};
        const auto link = QByteArray("/proc/self/fd/") + QByteArray::number(fd);
        const auto length = readlink(link.constData(), path, sizeof(path) - 1);
        const QByteArray target(path, length > 0 ? length : 0);
        if (target == "/dev/ntx_io" || target == "/firmware/rootfs/dev/ntx_io") {
            observedLightDevice = true;
            lightDeviceError = result < 0 ? error : 0;
            // Opt-in test support for the simulator's regular /dev/ntx_io file.
            // Nickel still owns brightness state and persists it natively.
            if (simulateLightDevice && result < 0 && error == ENOTTY)
                return 0;
        }
    }
    errno = error;
    return result;
}
#endif
