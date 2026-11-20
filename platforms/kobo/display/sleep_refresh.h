#pragma once
#include <QApplication>
#include <QPointer>
#include <QWidget>
#include <QWindow>

namespace kobalt::platform {
// One cleanup belongs to the first painted frame of a sleep or power-off presentation.
// The two firmware adapters consume it on the GUI thread, at the native submission.
class SleepRefresh
{
public:
    ~SleepRefresh() { reset(); }

    void begin(bool clean)
    {
        reset();
        pending = clean;
    }

    void painted(QWidget *widget)
    {
        if (pending) {
            frame = widget;
            active() = this;
        }
    }

    void reset()
    {
        if (active() == this)
            active() = nullptr;
        pending = false;
        submitted = false;
        frame.clear();
    }

    // A backing-store flush has a target window. MTK can also compose the
    // screen directly upon hide, exposing only the final rectangle.
    static bool take(QWindow *window, const QRect &screen = {}, QWidget *yieldingReader = nullptr)
    {
        if (!matches(window, screen, yieldingReader))
            return false;
        active()->reset();
        return true;
    }

    // MTK can ask for the actual waveform after the backing-store flush
    // returns. Enlarge one submission, then let that lookup consume it.
    static bool submit(QWindow *window, QWidget *yieldingReader, bool awaitLookup)
    {
        if (!awaitLookup)
            return take(window, {}, yieldingReader);
        if (!matches(window, {}, yieldingReader) || active()->submitted)
            return false;
        active()->submitted = true;
        return true;
    }

private:
    static bool matches(QWindow *window, const QRect &screen, QWidget *yieldingReader)
    {
        auto *request = active();
        if (!request || !request->frame || !request->frame->isVisible())
            return false;
        auto *owner = request->frame->window();
        if (!owner->windowHandle() || (window && owner->windowHandle() != window))
            return false;
        if (!screen.isEmpty() && screen != owner->rect()
            && screen != QRect(QPoint(), owner->size().transposed()))
            return false;
        // A modal reader may still be registered while its native window is
        // being hidden. A PIN, USB dialog or popup must keep its own policy.
        for (auto *top : { QApplication::activeModalWidget(), QApplication::activePopupWidget() })
            if (top && top->window() != owner && top != yieldingReader && top->isVisible())
                return false;
        if (!window) {
            auto *focus = QGuiApplication::focusWindow();
            if (focus && focus != owner->windowHandle() && focus->isVisible())
                return false;
        }
        return true;
    }

    static SleepRefresh *&active()
    {
        static SleepRefresh *request = nullptr;
        return request;
    }

    bool pending = false, submitted = false;
    QPointer<QWidget> frame;
};
} // namespace kobalt::platform
