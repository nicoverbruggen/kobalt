#include "window.h"
#include "services.h"
#include "platform/android/canvas.h"
#include "platform/android/start_screen.h"
#include "interfaces/reader/views/reading_page.h"
#include "reading/reading_density.h"
#include "interfaces/reader/ui/style/scale.h"
#include <QComboBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QLabel>
#include <QGridLayout>
#include <QFile>
#include <lunasvg.h>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolBar>
#include <QPainter>
#include <QTime>
#include <QScreen>
#include <QWindow>
#include <QShowEvent>

static void initializeDesktopArtwork() {
    Q_INIT_RESOURCE(desktop_artwork);
}

namespace kobalt::desktop {
// Two reader pixels per Mac point match the 300 PPI controls on a Retina
// display. Keep that scale on other displays so moving the window does not
// change the controls' apparent size.
constexpr qreal readerPixelsPerPoint = 2.0;
class ToolbarLogo final : public QWidget {
public:
    explicit ToolbarLogo(QWidget *parent) : QWidget(parent) {
        setObjectName("desktopLogo");
        setAccessibleName(tr("Kobalt"));
        setFixedSize(28, 28);
        initializeDesktopArtwork();
        QFile artwork(":/desktop/kobalt-android.svg");
        if (artwork.open(QIODevice::ReadOnly))
            svg = lunasvg::Document::loadFromData(artwork.readAll().toStdString());
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (!svg)
            return;
        const qreal dpr = devicePixelRatioF();
        if (image.isNull() || image.devicePixelRatio() != dpr) {
            auto bitmap = svg->renderToBitmap(qRound(width() * dpr), qRound(height() * dpr));
            image = QImage(bitmap.data(), bitmap.width(), bitmap.height(), bitmap.stride(),
                           QImage::Format_ARGB32_Premultiplied)
                        .copy();
            image.setDevicePixelRatio(dpr);
        }
        QPainter painter(this);
        painter.drawImage(QPoint(), image);
    }

private:
    std::unique_ptr<lunasvg::Document> svg;
    QImage image;
};
class DeviceButton final : public QPushButton {
public:
    explicit DeviceButton(QWidget *parent) : QPushButton(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#111111"), 1));
        painter.setBrush(isDown() ? QColor("#444444") : QColor("#303030"));
        const qreal radius = qMax(3.0, width() * 0.25);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
};
// Dimensions are in device pixels at 300 PPI. The asymmetric grip belongs
// outside the Libra screen, so its buttons never reduce the reading viewport.
class DeviceFrame final : public QWidget {
public:
    explicit DeviceFrame(QWidget *parent) : QWidget(parent) {
        previous = new DeviceButton(this);
        next = new DeviceButton(this);
        previous->setObjectName("desktopDevicePrevious");
        next->setObjectName("desktopDeviceNext");
        previous->setAccessibleName(tr("Previous page"));
        next->setAccessibleName(tr("Next page"));
        previous->setToolTip(tr("Previous page"));
        next->setToolTip(tr("Next page"));
        for (auto *button : {previous, next})
            button->setFocusPolicy(Qt::NoFocus);
    }
    QRect layoutScreen(int preset, QRect bounds) {
        libra = preset == 2;
        const QSize body = libra ? QSize(1708, 1902) : QSize(1323, 1890);
        const QRect screen = libra ? QRect(90, 111, 1264, 1680) : QRect(126, 150, 1072, 1448);
        const auto fitted = body.scaled(bounds.size(), Qt::KeepAspectRatio);
        setGeometry(QRect(bounds.center() - QPoint(fitted.width() / 2, fitted.height() / 2), fitted));
        const qreal scale = qMin(qreal(width()) / body.width(), qreal(height()) / body.height());
        auto scaled = [scale](QRect rect) {
            return QRect(qRound(rect.x() * scale), qRound(rect.y() * scale), qRound(rect.width() * scale),
                         qRound(rect.height() * scale));
        };
        screenBounds = scaled(screen);
        previous->setGeometry(scaled(QRect(1450, 715, 110, 180)));
        next->setGeometry(scaled(QRect(1450, 1005, 110, 180)));
        for (auto *button : {previous, next})
            button->setVisible(libra);
        show();
        update();
        return screenBounds;
    }
    QPushButton *previous, *next;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::black, 1.5));
        painter.setBrush(QColor("#242424"));
        const qreal radius = qMax(5.0, width() * 0.025);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius, radius);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::black, 2));
        painter.drawRect(QRectF(screenBounds).adjusted(-1, -1, 1, 1));
    }

