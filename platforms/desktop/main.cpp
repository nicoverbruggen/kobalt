#include "platform/posix/diagnostics.h"
#include "window.h"
#include "engine_monitor.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "ui/style/fonts.h"
#include "reading/reading_density.h"
#include "support/diagnostics.h"
#include <QApplication>
#include <QFileOpenEvent>
#include <QFontDatabase>
#include <QScreen>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

namespace {
class Application final : public QApplication
{
public:
    using QApplication::QApplication;
    std::function<void(QString)> open;
    QString pending;

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            const auto path = static_cast<QFileOpenEvent *>(event)->file();
            if (open)
                open(path);
            else
                pending = path;
            return true;
        }
        return QApplication::event(event);
    }
};
} // namespace

int main(int argc, char **argv)
{
    using namespace kobalt;
#ifdef Q_OS_MACOS
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "cocoa:fontengine=freetype");
#endif
    FontCopies copies;
    Application app(argc, argv);
    app.setApplicationName(QStringLiteral(KOBALT_PROFILE_NAME));
    app.setOrganizationName("Kobalt");
    app.setStyle("Fusion");
    AssetBundle assets(QDir(QCoreApplication::applicationDirPath())
                               .absoluteFilePath(QStringLiteral(KOBALT_RUNTIME_ASSET_PATH)),
                       QStringLiteral(KOBALT_VERSION));
    if (!assets.available() || !ui::initializeInterfaceFonts(assets)) {
        qCritical() << "Kobalt bundled assets are unavailable";
        return 2;
    }
    for (const auto &file : bundledFontFiles(assets))
        if (QFontDatabase::addApplicationFont(file) < 0) {
            qCritical() << "Cannot register font" << file;
            return 2;
        }
    readingPpi.store(300);
    ui::setDensity(300);
    app.setFont(ui::interfaceFont());
    const auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    setDiagnosticSystem(platform::processDiagnosticSystem());
    initializeDiagnostics(root + "/diagnostics");
    desktop::Window window(storage::Paths{ root + "/config" },
                           engineFontInputs(assets, {}, root + "/fonts"),
                           placeholderFontFamily(assets, copies));
    window.resize(window.size().boundedTo(app.primaryScreen()->availableGeometry().size()
                                          - QSize(80, 80)));
    app.open = [&window](const QString &path) {
        window.openBook(path);
        window.show();
        window.raise();
    };
    desktop::EngineMonitor monitor(&window);
    setDiagnosticObserver(monitor.observer());
    QObject::connect(&window, &desktop::Window::engineMonitorRequested, &monitor, [&monitor] {
        monitor.show();
        monitor.raise();
        monitor.activateWindow();
    });
    window.show();
    if (qgetenv("KOBALT_ENGINE_MONITOR") == "1") {
        const QRect available = app.primaryScreen()->availableGeometry();
        window.move(available.topLeft());
        monitor.resize(qMax(640, available.width() - window.frameGeometry().width() - 24),
                       qMin(800, available.height() - 60));
        monitor.move(qMin(available.right() - monitor.width(), window.frameGeometry().right() + 16),
                     available.top() + 24);
        monitor.show();
    }
    const auto args = app.arguments();
    const auto path = !app.pending.isEmpty() ? app.pending
            : args.size() > 1                ? args.at(1)
                                             : QString();
    if (!path.isEmpty())
        QTimer::singleShot(0, &window, [&window, path] { window.openBook(path); });
    const int result = app.exec();
    setDiagnosticObserver({});
    return result;
}
