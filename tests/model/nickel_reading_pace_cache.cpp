#include "platforms/kobo/common/reading_pace_cache.h"
#include "check.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <limits>

using kobalt::nickel::ReadingPaceCache;

namespace {
void waitFor(const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "Timed out waiting for the pace worker");
}

void runFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    static_assert(ReadingPaceCache::refreshInterval == 60000,
                  "Production pace refresh must be once a minute");
    QThread thread;
    auto *worker = new QObject;
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    thread.start();
    {
        ReadingPaceCache cache;
        QSemaphore entered, release;
        int notifications = 0;
        cache.changed = [&] { ++notifications; };
        cache.start(worker, [&] {
            entered.release();
            release.acquire();
            return .25;
        });
        waitFor([&] { return entered.available() == 1; });
        // The native worker is held here. GUI events and every cached read must
        // still finish, even before the first valid pace exists.
        bool heartbeat = false;
        kobalt::qt::invoke(&app, [&] { heartbeat = true; }, Qt::QueuedConnection);
        waitFor([&] { return heartbeat; });
        for (int turn = 0; turn < 100000; ++turn) {
            require(cache.value() == -1, "An unfinished query supplied a pace");
        }
        release.release();
        waitFor([&] { return cache.value() == .25; });
        cache.stop();
        cache.start(worker, [] { return .5; });
        runFor(40);
        require(cache.value() == .25 && notifications == 1,
                "Resuming bypassed the one-minute refresh limit");
        cache.stop();
        require(cache.value() == .25,
                "Stopping discarded the pace needed by sleep and saved estimates");
    }
    {
        ReadingPaceCache cache(40);
        // Delay the first query past the refresh interval. The next query must
        // still wait, even though the original request has been queued longer.
        QSemaphore blocked, unblock;
        kobalt::qt::invoke(
                worker,
                [&] {
                    blocked.release();
                    unblock.acquire();
                },
                Qt::QueuedConnection);
        waitFor([&] { return blocked.available() == 1; });
        std::atomic_int calls{ 0 };
        std::atomic_bool wrongThread{ false };
        std::atomic<double> result{ .25 };
        int notifications = 0;
        cache.changed = [&] {
            require(QThread::currentThread() == app.thread(),
                    "Pace notification left the GUI thread");
            ++notifications;
        };
        QElapsedTimer spacing;
        spacing.start();
        std::atomic<qint64> previous{ -1 };
        std::atomic_bool tooSoon{ false };
        cache.start(worker, [&] {
            if (QThread::currentThread() != &thread) {
                wrongThread = true;
            }
            const auto now = spacing.elapsed();
            const auto before = previous.exchange(now);
            if (before >= 0 && now - before < 40) {
                tooSoon = true;
            }
            ++calls;
            return result.load();
        });
        runFor(60);
        require(calls == 0, "A query bypassed the blocked worker");
        unblock.release();
        waitFor([&] { return cache.value() == .25; });
        for (const double invalid : { -1.0, 0.0, std::numeric_limits<double>::infinity(),
                                      std::numeric_limits<double>::quiet_NaN() }) {
            result = invalid;
            const int before = calls;
            waitFor([&] { return calls > before; });
            runFor(3);
            require(cache.value() == .25 && notifications == 1,
                    "A failed refresh replaced the last valid pace");
        }
        result = .5;
        waitFor([&] { return cache.value() == .5; });
        const int before = calls;
        waitFor([&] { return calls > before; });
        runFor(3);
        cache.stop();
        require(notifications == 2, "Unchanged pace requested another paint");
        require(!tooSoon, "Refresh ran before the interval elapsed");
        require(!wrongThread, "Refresh ran outside the worker thread");
    }
    {
        ReadingPaceCache cache(30);
        QSemaphore entered, release;
        std::atomic_int calls{ 0 };
        int notifications = 0;
        cache.changed = [&] { ++notifications; };
        cache.start(worker, [&] {
            ++calls;
            entered.release();
            release.acquire();
            return .25;
        });
        waitFor([&] { return entered.available() == 1; });
        runFor(80);
        require(calls == 1, "A slow query accumulated requests");
        cache.stop();
        cache.start(worker, [&] {
            ++calls;
            return .5;
        });
        runFor(40);
        require(calls == 1, "Restart queued work behind the unfinished request");
        release.release();
        waitFor([&] { return cache.value() == .5; });
        cache.stop();
        require(notifications == 1 && calls == 2, "A stopped session published its late result");
    }
    {
        ReadingPaceCache cache;
        QSemaphore entered, release;
        cache.start(worker, [] { return .25; });
        waitFor([&] { return cache.value() == .25; });
        cache.clear();
        require(cache.value() == -1, "Account invalidation retained the old pace");
        cache.start(worker, [&] {
            entered.release();
            release.acquire();
            return .5;
        });
        waitFor([&] { return entered.available() == 1; });
        cache.clear();
        cache.start(worker, [] { return .75; });
        release.release();
        waitFor([&] { return cache.value() == .75; });
        cache.stop();
    }
    {
        QSemaphore entered, release;
        std::atomic_int calls{ 0 };
        kobalt::qt::invoke(
                worker,
                [&] {
                    entered.release();
                    release.acquire();
                },
                Qt::QueuedConnection);
        waitFor([&] { return entered.available() == 1; });
        ReadingPaceCache cache;
        cache.start(worker, [&] {
            ++calls;
            return .25;
        });
        cache.stop();
        release.release();
        runFor(30);
        require(calls == 0, "A cancelled queued request still accessed Nickel");
    }
    {
        QSemaphore entered, release;
        int notifications = 0;
        auto cache = std::make_unique<ReadingPaceCache>();
        cache->changed = [&] { ++notifications; };
        cache->start(worker, [&] {
            entered.release();
            release.acquire();
            return .25;
        });
        waitFor([&] { return entered.available() == 1; });
        cache.reset();
        release.release();
        runFor(30);
        require(notifications == 0, "A destroyed cache published a result");
    }
    {
        // A ticked cache has no timer: it queries at start, then only on a tick
        // that comes at least eleven twelfths of the interval later.
        int calls = 0;
        ReadingPaceCache cache(200, ReadingPaceCache::Schedule::Ticked);
        cache.start(worker, [&] {
            ++calls;
            return .25;
        });
        waitFor([&] { return cache.value() == .25; });
        runFor(400);
        require(calls == 1, "A ticked cache refreshed on its own timer");
        cache.tick();
        waitFor([&] { return calls == 2; });
        cache.tick();
        runFor(30);
        require(calls == 2, "A tick before the interval refreshed the pace");
        cache.stop();
        runFor(250);
        cache.tick();
        runFor(30);
        require(calls == 2, "A stopped cache refreshed on a tick");
    }
    thread.quit();
    thread.wait();
    std::puts(
            "PASS: pace reads never wait; background refresh, retry, pause, invalidation and destruction are bounded");
}
