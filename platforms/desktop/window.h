#pragma once
#include "platforms/standalone/reader.h"
#include "library/library_store.h"
#include <QMainWindow>
#include <QTimer>

#include <functional>

class QStackedWidget;
class QComboBox;
class QScreen;
class QActionGroup;
class QAction;
class QLabel;
class QPushButton;

namespace kobalt::standalone {
class WidgetCanvas;
class StartScreen;
} // namespace kobalt::standalone

namespace kobalt::desktop {
class DeviceFrame;

class Window final : public QMainWindow
{
    Q_OBJECT
public:
    Window(storage::Paths paths, EngineFontInputs fonts, QString placeholder,
           std::function<QString()> clock = {},
           std::function<void(ReaderHostServices &)> configureServices = {});
    ~Window() override;
    void openBook(const QString &path);
    // Saves the visible preview without Mac chrome. Device surroundings remain transparent.
    // Returns the new PNG path, or an empty string and an error. Existing files are never replaced.
    QString saveScreenshot(const QString &directory, QString *error = nullptr);

    ReaderWindow *readerWindow() const { return controller.window(); }

    bool idle() const { return controller.idle(); }

signals:
    void bookOpened();
    void bookFailed(QString error);
    void drained();

protected:
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    const std::function<QString()> clockSource;
    // This controller and canvas contain no Android API calls. Reuse their
    // publication lifetime and physical-pixel rendering on the desktop.
    standalone::ReaderController controller;
    library::Store store;
    // The library entry of the book being read.
    QString openedKey;
    QStackedWidget *stack;
    DeviceFrame *deviceFrame;
    standalone::StartScreen *start;
    standalone::WidgetCanvas *canvas;
    QComboBox *screenPicker, *resolutionPicker, *textSizePicker;
    QActionGroup *screenActions, *resolutionActions, *textSizeActions;
    QAction *rotateAction;
    int screenPreset = 1, renderingDpi = 300, textSize = 100;
    bool touchCursorVisible = false, touchCursorPressed = false;
    void setTouchCursor(bool visible, bool pressed = false);
    QTimer resizeTimer;
    QMetaObject::Connection screenChanged, logicalDpiChanged, physicalDpiChanged;
    bool closing = false, repairPending = false, failed = false;
    bool phonePreview() const;
    // An e-reader preview can sleep, wake and power off as a Kobo does. The
    // reader gets the same calls as from the Kobo host, so sleep and
    // shutdown behavior can be checked on the Mac.
    QPushButton *sleepButton, *powerButton;
    QLabel *offScreen;
    bool deviceAsleep = false, poweredOff = false;
    bool ereaderPreview() const;
    void setDeviceAsleep(bool asleep);
    void setPoweredOff(bool off);
    void updateDeviceControls();
    QSize pixels() const;
    void sizeCanvas();
    void watchScreen(QScreen *screen);
    void selectScreen(int index);
    void setPreviewControlsEnabled(bool enabled);
    void chooseBook();
    void chooseFolder();
    void recordProgress();
    void turnPage(int direction, bool hardware = false);
};
} // namespace kobalt::desktop
