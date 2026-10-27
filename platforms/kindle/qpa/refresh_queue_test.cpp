#include "refresh_queue.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
bool wait(const std::function<bool()> &done) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 4000) {
        if (done())
            return true;
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return false;
}
kindle::RefreshFrame frame(quint64 serial, quint64 epoch, int gray) {
    QImage pixels(8, 8, QImage::Format_Grayscale8);
    pixels.fill(gray);
    return {pixels, QRect(0, 0, 1, 1), serial, epoch, false, 1};
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    using namespace kindle;
    {
        QSemaphore entered, release;
        QVector<RefreshFrame> seen;
        RefreshQueue queue([&](const RefreshFrame &value) {
            seen.push_back(value);
            if (value.serial == 2) {
                entered.release();
                release.tryAcquire(1, 3000);
            }
            return RefreshResult{};
        });
        int completed = 0;
        QObject::connect(&queue, &RefreshQueue::completed, &app, [&](quint64, quint64) { ++completed; });
        check(queue.enqueue(frame(1, queue.epoch(), 10)), "Animation initial frame rejected");
        check(wait([&] { return completed == 1; }), "Animation initial frame did not finish");
        check(queue.enqueue(frame(2, queue.epoch(), 20)), "Animated frame rejected");
        check(wait([&] { return entered.tryAcquire(); }), "Animated frame did not enter driver");
        check(queue.enqueue(frame(3, queue.epoch(), 30)), "Pending animated frame rejected");
        queue.cancelAnimation();
        release.release();
        check(wait([&] { return completed == 3; }), "Animation cancellation did not settle");
        queue.stop();
        check(seen.size() == 3 && !seen[1].full && seen[1].animationDirection == 1 &&
              !seen[2].full && !seen[2].animationDirection,
              "Single page animation or pending cancellation was lost");
    }
    {
        QSemaphore entered, release;
        std::mutex seenMutex;
        QVector<RefreshFrame> seen;
        RefreshQueue queue([&](const RefreshFrame &value) {
            if (value.serial == 1) {
                entered.release();
                release.tryAcquire(1, 3000);
            }
            std::lock_guard<std::mutex> lock(seenMutex);
            seen.push_back(value);
            return RefreshResult{};
        });
        QVector<quint64> completed, dropped;
        QObject::connect(&queue, &RefreshQueue::completed, &app,
                         [&](quint64 serial, quint64) { completed.push_back(serial); });
        QObject::connect(&queue, &RefreshQueue::discarded, &app,
                         [&](quint64 serial) { dropped.push_back(serial); });
        auto first = frame(1, queue.epoch(), 10);
        auto retained = first.pixels;
        check(queue.enqueue(std::move(first)), "First frame rejected");
        check(wait([&] { return entered.tryAcquire(); }), "Driver did not start");
        retained.fill(99);
        auto second = frame(2, queue.epoch(), 20);
        second.damage = QRect(2, 2, 2, 2);
        second.full = true;
        check(queue.enqueue(second), "Second frame rejected");
        auto third = frame(3, queue.epoch(), 30);
        third.damage = QRect(6, 6, 1, 1);
        third.animationDirection = -1;
        check(queue.enqueue(third), "Latest frame rejected");
        bool timerRan = false;
        QTimer::singleShot(0, &app, [&] { timerRan = true; });
        check(wait([&] { return timerRan; }), "GUI timer blocked by driver wait");
        check(completed.isEmpty(), "Blocked driver completed prematurely");
        release.release();
        check(wait([&] { return completed.size() == 2; }), "Coalesced frames did not finish");
        queue.stop();
        check(completed == QVector<quint64>{1, 3} && dropped.contains(2), "Pending frame was not replaced");
        check(seen.size() == 2 && seen[0].pixels.constScanLine(0)[0] == 10,
              "GUI mutation changed in-flight pixels");
        check(seen[1].pixels.constScanLine(0)[0] == 30 && seen[1].full && !seen[1].animationDirection &&
              seen[1].damage.contains(QRect(2, 2, 2, 2)) && seen[1].damage.contains(QRect(6, 6, 1, 1)),
              "Coalescing lost pixels, damage, full refresh or animation cancellation");
    }
    {
        QSemaphore entered, release;
        std::atomic<unsigned> calls{0};
        RefreshQueue queue([&](const RefreshFrame &value) {
            ++calls;
            if (value.serial == 1) {
                entered.release();
                release.tryAcquire(1, 3000);
            }
            check(value.full && !value.animationDirection, "Epoch recovery did not force a clean frame");
            return RefreshResult{};
        });
        QVector<quint64> completed, dropped;
        QObject::connect(&queue, &RefreshQueue::completed, &app,
                         [&](quint64 serial, quint64) { completed.push_back(serial); });
        QObject::connect(&queue, &RefreshQueue::discarded, &app,
                         [&](quint64 serial) { dropped.push_back(serial); });
        check(queue.enqueue(frame(1, queue.epoch(), 10)), "Epoch first frame rejected");
        check(wait([&] { return entered.tryAcquire(); }), "Epoch driver did not start");
        check(queue.enqueue(frame(2, queue.epoch(), 20)), "Epoch pending frame rejected");
        const auto old = queue.epoch();
        queue.invalidate();
        check(!queue.enqueue(frame(4, old, 40)), "Stale epoch accepted");
        check(queue.enqueue(frame(3, queue.epoch(), 30)), "New epoch rejected");
        release.release();
        check(wait([&] { return completed.contains(3) && dropped.contains(1); }), "Epoch recovery did not settle");
        queue.stop();
        check(completed == QVector<quint64>{3} && dropped.contains(2) && calls == 2,
              "Stale queued work or completion survived invalidation");
    }
    {
        std::atomic<unsigned> calls{0};
        RefreshQueue queue([&](const RefreshFrame &value) {
            const auto call = ++calls;
            if (call == 2)
                return RefreshResult{false, false, QStringLiteral("busy")};
            if (call == 3)
                check(value.full && !value.animationDirection, "Idle retry lost full cleanup");
            return RefreshResult{};
        });
        int completed = 0;
        QObject::connect(&queue, &RefreshQueue::completed, &app, [&](quint64, quint64) { ++completed; });
        check(queue.enqueue(frame(1, queue.epoch(), 10)), "Retry first frame rejected");
        check(wait([&] { return completed == 1; }), "Retry first frame did not finish");
        check(queue.enqueue(frame(2, queue.epoch(), 20)), "Retry frame rejected");
        check(wait([&] { return completed == 2; }), "Failed submission did not retry without a paint");
        queue.stop();
        check(calls == 3, "Unexpected retry count");
    }
    {
        std::atomic<unsigned> calls{0};
        RefreshQueue queue([&](const RefreshFrame &) {
            ++calls;
            return RefreshResult{false, true, QStringLiteral("uncertain completion")};
        });
        bool failed = false;
        QObject::connect(&queue, &RefreshQueue::failed, &app, [&](const QString &) { failed = true; });
        check(queue.enqueue(frame(1, queue.epoch(), 10)), "Fatal test frame rejected");
        check(wait([&] { return failed; }), "Fatal driver failure not reported");
        check(!queue.enqueue(frame(2, queue.epoch(), 20)) && calls == 1, "Queue continued after fatal failure");
    }
    {
        QSemaphore entered, release;
        std::atomic<unsigned> calls{0};
        RefreshQueue queue([&](const RefreshFrame &) {
            ++calls;
            entered.release();
            release.tryAcquire(1, 3000);
            return RefreshResult{};
        });
        check(queue.enqueue(frame(1, queue.epoch(), 10)), "Shutdown first frame rejected");
        check(wait([&] { return entered.tryAcquire(); }), "Shutdown driver did not start");
        check(queue.enqueue(frame(2, queue.epoch(), 20)), "Shutdown pending frame rejected");
        std::thread completion([&] { QThread::msleep(20); release.release(); });
        queue.stop();
        completion.join();
        check(calls == 1 && !queue.enqueue(frame(3, queue.epoch(), 30)), "Shutdown submitted work after stopping");
    }
    std::puts("Kindle refresh ownership, coalescing, GUI responsiveness, epochs, retry and shutdown passed.");
}
