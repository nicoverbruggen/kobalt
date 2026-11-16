#pragma once

#include "support/qt_compat.h"
#include "support/save_wait.h"
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

// GUI-thread queue of reading-time writes. An interval is removed only after
// its write succeeds. The writer retains its own commit state, so retrying
// verification cannot add the interval twice. Owners must drain active calls
// before releasing storage; retained failures require an explicit lifecycle
// policy or user choice. A write reports through its completion, so on a
// device it does not have to run on the GUI thread: a tap can arrive while
// it writes.
//
// A close holds the queue while it writes Nickel's progress on the GUI thread,
// so a reading-time write cannot keep Nickel's database busy at that moment.
// The write then follows in the background. A caller of finish() waits for
// every interval, so finish() releases the hold.
class NativeReadingSave final : public QObject
{
public:
    using Completion = std::function<void(bool)>;
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

    void append(Write write)
    {
        intervals.push_back(std::move(write));
        if (!elapsed.isValid())
            elapsed.start();
        if (!failed_ && !active && !held && !timer.isActive())
            timer.start(0);
    }

    bool pending() const { return !intervals.empty(); }

    bool inFlight() const { return active; }

    bool failed() const { return failed_; }

    bool isHeld() const { return held; }

    // The process's queue for Nickel's reading time. It is never destroyed,
    // so a write that outlives a reader still reports back.
    static NativeReadingSave &shared()
    {
        static auto *queue = new NativeReadingSave;
        return *queue;
    }

    // Runs `done` once no write is in flight, at once when none is.
    void whenIdle(std::function<void()> done)
    {
        if (!active) {
            done();
            return;
        }
        idle.push_back(std::move(done));
    }

    // Starts no new write until release() or finish(). A write already in
    // flight continues.
    void hold() { held = true; }

    void release()
    {
        if (!held)
            return;
        held = false;
        if (pending() && !failed_ && !active)
            timer.start(0);
    }

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
        held = false;
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
        saves::settled();
        const auto ready = std::exchange(completions, {});
        // Keep the interval and its commit phase for the next attempt.
        const QPointer<NativeReadingSave> guard(this);
        for (const auto &done : ready) {
            if (!guard)
                break;
            done(false);
        }
    }

    void advance()
    {
        if (failed_ || active || held || intervals.empty())
            return;
        active = true;
        const QPointer<NativeReadingSave> guard(this);
        intervals.front()([guard](bool saved) {
            if (!guard)
                return;
            guard->active = false;
            for (const auto &done : std::exchange(guard->idle, {})) {
                done();
                if (!guard)
                    return;
            }
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
            if (!held)
                timer.start(0);
            return;
        }
        const auto ready = std::exchange(completions, {});
        elapsed.invalidate();
        saves::settled();
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
    std::deque<Write> intervals;
    std::vector<Completion> completions;
    std::vector<std::function<void()>> idle;
    bool active = false;
    bool held = false;
    bool failed_ = false;
    int failures = 0;
};
} // namespace kobalt::nickel
