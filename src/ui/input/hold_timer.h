#pragma once
#include <QInputEvent>
#include <QTimer>

namespace kobalt::ui {

class HoldTimer : public QTimer {
public:
    void start(const QInputEvent *event) {
        pressedAt = quint32(event->timestamp());
        QTimer::start();
    }
    bool finish(const QInputEvent *event) {
        const quint32 elapsed = quint32(event->timestamp()) - pressedAt;
        // Source timestamps keep a queued release from turning a real hold into
        // a tap when painting blocks the GUI. Subtraction permits clock wrap.
        const bool due = isActive() && elapsed < 0x80000000u && elapsed >= quint32(interval());
        stop();
        return due;
    }
private:
    quint32 pressedAt = 0;
};

} // namespace kobalt::ui
