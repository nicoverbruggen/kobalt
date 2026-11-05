#include "window_presentation.h"
#include "native_dialog_overlay.h"
#include "native_font_scale.h"
#include "support/qt_compat.h"
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QVector>
#include <QPainter>
#include <QScreen>
#include <utility>
#include <algorithm>
#include <vector>

namespace kobalt::nickel {
namespace {
bool nativeDialog(const QWidget *widget)
{
    return qobject_cast<const QDialog *>(widget) || widget->inherits("ConfirmationDialog")
            || widget->inherits("ProgressDialog")
            || widget->objectName() == QStringLiteral("N3Dialog");
}

class SleepFrame final : public QWidget
{
public:
    SleepFrame(QWidget *parent, QImage image, Qt::ScreenOrientation orientation)
        : QWidget(parent), image(std::move(image)), orientation(orientation)
    {
        setObjectName("rrSleepFrame");
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(parent->rect());
        parent->installEventFilter(this);
    }

    void replaceImage(QImage next, const QRect &damage)
    {
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
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == parentWidget() && event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto *screen = QGuiApplication::primaryScreen();
        const int angle = screen ? screen->angleBetween(orientation, screen->orientation()) : 0;
        const QSize target = angle % 180 ? QSize(height(), width()) : size();
        painter.translate(width() / 2.0, height() / 2.0);
        painter.rotate(angle);
        painter.drawImage(QRectF(-target.width() / 2.0, -target.height() / 2.0, target.width(),
                                 target.height()),
                          image);
    }

private:
    // One opaque screen-sized image, released with the native view or on wake,
    // USB, reader closure or destruction. No file reads or timers during sleep.
    QImage image;
    Qt::ScreenOrientation orientation;
};

class Presentation final : public QObject, public WindowPresentation
{
public:
    Presentation(QWidget *reader, bool sleepScreenSupported,
                 std::function<void(const sleep::Snapshot &)> save)
        : save(std::move(save)), reader(reader), sleepSupported(sleepScreenSupported)
    {
        qApp->installEventFilter(this);
    }

    void saveSleepScreen(const sleep::Snapshot &snapshot) override
    {
        if (save)
            save(snapshot);
    }

    ~Presentation() override
    {
        dialogs.clear();
        clearSleepScreen();
        resumeBackground();
    }

    bool supportsSleepScreen() const override { return sleepSupported; }

    bool presentSleepScreen(QImage image, Qt::ScreenOrientation orientation) override
    {
        clearSleepScreen();
        if (!sleepSupported || image.isNull()) {
            return false;
        }
        QWidget *powerView = nullptr;
        for (auto *widget : QApplication::allWidgets()) {
            // Cover the native power view only. PIN and USB dialogs retain
            // their own windows and input, regardless of the sleep layout.
            if (widget->inherits("AbstractPowerView") && widget->isVisible()
                && widget->window()->inherits("MainWindowView")) {
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

    void clearSleepScreen() override { delete sleepFrame.data(); }

    bool updateSleepScreen(QImage image, const QRect &damage) override
    {
        if (!sleepFrame || !sleepFrame->isVisible() || image.isNull()) {
            return false;
        }
        sleepFrame->replaceImage(std::move(image), damage);
        return true;
    }

    void pauseBackground(bool waking) override
    {
        if (!reader)
            return;
        for (auto *window : QApplication::topLevelWidgets()) {
            // During wake only pause the library/sleep backing store. Native
            // PIN and USB windows must remain able to draw and respond.
            if (waking && !window->inherits("MainWindowView"))
                continue;
            if (window != reader && !reader->isAncestorOf(window) && window->isVisible()
                && window->updatesEnabled()) {
                paused.append(window);
                window->setUpdatesEnabled(false);
            }
        }
    }

    void resumeBackground() override
    {
        for (const auto &window : paused)
            if (window)
                window->setUpdatesEnabled(true);
        paused.clear();
    }

    void resumeAndRepaintBackground() override
    {
        const auto windows = paused;
        resumeBackground();
        for (const auto &window : windows)
            if (window && window->isVisible()) {
                // Complete deferred child paints before exposing Nickel's
                // backing store. After a PIN unlock it can still hold the PIN
                // frame. Do not process unrelated events during the handoff.
                for (auto *child : window->findChildren<QWidget *>())
                    QCoreApplication::sendPostedEvents(child, QEvent::UpdateLater);
                QCoreApplication::sendPostedEvents(window, QEvent::UpdateLater);
                window->repaint();
            }
    }

    void acceptHandoff(QDialog &dialog, const std::function<void()> &accept) override
    {
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
    bool eventFilter(QObject *object, QEvent *event) override
    {
        // An application-wide filter: check the event type first.
        if (event->type() != QEvent::Polish && event->type() != QEvent::Show)
            return false;
        if (reader && reader->isVisible()) {
            // Nickel's dialogs keep Nickel's Large Print scale over the reader.
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && widget->isWindow() && nativeDialog(widget) && !ownedByReader(widget))
                allowNativeFontScale(widget, event->type() == QEvent::Polish);
        }
        if (reader && reader->isVisible() && event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && nativeDialog(widget)) {
                QPointer<QWidget> candidate(widget);
                qt::later(0, this, [this, candidate] {
                    if (candidate)
                        presentDialog(candidate);
                });
            }
        }
        return false;
    }

    bool ownedByReader(const QWidget *widget) const
    {
        for (auto *parent = widget; parent; parent = parent->parentWidget())
            if (parent == reader)
                return true;
        return false;
    }

    void presentDialog(QWidget *widget)
    {
        if (!reader || !reader->isVisible() || !widget->isVisible())
            return;
        auto *dialog = widget;
        for (auto *parent = widget->parentWidget(); parent; parent = parent->parentWidget())
            if (nativeDialog(parent))
                dialog = parent;
        // The reader can itself be a QDialog owned by Nickel's main window.
        // Never move it, or a dialog that contains it, into itself.
        if (dialog == reader->window() || dialog->isAncestorOf(reader))
            return;
        // The browser retains its existing full-screen handoff and wake guard.
        if (dialog->findChild<QWidget *>(QStringLiteral("N3BrowserView")))
            return;
        dialogs.erase(std::remove_if(dialogs.begin(), dialogs.end(),
                                     [](const auto &overlay) { return !overlay->active(); }),
                      dialogs.end());
        for (const auto &overlay : dialogs)
            if (overlay->contains(dialog))
                return;
        bool native = false;
        for (auto *parent = dialog->parentWidget(); parent; parent = parent->parentWidget())
            native = native || parent->inherits("MainWindowView");
        for (const auto &overlay : dialogs)
            native = native || overlay->contains(dialog->parentWidget());
        if (!native)
            return;
        auto overlay = std::make_unique<NativeDialogOverlay>();
        overlay->present(dialog, reader->window());
        dialogs.push_back(std::move(overlay));
    }

    std::function<void(const sleep::Snapshot &)> save;
    std::vector<std::unique_ptr<NativeDialogOverlay>> dialogs;
    QPointer<QWidget> reader;
    bool sleepSupported;
    QPointer<SleepFrame> sleepFrame;
    QVector<QPointer<QWidget>> paused;
};
} // namespace

std::unique_ptr<WindowPresentation>
makeWindowPresentation(QWidget *reader, bool sleepScreenSupported,
                       std::function<void(const sleep::Snapshot &)> save)
{
    return std::make_unique<Presentation>(reader, sleepScreenSupported, std::move(save));
}
} // namespace kobalt::nickel
