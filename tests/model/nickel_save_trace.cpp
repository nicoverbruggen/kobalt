#include "platforms/kobo/common/native_save_trace.h"
#include "check.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QVector>

// Checks that a Nickel save logs one native-save-slow warning with its stages
// once it passes its threshold, also without detailed diagnostics, and stays
// silent while it is quick.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "Diagnostics directory");
    kobalt::initializeDiagnostics(directory.path(), false);
    require(!kobalt::diagnosticsActive(), "The check runs without detailed diagnostics");
    QVector<QJsonObject> slow;
    kobalt::setDiagnosticObserver([&slow](const QJsonObject &event) {
        if (event.value("event").toString() == QStringLiteral("native-save-slow"))
            slow.append(event);
    });

    {
        // Far below its threshold: no warning.
        const kobalt::nickel::NativeSaveTrace trace("quick", 60000);
        trace.stage("begin");
        trace.stage("finished");
    }
    {
        // Every save passes a threshold of zero. A copy, as the worker gets,
        // adds to the same record.
        const kobalt::nickel::NativeSaveTrace trace("stop-events", 0);
        const auto worker = trace;
        worker.stage("begin");
        worker.stage("commit");
        worker.stage("finished");
    }
    require(kobalt::flushDiagnostics(5000), "Diagnostics flush");
    kobalt::setDiagnosticObserver({});
    require(slow.size() == 1, "One slow save logs one warning");
    const auto event = slow.first();
    require(event.value("job").toString() == QStringLiteral("stop-events"),
            "The warning names its job");
    require(event.value("severity").toString() == QStringLiteral("warning"),
            "The slow save is a warning");
    const auto stages = event.value("stages").toArray();
    require(stages.size() == 4, "The warning lists every stage");
    QStringList names;
    double last = -1;
    for (const auto &value : stages) {
        const auto stage = value.toObject();
        names.append(stage.value("stage").toString());
        require(stage.value("at_ms").toDouble() >= last, "Stage times go forward");
        last = stage.value("at_ms").toDouble();
        require(stage.contains("thread_cpu_ms"), "Each stage has the thread's CPU time");
    }
    require(names == QStringList({ "queued", "begin", "commit", "finished" }),
            "The stages keep their order");
    std::printf("PASS: slow Nickel saves log their stages\n");
    return 0;
}
