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
        QElapsedTimer timer;
        timer.start();
        const bool handled = QObject::event(event);
        const qint64 microseconds = timer.nsecsElapsed() / 1000;
        // An empty job is not engine work.
        if (microseconds >= 200)
            QThread::usleep(static_cast<unsigned long>(microseconds * (factor - 1)));
        return handled;
    }
};
} // namespace

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
