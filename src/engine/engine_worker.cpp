#include "engine/engine_worker.h"
#include "engine/engine_activity.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QThread>

namespace kobalt {
int engineSlowdown()
{
    static const int factor = qBound(1, qgetenv("KOBALT_KOBO_EMULATION").toInt(), 100);
    return factor;
}

namespace {
// The job that runs on the worker, and the time up to which its work is
// already slowed. Used on the worker thread only.
QElapsedTimer *jobTimer = nullptr;
qint64 jobSlowedTo = 0;

void slowDown(bool all)
{
    const int factor = engineSlowdown();
    if (factor <= 1 || !jobTimer)
        return;
    const qint64 work = jobTimer->nsecsElapsed() - jobSlowedTo;
    // Short work waits for the next call. An empty job is not engine work.
    if (work < (all ? 200000 : 1000000))
        return;
    QThread::usleep(static_cast<unsigned long>(work / 1000 * (factor - 1)));
    jobSlowedTo = jobTimer->nsecsElapsed();
}

// Runs each engine job, then waits so that the job takes engineSlowdown()
// times as long. The wait keeps the worker busy, as the work does on a slow
// device: the next job, also one for a tap, starts after it.
class WorkerObject : public QObject
{
public:
    bool event(QEvent *event) override
    {
        const int factor = engineSlowdown();
        if (factor <= 1)
            return QObject::event(event);
        // A job can run another one from a nested event loop.
        if (jobTimer)
            return QObject::event(event);
        QElapsedTimer timer;
        timer.start();
        jobTimer = &timer;
        jobSlowedTo = 0;
        const bool handled = QObject::event(event);
        slowDown(true);
        jobTimer = nullptr;
        return handled;
    }
};
} // namespace

void engineSlowdownPause()
{
    if (jobTimer && QThread::currentThread() == engineWorkerState()->thread)
        slowDown(false);
}

const std::shared_ptr<EngineWorker> &engineWorkerState()
{
    static const auto state = [] {
        auto state = std::make_shared<EngineWorker>();
        auto *thread = state->thread = new QThread(qApp);
        auto *object = state->object = new WorkerObject;
        object->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, object, &QObject::deleteLater);
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, [state] {
            engineActivity().pause(false);
            state->thread->quit();
            state->thread->wait();
            state->stopped.store(true);
        });
        thread->start();
        return state;
    }();
    return state;
}

QObject *engineWorker()
{
    return engineWorkerState()->object;
}
} // namespace kobalt
