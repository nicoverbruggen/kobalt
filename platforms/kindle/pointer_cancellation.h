#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QPointer>
#include <QTouchEvent>
#include <QWindow>

namespace kobalt::kindle {

// A mouse release completes a gesture. Lost evdev input must cancel it first,
// including when a graphics proxy has forwarded the press to an embedded widget.
class PointerCancellation final : public QObject {
public:
    explicit PointerCancellation(QObject *parent) : QObject(parent) {
        qApp->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        auto *widget = qobject_cast<QWidget *>(object);
        if (widget && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick))
            pressed = widget;
        else if (widget && event->type() == QEvent::MouseButtonRelease && pressed == widget)
            pressed.clear();
        else if (event->type() == QEvent::UngrabMouse && qobject_cast<QWindow *>(object)) {
            QPointer<QObject> receiver(object);
            QPointer<QWidget> cancelled = pressed;
            pressed.clear();
            if (cancelled) {
                QTouchEvent cancel(QEvent::TouchCancel);
                QApplication::sendEvent(cancelled, &cancel);
            }
            if (auto *button = qobject_cast<QAbstractButton *>(cancelled.data()))
                button->setDown(false);
            return !receiver;
        }
        return false;
    }
private:
    QPointer<QWidget> pressed;
};

} // namespace kobalt::kindle
