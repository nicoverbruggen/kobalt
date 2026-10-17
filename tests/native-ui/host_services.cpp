#include "host_services.h"
#include "host_presentation.h"
#include "host_fixture.h"
#include "interfaces/reader/ui/style/fonts.h"
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QTime>
#include <QWidget>
#include <utility>

namespace kobalt::test {
namespace {
class Light final : public LightControls {
public:
    Light(QWidget *anchor, bool available, bool ambientLightSensor)
        : anchor(anchor), available(available), ambientLightSensor(ambientLightSensor) {}
    bool initialize() override {
        return anchor && available;
    }
    bool hasAutomaticBrightness() const override {
        return anchor && available && ambientLightSensor;
    }
    bool automaticBrightness() const override {
        return hasAutomaticBrightness() && automaticBrightness_;
    }
    bool setAutomaticBrightness(bool enabled) override {
        if (!hasAutomaticBrightness()) {
            return false;
        }
        automaticBrightness_ = enabled;
        return true;
    }
    bool hasWarmth() const override {
        return anchor && available;
    }
    int warmth() const override {
        return hasWarmth() ? warmth_ : 0;
    }
    int warmthMinimum() const override {
        return 0;
    }
    int warmthMaximum() const override {
        return hasWarmth() ? 100 : 0;
    }
    int warmthStep() const override {
        return 1;
    }
    bool automaticWarmth() const override {
        return hasWarmth() && automatic_;
    }
    int setWarmth(int value) override {
        if (!hasWarmth())
            return 0;
        warmth_ = qBound(0, value, 100);
        automatic_ = false;
        return warmth_;
    }
    bool saveWarmth() override {
        return hasWarmth();
    }
    bool setAutomaticWarmth(bool enabled) override {
        if (!hasWarmth())
            return false;
        automatic_ = enabled;
        return true;
    }
    bool beginGesture() override {
        if (!initialize())
            return false;
        if (gestureStart < 0) {
            gestureStart = brightness_;
            gestureAutomatic = automaticBrightness_;
        }
        return true;
    }
    int setGestureLevel(int value) override {
        if (initialize() && gestureStart >= 0) {
            automaticBrightness_ = false;
            brightness_ = qBound(0, value, 100);
        }
        return brightness();
    }
    void finishGesture(bool save) override {
        const int original = std::exchange(gestureStart, -1);
        if (!save && original >= 0) {
            brightness_ = original;
            automaticBrightness_ = gestureAutomatic;
        }
    }
    int brightness() const override {
        return hasWarmth() ? brightness_ : 0;
    }
    bool gesturing() const override {
        return anchor && gestureStart >= 0;
    }

private:
    QPointer<QWidget> anchor;
    bool available, ambientLightSensor, automatic_ = false, automaticBrightness_ = false, gestureAutomatic = false;
    int brightness_ = 50, warmth_ = 0, gestureStart = -1;
};
class Dictionary final : public DictionaryService {
public:
    QVector<DictionaryChoice> choices() const override {
        return {};
    }
    bool select(const QString &, const QString &, QString *error) override {
        if (error)
            *error = "No dictionary installed.";
        return false;
    }
    DictionaryResult lookup(const QString &, const QString &word) override {
        DictionaryResult result;
        result.word = word;
        result.error = "No dictionary installed.";
        return result;
    }
};
class Keyboard final : public KeyboardController {
public:
    Keyboard(QWidget *parent, QLineEdit *editor) : parent(parent), editor(editor) {}
    bool show() override {
        if (!parent || !editor)
            return false;
        editor->setFocus();
        return true;
    }
    void hide() override {}
    void place() override {}

private:
    // SearchPanel already handles returnPressed. Connecting it here as well
    // would submit twice. The desktop controller owns no keyboard widget.
    QPointer<QWidget> parent;
    QPointer<QLineEdit> editor;
};
}
ReaderHostServices hostReaderServices(bool ambientLightSensor) {
    ReaderHostServices result;
    result.browserAvailable = [] {
        return false;
    };
    result.openBrowser = [](const QUrl &, QWidget *) {
        return false;
    };
    result.clock = [] {
        return QTime(9, 0).toString("H:mm");
    };
    result.battery = [] {
        return BatteryStatus{100, false};
    };
    result.pageButtons = [] {
        return true;
    };
    result.reverseLandscapeButtons = [] {
        return false;
    };
    result.pageButtonDirection = [](QKeyEvent *event) {
        return event->key() == Qt::Key_VolumeDown ? 1 : event->key() == Qt::Key_VolumeUp ? -1 : 0;
    };
    result.fontFamilies = [] {
        return QStringList{};
    };
    result.uiFont = [] {
        return ui::interfaceFont();
    };
    result.suspended = [] {
        return false;
    };
    result.lightAvailable = fixtureDeviceControls;
    result.animationsAvailable = fixtureDeviceControls;
    result.cancelAnimation = [] {};
    result.light = [ambientLightSensor](QWidget *anchor) {
        return std::make_unique<Light>(anchor, fixtureDeviceControls(), ambientLightSensor);
    };
    result.dictionary = [] {
        return std::make_unique<Dictionary>();
    };
    result.keyboard = [](QWidget *parent, QLineEdit *editor) {
        return std::make_unique<Keyboard>(parent, editor);
    };
    result.presentation = test::makeHostPresentation;
#ifndef KOBALT_READER_HARDWARE
    result.debugProgressAction = [](const QJsonObject &) {
        return QJsonObject{{"ok", false}};
    };
#endif
    return result;
}
}
