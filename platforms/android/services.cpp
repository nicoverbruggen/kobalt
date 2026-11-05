#include "services.h"
#include "runtime.h"
#include "light.h"
#include "ui/style/fonts.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QTime>
#include <QJniObject>

namespace kobalt::android {
namespace {
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

class Keyboard final : public KeyboardController
{
public:
    explicit Keyboard(QLineEdit *editor) : editor(editor) { }

    bool show() override
    {
        if (!editor)
            return false;
        editor->setFocus();
        showKeyboard();
        return true;
    }

    void hide() override { hideKeyboard(); }

    void place() override { }

private:
    QPointer<QLineEdit> editor;
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
    const auto model =
            QJniObject::getStaticObjectField("android/os/Build", "MODEL", "Ljava/lang/String;")
                    .toString()
                    .trimmed();
    const auto maker = QJniObject::getStaticObjectField("android/os/Build", "MANUFACTURER",
                                                        "Ljava/lang/String;")
                               .toString()
                               .trimmed();
    result.deviceName =
            model.startsWith(maker, Qt::CaseInsensitive) ? model : (maker + " " + model).trimmed();
    if (result.deviceName.isEmpty())
        result.deviceName = QStringLiteral("Android");
    result.colorDisplay = [] { return true; };
    result.systemRotation = true;
    result.browserAvailable = [] { return true; };
    result.openBrowser = [](const QUrl &url, QWidget *) {
        return url.isValid() && !url.host().isEmpty()
                && (url.scheme() == "http" || url.scheme() == "https")
                && QDesktopServices::openUrl(url);
    };
    result.clock = [] { return QTime::currentTime().toString("HH:mm"); };
    result.battery = [] {
        const auto value = battery();
        return BatteryStatus{ value.percent, value.charging };
    };
    result.pageButtons = [] { return true; };
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
    result.lightAvailable = [] { return lightAvailable(); };
    result.textScale = [] {
        // The system font size from Settings, read when the Library shows and when each book opens.
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        const auto configuration =
                context.callObjectMethod("getResources", "()Landroid/content/res/Resources;")
                        .callObjectMethod("getConfiguration",
                                          "()Landroid/content/res/Configuration;");
        return configuration.isValid() ? qreal(configuration.getField<jfloat>("fontScale"))
                                       : qreal(1);
    };
    const auto manufacturer = QJniObject::getStaticObjectField("android/os/Build", "MANUFACTURER",
                                                               "Ljava/lang/String;")
                                      .toString();
    const auto brand =
            QJniObject::getStaticObjectField("android/os/Build", "BRAND", "Ljava/lang/String;")
                    .toString();
    result.softwarePageAnimations = manufacturer.compare("ONYX", Qt::CaseInsensitive) != 0
            && brand.compare("ONYX", Qt::CaseInsensitive) != 0;
    result.animationSupport = [curl = result.softwarePageAnimations] {
        return curl ? PageAnimationSupport::Supported : PageAnimationSupport::Unavailable;
    };
    result.cancelAnimation = [] { };
    result.light = [](QWidget *) { return std::make_unique<Light>(lightValue); };
    result.dictionary = [] { return std::make_unique<Dictionary>(); };
    result.keyboard = [](QWidget *, QLineEdit *editor) {
        return std::make_unique<Keyboard>(editor);
    };
    result.presentation = [](QWidget *) { return std::make_unique<Presentation>(); };
    return result;
}
} // namespace kobalt::android
