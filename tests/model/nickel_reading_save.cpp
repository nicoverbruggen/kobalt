#include "platforms/kobo/common/native_reading_save.h"
#include "check.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <memory>

using kobalt::nickel::NativeReadingSave;
using kobalt::nickel::NativeReadingTime;

namespace {
void waitFor(const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "Timed out waiting for native save");
}
} // namespace

// Checks the native reading-time save worker: queued intervals, holds for a close, repeated finishes and timeouts.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    {
        NativeReadingTime time;
        int attempts = 0, stored = 29, verifies = 0;
        const auto commit = [&](QVariantMap &expected) {
            if (++attempts == 1)
                return false;
            stored += 15;
            expected.insert("time", stored);
            return true;
        };
        const auto verify = [&](const QVariantMap &expected) {
            require(expected.value("time").toInt() == 44,
                    "Verification lost its original committed target");
            return ++verifies > 1;
        };
        require(!time.save(commit, verify) && !time.committed && stored == 29,
                "Busy transaction changed or consumed the delta");
        require(!time.save(commit, verify) && time.committed && stored == 44,
                "Failed readback lost the successful commit");
        require(time.save(commit, verify) && attempts == 2 && stored == 44,
                "Readback retry added the interval twice");
    }
    {
        NativeReadingTime time;
        int attempts = 0;
        auto commit = [&](QVariantMap &) {
            ++attempts;
            time.uncertain = true;
            return false;
        };
        require(!time.save(commit, [](const QVariantMap &) { return true; }),
                "Uncertain commit succeeded");
        require(!time.save(commit, [](const QVariantMap &) { return true; }) && attempts == 1,
                "Unknown rollback outcome replayed an increment");
    }
    {
        NativeReadingSave saves(1, 20);
        NativeReadingSave::Completion first;
        int writes = 0, completions = 0;
        saves.append([&](auto done) { first = std::move(done); });
        saves.finish([&](bool saved) {
            require(saved, "Held write failed");
            ++completions;
        });
        saves.finish([&](bool saved) {
            require(saved, "Repeated finish failed");
            ++completions;
        });
        waitFor([&] { return bool(first); });
        bool heartbeat = false;
        kobalt::qt::invoke(&app, [&] { heartbeat = true; }, Qt::QueuedConnection);
        waitFor([&] { return heartbeat; });
        require(saves.pending() && completions == 0, "Finish passed an unfinished write");
        saves.append([&](auto done) {
            ++writes;
            done(true);
        });
        first(true);
        waitFor([&] { return completions == 2; });
        require(!saves.pending() && writes == 1, "Queued intervals were lost or duplicated");
    }
    {
        NativeReadingSave saves(1, 10);
        bool writable = false, completed = false, result = true;
        int writes = 0;
        saves.append([&](auto done) {
            ++writes;
            done(writable);
        });
        saves.finish([&](bool saved) {
            result = saved;
            completed = true;
        });
        waitFor([&] { return completed; });
        require(!result && saves.pending(), "Timeout discarded the pending interval");
        writable = true;
        completed = false;
        const int exhaustedWrites = writes;
        saves.finish([&](bool saved) { require(!saved, "Finish renewed a failed job"); });
        require(writes == exhaustedWrites && saves.failed(),
                "Outer caller restarted an exhausted save");
        saves.retryFailed();
        saves.finish([&](bool saved) {
            result = saved;
            completed = true;
        });
        waitFor([&] { return completed; });
        require(result && !saves.pending() && writes > 1, "Explicit retry lost the delta");
    }
    {
        // A close holds the queue while it writes Nickel's progress. The held
        // write waits; a write already in flight is reported idle first.
        NativeReadingSave saves(1, 20);
        NativeReadingSave::Completion running;
        int started = 0;
        saves.append([&](auto done) {
            ++started;
            running = std::move(done);
        });
        waitFor([&] { return bool(running); });
        saves.append([&](auto done) {
            ++started;
            done(true);
        });
        saves.hold();
        bool idle = false;
        saves.whenIdle([&] {
            require(started == 1 && !saves.inFlight(),
                    "Idle report came while a write still held the database");
            idle = true;
        });
        require(!idle, "Idle report passed a write in flight");
        running(true);
        require(idle, "Finished write did not report idle");
        bool heartbeat = false;
        kobalt::qt::invoke(&app, [&] { heartbeat = true; }, Qt::QueuedConnection);
        waitFor([&] { return heartbeat; });
        require(started == 1 && saves.pending(), "A held queue started its next write");
        bool immediate = false;
        saves.whenIdle([&] { immediate = true; });
        require(immediate, "An idle queue did not report at once");
        saves.release();
        waitFor([&] { return !saves.pending(); });
        require(started == 2, "Released write did not run once");
        // finish() waits for every interval, so it also releases a hold.
        saves.hold();
        saves.append([&](auto done) {
            ++started;
            done(true);
        });
        bool finished = false;
        saves.finish([&](bool saved) { finished = saved; });
        waitFor([&] { return finished; });
        require(started == 3, "Finish did not release a held write");
    }
    {
        NativeReadingSave saves(1, 10);
        bool writable = false;
        int attempts = 0;
        saves.append([&](auto done) {
            ++attempts;
            done(writable);
        });
        waitFor([&] { return saves.failed(); });
        const int exhaustedAttempts = attempts;
        QElapsedTimer timeout;
        timeout.start();
        waitFor([&] { return timeout.elapsed() > 30; });
        require(saves.pending(), "Failed sleeping interval disappeared");
        writable = true;
        QCoreApplication::processEvents();
        require(saves.failed() && attempts == exhaustedAttempts && attempts <= 3,
                "Failed sleeping interval retried autonomously");
        saves.retryFailed();
        waitFor([&] { return !saves.pending(); });
    }
    {
        NativeReadingSave saves(1, 100, 30);
        int attempts = 0;
        bool writable = false;
        saves.append([&](auto done) {
            ++attempts;
            done(writable);
        });
        waitFor([&] { return saves.failed(); });
        require(attempts == 3, "Failed native job did not stop after three attempts");
        saves.retryIfDue();
        QCoreApplication::processEvents();
        require(attempts == 3, "Activity renewed the retry budget before cooldown");
        QElapsedTimer cooldown;
        cooldown.start();
        waitFor([&] { return cooldown.elapsed() >= 40; });
        writable = true;
        saves.retryIfDue();
        waitFor([&] { return !saves.pending(); });
        require(attempts == 4, "Activity did not recover retained data after cooldown");
    }
    {
        auto saves = std::make_unique<NativeReadingSave>(1, 10);
        NativeReadingSave::Completion write;
        bool completed = false;
        saves->append([&](auto done) { write = std::move(done); });
        saves->finish([&](bool) { completed = true; });
        waitFor([&] { return bool(write); });
        saves.reset();
        write(true);
        QCoreApplication::processEvents();
        require(!completed, "Destroyed queue called its owner");
    }
    {
        // On a device the write runs on Nickel's event thread. The queue stays
        // pending, and the GUI thread keeps running, until it reports back.
        NativeReadingSave saves(1, 20);
        NativeReadingSave::Completion write;
        bool completed = false;
        saves.append([&](auto done) { write = std::move(done); });
        saves.finish([&](bool saved) {
            require(saved, "Asynchronous write failed");
            completed = true;
        });
        waitFor([&] { return bool(write); });
        bool heartbeat = false;
        kobalt::qt::invoke(&app, [&] { heartbeat = true; }, Qt::QueuedConnection);
        waitFor([&] { return heartbeat; });
        require(saves.pending() && saves.inFlight() && !completed,
                "Finish passed a write still in flight");
        write(true);
        waitFor([&] { return completed; });
        require(!saves.pending(), "Completed asynchronous write stayed pending");
    }
    std::puts(
            "PASS: native interval retry, commit verification, close holds, responsiveness and cancellation");
}
