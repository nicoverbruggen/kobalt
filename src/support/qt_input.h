#pragma once
// Synthesized input across Qt versions, beside qt_compat.h. It needs Qt's
// private window-system header, so only code that sends input includes it.
#include <QEventLoop>
#include <QPointF>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
#  include <private/qhighdpiscaling_p.h>
#endif

namespace kobalt::qt {
// Sends a left-button press or release at `local`, in the window's own
// points, through the window system, as real input arrives.
inline void sendMouse(QWindow *window, QPointF local, bool press)
{
    QPointF global = window->mapToGlobal(local.toPoint());
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    // The window system takes native pixels; a scaled window's points are not.
    local = QHighDpi::toNativeLocalPosition(local, window);
    global = QHighDpi::toNativeGlobalPosition(global, window);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    QWindowSystemInterface::handleMouseEvent(
            window, local, global, press ? Qt::LeftButton : Qt::NoButton, Qt::LeftButton,
            press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease);
#else
    QWindowSystemInterface::handleMouseEvent(window, local, global,
                                             press ? Qt::LeftButton : Qt::NoButton);
#endif
    QWindowSystemInterface::flushWindowSystemEvents(QEventLoop::AllEvents);
}
} // namespace kobalt::qt
