#include "services.h"
#include "ui/controls/keyboard.h"
#include "ui/style/fonts.h"
#include <QGuiApplication>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QTime>
#ifdef Q_OS_MACOS
#  include <sys/sysctl.h>
#else
#  include <QSysInfo>
#endif

namespace kobalt::desktop {
namespace {
class Light final : public LightControls
{
public:
    bool initialize() override { return false; }

    bool hasWarmth() const override { return false; }

    int warmth() const override { return 0; }

    int warmthMinimum() const override { return 0; }

    int warmthMaximum() const override { return 0; }

    int warmthStep() const override { return 1; }

    bool automaticWarmth() const override { return false; }

    int setWarmth(int) override { return 0; }

    bool saveWarmth() override { return false; }

    bool setAutomaticWarmth(bool) override { return false; }

    bool beginGesture() override { return false; }

    int setGestureLevel(int) override { return 0; }

    void finishGesture(bool) override { }

    int brightness() const override { return 0; }

    bool gesturing() const override { return false; }
};

class Dictionary final : public DictionaryService
{
public:
    QVector<DictionaryChoice> choices() const override { return {}; }

    bool select(const QString &, const QString &, QString *error) override
    {
        if (error)
            *error = QStringLiteral("No dictionary installed.");
        return false;
    }

    DictionaryResult lookup(const QString &, const QString &word) override
    {
        DictionaryResult result;
        result.word = word;
        result.error = QStringLiteral("No dictionary installed.");
        return result;
    }
};

class Presentation final : public WindowPresentation
{
public:
    void pauseBackground(bool) override { }

    void resumeBackground() override { }

    void resumeAndRepaintBackground() override { }

    void acceptHandoff(QDialog &, const std::function<void()> &accept) override { accept(); }
};
} // namespace

ReaderHostServices readerHostServices()
{
    ReaderHostServices result;
#ifdef Q_OS_MACOS
    result.deviceName = QStringLiteral("Mac");
    char model[256] = {};
    size_t modelSize = sizeof(model);
    if (::sysctlbyname("hw.model", model, &modelSize, nullptr, 0) == 0 && modelSize > 1)
        result.deviceName = QString::fromLatin1(model, int(qMin(modelSize - 1, sizeof(model) - 1)));
#else
    result.deviceName = QSysInfo::prettyProductName();
#endif
    result.colorDisplay = [] { return true; };
    result.browserAvailable = [] { return true; };
    result.openBrowser = [](const QUrl &url, QWidget *) {
        return url.isValid() && !url.host().isEmpty()
                && (url.scheme() == "http" || url.scheme() == "https")
                && QDesktopServices::openUrl(url);
    };
    result.clock = [] { return QTime::currentTime().toString("HH:mm"); };
    result.battery = [] { return BatteryStatus{}; };
    result.pageButtons = [] { return false; };
    result.reverseLandscapeButtons = [] { return false; };
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
    result.fontFamilies = [] { return QStringList{}; };
    result.uiFont = [] { return ui::interfaceFont(); };
    result.suspended = [] { return QGuiApplication::applicationState() != Qt::ApplicationActive; };
    result.lightAvailable = [] { return false; };
    result.desktopUi = true;
    result.animationSupport = [] { return PageAnimationSupport::Unavailable; };
    result.cancelAnimation = [] { };
    result.light = [](QWidget *) { return std::make_unique<Light>(); };
    result.dictionary = [] { return std::make_unique<Dictionary>(); };
    result.keyboard = ui::makeWidgetKeyboard;
    result.presentation = [](QWidget *) { return std::make_unique<Presentation>(); };
    return result;
}
} // namespace kobalt::desktop
