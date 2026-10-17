#include "window_presentation.h"
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QVector>
#include <QPainter>
#include <QScreen>
#include <utility>

namespace kobalt::nickel {
namespace {
class SleepFrame final : public QWidget {
public:
    SleepFrame(QWidget *parent, QImage image, Qt::ScreenOrientation orientation)
        : QWidget(parent), image(std::move(image)), orientation(orientation) {
        setObjectName("rrSleepFrame");
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(parent->rect());
        parent->installEventFilter(this);
    }
    void replaceImage(QImage next, const QRect &damage) {
        image = std::move(next);
        const auto *screen = QGuiApplication::primaryScreen();
        const int angle = screen ? screen->angleBetween(orientation, screen->orientation()) : 0;
        if (!angle) {
            update(damage.intersected(rect()));
        } else {
            update();
        }
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == parentWidget() && event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return false;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const auto *screen = QGuiApplication::primaryScreen();
        const int angle = screen ? screen->angleBetween(orientation, screen->orientation()) : 0;
        const QSize target = angle % 180 ? QSize(height(), width()) : size();
        painter.translate(width() / 2.0, height() / 2.0);
        painter.rotate(angle);
        painter.drawImage(QRectF(-target.width() / 2.0, -target.height() / 2.0, target.width(), target.height()),
                          image);
    }

private:
    // One opaque screen-sized image, released with the native view or on wake,
    // USB, reader closure or destruction. No file reads or timers during sleep.
    QImage image;
    Qt::ScreenOrientation orientation;
};

class Presentation final : public WindowPresentation {
public:
    Presentation(QWidget *reader, bool sleepScreenSupported) : reader(reader), sleepSupported(sleepScreenSupported) {}
    ~Presentation() override {
        clearSleepScreen();
        resumeBackground();
    }
    bool supportsSleepScreen() const override {
        return sleepSupported;
    }
    bool presentSleepScreen(QImage image, Qt::ScreenOrientation orientation) override {
        clearSleepScreen();
        if (!sleepSupported || image.isNull()) {
            return false;
        }
        QWidget *powerView = nullptr;
        for (auto *widget : QApplication::allWidgets()) {
            // Cover the native power view only. PIN and USB dialogs retain
            // their own windows and input, regardless of the sleep layout.
            if (widget->inherits("AbstractPowerView") && widget->isVisible() &&
                widget->window()->inherits("MainWindowView")) {
                if (powerView) {
                    return false;
                }
                powerView = widget;
            }
        }
        if (!powerView) {
            return false;
        }
        sleepFrame = new SleepFrame(powerView, std::move(image), orientation);
        sleepFrame->show();
        sleepFrame->raise();
        return true;
    }
    void clearSleepScreen() override {
        delete sleepFrame.data();
    }
    bool updateSleepScreen(QImage image, const QRect &damage) override {
        if (!sleepFrame || !sleepFrame->isVisible() || image.isNull()) {
            return false;
        }
        sleepFrame->replaceImage(std::move(image), damage);
        return true;
    }
    void pauseBackground(bool waking) override {
        if (!reader)
            return;
        for (auto *window : QApplication::topLevelWidgets()) {
            // During wake only pause the library/sleep backing store. Native
            // PIN and USB windows must remain able to draw and respond.
            if (waking && !window->inherits("MainWindowView"))
                continue;
            if (window != reader && !reader->isAncestorOf(window) && window->isVisible() && window->updatesEnabled()) {
                paused.append(window);
                window->setUpdatesEnabled(false);
            }
        }
    }
    void resumeBackground() override {
        for (const auto &window : paused)
            if (window)
                window->setUpdatesEnabled(true);
        paused.clear();
    }
    void resumeAndRepaintBackground() override {
        const auto windows = paused;
        resumeBackground();
        for (const auto &window : windows)
            if (window && window->isVisible()) {
                // Flush one full update before exposing the backing store.
                // repaint() can first flush a queued update, then draw it again.
                window->update();
                QCoreApplication::sendPostedEvents(window, QEvent::UpdateRequest);
            }
    }
    void acceptHandoff(QDialog &dialog, const std::function<void()> &accept) override {
        if (!dialog.parentWidget()) {
            accept();
            return;
        }
        // Nickel closes the dialog before installing its reader. Cover the
        // library's backing store until that synchronous continuation returns.
        QPointer<QWidget> background = dialog.parentWidget();
        QPointer<QLabel> cover = new QLabel(background);
        cover->setObjectName("rrHandoffCover");
        cover->setGeometry(background->rect());
        cover->setPixmap(dialog.grab());
        cover->show();
        cover->raise();
        cover->repaint();
        accept();
        delete cover.data();
        if (background && background->isVisible())
            background->repaint();
    }

private:
    QPointer<QWidget> reader;
    bool sleepSupported;
    QPointer<SleepFrame> sleepFrame;
    QVector<QPointer<QWidget>> paused;
};
}
std::unique_ptr<WindowPresentation> makeWindowPresentation(QWidget *reader, bool sleepScreenSupported) {
    return std::make_unique<Presentation>(reader, sleepScreenSupported);
}
}
