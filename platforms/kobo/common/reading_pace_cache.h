#pragma once

#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace kobalt::nickel {
// GUI-thread cache. Queries run on Nickel's worker and never retain the session.
// Stop cancels publication, including work already running. The last valid pace
// survives pauses; clear also discards it when the account changes.
// A ticked cache has no timer of its own: the reader's minute tick calls
// tick(), so the device wakes once a minute for the clock and the pace together.
class ReadingPaceCache final : public QObject
{
public:
    static constexpr int refreshInterval = 60000;
    enum class Schedule { Timed, Ticked };

    explicit ReadingPaceCache(int interval = refreshInterval, Schedule schedule = Schedule::Timed)
        : interval(qMax(1, interval)), timed(schedule == Schedule::Timed)
    {
        attempted.invalidate();
        timer.setSingleShot(true);
        timer.setTimerType(Qt::PreciseTimer);
        connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
    }

    ~ReadingPaceCache() override { stop(); }

    void start(QObject *target, std::function<double()> query)
    {
        worker = target;
        this->query = std::move(query);
        active = true;
        refresh();
    }

    void stop()
    {
        active = false;
        timer.stop();
        if (pending) {
            pending->active.store(false);
        }
        query = {};
    }

    void clear()
    {
        stop();
        secondsPerWord = -1;
        attempted.invalidate();
    }

    double value() const { return secondsPerWord; }

    // A minute tick can arrive slightly before a full interval has passed.
    // Accept it within a twelfth of the interval, so one tick a minute refreshes.
    void tick()
    {
        if (active && !pending
            && (!attempted.isValid() || attempted.elapsed() >= interval - interval / 12))
            refresh(true);
    }

    std::function<void()> changed;

private:
    struct Request
    {
        std::atomic_bool active{ true };
    };

    void refresh(bool ticked = false)
    {
        if (!active || pending) {
            return;
        }
        const qint64 left = attempted.isValid() ? interval - attempted.elapsed() : 0;
        if (left > 0 && !ticked) {
            if (timed)
                timer.start(int(left));
            return;
        }
        attempted.start();
        if (!worker || !query) {
            if (timed)
                timer.start(interval);
            return;
        }
        auto request = std::make_shared<Request>();
        pending = request;
        const auto fetch = query;
        const QPointer<ReadingPaceCache> guard(this);
        if (!qt::invoke(
                    worker,
                    [guard, request, fetch] {
                        const double result = request->active.load() ? fetch() : -1;
                        qt::invoke(
                                qApp,
                                [guard, request, result] {
                                    if (guard) {
                                        guard->complete(request, result);
                                    }
                                },
                                Qt::QueuedConnection);
                    },
                    Qt::QueuedConnection)) {
            complete(request, -1);
        }
    }

    void complete(const std::shared_ptr<Request> &request, double result)
    {
        if (pending != request) {
            return;
        }
        pending.reset();
        // Queueing and query time must not consume the delay before the next
        // request. clear() invalidates the clock so a new account need not wait.
        if (attempted.isValid()) {
            attempted.start();
        }
        const bool publish = request->active.load() && std::isfinite(result) && result > 0
                && result != secondsPerWord;
        if (publish) {
            secondsPerWord = result;
        }
        if (active && timed) {
            // A slow query must not cause a burst of catch-up requests.
            timer.start(
                    int(qMax<qint64>(1, attempted.isValid() ? interval - attempted.elapsed() : 0)));
        }
        if (publish && changed) {
            changed();
        }
    }

    const int interval;
    const bool timed;
    QTimer timer;
    QElapsedTimer attempted;
    QPointer<QObject> worker;
    std::function<double()> query;
    std::shared_ptr<Request> pending;
    double secondsPerWord = -1;
    bool active = false;
};
} // namespace kobalt::nickel
