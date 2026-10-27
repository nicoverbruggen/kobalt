#include "pointer_delivery.h"
#include "sleep_frame.h"
#include "ui/hold_timer.h"
#include "platforms/android/canvas.h"

#include <QGuiApplication>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <unistd.h>

class PointerWindow final : public QWindow {
public:
    kobalt::ui::HoldTimer hold;
    int holds = 0;
    int releases = 0;
    PointerWindow()
    {
        hold.setSingleShot(true);
        hold.setInterval(550);
        QObject::connect(&hold, &QTimer::timeout, this, [this] { ++holds; });
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            hold.start(static_cast<QInputEvent *>(event));
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            if (hold.finish(static_cast<QInputEvent *>(event)))
                ++holds;
            ++releases;
            return true;
        }
        return QWindow::event(event);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    {
        QObject surface;
        const auto first = kindle::requestSleepFrame(surface);
        if (kindle::displayFlushAllowed(surface)) {
            std::fputs("A pending ordinary flush can overwrite the sleep frame.\n", stderr);
            return 1;
        }
        kindle::sleepFramePainted(surface, first);
        QObject dialog;
        if (kindle::displayFlushAllowed(dialog, &surface)) {
            std::fputs("A native dialog can overwrite the pending sleep frame.\n", stderr);
            return 1;
        }
        if (!kindle::displayFlushAllowed(surface))
            return 1;
        kindle::sleepFramePresented(surface, first);
        if (kindle::displayFlushAllowed(dialog, &surface))
            return 1;
        if (kindle::displayFlushAllowed(surface)) {
            std::fputs("Display did not freeze after submitting the sleep frame.\n", stderr);
            return 1;
        }
        const auto cover = kindle::requestSleepFrame(surface);
        kindle::sleepFramePainted(surface, first);
        if (kindle::displayFlushAllowed(surface))
            return 1;
        kindle::sleepFramePainted(surface, cover);
        if (!kindle::displayFlushAllowed(surface))
            return 1;
        kindle::sleepFramePresented(surface, cover);
        kindle::wakeDisplay(surface);
        if (!kindle::displayFlushAllowed(surface) || !kindle::displayFlushAllowed(dialog, &surface))
            return 1;
        const auto next = kindle::requestSleepFrame(surface);
        kindle::sleepFramePainted(surface, cover);
        if (kindle::displayFlushAllowed(surface)) {
            std::fputs("A stale sleep paint survived wake and a new sleep cycle.\n", stderr);
            return 1;
        }
        kindle::sleepFramePainted(surface, next);
        if (!kindle::displayFlushAllowed(surface))
            return 1;
        kindle::sleepFramePresented(surface, cover);
        if (!kindle::displayFlushAllowed(surface)) {
            std::fputs("A late completion acknowledged a different sleep frame.\n", stderr);
            return 1;
        }
        kindle::sleepFramePresented(surface, next);
        if (kindle::displayFlushAllowed(surface))
            return 1;
    }
    QWindowSystemInterface::setSynchronousWindowSystemEvents(false);
    PointerWindow window;
    window.setGeometry(0, 0, 100, 100);
    window.show();
    app.processEvents();
    const QPointF point(50, 50);
    kindle::deliverPointer(&window, point, point, true, false, 0);
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    if (!window.hold.isActive()) {
        std::fputs("Pointer press was not delivered.\n", stderr);
        return 1;
    }

    int descriptors[2];
    if (::pipe(descriptors) != 0)
        return 1;
    QSocketNotifier notifier(descriptors[0], QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &window, [&] {
        char byte;
        if (::read(descriptors[0], &byte, 1) == 1)
            kindle::deliverPointer(&window, point, point, false, true, 85);
        notifier.setEnabled(false);
    });
    // The release is ready before the hold deadline. A refresh wait keeps the
    // GUI busy until both are ready for the next event-dispatcher iteration.
    if (::write(descriptors[1], "x", 1) != 1)
        return 1;
    QThread::msleep(650);
    app.processEvents();
    notifier.setEnabled(false);
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    if (window.releases != 1 || window.holds != 0) {
        std::fprintf(stderr, "Pending release lost to overdue hold timer: releases=%d held=%d\n",
                     window.releases, window.holds);
        return 1;
    }
    auto queued = [&](ulong pressed, ulong released, bool cancel, bool fireTimer) {
        window.holds = window.releases = 0;
        kindle::deliverPointer(&window, point, point, true, false, pressed);
        if (cancel)
            window.hold.stop();
        if (fireTimer) {
            QThread::msleep(650);
            app.processEvents();
        }
        kindle::deliverPointer(&window, point, point, false, true, released);
        return window.releases == 1;
    };
    if (!queued(0, 1000, false, false) || window.holds != 1 ||
        !queued(0xfffffff0u, 0x3d8u, false, false) || window.holds != 1 ||
        !queued(1000, 900, false, false) || window.holds != 0 ||
        !queued(0, 1000, true, false) || window.holds != 0 ||
        !queued(0, 1000, false, true) || window.holds != 1) {
        std::fputs("Source duration, cancellation, wrap or duplicate hold failed.\n", stderr);
        return 1;
    }
    class StampWidget final : public QWidget {
    public:
        ulong pressed = 0, released = 0;
        void mousePressEvent(QMouseEvent *event) override { pressed = event->timestamp(); }
        void mouseReleaseEvent(QMouseEvent *event) override { released = event->timestamp(); }
    };
    kobalt::android::WidgetCanvas canvas(nullptr);
    auto *widget = new StampWidget;
    canvas.setContent(widget);
    QPointingDevice device("Input regression", 1, QInputDevice::DeviceType::TouchScreen,
                           QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 1, 0);
    for (const auto type : {QEvent::TouchBegin, QEvent::TouchEnd}) {
        QTouchEvent event(type, &device, Qt::NoModifier,
                         {QEventPoint(0, type == QEvent::TouchBegin ? QEventPoint::Pressed : QEventPoint::Released,
                                      QPointF(), QPointF())});
        event.setTimestamp(type == QEvent::TouchBegin ? 1000 : 2000);
        QApplication::sendEvent(canvas.viewport(), &event);
    }
    if (widget->pressed != 1000 || widget->released != 2000) {
        std::fprintf(stderr, "Canvas discarded touch timestamps: press=%lu release=%lu\n",
                     widget->pressed, widget->released);
        return 1;
    }
    std::puts("Queued taps, holds, cancellation, clock wrap and canvas source timestamps passed.");
    return 0;
}
