#include "window.h"
#include "services.h"
#include "device.h"
#include "pointer_cancellation.h"
#include "qpa/sleep_frame.h"
#include "platforms/android/canvas.h"
#include "platforms/android/start_screen.h"
#include "interfaces/library/library_view.h"
#include "platform/api/kobo_refresh.h"
#include "interfaces/reader/ui/style/fonts.h"
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QLabel>
#include <QHBoxLayout>
#include <QApplication>
#include <QWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <utility>

namespace kobalt::kindle {
namespace {
class SleepView final : public QLabel {
public:
    explicit SleepView(QWidget *parent) : QLabel(parent) {}
    void paintEvent(QPaintEvent *event) override {
        auto *surface = parentWidget()->windowHandle();
        const auto generation = surface ? surface->property("kobalt.sleepRequested").toULongLong() : 0;
        QLabel::paintEvent(event);
        if (surface)
            ::kindle::sleepFramePainted(*surface, generation);
    }
};
}
Window::Window(storage::Paths paths, EngineFontInputs fonts, QString placeholder, QString root)
    : device(makeDevice()), controller(paths, std::move(fonts), std::move(placeholder),
                 [device = device, exports = paths.directory + QStringLiteral("/exports")] {
                     auto services = readerServices(device);
                     services.annotationExportDirectory = [exports] { return exports; };
                     return services;
                 },
                 [](Qt::ScreenOrientation) {}),
      store(paths.directory, std::make_shared<library::LocalSource>()),
      libraryRoot(QFileInfo(root).absoluteFilePath()) {
    setWindowTitle(QStringLiteral("Kobalt"));
    new PointerCancellation(this);
    setObjectName("kindleWindow");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stack = new QStackedWidget(this);
    layout->addWidget(stack, 1);
    exit = new QPushButton(tr("Exit Kobalt"), this);
    exit->setObjectName("kindleExit");
    exit->setMinimumHeight(90);
    auto footerFont = ui::interfaceFont();
    footerFont.setPixelSize(32);
    exit->setFont(footerFont);
    exit->setFocusPolicy(Qt::NoFocus);
    auto *footer = new QHBoxLayout;
    status = new QLabel(this);
    status->setObjectName("kindleDeviceStatus");
    footerFont.setPixelSize(28);
    status->setFont(footerFont);
    status->setMargin(8);
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    footer->addWidget(status, 1);
    footer->addWidget(exit);
    layout->addLayout(footer);
    sleepView = new SleepView(this);
    sleepView->setObjectName("kindleSleepView");
    sleepView->setAutoFillBackground(true);
    auto sleepFont = ui::interfaceFont();
    sleepFont.setPixelSize(64);
    sleepView->setFont(sleepFont);
    sleepView->setAlignment(Qt::AlignCenter);
    sleepView->hide();
    retirementView = new QLabel(this);
    retirementView->setObjectName("kindleRetirementView");
    retirementView->setScaledContents(true);
    retirementView->hide();
    auto updateStatus = [this] {
        const auto &state = device->state();
        const auto battery = state.battery.percent < 0 ? tr("Battery unavailable")
            : tr("%1% battery%2").arg(state.battery.percent).arg(state.battery.charging ? tr(" (charging)") : QString());
        const auto wifi = !state.wifi.enabled ? tr("Wi-Fi off") : state.wifi.connected ? tr("Wi-Fi connected") : tr("Wi-Fi disconnected");
        status->setText(battery + QStringLiteral("   |   ") + wifi + '\n' +
                        (state.bluetoothConnected ? tr("Bluetooth connected") : tr("Bluetooth disconnected")));
    };
    connect(device.get(), &Device::changed, this, updateStatus);
    updateStatus();
    connect(device.get(), &Device::sleepingChanged, this, &Window::sleepingChanged);
    connect(device.get(), &Device::sleepFrameChanged, this, &Window::updateSleepView);
    connect(device.get(), &Device::usbRequested, this, &QWidget::close);
    connect(device.get(), &Device::lifecycleFailed, this, [this](const QString &error) {
        deviceFailure = true;
        qCritical().noquote() << error;
        close();
    });
    standby.setInterval(100);
    connect(&standby, &QTimer::timeout, this, [this] {
        auto *reader = controller.window();
        if (!reader || reader->flushBeforeStandby())
            standby.stop();
    });
    qApp->installEventFilter(this);
    connect(exit, &QPushButton::clicked, this, &QWidget::close);
    start = new android::StartScreen(readerServices(device).presentation, store, stack);
    start->setDark(controller.preferences().value("dark_mode").toBool(false));
    canvas = new android::WidgetCanvas(stack);
    canvas->setObjectName("kindleReaderCanvas");
    stack->addWidget(start);
    stack->addWidget(canvas);
    platform::setReaderRefreshWindow(this);
    canvas->viewportResized = [this] { sizeCanvas(); };
    canvas->framePainted = [this](const QRegion &) {
        auto *reader = controller.window();
        int direction = 0;
        if (reader) {
            direction = reader->property("reader_animation_direction").toInt();
            reader->setProperty("reader_animation_direction", 0);
            if (!reader->property("reader_page_turn").toBool())
                direction = 0;
        }
        if (windowHandle()) {
            if (closing || device->state().sleeping || stack->currentWidget() != canvas ||
                QApplication::activeModalWidget() || retirementView->isVisible())
                direction = 0;
            windowHandle()->setProperty("kobalt.pageTurnDirection", direction);
        }
    };
    connect(start, &android::StartScreen::bookRequested, this, &Window::openBook);
    connect(start, &android::StartScreen::exitRequested, this, &Window::closeBook);
    const auto fixedFolderNotice = [this] {
        start->showLibraryError(tr("Copy EPUB books to %1, then choose Rescan all folders in Library folders. "
                                  "Another folder can be selected with the --library launch option.").arg(libraryRoot));
    };
    connect(start, &android::StartScreen::libraryOpenRequested, this, fixedFolderNotice);
    connect(start, &android::StartScreen::addFolderRequested, this, fixedFolderNotice);
    connect(start, &android::StartScreen::removeFolderRequested, &store, &library::Store::removeFolder);
    connect(start, &android::StartScreen::removeBookRequested, &store, &library::Store::removeBook);
    connect(start, &android::StartScreen::markUnreadRequested, this, [this](const QString &key) {
        if (controller.resetBook(key))
            store.setMark(key, library::Book::Mark::Unread);
        else
            start->showLibraryError(tr("The book's reading settings and position could not be reset."));
    });
    auto *query = start->library()->findChild<QLineEdit *>("librarySearchQuery");
    if (query) {
        libraryQuery = query;
        libraryKeyboard = readerServices(device).keyboard(start->library(), query);
    }
    connect(&controller, &android::ReaderController::openingProgress, start, &android::StartScreen::setProgress);
    connect(&controller, &android::ReaderController::opened, this, [this] {
        auto *reader = controller.window();
        canvas->setContent(reader);
        stack->setCurrentWidget(canvas);
        sizeCanvas();
        reader->show();
        reader->checkpoint();
        if (device->state().sleeping) {
            reader->sleepReader();
            reader->sleepViewReady();
            updateSleepView();
        }
        platform::pageRefresh(this, true);
        emit bookOpened();
    });
    connect(&controller, &android::ReaderController::failed, this, [this](const QString &error) {
        failed = true;
        start->showLibraryError(error);
        stack->setCurrentWidget(start);
        emit bookFailed(error);
    });
    connect(&controller, &android::ReaderController::retiring, this, [this] {
        if (stack->currentWidget() != canvas || !controller.window())
            return;
        // Reader destruction precedes asynchronous engine/resource cleanup.
        // Retain its visible frame until the Library can replace it.
        retirementView->setPixmap(grab());
        retirementView->setGeometry(rect());
        retirementView->show();
        retirementView->raise();
        if (device->state().sleeping)
            sleepView->raise();
    });
    connect(&controller, &android::ReaderController::closed, this, [this] {
        recordProgress();
        start->setDark(controller.preferences().value("dark_mode").toBool(false));
        if (!failed)
            start->showLibrary();
        stack->setCurrentWidget(start);
        exit->show();
        status->show();
        this->layout()->activate();
        sizeCanvas();
        retirementView->hide();
        retirementView->setPixmap({});
        platform::pageRefresh(this, true);
        repaint();
        emit bookClosed();
        if (closing)
            close();
    });
    connect(device.get(), &Device::initializedChanged, this, [this] {
        if (!pendingOpen.isEmpty())
            openBook(std::exchange(pendingOpen, {}));
        if (device->initialized()) {
            platform::pageRefresh(this, true);
            update();
        }
    });
    QTimer::singleShot(0, this, [this] {
        if (!QFileInfo(libraryRoot).isDir()) {
            start->showLibraryError(tr("Library folder %1 does not exist.").arg(libraryRoot));
            return;
        }
        bool known = false;
        for (const auto &folder : store.index().folders)
            known = known || folder.key == libraryRoot;
        if (!known)
            store.addFolder({libraryRoot, QFileInfo(libraryRoot).fileName()});
        else if (store.index().settings.checkAtStart)
            store.scan();
    });
}
void Window::startDeviceLifecycle() {
    device->startLifecycle(qEnvironmentVariable("KOBALT_KINDLE_POWER_BRIDGE",
        QCoreApplication::applicationDirPath() + QStringLiteral("/kobalt-power-bridge")));
}
#ifdef KOBALT_KINDLE_NATIVE_TESTING
void Window::simulatePowerEvent(const QByteArray &event) { device->simulatePowerEvent(event); }
#endif
void Window::updateSleepView() {
    if (!device->state().sleeping)
        return;
    if (windowHandle())
        ::kindle::requestSleepFrame(*windowHandle());
    sleepView->setGeometry(rect());
    const auto &frame = device->sleepFrame();
    if (frame.isNull()) {
        sleepView->setPixmap({});
        sleepView->setText(tr("Kobalt\nSleeping"));
    } else {
        sleepView->setPixmap(QPixmap::fromImage(frame).scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    sleepView->show();
    sleepView->raise();
    platform::pageRefresh(this, true);
}
void Window::sleepingChanged(bool sleeping) {
    if (windowHandle()) {
        windowHandle()->setProperty("kobalt.inputBlocked", sleeping);
        QEvent cancel(QEvent::UngrabMouse);
        QCoreApplication::sendEvent(windowHandle(), &cancel);
    }
    if (sleeping) {
        if (libraryKeyboard)
            libraryKeyboard->hide();
        if (auto *reader = controller.window()) {
            reader->sleepReader();
            reader->sleepViewReady();
            standby.start();
        }
        updateSleepView();
    } else {
        standby.stop();
        if (windowHandle())
            ::kindle::wakeDisplay(*windowHandle());
        sleepView->hide();
        if (auto *reader = controller.window()) {
            reader->prepareWake();
            reader->wakeReader();
        }
        sizeCanvas();
        platform::pageRefresh(this, true);
        update();
    }
}
Window::~Window() {
    qApp->removeEventFilter(this);
    standby.stop();
    device->stop();
    libraryKeyboard.reset();
    platform::setReaderRefreshWindow(nullptr);
    // The start screen's destructor disconnects Store callbacks.
    delete stack;
}
bool Window::libraryContains(const QString &path) const {
    return store.index().books.contains(QFileInfo(path).absoluteFilePath());
}
void Window::sizeCanvas() {
    if (preview) {
        const QSize pixels(1236, 1648);
        const qreal scale = qMin(qreal(width()) / pixels.width(), qreal(height()) / pixels.height());
        exit->setMinimumHeight(qMax(1, qRound(90 * scale)));
        auto font = ui::interfaceFont();
        font.setPixelSize(qMax(1, qRound(32 * scale)));
        exit->setFont(font);
        font.setPixelSize(qMax(1, qRound(28 * scale)));
        status->setFont(font);
        status->setMargin(qMax(1, qRound(8 * scale)));
        if (auto *reader = controller.window())
            reader->resize(pixels);
        canvas->setTransform(QTransform::fromScale(scale, scale));
        canvas->setSceneRect(QRectF(QPointF(), pixels));
        start->setLibraryScale({pixels.width(), pixels.height() - 90}, scale, true, 1);
    } else {
        canvas->fitContent();
    }
    if (libraryKeyboard)
        libraryKeyboard->place();
}
void Window::setPreview(bool enabled) {
    preview = enabled;
    sizeCanvas();
}
QSize Window::readerPixels() const {
    return preview ? QSize(1236, 1648) : canvas->pixelSize();
}
void Window::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    retirementView->setGeometry(rect());
    sizeCanvas();
    updateSleepView();
}
void Window::recordProgress() {
    if (openedKey.isEmpty())
        return;
    const auto progress = library::savedProgress(controller.savedState(openedKey));
    store.setProgress(std::exchange(openedKey, {}), progress.percent, progress.finished);
    store.save();
}
void Window::openBook(const QString &path) {
    if (closing || !controller.idle())
        return;
    if (!device->initialized()) {
        pendingOpen = path;
        start->showOpening();
        exit->hide();
        return;
    }
    const auto absolute = QFileInfo(path).absoluteFilePath();
    if (store.index().books.contains(absolute) || store.addBook(absolute)) {
        openedKey = absolute;
        store.setOpened(absolute);
    }
    failed = false;
    if (libraryKeyboard)
        libraryKeyboard->hide();
    exit->hide();
    status->hide();
    start->showOpening();
    stack->setCurrentWidget(start);
    // Wait for hiding the library-only Exit button to restore the full viewport.
    layout()->activate();
    controller.open(absolute, readerPixels());
}
void Window::closeBook() {
    if (!pendingOpen.isEmpty()) {
        pendingOpen.clear();
        start->showLibrary();
        exit->show();
        status->show();
    }
    controller.close();
}
void Window::closeEvent(QCloseEvent *event) {
    if (controller.idle()) {
        if (QGuiApplication::platformName() == QStringLiteral("kindle") && !deviceFailure &&
            !device->state().sleeping && !exitFrameReady) {
            event->ignore();
            if (!std::exchange(exitFramePending, true)) {
                closing = true;
                pendingOpen.clear();
                sleepView->setText(QStringLiteral("<div>%1</div><div style='font-size:32px; margin-top:24px'>%2</div>")
                                       .arg(tr("Returning to Kindle…").toHtmlEscaped(),
                                            tr("This may take a few minutes.").toHtmlEscaped()));
                sleepView->setGeometry(rect());
                sleepView->show();
                sleepView->raise();
                windowHandle()->setProperty("kobalt.inputBlocked", true);
                windowHandle()->setProperty("kobalt.exitRequested", true);
                platform::pageRefresh(this, true);
                repaint();
                // A failed driver must not prevent saved-state shutdown and
                // launcher recovery. The kernel's refresh wait is five seconds.
                QTimer::singleShot(6500, this, [this] {
                    if (!exitFrameReady) {
                        qWarning("kindle: exit feedback was not acknowledged before recovery");
                        exitFrameReady = true;
                        close();
                    }
                });
            }
            return;
        }
        closing = true;
        pendingOpen.clear();
        store.save();
        event->accept();
        return;
    }
    event->ignore();
    if (!std::exchange(closing, true))
        controller.close();
}
bool Window::eventFilter(QObject *object, QEvent *event) {
    if (object == windowHandle() && event->type() == QEvent::DynamicPropertyChange &&
        static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "kobalt.exitPresented" &&
        object->property("kobalt.exitPresented").toBool() && exitFramePending) {
        exitFrameReady = true;
        QTimer::singleShot(0, this, &QWidget::close);
    }
    if (object == qApp && event->type() == QEvent::DynamicPropertyChange &&
        static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "kobalt.displayFailure") {
        deviceFailure = true;
        close();
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick ||
        event->type() == QEvent::MouseMove || event->type() == QEvent::TouchBegin ||
        event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
        if (device->state().sleeping)
            return true;
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick ||
            event->type() == QEvent::TouchBegin)
            device->activity();
    }
    if (libraryKeyboard && object == libraryQuery) {
        if (event->type() == QEvent::Show || event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress)
            libraryKeyboard->show();
        else if (event->type() == QEvent::Hide)
            libraryKeyboard->hide();
    }
    return QWidget::eventFilter(object, event);
}
}
