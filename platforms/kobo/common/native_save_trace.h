#pragma once
#include "support/diagnostics.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <atomic>
#include <memory>
#include <time.h>

namespace kobalt::nickel {
// A queued save crosses threads. The job ID links the enqueue, worker stages
// and completion. Compare CPU samples only between stages on the same thread.
//
// With verbose diagnostics each stage is its own event. Every build also
// keeps the stages of a save in memory and logs them as one warning when
// the save took longer than slowMilliseconds, so a slow save in the field
// shows whether it waited (time passes, the thread's CPU time does not) or
// worked. Copies share one record; the stages of a save run one after the
// other, handed on through queued calls.
class NativeSaveTrace
{
public:
    static constexpr qint64 slowMilliseconds = 1000;

    explicit NativeSaveTrace(const char *job, qint64 slow = slowMilliseconds)
        : job(job), slow(slow), record(std::make_shared<Record>())
    {
        record->started.start();
        if (diagnosticsActive()) {
            static std::atomic<unsigned> nextId{ 0 };
            id = ++nextId;
        }
        stage("queued");
    }

    void stage(const char *name) const
    {
        const double cpu = threadMilliseconds();
        const qint64 at = record->started.elapsed();
        record->stages.append(qt::object({ { "stage", QString::fromLatin1(name) },
                                           { "at_ms", double(at) },
                                           { "thread_cpu_ms", cpu } }));
        if (id) {
            auto fields = qt::object({ { "job_id", double(id) },
                                       { "job", QString::fromLatin1(job) },
                                       { "stage", QString::fromLatin1(name) } });
            if (cpu >= 0)
                fields["thread_cpu_ms"] = cpu;
            diagnostic("nickel-session-save-stage", fields, false);
        }
        if (qstrcmp(name, "finished") == 0 && at >= slow)
            diagnostic("native-save-slow",
                       qt::object({ { "job", QString::fromLatin1(job) },
                                    { "total_ms", double(at) },
                                    { "stages", record->stages } }),
                       DiagnosticSeverity::Warning);
    }

private:
    struct Record
    {
        QElapsedTimer started;
        QJsonArray stages;
    };

    static double threadMilliseconds()
    {
#ifdef CLOCK_THREAD_CPUTIME_ID
        timespec cpu{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0)
            return double(cpu.tv_sec) * 1000 + cpu.tv_nsec / 1e6;
#endif
        return -1;
    }

    const char *job;
    qint64 slow;
    std::shared_ptr<Record> record;
    unsigned id = 0;
};
} // namespace kobalt::nickel
