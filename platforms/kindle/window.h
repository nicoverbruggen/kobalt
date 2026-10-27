#pragma once
#include "platforms/android/reader.h"
#include "library/library_store.h"
#include <QWidget>

class QStackedWidget;
class QPushButton;
class QLabel;
class QLineEdit;
namespace kobalt::android {
class StartScreen;
class WidgetCanvas;
}
namespace kobalt::kindle {
class Device;
class Window final : public QWidget {
    Q_OBJECT
public:
    Window(storage::Paths paths, EngineFontInputs fonts, QString placeholder, QString libraryRoot);
    ~Window() override;
    void openBook(const QString &path);
    void closeBook();
    ReaderWindow *readerWindow() const { return controller.window(); }
    QJsonObject savedState(const QString &path) const { return controller.savedState(path); }
    bool idle() const { return controller.idle(); }
    bool deviceFailed() const { return deviceFailure; }
    bool libraryContains(const QString &path) const;
    void setPreview(bool enabled);
    void startDeviceLifecycle();
#ifdef KOBALT_KINDLE_NATIVE_TESTING
    void simulatePowerEvent(const QByteArray &event);
#endif
signals:
    void bookOpened();
    void bookClosed();
    void bookFailed(QString error);
protected:
    void resizeEvent(QResizeEvent *) override;
    void closeEvent(QCloseEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
private:
    std::shared_ptr<Device> device;
    android::ReaderController controller;
    library::Store store;
    QString openedKey, libraryRoot, pendingOpen;
    QStackedWidget *stack;
    android::StartScreen *start;
    android::WidgetCanvas *canvas;
    QPushButton *exit;
    QLabel *status, *sleepView, *retirementView;
    QTimer standby;
    std::unique_ptr<KeyboardController> libraryKeyboard;
    QPointer<QLineEdit> libraryQuery;
    bool closing = false, failed = false;
    bool deviceFailure = false;
    bool exitFramePending = false, exitFrameReady = false;
    bool preview = false;
    QSize readerPixels() const;
    void sizeCanvas();
    void recordProgress();
    void sleepingChanged(bool sleeping);
    void updateSleepView();
};
}
