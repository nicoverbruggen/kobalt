#include "reader.h"
#include "start_screen.h"
#include "services.h"
#include "runtime.h"
#include "canvas.h"
#include "window_geometry.h"
#include "refresh.h"
#include "canvas_refresh.h"
#include "assets.h"
#include "library_source.h"
#include "interfaces/library/library_view.h"
#include "library/library_store.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "interfaces/reader/views/reading_page.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "support/diagnostics.h"
#include "support/strings.h"
#include "reading/reading_density.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPushButton>
#include <QSettings>
#include <QJniObject>
#include <QScopedValueRollback>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QTimer>
#include <utility>

using namespace kobalt;
namespace {
class Window : public QWidget {
public:
    Window(EngineFontInputs fonts, QString placeholder)
        : controller(storage::Paths{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config"},
                     std::move(fonts), std::move(placeholder), [this] {
                         auto services = android::readerServices();
                         services.nativeDialogParent = [this] { return this; };
                         return services;
                     },
                     [](Qt::ScreenOrientation) { /* Android owns display rotation. */ }),
          store(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config",
                std::make_shared<android::LibrarySource>()) {
        setWindowTitle("Kobalt");
        // Match the activity's edge-to-edge layout. A normal Qt window can
        // restore decor fitting when the embedded reader closes.
        setWindowFlag(Qt::ExpandedClientAreaHint);
        QPalette colors;
        colors.setColor(QPalette::Window, Qt::white);
        colors.setColor(QPalette::WindowText, Qt::black);
        colors.setColor(QPalette::Base, Qt::white);
        colors.setColor(QPalette::Text, Qt::black);
        setPalette(colors);
        setAutoFillBackground(true);
        layout = new QStackedLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        const auto services = android::readerServices();
        start = new android::StartScreen(services.presentation, store, services.textScale);
        start->setLibrarySystemBarBorders(true);
        // A book picked while the library is shown joins it; see openBook.
        connect(start, &android::StartScreen::libraryOpenRequested, this, [] {
            android::activity("chooseBook");
        });
        connect(start, &android::StartScreen::addFolderRequested, this, [] {
            android::activity("chooseFolder");
        });
        connect(start, &android::StartScreen::exitRequested, this, &QWidget::close);
        connect(start, &android::StartScreen::libraryExitRequested, this, &QWidget::close);
        connect(start, &android::StartScreen::bookRequested, this, &Window::openLibraryBook);
        connect(start, &android::StartScreen::markUnreadRequested, this, [this](const QString &key) {
            const auto book = store.index().books.value(key);
            const auto path = book.folder.isEmpty() ? key : cachePath(key);
            if (controller.resetBook(path))
                store.setMark(key, library::Book::Mark::Unread);
            else
                start->showLibraryError(tr("The book's reading settings and position could not be reset."));
        });
        connect(start, &android::StartScreen::removeFolderRequested, this, [this](const QString &key) {
            store.removeFolder(key);
            android::releaseFolder(key);
        });
        connect(start, &android::StartScreen::removeBookRequested, this, [this](const QString &key) {
            const auto book = store.index().books.value(key);
            // A book opened on its own is Kobalt's copy; delete it with the entry.
            if (book.folder.isEmpty() &&
                QFileInfo(QFileInfo(key).absolutePath()).canonicalFilePath() == importedBooks())
                QFile::remove(key);
            store.removeBook(key);
        });
        start->setSingleBooksAreCopies(true);
        start->setDark(controller.preferences().value("dark_mode").toBool(false));
        layout->addWidget(start);
        canvas = new android::WidgetCanvas(this);
        layout->addWidget(canvas);
        new android::CanvasRefresh(canvas, android::refreshAvailable, submitRefresh);
        libraryRefresh = new android::CanvasRefresh(start->libraryCanvas(), android::refreshAvailable, submitRefresh);
        canvas->viewportResized = [this] { sizeCanvas(); };
        libraryHandoff.setInterval(16);
        connect(&libraryHandoff, &QTimer::timeout, this, [this] {
            if (!android::libraryGeometryReady() || !android::contentFitsSurface(*this, android::libraryContentSize()))
                return;
            // Lay out the hidden Library at its final size. Leave its queued
            // layout and proxy updates one event-loop pass before revealing it.
            if (libraryPreparedSize != contentsRect().size()) {
                start->setGeometry(contentsRect());
                start->libraryCanvas()->fitContent();
                libraryPreparedSize = contentsRect().size();
                return;
            }
            layout->setCurrentWidget(start);
            layout->activate();
            libraryRefresh->requestClean();
            surfaceHandoff.present(true);
            libraryHandoff.stop();
            syncPageButtons();
        });
        connect(&controller, &android::ReaderController::retiring, this, [this] {
            if (layout->currentWidget() == canvas) {
                surfaceHandoff.hold([](android::WidgetCanvas *surface) {
                    new android::CanvasRefresh(surface, android::refreshAvailable, submitRefresh, false);
                });
            }
        });
        openingHandoff.setInterval(16);
        connect(&openingHandoff, &QTimer::timeout, this, [this] {
            auto *reader = controller.window();
            if (closing || !reader) {
                openingHandoff.stop();
                return;
            }
            sizeCanvas();
            if (!android::readingGeometryReady() || !reader->readyForPresentation())
                return;
            // Discard any proxy pixels prepared while the canvas was covered.
            QCoreApplication::sendPostedEvents(reader, QEvent::UpdateLater);
            QCoreApplication::sendPostedEvents(reader, QEvent::UpdateRequest);
            reader->graphicsProxyWidget()->update();
            layout->setCurrentWidget(canvas);
            layout->setStackingMode(QStackedLayout::StackOne);
            canvas->viewport()->repaint();
            openingHandoff.stop();
            syncPageButtons();
            reader->checkpoint();
            qInfo() << "Kobalt frame ready";
        });
        qApp->installEventFilter(this);
        android::setCallbacks(
            this,
            [this](const QString &path) {
                openBook(path);
            },
            [this](int direction) {
                if (!pageButtonsReady())
                    return;
                if (layout->currentWidget() == start)
                    start->library()->pageButton(direction);
                else
                    controller.window()->pageButton(direction);
            },
            [this](const QString &uri, const QString &label) {
                store.addFolder({uri, label});
            },
            [this](QSize pixels, QMargins safe) {
                android::fitWindowToSurface(*this, pixels, safe);
            });
        canvas->cancelGesture = [this] {
            if (auto *reader = controller.window())
                if (auto *page = static_cast<ReadingPage *>(reader->findChild<QWidget *>("rrPage")))
                    page->cancelHold();
        };
        connect(qApp, &QCoreApplication::aboutToQuit, this, [] {
            android::setPageButtonsAvailable(false);
        });
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            // The system font size can change in Settings while Kobalt is in the background.
            if (state == Qt::ApplicationActive && layout->currentWidget() == start && start->libraryShown())
                start->applyTextSize();
            syncPageButtons();
            if (state != Qt::ApplicationActive && controller.window())
                controller.window()->checkpoint();
        });
        connect(&controller, &android::ReaderController::openingProgress, start, &android::StartScreen::setProgress);
        connect(&controller, &android::ReaderController::opened, this, [this] {
            auto *reader = controller.window();
            canvas->setContent(reader);
            setPalette(reader->palette());
            sizeCanvas();
            if (!android::refreshAvailable())
                if (auto *control = reader->findChild<QWidget *>("rrScreenRefresh"))
                    control->hide();
            reader->show();
            surfaceHandoff.present(true);
            setUpdatesEnabled(true);
            openingHandoff.start();
            QSettings settings;
            settings.remove("lastBook");
            settings.sync();
        });
        connect(&controller, &android::ReaderController::failed, this, [this](const QString &error) {
            failedOpening = true;
            android::setPageButtonsAvailable(false);
            openingHandoff.stop();
            layout->setStackingMode(QStackedLayout::StackOne);
            setPalette(start->palette());
            android::activity("showStartup");
            start->showLibraryError(error);
            layout->setCurrentWidget(start);
            syncPageButtons();
            qWarning() << "Kobalt reader error:" << error;
        });
        connect(&controller, &android::ReaderController::closed, this, [this] {
            android::setPageButtonsAvailable(false);
            if (closing) {
                android::activity("exitReader");
                QCoreApplication::quit();
                return;
            }
            openingHandoff.stop();
            layout->setStackingMode(QStackedLayout::StackOne);
            setPalette(start->palette());
            android::activity("showStartup");
            recordProgress();
            start->setDark(controller.preferences().value("dark_mode").toBool(false));
            if (!failedOpening)
                start->showLibrary();
            libraryPreparedSize = {};
            libraryHandoff.start();
        });
        QTimer::singleShot(0, this, [this] {
            android::activity("readerReady");
            if (store.index().settings.checkAtStart)
                store.scan();
            syncPageButtons();
        });
    }
    ~Window() override {
        android::setCallbacks(nullptr, {}, {});
    }

private:
    android::ReaderController controller;
    library::Store store;
    QStackedLayout *layout;
    android::StartScreen *start;
    // The library entry of the book being read, and the local file it opened.
    QString openedKey, openedPath;
    android::WidgetCanvas *canvas;
    android::SurfaceHandoff surfaceHandoff{*this};
    android::CanvasRefresh *libraryRefresh = nullptr;
    QTimer libraryHandoff;
    QSize libraryPreparedSize;
    static bool submitRefresh(const android::RefreshFrame &frame) {
        return android::prepareRefreshRegions(frame.screenPixels - frame.textPixels, frame.textPixels, frame.clean);
    }
    QTimer openingHandoff;
    bool closing = false, layoutRepairPending = false, failedOpening = false;
    QSize pixels() const {
        if (canvas->isVisible())
            return canvas->pixelSize();
        // The loading page occupies the same safe content area as the canvas.
        // A hidden canvas can still have its old viewport size until shown.
        const auto area = contentsRect().size();
        return {qRound(area.width() * devicePixelRatioF()), qRound(area.height() * devicePixelRatioF())};
    }
    void sizeCanvas() {
        canvas->fitContent();
    }
    bool pageButtonsReady() const {
        if (closing || !isEnabled() || openingHandoff.isActive() || libraryHandoff.isActive() ||
            QGuiApplication::applicationState() != Qt::ApplicationActive)
            return false;
        if (layout->currentWidget() == start)
            return start->libraryShown();
        auto *reader = controller.window();
        return layout->currentWidget() == canvas && reader && reader->isVisible() && reader->isEnabled();
    }
    void syncPageButtons() {
        android::setPageButtonsAvailable(pageButtonsReady());
    }
    static QString appData() {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    // The activity copies picked and shared files to getFilesDir()/books,
    // which is Qt's application data folder on Android.
    static QString importedBooks() {
        return QFileInfo(appData() + "/books").canonicalFilePath();
    }
    QString cachePath(const QString &uri) const {
        return appData() + "/library-cache/" +
               QString::fromLatin1(QCryptographicHash::hash(uri.toUtf8(), QCryptographicHash::Sha1).toHex()) + ".epub";
    }
    void openLibraryBook(const QString &key) {
        const auto book = store.index().books.value(key);
        if (book.key.isEmpty() || closing)
            return;
        if (book.folder.isEmpty()) {
            openBook(key);
            return;
        }
        // Library books are read in place; the reader opens a cached copy.
        pendingKey = key;
        pendingPath = cachePath(key);
        showOpening();
        android::openLibraryBook(key, pendingPath, book.size, book.modified);
    }
    void recordProgress() {
        if (openedKey.isEmpty())
            return;
        const auto progress = library::savedProgress(controller.savedState(openedPath));
        store.setProgress(std::exchange(openedKey, {}), progress.percent, progress.finished);
    }
    QString pendingKey, pendingPath;
    void showOpening() {
        start->showOpening();
        new android::CanvasRefresh(start->openingCanvas(), android::refreshAvailable, submitRefresh);
    }
    void openBook(const QString &path) {
        if (closing)
            return;
        recordProgress();
        if (path.isEmpty()) {
            // The activity could not copy a library book; it said why.
            pendingKey.clear();
            pendingPath.clear();
            start->showLibrary();
            return;
        }
        if (path == pendingPath && !pendingKey.isEmpty()) {
            openedKey = std::exchange(pendingKey, {});
        } else {
            // Document picking and Android Open With use the same library path.
            const auto key = QFileInfo(path).absoluteFilePath();
            if (!store.index().books.contains(key))
                store.addBook(path);
            openedKey = store.index().books.contains(key) ? key : QString();
        }
        pendingPath.clear();
        openedPath = path;
        if (!openedKey.isEmpty())
            store.setOpened(openedKey);
        if (controller.window() && controller.path() == path)
            return;
        failedOpening = false;
        android::setPageButtonsAvailable(false);
        openingHandoff.stop();
        showOpening();
        layout->setCurrentWidget(start);
        // Keep the canvas laid out behind the opaque loader as insets change.
        layout->setStackingMode(QStackedLayout::StackAll);
        android::activity("showReading");
        layout->activate();
        controller.open(path, pixels(), openedKey);
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        auto *reader = controller.window();
        if (reader && object == reader && event->type() == QEvent::Close && reader->updatesEnabled())
            surfaceHandoff.capture();
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
            event->type() == QEvent::EnabledChange) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && (widget == this || widget == start || start->isAncestorOf(widget) ||
                           (reader && (widget == reader || reader->isAncestorOf(widget)))))
                syncPageButtons();
        }
        if (reader && object == reader && event->type() == QEvent::PaletteChange && palette() != reader->palette())
            setPalette(reader->palette());
        if (reader && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && (widget == reader || reader->isAncestorOf(widget)) && !layoutRepairPending) {
                layoutRepairPending = true;
                QTimer::singleShot(0, this, [this] {
                    layoutRepairPending = false;
                    auto *reader = controller.window();
                    if (!reader)
                        return;
                    // Proxy resize can leave newly created children with an
                    // inherited disabled-update flag. Preserve explicit disables.
                    for (auto *child : reader->findChildren<QWidget *>())
                        if (!child->updatesEnabled() && !child->testAttribute(Qt::WA_ForceUpdatesDisabled) &&
                            child->parentWidget() && child->parentWidget()->updatesEnabled())
                            child->setUpdatesEnabled(true);
                });
            }
        }
        return QWidget::eventFilter(object, event);
    }
    void closeEvent(QCloseEvent *event) override {
        // Qt closes top-level windows while quitting. Accept that request
        // once reader work has drained, or the event loop cannot exit.
        if (closing && controller.idle()) {
            event->accept();
            return;
        }
        event->ignore();
        if (closing)
            return;
        if (start->dismissModal())
            return;
        if (auto *reader = controller.window()) {
            android::setPageButtonsAvailable(false);
            reader->close();
            return;
        }
        if (!controller.idle()) {
            controller.close();
            return;
        }
        closing = true;
        controller.close();
    }
};
}
int main(int argc, char **argv) {
    FontCopies copies;
    // BOOX attaches refresh metadata to Surface buffers. Qt otherwise uses
    // a TextureView when the embedded library creates another Qt window first.
    const auto onyx = [](const char *field) {
        return QJniObject::getStaticObjectField("android/os/Build", field, "Ljava/lang/String;")
                   .toString().compare(QStringLiteral("ONYX"), Qt::CaseInsensitive) == 0;
    };
    if (onyx("MANUFACTURER") || onyx("BRAND"))
        qputenv("QT_ANDROID_SURFACE_CONTAINER_TYPE", "0");
    QApplication app(argc, argv);
    app.setApplicationName("Kobalt");
    app.setOrganizationName("Kobalt");
    app.setQuitOnLastWindowClosed(false);
    const auto assets = android::installAssets(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!assets || !ui::initializeInterfaceFonts(*assets)) {
        qCritical() << "Kobalt bundled assets are unavailable";
        return 2;
    }
    for (const auto &file : bundledFontFiles(*assets))
        if (QFontDatabase::addApplicationFont(file) < 0) {
            qCritical() << "Cannot register font" << file;
            return 2;
        }
    const auto physicalPpi = android::screenDensity();
    readingPpi.store(physicalPpi);
    ui::setDensity(physicalPpi);
    QApplication::setFont(ui::interfaceFont());
    initializeDiagnostics(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics");
    Window window(engineFontInputs(*assets, "/system/fonts", {}), placeholderFontFamily(*assets, copies));
    // Keep edge-to-edge geometry while the activity controls bar visibility.
    window.show();
    qInfo() << "Kobalt Android ready; density" << physicalPpi;
    return app.exec();
}
