#pragma once

#include "support/qt_compat.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QVariantMap>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

namespace kobalt::nickel {
class NativeReadingTime
{
public:
    template <class Commit, class Verify>
    bool save(Commit commit, Verify verify)
    {
        if (uncertain)
            return false;
        if (!committed) {
            if (!commit(expected))
                return false;
            committed = true;
        }
        return verify(expected);
    }

    bool committed = false;
    // A failed rollback leaves the transaction outcome unknown. Never replay
    // the increment merely because a later retry can obtain the database.
    bool uncertain = false;

private:
    QVariantMap expected;
};

// GUI-thread queue. An interval is removed only after both operations succeed.
// The writer retains its own commit state, so retrying verification cannot add
// the interval twice. Owners must drain active calls before releasing storage;
// retained failures require an explicit lifecycle policy or user choice.
// Both operations report through their completion, so on a device neither
// has to run on the GUI thread: a tap can arrive while they write.
class NativeReadingSave final : public QObject
{
public:
    using Completion = std::function<void(bool)>;
    using Flush = std::function<void(Completion)>;
    using Write = std::function<void(Completion)>;

    explicit NativeReadingSave(int retryMilliseconds = 100, int timeoutMilliseconds = 2000,
                               int cooldownMilliseconds = 300000)
        : retryMilliseconds(retryMilliseconds),
          timeoutMilliseconds(timeoutMilliseconds),
          cooldownMilliseconds(cooldownMilliseconds)
    {
        elapsed.invalidate();
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this] { advance(); });
    }

    void append(Flush flush, Write write)
    {
        intervals.push_back({ std::move(flush), std::move(write), false });
        if (!elapsed.isValid())
            elapsed.start();
        if (!failed_ && !active && !timer.isActive())
            timer.start(0);
    }

    bool pending() const { return !intervals.empty(); }

    bool inFlight() const { return active; }

    bool failed() const { return failed_; }

    // Activity retries only after cooldown. A distinct lifecycle boundary or
    // explicit user retry may start sooner; finish itself never renews a job.
    void retryFailed()
    {
        if (!failed_)
            return;
        failed_ = false;
        failures = 0;
        elapsed.start();
        timer.start(0);
    }

    void retryIfDue()
    {
        if (failed_ && failedSince.isValid() && failedSince.elapsed() >= cooldownMilliseconds)
            retryFailed();
    }

    void finish(Completion done)
    {
        if (failed_) {
            done(false);
            return;
        }
        if (!pending()) {
            done(true);
            return;
        }
        completions.push_back(std::move(done));
        if (!active) {
            timer.start(0);
        }
    }

private:
    void retry()
    {
        if (++failures < 3 && elapsed.elapsed() < timeoutMilliseconds) {
            timer.start(retryMilliseconds);
            return;
        }
        failed_ = true;
        failedSince.start();
        timer.stop();
        const auto ready = std::exchange(completions, {});
        // Keep the interval and its commit phase for the next attempt.
        const QPointer<NativeReadingSave> guard(this);
        for (const auto &done : ready) {
            if (!guard)
                break;
            done(false);
        }
    }

    struct Interval
    {
        Flush flush;
        Write write;
        bool flushed;
    };

    void advance()
    {
        if (failed_ || active || intervals.empty())
            return;
        if (!intervals.front().flushed) {
            active = true;
            const QPointer<NativeReadingSave> guard(this);
            intervals.front().flush([guard](bool saved) {
                if (!guard)
                    return;
                guard->active = false;
                if (saved)
                    guard->intervals.front().flushed = true;
                if (saved)
                    guard->timer.start(0);
                else
                    guard->retry();
            });
            return;
        }
        active = true;
        const QPointer<NativeReadingSave> guard(this);
        intervals.front().write([guard](bool saved) {
            if (!guard)
                return;
            guard->active = false;
            if (saved)
                guard->written();
            else
                guard->retry();
        });
    }

    void written()
    {
        intervals.pop_front();
        failures = 0;
        if (!intervals.empty()) {
            elapsed.start();
            timer.start(0);
            return;
        }
        const auto ready = std::exchange(completions, {});
        elapsed.invalidate();
        // A completion can destroy this queue. Do not access members afterward.
        const QPointer<NativeReadingSave> guard(this);
        for (const auto &done : ready) {
            if (!guard)
                break;
            done(true);
        }
    }

    const int retryMilliseconds;
    const int timeoutMilliseconds;
    const int cooldownMilliseconds;
    QElapsedTimer failedSince;
    QTimer timer;
    QElapsedTimer elapsed;
    std::deque<Interval> intervals;
    std::vector<Completion> completions;
    bool active = false;
    bool failed_ = false;
    int failures = 0;
};
} // namespace kobalt::nickel
