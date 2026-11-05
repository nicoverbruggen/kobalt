#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QThread>
#include <QEventLoop>
#include <atomic>
#include <cstdlib>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QThread thread;
    QObject worker;
    worker.moveToThread(&thread);
    thread.start();
    std::atomic<int> observed{ 0 };
    if (!kobalt::qt::invoke(
                &worker,
                [&] {
                    if (QThread::currentThread() != &thread)
                        std::abort();
                    observed = 1;
                },
                Qt::BlockingQueuedConnection)
        || observed != 1)
        std::abort();
    QObject *discarded = new QObject;
    kobalt::qt::invoke(discarded, [&] { observed = 9; }, Qt::QueuedConnection);
    delete discarded;
    QEventLoop loop;
    kobalt::qt::later(0, &loop, [&] { loop.quit(); });
    loop.exec();
    if (observed != 1)
        std::abort();
    const auto object = kobalt::qt::object(
            { { "count", qint64(42) }, { "title", "Chapter" }, { "enabled", true } });
    if (object.value("count").toDouble() != 42 || object.value("title").toString() != "Chapter"
        || !object.value("enabled").toBool())
        std::abort();
    thread.quit();
    thread.wait();
    return 0;
}
