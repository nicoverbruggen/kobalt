#pragma once

#include <QEvent>
#include <QCoreApplication>
#include <QPointF>
#include <QPointer>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

namespace kindle {

inline void deliverPointer(QWindow *window, const QPointF &local, const QPointF &global,
                           bool down, bool previousDown, ulong timestamp)
{
    const QEvent::Type type = down == previousDown ? QEvent::MouseMove
        : (down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease);
    // Pending releases must reach widgets before the dispatcher runs overdue hold timers.
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(window, timestamp, local, global,
        down ? Qt::LeftButton : Qt::NoButton,
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, type);
}

inline void cancelPointer(QWindow *window, const QPointF &local, const QPointF &global, ulong timestamp)
{
    QPointer<QWindow> guard(window);
    QEvent cancel(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(window, &cancel);
    // Clear Qt's button state and graphics-scene grab after the app has
    // cancelled its widget gesture. This release must not act as a tap.
    if (guard)
        deliverPointer(window, local, global, false, true, timestamp);
}

} // namespace kindle
