#pragma once
#include "qt_compat.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <functional>
#include <utility>

namespace kobalt {
class SaveWait;

namespace saves {
// Reports that a save, or one step of it, has finished. Call it from any
// thread after the state that a waiter checks has changed. Every SaveWait
// then runs its waiting checks on the GUI thread, from the event loop.
// Several reports before that run count as one.
void settled();

namespace detail {
inline QVector<SaveWait *> &waits()
{
    static QVector<SaveWait *> list;
    return list;
}

inline std::atomic_bool &queued()
{
    static std::atomic_bool flag{ false };
    return flag;
}
} // namespace detail
} // namespace saves

// Waits for saves without polling. A caller that finds a save still pending
// hands its own check to after(). The check runs again when a save reports
// saves::settled(), and hands itself over again while it still has to wait.
// One long fallback timer runs the checks too, so a save that does not report
// can delay its waiter but cannot stall it. Create and use it on the GUI
// thread; the checks end with the object.
class SaveWait final : public QObject
{
public:
    static constexpr int fallbackMilliseconds = 1000;

    explicit SaveWait(QObject *parent = nullptr) : QObject(parent)
    {
        // A child, so tests can find the timer and run it.
        fallback.setParent(this);
        fallback.setObjectName("rrSaveWaitFallback");
        fallback.setSingleShot(true);
        fallback.setInterval(fallbackMilliseconds);
        connect(&fallback, &QTimer::timeout, this, [this] {
            ++fallbacks_;
            run();
        });
        saves::detail::waits().append(this);
    }

    ~SaveWait() override { saves::detail::waits().removeAll(this); }

    void after(std::function<void()> check)
    {
        checks.append(std::move(check));
        if (!fallback.isActive())
            fallback.start();
    }

    bool waiting() const { return !checks.isEmpty(); }

    // How often the fallback timer ran the checks. Tests use it to find a
    // save that does not report.
    int fallbacks() const { return fallbacks_; }

    void run()
    {
        fallback.stop();
        const auto ready = std::exchange(checks, {});
        const QPointer<SaveWait> guard(this);
        for (const auto &check : ready) {
            if (!guard)
                return;
            check();
        }
    }

private:
    QVector<std::function<void()>> checks;
    QTimer fallback;
    int fallbacks_ = 0;
};

namespace saves {
inline void settled()
{
    auto *application = QCoreApplication::instance();
    if (!application || detail::queued().exchange(true))
        return;
    const bool posted = qt::invoke(
            application,
            [] {
                detail::queued().store(false);
                // A check can destroy another waiter.
                const auto waiting = detail::waits();
                for (auto *wait : waiting)
                    if (detail::waits().contains(wait))
                        wait->run();
            },
            Qt::QueuedConnection);
    if (!posted)
        detail::queued().store(false);
}
} // namespace saves
} // namespace kobalt
