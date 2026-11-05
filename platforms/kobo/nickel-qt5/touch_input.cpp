#include "touch_input.h"
#include <QApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QTouchEvent>
#include <QWidget>

namespace kobalt::nickel_qt5 {
namespace {
// Kobalt names its top-level windows with an "rr" prefix. Its popups are
// separate windows whose parent widgets lead back to the reader.
bool ownWindow(const QWidget *widget)
{
    for (const QWidget *window = widget ? widget->window() : nullptr; window;
         window = window->parentWidget() ? window->parentWidget()->window() : nullptr)
        if (window->objectName().startsWith("rr"))
            return true;
    return false;
}

bool takesTouch(const QWidget *target, bool &scrollViewport)
{
    for (const QWidget *widget = target; widget;
         widget = widget->isWindow() ? nullptr : widget->parentWidget()) {
        if (!widget->testAttribute(Qt::WA_AcceptTouchEvents)) {
            continue;
        }
        const auto *area = qobject_cast<const QScrollArea *>(widget->parentWidget());
        if (widget != target && area && area->viewport() == widget
            && widget->metaObject() == &QWidget::staticMetaObject) {
            // A scroll viewport accepts touch without making its mouse-only
            // child controls touch handlers. Keep their tap fallback available.
            scrollViewport = true;
            continue;
        }
        return true;
    }
    return false;
}

class TouchInput : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *receiver, QEvent *event) override
    {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress && event->spontaneous() && receiver->isWidgetType()
            && ownWindow(static_cast<QWidget *>(receiver))) {
            // This Qt synthesizes mouse events itself. Do not click twice.
            native = true;
            pressed.clear();
        }
        if (native || !receiver->isWindowType()
            || (type != QEvent::TouchBegin && type != QEvent::TouchUpdate
                && type != QEvent::TouchEnd && type != QEvent::TouchCancel))
            return false;
        const auto points = static_cast<QTouchEvent *>(event)->touchPoints();
        if (type == QEvent::TouchBegin) {
            pressed.clear();
            cancelOnMove = false;
            if (points.size() != 1)
                return false;
            const QPoint screen = points.first().screenPos().toPoint();
            auto *target = QApplication::widgetAt(screen);
            if (!target || !ownWindow(target) || takesTouch(target, cancelOnMove))
                return false;
            pressed = target;
            origin = screen;
            // Qt gives click focus only for a spontaneous press. Do it here, or
            // a tapped text field never gains focus and its keyboard stays hidden.
            for (QWidget *widget = target; widget;
                 widget = widget->isWindow() ? nullptr : widget->parentWidget())
                if (widget->isEnabled() && (widget->focusPolicy() & Qt::ClickFocus)) {
                    widget->setFocus(Qt::MouseFocusReason);
                    break;
                }
            if (!pressed)
                return false;
            send(QEvent::MouseButtonPress, screen, Qt::LeftButton);
            return false;
        }
        if (!pressed)
            return false;
        if (type == QEvent::TouchCancel || points.size() != 1) {
            // Release outside the widget, so a cancelled touch does not click.
            send(QEvent::MouseButtonRelease, pressed->mapToGlobal(QPoint(-1, -1)), Qt::NoButton);
            pressed.clear();
            return false;
        }
        const QPoint screen = points.first().screenPos().toPoint();
        if (cancelOnMove
            && (screen - origin).manhattanLength() >= QApplication::startDragDistance()) {
            // A drag across a scroll row must not activate its control, even
            // when the release remains inside that control.
            send(QEvent::MouseButtonRelease, pressed->mapToGlobal(QPoint(-1, -1)), Qt::NoButton);
            pressed.clear();
            return false;
        }
        if (type == QEvent::TouchUpdate)
            send(QEvent::MouseMove, screen, Qt::LeftButton);
        else {
            send(QEvent::MouseButtonRelease, screen, Qt::NoButton);
            pressed.clear();
        }
        return false;
    }

private:
    QPointer<QWidget> pressed;
    bool native = false;
    bool cancelOnMove = false;
    QPoint origin;

    void send(QEvent::Type type, const QPoint &screen, Qt::MouseButtons held)
    {
        const QPointer<QWidget> target = pressed;
        const QPointF local = target->mapFromGlobal(screen);
        const QPointF window = target->window()->mapFromGlobal(screen);
        QMouseEvent mouse(type, local, window, screen,
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held,
                          Qt::NoModifier);
        QApplication::sendEvent(target, &mouse);
    }
};
} // namespace

void installTouchInput(QObject *owner)
{
    qApp->installEventFilter(new TouchInput(owner));
}
} // namespace kobalt::nickel_qt5
