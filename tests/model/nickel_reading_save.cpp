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

// Checks the native reading-time save worker: queued intervals, held flushes, repeated finishes and timeouts.
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
        NativeReadingSave::Completion flush;
        int writes = 0, completions = 0;
        saves.append([&](auto done) { flush = std::move(done); },
                     [&](auto done) {
                         ++writes;
                         done(true);
                     });
        saves.finish([&](bool saved) {
            require(saved, "Held flush failed");
            ++completions;
        });
        saves.finish([&](bool saved) {
            require(saved, "Repeated finish failed");
            ++completions;
        });
        waitFor([&] { return bool(flush); });
        bool heartbeat = false;
        kobalt::qt::invoke(&app, [&] { heartbeat = true; }, Qt::QueuedConnection);
        waitFor([&] { return heartbeat; });
        require(saves.pending() && writes == 0 && completions == 0,
                "Finish passed an unfinished event flush");
        saves.append([](auto done) { done(true); },
                     [&](auto done) {
                         ++writes;
                         done(true);
                     });
        flush(true);
        waitFor([&] { return completions == 2; });
        require(!saves.pending() && writes == 2, "Queued intervals were lost or duplicated");
    }
    {
        NativeReadingSave saves(1, 10);
        bool writable = false, completed = false, result = true;
        int flushes = 0, writes = 0;
        saves.append(
                [&](auto done) {
                    ++flushes;
                    done(true);
                },
                [&](auto done) {
                    ++writes;
                    done(writable);
                });
        saves.finish([&](bool saved) {
            result = saved;
            completed = true;
        });
        waitFor([&] { return completed; });
        require(!result && saves.pending() && flushes == 1,
                "Timeout discarded the pending interval");
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
        require(result && !saves.pending() && flushes == 1 && writes > 1,
                "Explicit retry repeated the event flush or lost the delta");
    }
    {
        NativeReadingSave saves(1, 10);
        bool writable = false;
        int attempts = 0;
        saves.append([](auto done) { done(true); },
                     [&](auto done) {
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
        saves.append([](auto done) { done(true); },
                     [&](auto done) {
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
        NativeReadingSave::Completion flush;
        bool wrote = false, completed = false;
        saves->append([&](auto done) { flush = std::move(done); },
                      [&](auto done) {
                          wrote = true;
                          done(true);
                      });
        saves->finish([&](bool) { completed = true; });
        waitFor([&] { return bool(flush); });
        saves.reset();
        flush(true);
        QCoreApplication::processEvents();
        require(!wrote && !completed, "Destroyed queue performed late work or called its owner");
    }
    {
        // On a device the write runs on Nickel's event thread. The queue stays
        // pending, and the GUI thread keeps running, until it reports back.
        NativeReadingSave saves(1, 20);
        NativeReadingSave::Completion write;
        bool completed = false;
        saves.append([](auto done) { done(true); }, [&](auto done) { write = std::move(done); });
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
            "PASS: native interval retry, commit verification, flush ordering, responsiveness and cancellation");
}
