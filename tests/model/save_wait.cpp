#include "support/save_wait.h"
#include "storage/record_writer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>
#include <functional>
#include <thread>
#include "check.h"

using namespace kobalt;

// Checks that save waits run on a report instead of a poll, and that the fallback timer covers a save that does not report.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    SaveWait wait;
    int runs = 0;
    bool pending = true;
    std::function<void()> check = [&] {
        ++runs;
        if (pending)
            wait.after(check);
    };
    wait.after(check);
    auto *fallback = wait.findChild<QTimer *>("rrSaveWaitFallback");
    require(fallback && fallback->isSingleShot() && fallback->isActive()
                    && fallback->interval() >= 1000,
            "A waiting check has no long fallback timer");
    QCoreApplication::processEvents();
    require(runs == 0 && wait.waiting(), "A check ran without a report");

    // Reports are delivered from the event loop, and several count as one.
    saves::settled();
    saves::settled();
    require(runs == 0, "A report ran the check inside the reporting call");
    QCoreApplication::processEvents();
    require(runs == 1 && wait.waiting() && fallback->isActive(),
            "Two reports did not run a still-waiting check once");

    // A report from another thread reaches the GUI thread.
    pending = false;
    std::thread([] { saves::settled(); }).join();
    QCoreApplication::processEvents();
    require(runs == 2 && !wait.waiting() && !fallback->isActive(),
            "A report from a worker thread did not finish the wait");
    saves::settled();
    QCoreApplication::processEvents();
    require(runs == 2, "A finished check ran again");

    // The profile writer reports each write after it stops running.
    storage::RecordWriter writer;
    bool written = false, idle = false;
    writer.post([] { return true; }, [&](bool) { written = true; });
    std::function<void()> drained = [&] {
        if (writer.idle())
            idle = true;
        else
            wait.after(drained);
    };
    drained();
    QElapsedTimer timer;
    timer.start();
    while (!idle && timer.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    require(idle && written && wait.fallbacks() == 0,
            "The writer thread did not report the end of its write");

    // A save that never reports is found by the fallback timer.
    pending = true;
    wait.after(check);
    require(fallback->isActive(), "The fallback timer did not start for a new check");
    QMetaObject::invokeMethod(fallback, "timeout", Qt::DirectConnection);
    require(runs == 3 && wait.fallbacks() == 1 && wait.waiting() && fallback->isActive(),
            "The fallback timer did not run and keep a waiting check");
    pending = false;
    QMetaObject::invokeMethod(fallback, "timeout", Qt::DirectConnection);
    require(runs == 4 && !wait.waiting(), "The fallback timer did not finish the wait");

    // A destroyed waiter drops its checks.
    bool late = false;
    {
        SaveWait gone;
        gone.after([&] { late = true; });
        saves::settled();
    }
    QCoreApplication::processEvents();
    require(!late, "A check ran after its waiter was destroyed");
    std::puts("PASS: save waits follow reports, with a fallback timer");
}
