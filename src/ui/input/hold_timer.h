#pragma once
#include <QInputEvent>
#include <QTimer>

namespace kobalt::ui {

class HoldTimer : public QTimer
{
public:
    void start(const QInputEvent *event)
    {
        pressedAt = quint32(event->timestamp());
        sourceTimeOnly = false;
        QTimer::start();
    }

    // Cancels the timeout, because a blocked GUI can run it before a queued
    // release. finish() still reports a long hold from source time.
    void stopTimeout()
    {
        sourceTimeOnly = sourceTimeOnly || isActive();
        QTimer::stop();
    }

    void stop()
    {
        sourceTimeOnly = false;
        QTimer::stop();
    }

    bool finish(const QInputEvent *event)
    {
        const quint32 elapsed = quint32(event->timestamp()) - pressedAt;
        // Source timestamps keep a queued release from turning a real hold into
        // a tap when painting blocks the GUI. Subtraction permits clock wrap.
        const bool due = (isActive() || sourceTimeOnly) && elapsed < 0x80000000u
                && elapsed >= quint32(interval());
        stop();
        return due;
    }

private:
    quint32 pressedAt = 0;
    bool sourceTimeOnly = false;
};

} // namespace kobalt::ui
