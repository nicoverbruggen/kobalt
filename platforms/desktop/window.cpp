#include "window.h"
#include "app/library/library_view.h"
#include "services.h"
#include "platforms/standalone/canvas.h"
#include "platforms/standalone/start_screen.h"
#include "platform/api/safe_area.h"
#include "app/reader/views/reading_page.h"
#include "reading/reading_density.h"
#include "ui/style/scale.h"
#include <QActionGroup>
#include <QComboBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QLabel>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QFile>
#include <lunasvg.h>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QToolBar>
#include <QToolTip>
#include <QPainter>
#include <QPainterPath>
#include <QTime>
#include <QScreen>
#include <QWindow>
#include <QShowEvent>
#include <QCursor>
#include <QMouseEvent>

static void initializeDesktopArtwork()
{
    Q_INIT_RESOURCE(desktop_artwork);
}

namespace kobalt::desktop {
// Two reader pixels per Mac point match the 300 PPI controls on a Retina
// display. Keep that scale on other displays so moving the window does not
// change the controls' apparent size.
constexpr qreal readerPixelsPerPoint = 2.0;

class ToolbarLogo final : public QWidget
{
public:
    explicit ToolbarLogo(QWidget *parent) : QWidget(parent)
    {
        setObjectName("desktopLogo");
        setAccessibleName(tr("Kobalt"));
        setFixedSize(28, 28);
        initializeDesktopArtwork();
        QFile artwork(":/desktop/kobalt-android.svg");
        if (artwork.open(QIODevice::ReadOnly))
            svg = lunasvg::Document::loadFromData(artwork.readAll().toStdString());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
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

class DeviceButton final : public QPushButton
{
public:
    explicit DeviceButton(QWidget *parent) : QPushButton(parent) { }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#111111"), 1));
        painter.setBrush(isDown() ? QColor("#444444") : QColor("#303030"));
        const qreal radius = qMax(3.0, qMin(width(), height()) * 0.25);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
};

// The phone preview has a camera in a band at the top of its portrait screen
// and rounded screen corners. In landscape the camera side stays outside the
// app, as Android letterboxes it; both sides keep that band so the page stays
// centred. Sizes are device pixels at 300 PPI.
constexpr int phoneCameraBand = 60, phoneCameraSize = 34, phoneCornerRadius = 80;

// The rounded screen corners and the camera, drawn over the screen. They do
// not take input, so taps reach the reader below them, as on a phone.
class PhoneScreenMask final : public QWidget
{
public:
    explicit PhoneScreenMask(QWidget *parent) : QWidget(parent)
    {
        setObjectName("desktopPhoneScreenMask");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

    void place(QRect screen, qreal scale, bool landscape)
    {
        this->scale = scale;
        this->landscape = landscape;
        setGeometry(screen);
        show();
        raise();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath corners;
        corners.addRect(rect());
        QPainterPath screen;
        const qreal radius = phoneCornerRadius * scale;
        screen.addRoundedRect(QRectF(rect()), radius, radius);
        painter.fillPath(corners.subtracted(screen), QColor("#242424"));
        const qreal camera = phoneCameraSize * scale, band = phoneCameraBand * scale;
        const QPointF centre =
                landscape ? QPointF(band / 2, height() / 2.0) : QPointF(width() / 2.0, band / 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawEllipse(centre, camera / 2, camera / 2);
    }

private:
    qreal scale = 1;
    bool landscape = false;
};

// Dimensions are in device pixels at 300 PPI. The asymmetric grip belongs
// outside the Libra screen, so its buttons never reduce the reading viewport.
class DeviceFrame final : public QWidget
{
public:
    explicit DeviceFrame(QWidget *parent) : QWidget(parent)
    {
        setObjectName("desktopDeviceFrame");
        mask = new PhoneScreenMask(parent);
        previous = new DeviceButton(this);
        next = new DeviceButton(this);
        previous->setObjectName("desktopDevicePrevious");
        next->setObjectName("desktopDeviceNext");
        previous->setAccessibleName(tr("Previous page"));
        next->setAccessibleName(tr("Next page"));
        previous->setToolTip(tr("Previous page"));
        next->setToolTip(tr("Next page"));
        for (auto *button : { previous, next })
            button->setFocusPolicy(Qt::NoFocus);
    }

    // Returns the reader's part of the screen. Only the phone in landscape
    // keeps part of its screen outside the reader.
    QRect layoutScreen(int preset, bool landscape, QRect bounds)
    {
        libra = preset == 2;
        phone = preset == 3;
        QSize body = libra ? QSize(1708, 1902) : phone ? QSize(800, 1680) : QSize(1323, 1890);
        QRect screen = libra ? QRect(90, 111, 1264, 1680)
                : phone      ? QRect(40, 40, 720, 1600)
                             : QRect(126, 150, 1072, 1448);
        // Long, narrow buttons like the Libra's, centred in the grip.
        QRect previousBounds(1473, 640, 64, 300), nextBounds(1473, 980, 64, 300);
        if (landscape) {
            // Turn the physical body clockwise; reader content stays upright.
            const auto rotated = [body](QRect rect) {
                return QRect(body.height() - rect.y() - rect.height(), rect.x(), rect.height(),
                             rect.width());
            };
            screen = rotated(screen);
            previousBounds = rotated(previousBounds);
            nextBounds = rotated(nextBounds);
            body.transpose();
        }
        const auto fitted = body.scaled(bounds.size(), Qt::KeepAspectRatio);
        setGeometry(
                QRect(bounds.center() - QPoint(fitted.width() / 2, fitted.height() / 2), fitted));
        const qreal scale = qMin(qreal(width()) / body.width(), qreal(height()) / body.height());
        auto scaled = [scale](QRect rect) {
            return QRect(qRound(rect.x() * scale), qRound(rect.y() * scale),
                         qRound(rect.width() * scale), qRound(rect.height() * scale));
        };
        screenBounds = scaled(screen);
        screenCornerRadius = phone ? phoneCornerRadius * scale : 0;
        previous->setGeometry(scaled(previousBounds));
        next->setGeometry(scaled(nextBounds));
        for (auto *button : { previous, next })
            button->setVisible(libra);
        show();
        update();
        if (!phone) {
            mask->hide();
            return screenBounds;
        }
        mask->place(screenBounds.translated(pos()), scale, landscape);
        const int band = landscape ? qRound(phoneCameraBand * scale) : 0;
        return screenBounds.adjusted(band, 0, -band, 0);
    }

    void hideScreen()
    {
        hide();
        mask->hide();
    }

    // Keeps the corners and the camera above the reader after it moves.
    void raiseMask()
    {
        if (mask->isVisible())
            mask->raise();
    }

    QPushButton *previous, *next;
    PhoneScreenMask *mask;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::black, 1.5));
        painter.setBrush(QColor("#242424"));
        const qreal radius = qMax(5.0, qMin(width(), height()) * (phone ? 0.11 : 0.025));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius, radius);
        painter.setBrush(phone ? QBrush(Qt::black) : QBrush(Qt::NoBrush));
        painter.setPen(QPen(Qt::black, 2));
        const auto border = QRectF(screenBounds).adjusted(-1, -1, 1, 1);
        if (phone)
            painter.drawRoundedRect(border, screenCornerRadius + 1, screenCornerRadius + 1);
        else
            painter.drawRect(border);
    }

private:
    bool libra = false, phone = false;
    QRect screenBounds;
    qreal screenCornerRadius = 0;
};

Window::Window(storage::Paths paths, EngineFontInputs fonts, QString placeholder,
               std::function<QString()> clock,
               std::function<void(ReaderHostServices &)> configureServices)
    : clockSource(std::move(clock)),
      controller(
              paths, std::move(fonts), std::move(placeholder),
              [this, configureServices = std::move(configureServices)] {
                  auto services = readerHostServices();
                  services.nativeDialogParent = [this] { return this; };
                  services.devicePreview = [this] { return screenPreset != 0; };
                  // An emulated sleep or power-off suspends the reader as on Kobo.
                  services.suspended = [this, host = services.suspended] {
                      return deviceAsleep || poweredOff || (host && host());
                  };
                  services.pageButtons = [this] { return screenPreset == 2; };
                  services.clock = [this] {
                      return clockSource     ? clockSource()
                              : screenPreset ? QStringLiteral("09:00")
                                             : QTime::currentTime().toString("HH:mm");
                  };
                  services.battery = [this] {
                      return screenPreset ? BatteryStatus{ 100, false } : BatteryStatus{};
                  };
                  services.textScale = [this] { return textSize / 100.0; };
                  // Only the portrait phone has a camera band inside the reader.
                  services.safeArea = [this](QSize size) {
                      if (!phonePreview() || size.width() > size.height())
                          return SafeArea{};
                      return SafeArea{ phoneCameraBand, 0, phoneCornerRadius };
                  };
                  if (configureServices)
                      configureServices(services);
                  return services;
              },
              [](Qt::ScreenOrientation) {}),
      store(paths.directory, std::make_shared<library::LocalSource>())
{
    setWindowTitle(QStringLiteral(""));
    setMinimumSize(420, 500);
    resize(800, 1136);
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
    // Covers the screen while the emulated device sleeps or is off, and
    // takes the taps a sleeping device would ignore.
    offScreen = new QLabel(stage);
    offScreen->setObjectName("desktopOffScreen");
    offScreen->setAlignment(Qt::AlignCenter);
    offScreen->setAutoFillBackground(true);
    offScreen->setPalette(palette);
    offScreen->hide();
    connect(deviceFrame->previous, &QPushButton::clicked, this, [this] { turnPage(-1, true); });
    connect(deviceFrame->next, &QPushButton::clicked, this, [this] { turnPage(1, true); });
    auto *toolbar = addToolBar(tr("Preview Toolbar"));
    toolbar->setObjectName("desktopPreviewToolbar");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    screenPicker = new QComboBox(toolbar);
    screenPicker->setObjectName("desktopScreenPicker");
    screenPicker->setAccessibleName(tr("Reader screen"));
    screenPicker->addItems({ tr("Fill window"), tr("6-inch e-reader"),
                             tr("7-inch e-reader with buttons"), tr("5.8-inch smartphone") });
    screenPicker->setCurrentIndex(screenPreset);
    screenPicker->setMinimumWidth(100);
    screenPicker->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    resolutionPicker = new QComboBox(toolbar);
    resolutionPicker->setObjectName("desktopResolutionPicker");
    resolutionPicker->setAccessibleName(tr("Rendering resolution"));
    resolutionPicker->setToolTip(tr(
            "Change preview sharpness without changing the book layout. Native uses the display resolution."));
    resolutionPicker->addItem(tr("212 DPI"), 212);
    resolutionPicker->addItem(tr("300 DPI"), 300);
    resolutionPicker->addItem(tr("Native"), 0);
    resolutionPicker->setCurrentIndex(resolutionPicker->findData(renderingDpi));
    // Kobo's Large Print and Android's font size set the same scale.
    textSizePicker = new QComboBox(toolbar);
    textSizePicker->setObjectName("desktopTextSizePicker");
    textSizePicker->setAccessibleName(tr("Text size"));
    textSizePicker->setToolTip(
            tr("Enlarge interface text, as the system text size does on Kobo and Android. "
               "Applies to the Library at once and to a book when it opens."));
    for (const int percent : { 100, 125, 150 })
        textSizePicker->addItem(tr("%1%").arg(percent), percent);
    textSizePicker->setCurrentIndex(textSizePicker->findData(textSize));
    auto *contents = new QWidget(toolbar);
    contents->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QGridLayout(contents);
    layout->setContentsMargins(12, 4, 12, 4);
    auto *logo = new ToolbarLogo(contents);
    auto *version = new QLabel(QStringLiteral(KOBALT_VERSION), contents);
    version->setObjectName("desktopVersion");
    // Equal side columns keep both pickers centered.
    const int sideWidth = qMax(logo->width(), version->sizeHint().width());
    layout->setColumnMinimumWidth(0, sideWidth);
    layout->setColumnMinimumWidth(2, sideWidth);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(2, 1);
    layout->addWidget(logo, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
    auto *pickers = new QWidget(contents);
    pickers->setObjectName("desktopPreviewPickers");
    auto *choices = new QHBoxLayout(pickers);
    choices->setContentsMargins(0, 0, 0, 0);
    choices->setSpacing(8);
    choices->addWidget(screenPicker, 1);
    choices->addWidget(resolutionPicker);
    choices->addWidget(textSizePicker);
    rotateAction = new QAction(tr("Rotate Device"), this);
    rotateAction->setObjectName("desktopRotateDevice");
    rotateAction->setCheckable(true);
    rotateAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    rotateAction->setToolTip(tr("Switch the e-reader preview between portrait and landscape."));
    auto *rotate = new QPushButton(tr("Rotate"), pickers);
    rotate->setObjectName("desktopRotateButton");
    rotate->setCheckable(true);
    rotate->setFocusPolicy(Qt::NoFocus);
    rotate->setToolTip(rotateAction->toolTip());
    choices->addWidget(rotate);
    connect(rotate, &QPushButton::clicked, rotateAction, &QAction::trigger);
    auto *screenshot = new QPushButton(tr("Screenshot"), pickers);
    screenshot->setObjectName("desktopScreenshot");
    screenshot->setToolTip(tr("Save the current preview to the Desktop as a PNG."));
    screenshot->setFocusPolicy(Qt::NoFocus);
    choices->addWidget(screenshot);
    connect(screenshot, &QPushButton::clicked, this, [this, screenshot] {
        QString error;
        const auto path = saveScreenshot(
                QStandardPaths::writableLocation(QStandardPaths::DesktopLocation), &error);
        if (path.isEmpty()) {
            QMessageBox::warning(this, tr("Screenshot could not be saved"), error);
        } else {
            QToolTip::showText(screenshot->mapToGlobal(screenshot->rect().bottomLeft()),
                               tr("Saved to %1").arg(QDir::toNativeSeparators(path)), screenshot);
        }
    });
    sleepButton = new QPushButton(tr("Sleep"), pickers);
    sleepButton->setObjectName("desktopSleep");
    sleepButton->setCheckable(true);
    sleepButton->setFocusPolicy(Qt::NoFocus);
    sleepButton->setToolTip(tr(
            "Put the e-reader preview to sleep, or wake it, as the power button does on a Kobo."));
    choices->addWidget(sleepButton);
    connect(sleepButton, &QPushButton::clicked, this,
            [this](bool checked) { setDeviceAsleep(checked); });
    powerButton = new QPushButton(tr("Power off"), pickers);
    powerButton->setObjectName("desktopPowerOff");
    powerButton->setCheckable(true);
    powerButton->setFocusPolicy(Qt::NoFocus);
    powerButton->setToolTip(
            tr("Power the e-reader preview off, or on again. The book closes as on a Kobo."));
    choices->addWidget(powerButton);
    connect(powerButton, &QPushButton::clicked, this,
            [this](bool checked) { setPoweredOff(checked); });
    layout->addWidget(pickers, 0, 1);
    layout->addWidget(version, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
    toolbar->addWidget(contents);
    layout->activate();
    const int toolbarInset = qMax(0, toolbar->sizeHint().width() - contents->sizeHint().width());
    setMinimumWidth(qMax(minimumWidth(), contents->minimumSizeHint().width() + toolbarInset));
    connect(screenPicker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &Window::selectScreen);
    connect(resolutionPicker, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        renderingDpi = resolutionPicker->currentData().toInt();
        sizeCanvas();
    });
    start = new standalone::StartScreen(readerHostServices().presentation, store,
                                        [this] { return textSize / 100.0; });
    start->setKeyboardFactory(readerHostServices().keyboard);
    connect(textSizePicker, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        textSize = textSizePicker->currentData().toInt();
        // An open book keeps its size until it closes, and the Library takes
        // the new size when it is shown again.
        if (!controller.window())
            start->applyTextSize();
    });
    start->setDark(controller.preferences().value("dark_mode").toBool(false));
    connect(start, &standalone::StartScreen::bookRequested, this, &Window::openBook);
    connect(start, &standalone::StartScreen::libraryOpenRequested, this, &Window::chooseBook);
    connect(start, &standalone::StartScreen::markUnreadRequested, this, [this](const QString &key) {
        if (controller.resetBook(key))
            store.setMark(key, library::Book::Mark::Unread);
        else
            start->showLibraryError(
                    tr("The book's reading settings and position could not be reset."));
    });
    connect(start, &standalone::StartScreen::addFolderRequested, this, &Window::chooseFolder);
    connect(start, &standalone::StartScreen::removeFolderRequested, this,
            [this](const QString &key) { store.removeFolder(key); });
    // The desktop never deletes a book file; it only leaves the library.
    connect(start, &standalone::StartScreen::removeBookRequested, this,
            [this](const QString &key) { store.removeBook(key); });
    if (store.index().settings.checkAtStart)
        QTimer::singleShot(0, this, [this] { store.scan(); });
    canvas = new standalone::WidgetCanvas(stack);
    canvas->setMouseTracking(true);
    canvas->viewport()->setMouseTracking(true);
    stack->addWidget(start);
    stack->addWidget(canvas);
    connect(start, &standalone::StartScreen::exitRequested, this, &QWidget::close);
    auto *file = menuBar()->addMenu(tr("File"));
    auto *open = file->addAction(tr("Open EPUB…"), this, &Window::chooseBook);
    open->setShortcut(QKeySequence::Open);
    auto *folder = file->addAction(tr("Add Library Folder…"), this, &Window::chooseFolder);
    folder->setObjectName("desktopAddFolder");
    auto *quit = file->addAction(tr("Quit Kobalt"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    auto *view = menuBar()->addMenu(tr("&View"));
    view->setObjectName("desktopViewMenu");
    const auto choicesMenu = [view](QComboBox *picker, const QString &title, const QString &name) {
        auto *menu = view->addMenu(title);
        menu->setObjectName(name);
        auto *group = new QActionGroup(menu);
        group->setExclusive(true);
        for (int index = 0; index < picker->count(); ++index) {
            auto *action = menu->addAction(picker->itemText(index));
            action->setCheckable(true);
            action->setChecked(index == picker->currentIndex());
            action->setData(index);
            group->addAction(action);
            connect(action, &QAction::triggered, picker,
                    [picker, index] { picker->setCurrentIndex(index); });
        }
        connect(picker, qOverload<int>(&QComboBox::currentIndexChanged), group, [group](int index) {
            for (auto *action : group->actions())
                action->setChecked(action->data().toInt() == index);
        });
        return group;
    };
    screenActions = choicesMenu(screenPicker, tr("Device"), "desktopDeviceMenu");
    resolutionActions =
            choicesMenu(resolutionPicker, tr("Rendering Resolution"), "desktopResolutionMenu");
    textSizeActions = choicesMenu(textSizePicker, tr("Text Size"), "desktopTextSizeMenu");
    view->addAction(rotateAction);
    connect(rotateAction, &QAction::toggled, this, [this, rotate](bool landscape) {
        rotate->setChecked(landscape);
        sizeCanvas();
    });
    connect(rotateAction, &QAction::changed, rotate,
            [this, rotate] { rotate->setEnabled(rotateAction->isEnabled()); });
    view->addSeparator();
    auto *showToolbar = toolbar->toggleViewAction();
    showToolbar->setObjectName("desktopShowToolbar");
    showToolbar->setText(tr("Show Toolbar"));
    showToolbar->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+T")));
    view->addAction(showToolbar);
    for (const auto key : { Qt::Key_Right, Qt::Key_PageDown, Qt::Key_Left, Qt::Key_PageUp }) {
        auto *shortcut = new QShortcut(QKeySequence(key), this);
        shortcut->setAutoRepeat(false);
        connect(shortcut, &QShortcut::activated, this, [this, key] {
            turnPage(key == Qt::Key_Right || key == Qt::Key_PageDown ? 1 : -1,
                     key == Qt::Key_PageUp || key == Qt::Key_PageDown);
        });
    }
    resizeTimer.setSingleShot(true);
    resizeTimer.setInterval(100);
    connect(toolbar, &QToolBar::visibilityChanged, this, [this] { resizeTimer.start(); });
    canvas->viewportResized = [this] {
        sizeCanvas();
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
    connect(&controller, &standalone::ReaderController::openingProgress, start,
            &standalone::StartScreen::setProgress);
    connect(&controller, &standalone::ReaderController::openingCover, start,
            &standalone::StartScreen::setCover);
    connect(&controller, &standalone::ReaderController::opened, this, [this] {
        if (!openedKey.isEmpty())
            store.setOpened(openedKey);
        auto *reader = controller.window();
        canvas->setContent(reader, renderingDpi != 0, renderingDpi ? renderingDpi / 300.0 : 1.0);
        stack->setCurrentWidget(canvas);
        sizeCanvas();
        reader->show();
        setUpdatesEnabled(true);
        reader->checkpoint();
        emit bookOpened();
        setPreviewControlsEnabled(true);
    });
    connect(&controller, &standalone::ReaderController::failed, this, [this](const QString &error) {
        failed = true;
        start->showLibraryError(error);
        stack->setCurrentWidget(start);
        emit bookFailed(error);
    });
    connect(&controller, &standalone::ReaderController::closed, this, [this] {
        recordProgress();
        start->setDark(controller.preferences().value("dark_mode").toBool(false));
        if (!failed)
            start->showLibrary();
        stack->setCurrentWidget(start);
        sizeCanvas();
        if (closing) {
            emit drained();
            close();
        } else {
            setPreviewControlsEnabled(true);
        }
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive && controller.window())
                    controller.window()->checkpoint();
            });
}

QString Window::saveScreenshot(const QString &directory, QString *error)
{
    if (error) {
        error->clear();
    }
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return QString();
    };
    if (directory.isEmpty()) {
        return fail(tr("The Desktop folder is unavailable."));
    }
    // One temporary image at the window's display resolution; no screenshot cache is retained.
    auto *stage = centralWidget();
    const qreal dpr = stage->devicePixelRatioF();
    QImage image(QSize(qRound(stage->width() * dpr), qRound(stage->height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return fail(tr("There is not enough memory to capture the current preview."));
    }
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        if (screenPreset) {
            // Draw the frame and reader separately, omitting only the stage's white paper.
            // Keep antialiased frame corners transparent and the reader's own white pixels opaque.
            deviceFrame->render(&painter, deviceFrame->pos(), {}, QWidget::DrawChildren);
            stack->render(&painter, stack->pos());
            if (deviceFrame->mask->isVisible())
                deviceFrame->mask->render(&painter, deviceFrame->mask->pos());
        } else {
            stage->render(&painter);
        }
    }
    const auto name = tr("Kobalt Screenshot %1 XXXXXX.png")
                              .arg(QDateTime::currentDateTime().toString(
                                      QStringLiteral("yyyy-MM-dd HH.mm.ss")));
    QTemporaryFile file(QDir(directory).filePath(name));
    if (!file.open()) {
        return fail(tr("Could not create a screenshot in %1: %2")
                            .arg(QDir::toNativeSeparators(directory), file.errorString()));
    }
    if (!image.save(&file, "PNG") || !file.flush()) {
        return fail(tr("Could not write the PNG screenshot: %1").arg(file.errorString()));
    }
    file.setAutoRemove(false);
    return file.fileName();
}

bool Window::phonePreview() const
{
    return screenPreset == 3;
}

bool Window::ereaderPreview() const
{
    return screenPreset != 0 && !phonePreview();
}

void Window::setDeviceAsleep(bool asleep)
{
    auto *reader = controller.window();
    if (asleep == deviceAsleep || poweredOff || (asleep && (!reader || !ereaderPreview()))) {
        updateDeviceControls();
        return;
    }
    if (asleep) {
        // The same order as the Kobo host: the reader prepares for sleep,
        // then hands the screen to the sleep view.
        deviceAsleep = true;
        reader->sleepReader();
        reader->sleepViewReady();
        offScreen->setText(tr("Asleep"));
        offScreen->show();
        offScreen->raise();
    } else {
        offScreen->hide();
        if (reader)
            reader->prepareWake();
        deviceAsleep = false;
        if (reader)
            reader->wakeReader();
        sizeCanvas();
    }
    setPreviewControlsEnabled(!deviceAsleep);
}

void Window::setPoweredOff(bool off)
{
    if (off == poweredOff || deviceAsleep || (off && !ereaderPreview())) {
        updateDeviceControls();
        return;
    }
    poweredOff = off;
    if (off) {
        offScreen->setText(tr("Powered off"));
        offScreen->show();
        offScreen->raise();
        // A Kobo power-off asks the reader to finish its saves, the same
        // call the controller makes when it closes the book.
        controller.close();
    } else {
        offScreen->hide();
    }
    setPreviewControlsEnabled(!off);
}

void Window::updateDeviceControls()
{
    const bool ereader = ereaderPreview();
    sleepButton->setVisible(ereader);
    powerButton->setVisible(ereader);
    sleepButton->setEnabled(ereader && !poweredOff && (deviceAsleep || controller.window()));
    powerButton->setEnabled(ereader && !deviceAsleep
                            && (poweredOff || controller.idle() || controller.window()));
    sleepButton->setChecked(deviceAsleep);
    sleepButton->setText(deviceAsleep ? tr("Wake") : tr("Sleep"));
    powerButton->setChecked(poweredOff);
    powerButton->setText(poweredOff ? tr("Power on") : tr("Power off"));
}

QSize Window::pixels() const
{
    if (screenPreset) {
        QSize size = screenPreset == 2 ? QSize(1264, 1680)
                : phonePreview()       ? QSize(720, 1600)
                                       : QSize(1072, 1448);
        if (rotateAction->isChecked()) {
            size.transpose();
            if (phonePreview())
                size.rwidth() -= 2 * phoneCameraBand;
        }
        return size;
    }
    return { qRound(stack->width() * readerPixelsPerPoint),
             qRound(stack->height() * readerPixelsPerPoint) };
}

void Window::sizeCanvas()
{
    auto bounds = centralWidget()->rect();
    if (screenPreset) {
        bounds.adjust(20, 20, -20, -20);
        const auto screen =
                deviceFrame->layoutScreen(screenPreset, rotateAction->isChecked(), bounds);
        deviceFrame->lower();
        stack->setGeometry(screen.translated(deviceFrame->pos()));
        deviceFrame->raiseMask();
    } else {
        deviceFrame->hideScreen();
        stack->setGeometry(bounds);
    }
    const bool paging = (controller.window() && controller.window()->isEnabled())
            || (start->isVisible() && start->libraryShown());
    deviceFrame->previous->setEnabled(paging);
    deviceFrame->next->setEnabled(paging);
    if (auto *reader = controller.window())
        reader->resize(pixels());
    const qreal scale = screenPreset ? qMin(qreal(stack->width()) / pixels().width(),
                                            qreal(stack->height()) / pixels().height())
                                     : 1.0 / readerPixelsPerPoint;
    canvas->setRasterScale(renderingDpi ? renderingDpi / 300.0 : 1.0);
    canvas->setNativePixels(renderingDpi != 0);
    canvas->setTransform(QTransform::fromScale(scale, scale));
    canvas->setSceneRect(QRectF(QPointF(), pixels()));
    offScreen->setGeometry(stack->geometry());
    // The library uses the reader's controls, so it gets the reader's size and scale.
    // Android shows its status bar over the Library, so the phone's camera
    // band stays outside it.
    const bool cameraBand = phonePreview() && pixels().height() > pixels().width();
    start->setLibraryScale(pixels(), scale, renderingDpi != 0,
                           renderingDpi ? renderingDpi / 300.0 : 1.0,
                           cameraBand ? phoneCameraBand : 0);
}

void Window::selectScreen(int index)
{
    if (closing || screenPreset == index)
        return;
    setTouchCursor(false);
    screenPreset = index;
    setTouchCursor(screenPreset && underMouse(), QApplication::mouseButtons() & Qt::LeftButton);
    rotateAction->setEnabled(screenPicker->isEnabled() && screenPreset != 0);
    sizeCanvas();
    updateDeviceControls();
    if (auto *reader = controller.window())
        reader->refreshHostPresentation();
}

void Window::setPreviewControlsEnabled(bool enabled)
{
    enabled = enabled && !deviceAsleep && !poweredOff;
    updateDeviceControls();
    screenPicker->setEnabled(enabled);
    resolutionPicker->setEnabled(enabled);
    textSizePicker->setEnabled(enabled);
    rotateAction->setEnabled(enabled && screenPreset != 0);
    screenActions->setEnabled(enabled);
    resolutionActions->setEnabled(enabled);
    textSizeActions->setEnabled(enabled);
}

void Window::chooseBook()
{
    const auto path =
            QFileDialog::getOpenFileName(this, tr("Open EPUB"), {}, tr("EPUB books (*.epub)"));
    if (!path.isEmpty())
        openBook(path);
}

void Window::chooseFolder()
{
    const auto path = QFileDialog::getExistingDirectory(this, tr("Add Library Folder"));
    if (path.isEmpty())
        return;
    const QFileInfo folder(path);
    store.addFolder(
            { folder.absoluteFilePath(),
              folder.fileName().isEmpty() ? folder.absoluteFilePath() : folder.fileName() });
}

void Window::recordProgress()
{
    if (openedKey.isEmpty())
        return;
    const auto progress = library::savedProgress(controller.savedState(openedKey));
    store.setProgress(std::exchange(openedKey, {}), progress.percent, progress.finished);
}

void Window::openBook(const QString &path)
{
    const auto absolute = QFileInfo(path).absoluteFilePath();
    if (closing || (controller.window() && controller.path() == absolute))
        return;
    recordProgress();
    const bool restart = store.index().books.value(absolute).status() == library::Status::Finished;
    // Every entry point, including Finder and drag-and-drop, joins the library.
    if (store.index().books.contains(absolute) || store.addBook(absolute)) {
        openedKey = absolute;
    }
    failed = false;
    sizeCanvas();
    setPreviewControlsEnabled(false);
    deviceFrame->previous->setEnabled(false);
    deviceFrame->next->setEnabled(false);
    start->showOpening(absolute);
    stack->setCurrentWidget(start);
    controller.open(absolute, pixels(), {}, restart);
}

void Window::turnPage(int direction, bool hardware)
{
    auto *reader = controller.window();
    if (closing || QApplication::activeModalWidget())
        return;
    if (!hardware
        && (screenPicker->hasFocus() || resolutionPicker->hasFocus()
            || qobject_cast<QLineEdit *>(QApplication::focusWidget())))
        return;
    if (!reader) {
        if (start->isVisible() && start->libraryShown())
            start->library()->pageButton(direction);
        return;
    }
    if (!reader->isVisible() || !reader->isEnabled())
        return;
    if (hardware) {
        reader->pageButton(direction);
        return;
    }
    // Only turn from the reading surface. The same keys must remain available
    // to search fields, sliders, lists and dialogs while controls are open.
    for (auto *child : reader->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (child->isVisible() && child->objectName() != "rrPage"
            && child->objectName() != "rrHeader")
            return;
    auto *page = static_cast<ReadingPage *>(reader->findChild<QWidget *>("rrPage"));
    if (page && page->swiped)
        page->swiped(direction);
}

void Window::watchScreen(QScreen *screen)
{
    disconnect(logicalDpiChanged);
    disconnect(physicalDpiChanged);
    if (screen) {
        logicalDpiChanged = connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                                    [this] { resizeTimer.start(); });
        physicalDpiChanged = connect(screen, &QScreen::physicalDotsPerInchChanged, this,
                                     [this] { resizeTimer.start(); });
    }
    resizeTimer.start();
}

void Window::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (auto *handle = windowHandle()) {
        disconnect(screenChanged);
        screenChanged = connect(handle, &QWindow::screenChanged, this, &Window::watchScreen);
        watchScreen(handle->screen());
    }
}

void Window::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (isVisible())
        sizeCanvas();
    resizeTimer.start();
}

Window::~Window()
{
    qApp->removeEventFilter(this);
    setTouchCursor(false);
}

void Window::setTouchCursor(bool visible, bool pressed)
{
    if (!visible) {
        if (!touchCursorVisible)
            return;
        QApplication::restoreOverrideCursor();
        touchCursorVisible = false;
        return;
    }
    if (touchCursorVisible && touchCursorPressed == pressed)
        return;
    touchCursorPressed = pressed;
    const qreal ratio = devicePixelRatioF();
    QPixmap pixels(qRound(28 * ratio), qRound(28 * ratio));
    pixels.setDevicePixelRatio(ratio);
    pixels.fill(Qt::transparent);
    {
        QPainter painter(&pixels);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, pressed ? 210 : 128));
        painter.drawEllipse(QRectF(2, 2, 24, 24));
    }
    const QCursor cursor(pixels, 14, 14);
    if (touchCursorVisible)
        QApplication::changeOverrideCursor(cursor);
    else
        QApplication::setOverrideCursor(cursor);
    touchCursorVisible = true;
}

