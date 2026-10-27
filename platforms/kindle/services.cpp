#include "services.h"
#include "device.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/controls/keyboard.h"
#include "support/strings.h"
#include <QKeyEvent>
#include <QGuiApplication>
#include <QApplication>
#include <QWindow>
#include <QTime>
#include <QWidget>

namespace kobalt::kindle {
namespace {
class Dictionary final : public DictionaryService {
public:
    QVector<DictionaryChoice> choices() const override {
        return {};
    }
    bool select(const QString &, const QString &, QString *error) override {
        if (error)
            *error = QStringLiteral("No dictionary installed.");
        return false;
    }
    DictionaryResult lookup(const QString &, const QString &word) override {
        DictionaryResult result;
        result.word = word;
        result.error = QStringLiteral("No dictionary installed.");
        return result;
    }
};
class Presentation final : public WindowPresentation {
public:
    explicit Presentation(std::shared_ptr<Device> device) : device(std::move(device)) {}
    void pauseBackground(bool) override {}
    void resumeBackground() override {}
    void resumeAndRepaintBackground() override {}
    bool supportsSleepScreen() const override { return true; }
    bool presentSleepScreen(QImage frame, Qt::ScreenOrientation) override {
        if (frame.isNull())
            return false;
        device->setSleepFrame(std::move(frame));
        return true;
    }
    bool updateSleepScreen(QImage frame, const QRect &) override {
        return presentSleepScreen(std::move(frame), Qt::PrimaryOrientation);
    }
    void clearSleepScreen() override { device->setSleepFrame({}); }
    void acceptHandoff(QDialog &, const std::function<void()> &accept) override {
        accept();
    }
private:
    std::shared_ptr<Device> device;
};
}
ReaderHostServices readerServices(std::shared_ptr<Device> device) {
    ReaderHostServices result;
    result.deviceName = QStringLiteral("Kindle");
    result.backButtonLabel = strings::get("library.back");
    result.colorDisplay = [] {
        return false;
    };
    result.browserAvailable = [] {
        return false;
    };
    result.openBrowser = [](const QUrl &url, QWidget *) {
        Q_UNUSED(url);
        return false;
    };
    result.clock = [] {
        return QTime::currentTime().toString("HH:mm");
    };
    result.battery = [device] {
        return device->state().battery;
    };
    result.observeBattery = [device](QObject *context, std::function<void()> changed) {
        QObject::connect(device.get(), &Device::batteryChanged, context, std::move(changed));
    };
    result.observeWifi = [device](QObject *context, std::function<void(WifiStatus)> changed) {
        changed(device->state().wifi);
        QObject::connect(device.get(), &Device::wifiChanged, context, [device, changed] { changed(device->state().wifi); });
    };
    result.observeBluetooth = [device](QObject *context, std::function<void(bool)> changed) {
        changed(device->state().bluetoothConnected);
        QObject::connect(device.get(), &Device::bluetoothChanged, context, [device, changed] { changed(device->state().bluetoothConnected); });
    };
    result.pageButtons = [] {
        return false;
    };
    result.reverseLandscapeButtons = [] {
        return false;
    };
    result.pageButtonDirection = [](QKeyEvent *event) {
        switch (event->key()) {
        case Qt::Key_VolumeDown:
        case Qt::Key_PageDown:
            return 1;
        case Qt::Key_VolumeUp:
        case Qt::Key_PageUp:
            return -1;
        default:
            return 0;
        }
    };
    result.fontFamilies = [] {
        return QStringList{};
    };
    result.uiFont = [] {
        return ui::interfaceFont();
    };
    result.suspended = [device] {
        return device->state().sleeping;
    };
    result.lightAvailable = [device] {
        return device->state().hasLight();
    };
    result.systemRotation = true;
    result.downloadableDictionaries = true;
    result.animationsAvailable = [] {
        return qApp && qApp->property("kobalt.swipeAvailable").toBool();
    };
    result.cancelAnimation = [] {
        for (auto *surface : QApplication::topLevelWidgets()) {
            if (surface->objectName() == QStringLiteral("kindleWindow") && surface->windowHandle()) {
                auto *window = surface->windowHandle();
                window->setProperty("kobalt.pageTurnDirection", 0);
                window->setProperty("kobalt.cancelAnimation", !window->property("kobalt.cancelAnimation").toBool());
            }
        }
    };
    result.light = [device](QWidget *) {
        return makeLight(device);
    };
    result.dictionary = [] {
        return std::make_unique<Dictionary>();
    };
    result.keyboard = [](QWidget *parent, QLineEdit *editor) {
        return ui::makeWidgetKeyboard(parent, editor);
    };
    result.presentation = [device](QWidget *) {
        return std::make_unique<Presentation>(device);
    };
    return result;
}
}
