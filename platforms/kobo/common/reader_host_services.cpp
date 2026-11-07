#include <QKeyEvent>
#include "reader_host_services.h"
#include "native_dictionary.h"
#include "native_keyboard.h"
#include "native_sync_network.h"
#include "native_lifecycle.h"
#include "native_light.h"
#include "native_progress.h"
#include "native_ui.h"
#include "native_wifi_power.h"
#include "platforms/kobo/display/page_animation.h"
#include "window_presentation.h"
#include "reading/reading_timer.h"
#include <QPointer>
#include <dlfcn.h>
#include <utility>

namespace kobalt::nickel {
namespace {
// The Wi-Fi power saver ships in beta 7. Until then Kobo offers neither the
// setting nor its Wi-Fi control, and the saved choice is kept as it is.
constexpr bool wifiPowerSaverReleased = false;

NativeWifiPowerBindings wifiPowerBindings()
{
    const auto wireless = reinterpret_cast<QObject *(*)()>(
            dlsym(RTLD_DEFAULT, "_ZN15WirelessManager14sharedInstanceEv"));
    const auto up = reinterpret_cast<bool (*)(QObject *)>(
            dlsym(RTLD_DEFAULT, "_ZNK15WirelessManager2upEv"));
    const auto workflow = reinterpret_cast<QObject *(*)()>(
            dlsym(RTLD_DEFAULT, "_ZN23WirelessWorkflowManager14sharedInstanceEv"));
    // Nickel's own off path, used by its idle timeout and the Wi-Fi toggle.
    // It honors the developer setting that keeps Wi-Fi on.
    const auto turnOff = reinterpret_cast<void (*)(QObject *)>(
            dlsym(RTLD_DEFAULT, "_ZN23WirelessWorkflowManager11turnWifiOffEv"));
    const auto syncManager = reinterpret_cast<void *(*)()>(
            dlsym(RTLD_DEFAULT, "_ZN13N3SyncManager14sharedInstanceEv"));
    const auto syncing = reinterpret_cast<bool (*)(const void *)>(
            dlsym(RTLD_DEFAULT, "_ZNK15SyncManagerBase9isSyncingEv"));
    if (!wireless || !up || !workflow || !turnOff || !syncManager || !syncing)
        return {};
    NativeWifiPowerBindings result;
    result.wifiOn = [wireless, up] {
        auto *manager = wireless();
        return manager && up(manager);
    };
    result.nickelSyncing = [syncManager, syncing] {
        const auto *manager = syncManager();
        return manager && syncing(manager);
    };
    result.bluetooth = queryNativeBluetoothPower;
    result.turnOff = [workflow, turnOff] {
        if (auto *manager = workflow())
            turnOff(manager);
    };
    return result;
}

QString syncDeviceModel()
{
    const auto current = reinterpret_cast<const void *(*)()>(
            dlsym(RTLD_DEFAULT, "_ZN6Device16getCurrentDeviceEv"));
    const auto *device = current ? current() : nullptr;
    if (!device)
        return QStringLiteral("Kobo");

    // Prefer the specific variants before their shared hardware families.
    const struct
    {
        const char *predicate;
        const char *name;
    } models[] = {
        { "_ZNK6Device11isSpaKoboBWEv", "Kobo Clara BW" },
        { "_ZNK6Device13isSpaTolinoBWEv", "tolino shine" },
        { "_ZNK6Device17isSpaTolinoColourEv", "tolino shine color" },
        { "_ZNK6Device13isMonzaTolinoEv", "tolino vision color" },
        { "_ZNK6Device9isSpaKoboEv", "Kobo Clara Colour" },
        { "_ZNK6Device11isMonzaKoboEv", "Kobo Libra Colour" },
        { "_ZNK6Device8isCondorEv", "Kobo Elipsa 2E" },
        { "_ZNK6Device11isGoldfinchEv", "Kobo Clara 2E" },
        { "_ZNK6Device4isIoEv", "Kobo Libra 2" },
        { "_ZNK6Device8isCadmusEv", "Kobo Sage" },
        { "_ZNK6Device6isNovaEv", "Kobo Clara HD" },
        { "_ZNK6Device6isLunaEv", "Kobo Nia" },
        { "_ZNK6Device6isSnowEv", "Kobo Aura H2O Edition 2" },
        { "_ZNK6Device6isStarEv", "Kobo Aura Edition 2" },
        { "_ZNK6Device6isPikaEv", "Kobo Touch 2.0" },
    };

    for (const auto &model : models) {
        const auto matches =
                reinterpret_cast<bool (*)(const void *)>(dlsym(RTLD_DEFAULT, model.predicate));
        if (matches && matches(device))
            return QString::fromLatin1(model.name);
    }
    return QStringLiteral("Kobo");
}

class Light final : public LightControls
{
public:
    explicit Light(QWidget *anchor) : adapter(new NativeLight(anchor)) { }

    ~Light() override { delete adapter.data(); }

    bool initialize() override { return adapter && adapter->initialize(); }

    bool hasAutomaticBrightness() const override
    {
        return adapter && adapter->hasAutomaticBrightness();
    }

    bool automaticBrightness() const override { return adapter && adapter->automaticBrightness(); }

    bool setAutomaticBrightness(bool enabled) override
    {
        return adapter && adapter->setAutomaticBrightness(enabled);
    }

    bool hasWarmth() const override { return adapter && adapter->hasWarmth(); }

    int warmth() const override { return adapter ? adapter->warmth() : 0; }

    int warmthMinimum() const override { return adapter ? adapter->warmthMinimum() : 0; }

    int warmthMaximum() const override { return adapter ? adapter->warmthMaximum() : 0; }

