#include "reader_host_services.h"
#include "native_dictionary.h"
#include "interfaces/reader/ui/controls/keyboard.h"
#include "native_sync_network.h"
#include "native_lifecycle.h"
#include "native_light.h"
#include "native_progress.h"
#include "native_ui.h"
#include "platforms/kobo/display/page_animation.h"
#include "window_presentation.h"
#include <QPointer>
#include <dlfcn.h>

namespace kobalt::nickel {
namespace {
QString syncDeviceModel() {
    const auto current = reinterpret_cast<const void *(*)()>(dlsym(RTLD_DEFAULT, "_ZN6Device16getCurrentDeviceEv"));
    const auto *device = current ? current() : nullptr;
    if (!device) return QStringLiteral("Kobo");
    // Prefer the specific variants before their shared hardware families.
    const struct { const char *predicate; const char *name; } models[] = {
        {"_ZNK6Device11isSpaKoboBWEv", "Kobo Clara BW"},
        {"_ZNK6Device13isSpaTolinoBWEv", "tolino shine"},
        {"_ZNK6Device17isSpaTolinoColourEv", "tolino shine color"},
        {"_ZNK6Device13isMonzaTolinoEv", "tolino vision color"},
        {"_ZNK6Device9isSpaKoboEv", "Kobo Clara Colour"},
        {"_ZNK6Device11isMonzaKoboEv", "Kobo Libra Colour"},
        {"_ZNK6Device8isCondorEv", "Kobo Elipsa 2E"},
        {"_ZNK6Device11isGoldfinchEv", "Kobo Clara 2E"},
        {"_ZNK6Device4isIoEv", "Kobo Libra 2"},
        {"_ZNK6Device8isCadmusEv", "Kobo Sage"},
        {"_ZNK6Device6isNovaEv", "Kobo Clara HD"},
        {"_ZNK6Device6isLunaEv", "Kobo Nia"},
        {"_ZNK6Device6isSnowEv", "Kobo Aura H2O Edition 2"},
        {"_ZNK6Device6isStarEv", "Kobo Aura Edition 2"},
        {"_ZNK6Device6isPikaEv", "Kobo Touch 2.0"},
    };
    for (const auto &model : models) {
        const auto matches = reinterpret_cast<bool (*)(const void *)>(dlsym(RTLD_DEFAULT, model.predicate));
        if (matches && matches(device)) return QString::fromLatin1(model.name);
    }
    return QStringLiteral("Kobo");
}
class Light final : public LightControls {
public:
    explicit Light(QWidget *anchor) : adapter(new NativeLight(anchor)) {}
    ~Light() override {
        delete adapter.data();
    }
    bool initialize() override {
        return adapter && adapter->initialize();
    }
    bool hasAutomaticBrightness() const override {
        return adapter && adapter->hasAutomaticBrightness();
    }
    bool automaticBrightness() const override {
        return adapter && adapter->automaticBrightness();
    }
    bool setAutomaticBrightness(bool enabled) override {
        return adapter && adapter->setAutomaticBrightness(enabled);
    }
    bool hasWarmth() const override {
        return adapter && adapter->hasWarmth();
    }
    int warmth() const override {
        return adapter ? adapter->warmth() : 0;
    }
    int warmthMinimum() const override {
        return adapter ? adapter->warmthMinimum() : 0;
    }
    int warmthMaximum() const override {
        return adapter ? adapter->warmthMaximum() : 0;
    }
    int warmthStep() const override {
        return adapter ? adapter->warmthStep() : 1;
    }
    bool automaticWarmth() const override {
        return adapter && adapter->automaticWarmth();
    }
    int setWarmth(int value) override {
        return adapter ? adapter->setWarmth(value) : 0;
    }
    bool saveWarmth() override {
        return adapter && adapter->saveWarmth();
    }
    bool setAutomaticWarmth(bool enabled) override {
        return adapter && adapter->setAutomaticWarmth(enabled);
    }
    bool beginGesture() override {
        return adapter && adapter->beginGesture();
    }
    int setGestureLevel(int percent) override {
        return adapter ? adapter->setGestureLevel(percent) : 0;
    }
    void finishGesture(bool save) override {
        if (adapter)
            adapter->finishGesture(save);
    }
    int brightness() const override {
        return adapter ? adapter->brightness() : 0;
    }
    bool gesturing() const override {
        return adapter && adapter->gesturing();
    }

private:
    // NativeLight is also parented to its anchor. The guarded pointer permits
    // either destruction order without transferring a native handle to the UI.
    QPointer<NativeLight> adapter;
};
class Dictionary final : public DictionaryService {
public:
    QVector<DictionaryChoice> choices() const override {
        return adapter.choices();
    }
    bool select(const QString &book, const QString &id, QString *error) override {
        return adapter.select(book, id, error);
    }
    DictionaryResult lookup(const QString &book, const QString &word) override {
        return adapter.lookup(book, word);
    }

private:
    NativeDictionary adapter;
};


}
ReaderHostServices readerHostServices() {
    ReaderHostServices result;
    result.annotationExportDirectory = [] { return QStringLiteral("/mnt/onboard/Kobalt/Annotations"); };

    result.deviceName = syncDeviceModel();
    result.colorDisplay = [] {
        return pageAnimationPanel() == AnimationPanel::Colour;
    };
    result.downloadableDictionaries = false;
    result.browserAvailable = nativeReaderBrowserAvailable;
    result.openBrowser = nativeReaderOpenBrowser;
    result.clock = nativeReaderClock;
    result.battery = [] {
        const auto status = nativeReaderBattery();
        return BatteryStatus{status.percent, status.charging, status.fullyCharged};
    };
    result.observeBattery = observeNativeBattery;
    result.observeBluetooth = observeNativeBluetooth;
    result.observeWifi = observeNativeWifi;
    result.remoteKeyActivity = reportNativeBluetoothActivity;
    result.pageButtons = nativeReaderHasPageButtons;
    result.reverseLandscapeButtons = nativeReaderReverseLandscapeButtons;
    result.pageButtonDirection = nativePageButtonDirection;
    result.fontFamilies = nativeReaderFontFamilies;
    result.uiFont = nativeReaderUiFont;
    result.suspended = NativeLifecycle::suspended;
    result.lightAvailable = NativeLight::available;
    result.animationSupport = pageAnimationSupport;
    result.cancelAnimation = cancelPageAnimation;
    result.light = [](QWidget *anchor) {
        return std::make_unique<Light>(anchor);
    };
    result.dictionary = [] {
        return std::make_unique<Dictionary>();
    };
    result.keyboard = ui::makeWidgetKeyboard;
    result.syncNetwork = [] { return std::make_unique<NativeSyncNetwork>(); };
    result.kosyncSleepWake = result.kosyncWifiControl = true;
    result.presentation = [](QWidget *reader) {
        return makeWindowPresentation(reader, nativeReaderSleepScreenAvailable());
    };
#ifndef KOBALT_READER_HARDWARE
    result.debugProgressAction = NativeProgress::testAction;
#endif
    return result;
}
}
