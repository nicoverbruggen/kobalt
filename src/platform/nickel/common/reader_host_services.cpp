#include "reader_host_services.h"
#include "native_dictionary.h"
#include "native_keyboard.h"
#include "native_lifecycle.h"
#include "native_light.h"
#include "native_progress.h"
#include "native_ui.h"
#include "platform/nickel/display/page_animation.h"
#include "window_presentation.h"
#include <QPointer>

namespace kobalt::nickel {
namespace {
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
class Keyboard final : public KeyboardController {
public:
    Keyboard(QWidget *parent, QLineEdit *editor) : adapter(new NativeKeyboard(parent, editor)) {
        adapter->submitted = [this] {
            const auto callback = submitted;
            if (callback)
                callback();
        };
    }
    ~Keyboard() override {
        if (adapter)
            adapter->submitted = {};
        delete adapter.data();
    }
    bool show() override {
        return adapter && adapter->show();
    }
    void hide() override {
        if (adapter)
            adapter->hide();
    }
    void place() override {
        if (adapter)
            adapter->place();
    }

private:
    QPointer<NativeKeyboard> adapter;
};

}
ReaderHostServices readerHostServices() {
    ReaderHostServices result;
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
    result.remoteKeyActivity = reportNativeBluetoothActivity;
    result.pageButtons = nativeReaderHasPageButtons;
    result.reverseLandscapeButtons = nativeReaderReverseLandscapeButtons;
    result.pageButtonDirection = nativePageButtonDirection;
    result.fontFamilies = nativeReaderFontFamilies;
    result.uiFont = nativeReaderUiFont;
    result.suspended = NativeLifecycle::suspended;
    result.lightAvailable = NativeLight::available;
    result.animationsAvailable = [] {
#ifdef KOBALT_PAGE_ANIMATIONS
        return pageAnimationsAvailable();
#else
        return false;
#endif
    };
    result.cancelAnimation = cancelPageAnimation;
    result.light = [](QWidget *anchor) {
        return std::make_unique<Light>(anchor);
    };
    result.dictionary = [] {
        return std::make_unique<Dictionary>();
    };
    result.keyboard = [](QWidget *parent, QLineEdit *editor) {
        return std::make_unique<Keyboard>(parent, editor);
    };
    result.presentation = [](QWidget *reader) {
        return makeWindowPresentation(reader, nativeReaderSleepScreenAvailable());
    };
#ifndef KOBALT_READER_HARDWARE
    result.debugProgressAction = NativeProgress::testAction;
#endif
    return result;
}
}