    int warmthStep() const override { return adapter ? adapter->warmthStep() : 1; }

    bool automaticWarmth() const override { return adapter && adapter->automaticWarmth(); }

    int setWarmth(int value) override { return adapter ? adapter->setWarmth(value) : 0; }

    bool saveWarmth() override { return adapter && adapter->saveWarmth(); }

    bool setAutomaticWarmth(bool enabled) override
    {
        return adapter && adapter->setAutomaticWarmth(enabled);
    }

    bool beginGesture() override { return adapter && adapter->beginGesture(); }

    int setGestureLevel(int percent) override
    {
        return adapter ? adapter->setGestureLevel(percent) : 0;
    }

    void finishGesture(bool save) override
    {
        if (adapter)
            adapter->finishGesture(save);
    }

    int brightness() const override { return adapter ? adapter->brightness() : 0; }

    bool gesturing() const override { return adapter && adapter->gesturing(); }

private:
    // NativeLight is also parented to its anchor. The guarded pointer permits
    // either destruction order without transferring a native handle to the UI.
    QPointer<NativeLight> adapter;
};

class Dictionary final : public DictionaryService
{
public:
    QVector<DictionaryChoice> choices() const override { return adapter.choices(); }

    bool select(const QString &book, const QString &id, QString *error) override
    {
        return adapter.select(book, id, error);
    }

    DictionaryResult lookup(const QString &book, const QString &word) override
    {
        return adapter.lookup(book, word);
    }

private:
    NativeDictionary adapter;
};

class Keyboard final : public KeyboardController
{
public:
    Keyboard(QWidget *parent, QLineEdit *editor) : adapter(new NativeKeyboard(parent, editor))
    {
        adapter->submitted = [this] {
            const auto callback = submitted;
            if (callback)
                callback();
        };
    }

    ~Keyboard() override
    {
        if (adapter)
            adapter->submitted = {};
        delete adapter.data();
    }

    bool show() override { return adapter && adapter->show(); }

    void hide() override
    {
        if (adapter)
            adapter->hide();
    }

    void place() override
    {
        if (adapter)
            adapter->place();
    }

    void setSubmitText(const QString &text) override
    {
        if (adapter)
            adapter->setSubmitText(text);
    }

private:
    QPointer<NativeKeyboard> adapter;
};

} // namespace

ReaderHostServices readerHostServices()
{
    ReaderHostServices result;
    result.readerStandbySupported = true;
    result.annotationExportDirectory = [] {
        return QStringLiteral("/mnt/onboard/Kobalt/Annotations");
    };

    result.sleepOverlayDirectories = QStringList{
        QStringLiteral(KOBALT_CONFIG_DIR "/screensaver"),
        QStringLiteral("/mnt/onboard/.adds/screensaver"),
    };
    result.deviceName = syncDeviceModel();
    result.colorDisplay = [] { return pageAnimationPanel() == AnimationPanel::Color; };
    result.downloadableDictionaries = false;
    result.browserAvailable = nativeReaderBrowserAvailable;
    result.openBrowser = nativeReaderOpenBrowser;
    result.clock = nativeReaderClock;
    // Nickel suspends the device between interactions. Count KOSync's
    // intervals in real time, including that standby, as the reading
    // duration does.
    result.monotonicMilliseconds = ReadingTimer::platformClock();
    result.battery = [] {
        const auto status = nativeReaderBattery();
        return BatteryStatus{ status.percent, status.charging, status.fullyCharged };
    };
    result.observeBattery = observeNativeBattery;
    result.observeBluetooth = observeNativeBluetooth;
    result.observeWifi = observeNativeWifi;
    result.remoteKeyActivity = reportNativeBluetoothActivity;
    result.pageButtons = nativeReaderHasPageButtons;
    result.buttonControls = [] {
        if (!nativeReaderHasPageButtons())
            return pageButtons::Buttons{};
        return pageButtons::paging();
    };
    result.reverseLandscapeButtons = nativeReaderReverseLandscapeButtons;
    result.pageButtonDirection = nativePageButtonDirection;
    result.fontFamilies = nativeReaderFontFamilies;
    result.uiFont = nativeReaderUiFont;
    result.suspended = NativeLifecycle::suspended;
    result.lightAvailable = NativeLight::available;
    result.animationSupport = pageAnimationSupport;
    result.cancelAnimation = cancelPageAnimation;
    result.light = [](QWidget *anchor) { return std::make_unique<Light>(anchor); };
    result.dictionary = [] { return std::make_unique<Dictionary>(); };
    result.keyboard = [](QWidget *parent, QLineEdit *editor) {
        return std::make_unique<Keyboard>(parent, editor);
    };
    result.syncNetwork = [] { return std::make_unique<NativeSyncNetwork>(); };
    result.kosyncSleepWake = result.kosyncWifiControl = true;
    // The hook is optional. Without it Nickel's background requests pass.
    if (wifiPowerSaverReleased && nativeConnectWirelessSilently)
        result.holdWifiOff = holdNativeWifiOff;
    if (const auto bindings = wifiPowerBindings(); wifiPowerSaverReleased && bindings.valid())
        result.turnOffWifi = [bindings](QObject *context, const char *reason,
                                        std::function<void(bool)> done) {
            saveNativeWifiPower(context, bindings, reason, std::move(done));
        };
    result.presentation = [](QWidget *reader) {
        return makeWindowPresentation(reader, nativeReaderSleepScreenAvailable());
    };
#ifndef KOBALT_READER_HARDWARE
    result.debugProgressAction = NativeProgress::testAction;
#endif
    return result;
}
} // namespace kobalt::nickel
