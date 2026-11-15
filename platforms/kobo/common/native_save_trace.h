#pragma once
#include "support/diagnostics.h"
#include <atomic>
#include <time.h>

namespace kobalt::nickel {
// A queued save crosses threads. The job ID links the enqueue, worker stages
// and completion. Compare CPU samples only between stages on the same thread.
class NativeSaveTrace
{
public:
    explicit NativeSaveTrace(const char *job) : job(job)
    {
        if (diagnosticsActive()) {
            static std::atomic<unsigned> nextId{ 0 };
            id = ++nextId;
            stage("queued");
        }
    }

    void stage(const char *name) const
    {
        if (!id)
            return;
        auto fields = qt::object({ { "job_id", double(id) },
                                   { "job", QString::fromLatin1(job) },
                                   { "stage", QString::fromLatin1(name) } });
#ifdef CLOCK_THREAD_CPUTIME_ID
        timespec cpu{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0)
            fields["thread_cpu_ms"] = double(cpu.tv_sec) * 1000 + cpu.tv_nsec / 1e6;
#endif
        diagnostic("nickel-session-save-stage", fields, false);
    }

private:
    const char *job;
    unsigned id = 0;
};
} // namespace kobalt::nickel