bool Window::eventFilter(QObject *object, QEvent *event)
{
    const auto type = event->type();
    if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseMove
        || type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
        || (type == QEvent::Hide && object == this)) {
        auto *widget = qobject_cast<QWidget *>(object);
        if (widget && widget->window() == this) {
            const auto *mouse = dynamic_cast<QMouseEvent *>(event);
            const QPoint global = mouse ? mouse->globalPosition().toPoint() : QCursor::pos();
            const bool leaving = object == this && (type == QEvent::Leave || type == QEvent::Hide);
            setTouchCursor(screenPreset && isVisible() && !leaving
                                   && rect().contains(mapFromGlobal(global)),
                           mouse ? bool(mouse->buttons() & Qt::LeftButton)
                                 : bool(QApplication::mouseButtons() & Qt::LeftButton));
        }
    }
    if (object == this && event->type() == QEvent::WindowDeactivate)
        setTouchCursor(false);
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
                    if (!child->updatesEnabled()
                        && !child->testAttribute(Qt::WA_ForceUpdatesDisabled)
                        && child->parentWidget() && child->parentWidget()->updatesEnabled())
                        child->setUpdatesEnabled(true);
            });
        }
    }
    return QMainWindow::eventFilter(object, event);
}

void Window::closeEvent(QCloseEvent *event)
{
    setTouchCursor(false);
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

void Window::dragEnterEvent(QDragEnterEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile()
        && QFileInfo(urls.first().toLocalFile()).suffix().compare("epub", Qt::CaseInsensitive) == 0)
        event->acceptProposedAction();
}

void Window::dropEvent(QDropEvent *event)
{
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile()) {
        openBook(urls.first().toLocalFile());
        event->acceptProposedAction();
    }
}
} // namespace kobalt::desktop