private:
    bool libra = false;
    QRect screenBounds;
};
Window::Window(storage::Paths paths, EngineFontInputs fonts, QString placeholder)
    : controller(
          std::move(paths), std::move(fonts), std::move(placeholder),
          [this] {
              auto services = readerServices();
              services.devicePreview = [this] {
                  return screenPreset != 0;
              };
              services.pageButtons = [this] {
                  return screenPreset == 2;
              };
              services.clock = [this] {
                  return screenPreset ? QStringLiteral("09:00") : QTime::currentTime().toString("HH:mm");
              };
              services.battery = [this] {
                  return screenPreset ? BatteryStatus{100, false} : BatteryStatus{};
              };
              return services;
          },
          [](Qt::ScreenOrientation) {}) {
    setWindowTitle("Kobalt");
    setMinimumSize(420, 500);
    resize(640, 800);
    setAcceptDrops(true);
    // Keep book settings and UI metrics in the same 300 PPI coordinates in
    // every mode. Only the preview transform depends on the Mac display.
    readingPpi.store(300);
    ui::setDensity(300);
    auto *stage = new QWidget(this);
    auto palette = stage->palette();
    palette.setColor(QPalette::Window, Qt::white);
    stage->setPalette(palette);
    stage->setAutoFillBackground(true);
    setCentralWidget(stage);
    deviceFrame = new DeviceFrame(stage);
    deviceFrame->hide();
    stack = new QStackedWidget(stage);
    connect(deviceFrame->previous, &QPushButton::clicked, this, [this] {
        if (auto *reader = controller.window())
            reader->pageButton(-1);
    });
    connect(deviceFrame->next, &QPushButton::clicked, this, [this] {
        if (auto *reader = controller.window())
            reader->pageButton(1);
    });
    auto *toolbar = addToolBar(tr("Screen"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    screenPicker = new QComboBox(toolbar);
    screenPicker->setObjectName("desktopScreenPicker");
    screenPicker->setAccessibleName(tr("Reader screen"));
    screenPicker->addItems({tr("Fill window"), tr("6-inch e-reader"), tr("7-inch e-reader with buttons")});
    auto *contents = new QWidget(toolbar);
    contents->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QGridLayout(contents);
    layout->setContentsMargins(12, 4, 12, 4);
    auto *logo = new ToolbarLogo(contents);
    auto *version = new QLabel(QStringLiteral(KOBALT_VERSION), contents);
    version->setObjectName("desktopVersion");
    // Equal side columns keep the picker centered even when the version is wider than the icon.
    const int sideWidth = qMax(logo->width(), version->sizeHint().width());
    layout->setColumnMinimumWidth(0, sideWidth);
    layout->setColumnMinimumWidth(2, sideWidth);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(2, 1);
    layout->addWidget(logo, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(screenPicker, 0, 1, Qt::AlignCenter);
    layout->addWidget(version, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
    toolbar->addWidget(contents);
    connect(screenPicker, qOverload<int>(&QComboBox::currentIndexChanged), this, &Window::selectScreen);
    start = new android::StartScreen(readerServices().presentation);
    start->setLastBook(controller.lastBook());
    connect(start, &android::StartScreen::reopenRequested, this, &Window::openBook);
    connect(start, &android::StartScreen::forgetRequested, this, [this] {
        if (controller.forgetLastBook())
            start->setLastBook({});
        else
            start->showForgetError();
    });
    canvas = new android::WidgetCanvas(stack);
    stack->addWidget(start);
    stack->addWidget(canvas);
    connect(start, &android::StartScreen::openRequested, this, &Window::chooseBook);
    connect(start, &android::StartScreen::exitRequested, this, &QWidget::close);
    auto *file = menuBar()->addMenu(tr("File"));
    auto *open = file->addAction(tr("Open EPUB…"), this, &Window::chooseBook);
    open->setShortcut(QKeySequence::Open);
    auto *quit = file->addAction(tr("Quit Kobalt"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    for (const auto key : {Qt::Key_Right, Qt::Key_PageDown, Qt::Key_Left, Qt::Key_PageUp}) {
        auto *shortcut = new QShortcut(QKeySequence(key), this);
        connect(shortcut, &QShortcut::activated, this, [this, key] {
            turnPage(key == Qt::Key_Right || key == Qt::Key_PageDown ? 1 : -1);
        });
    }
    resizeTimer.setSingleShot(true);
    resizeTimer.setInterval(100);
    canvas->viewportResized = [this] {
        resizeTimer.start();
    };
    connect(&resizeTimer, &QTimer::timeout, this, [this] {
        sizeCanvas();
        // Screen changes can keep the same logical bounds. Repaint the proxy
        // at the new display scale even when no QWidget resize occurred.
        canvas->scene()->invalidate();
        canvas->viewport()->update();
        update();
    });
    qApp->installEventFilter(this);
    connect(&controller, &android::ReaderController::openingProgress, start, &android::StartScreen::setProgress);
    connect(&controller, &android::ReaderController::opened, this, [this] {
        auto *reader = controller.window();
        canvas->setContent(reader, true);
        stack->setCurrentWidget(canvas);
        sizeCanvas();
        reader->show();
        setUpdatesEnabled(true);
        setWindowTitle(QFileInfo(controller.path()).fileName() + " | Kobalt");
        start->setLastBook(controller.lastBook());
        reader->checkpoint();
        emit bookOpened();
        screenPicker->setEnabled(true);
    });
    connect(&controller, &android::ReaderController::failed, this, [this](const QString &error) {
        failed = true;
        start->showError(error);
        stack->setCurrentWidget(start);
        emit bookFailed(error);
    });
    connect(&controller, &android::ReaderController::closed, this, [this] {
        if (!failed)
            start->showReady();
        stack->setCurrentWidget(start);
        setWindowTitle("Kobalt");
        sizeCanvas();
        if (closing) {
            emit drained();
            close();
        } else
            screenPicker->setEnabled(true);
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive && controller.window())
            controller.window()->checkpoint();
    });
}
QSize Window::pixels() const {
    if (screenPreset == 1)
        return {1072, 1448};
    if (screenPreset == 2)
        return {1264, 1680};
    return {qRound(stack->width() * readerPixelsPerPoint), qRound(stack->height() * readerPixelsPerPoint)};
}
void Window::sizeCanvas() {
    auto bounds = centralWidget()->rect();
    if (screenPreset) {
        bounds.adjust(20, 20, -20, -20);
        const auto screen = deviceFrame->layoutScreen(screenPreset, bounds);
        deviceFrame->lower();
        stack->setGeometry(screen.translated(deviceFrame->pos()));
    } else {
        deviceFrame->hide();
        stack->setGeometry(bounds);
    }
    const bool reading = controller.window() && controller.window()->isEnabled();
    deviceFrame->previous->setEnabled(reading);
    deviceFrame->next->setEnabled(reading);
    if (auto *reader = controller.window())
        reader->resize(pixels());
    const qreal scale = screenPreset
                            ? qMin(qreal(stack->width()) / pixels().width(), qreal(stack->height()) / pixels().height())
                            : 1.0 / readerPixelsPerPoint;
    canvas->setTransform(QTransform::fromScale(scale, scale));
    canvas->setSceneRect(QRectF(QPointF(), pixels()));
}
void Window::selectScreen(int index) {
    if (closing || screenPreset == index)
        return;
    screenPreset = index;
    if (auto *reader = controller.window())
        reader->refreshHostPresentation();
    sizeCanvas();
}
void Window::chooseBook() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Open EPUB"), {}, tr("EPUB books (*.epub)"));
    if (!path.isEmpty())
        openBook(path);
}
void Window::openBook(const QString &path) {
    const auto absolute = QFileInfo(path).absoluteFilePath();
    if (closing || (controller.window() && controller.path() == absolute))
        return;
    failed = false;
    sizeCanvas();
    screenPicker->setEnabled(false);
    deviceFrame->previous->setEnabled(false);
    deviceFrame->next->setEnabled(false);
    start->showOpening();
    stack->setCurrentWidget(start);
    controller.open(absolute, pixels());
}
void Window::turnPage(int direction) {
    auto *reader = controller.window();
    if (!reader || !reader->isVisible() || !reader->isEnabled() || QApplication::activeModalWidget())
        return;
    if (screenPicker->hasFocus() || qobject_cast<QLineEdit *>(QApplication::focusWidget()))
        return;
    // Only turn from the reading surface. The same keys must remain available
    // to search fields, sliders, lists and dialogs while controls are open.
    for (auto *child : reader->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (child->isVisible() && child->objectName() != "rrPage" && child->objectName() != "rrHeader")
            return;
    auto *page = static_cast<ReadingPage *>(reader->findChild<QWidget *>("rrPage"));
    if (page && page->swiped)
        page->swiped(direction);
}
void Window::watchScreen(QScreen *screen) {
    disconnect(logicalDpiChanged);
    disconnect(physicalDpiChanged);
    if (screen) {
        logicalDpiChanged = connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] {
            resizeTimer.start();
        });
        physicalDpiChanged = connect(screen, &QScreen::physicalDotsPerInchChanged, this, [this] {
            resizeTimer.start();
        });
    }
    resizeTimer.start();
}
void Window::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (auto *handle = windowHandle()) {
        disconnect(screenChanged);
        screenChanged = connect(handle, &QWindow::screenChanged, this, &Window::watchScreen);
        watchScreen(handle->screen());
    }
}
void Window::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    resizeTimer.start();
}
bool Window::eventFilter(QObject *object, QEvent *event) {
    auto *reader = controller.window();
    if (reader && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        auto *widget = qobject_cast<QWidget *>(object);
        if (widget && (widget == reader || reader->isAncestorOf(widget)) && !repairPending) {
            repairPending = true;
            QTimer::singleShot(0, this, [this] {
                repairPending = false;
                auto *reader = controller.window();
                if (!reader)
                    return;
                // Qt's proxy can leave newly created children disabled after
                // a resize. Keep explicit disables made by the reader.
                for (auto *child : reader->findChildren<QWidget *>())
                    if (!child->updatesEnabled() && !child->testAttribute(Qt::WA_ForceUpdatesDisabled) &&
                        child->parentWidget() && child->parentWidget()->updatesEnabled())
                        child->setUpdatesEnabled(true);
            });
        }
    }
    return QMainWindow::eventFilter(object, event);
}
void Window::closeEvent(QCloseEvent *event) {
    if (controller.idle()) {
        event->accept();
        return;
    }
    event->ignore();
    if (closing)
        return;
    closing = true;
    controller.close();
}
void Window::dragEnterEvent(QDragEnterEvent *event) {
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile() &&
        QFileInfo(urls.first().toLocalFile()).suffix().compare("epub", Qt::CaseInsensitive) == 0)
        event->acceptProposedAction();
}
void Window::dropEvent(QDropEvent *event) {
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile()) {
        openBook(urls.first().toLocalFile());
        event->acceptProposedAction();
    }
}
}
