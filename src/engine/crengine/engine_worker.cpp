#include "engine_worker.h"
#include "engine_activity.h"
#include <QCoreApplication>
#include <QThread>

namespace kobalt {
const std::shared_ptr<EngineWorker> &engineWorkerState()
{
    static const auto state = [] {
        auto state = std::make_shared<EngineWorker>();
        auto *thread = state->thread = new QThread(qApp);
        auto *object = state->object = new QObject;
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
