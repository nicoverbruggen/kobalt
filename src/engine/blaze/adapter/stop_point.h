#pragma once
#include "engine/engine_worker.h"
#include <functional>
#if __has_include(<koblaze.h>)
#  include <koblaze.h>
#endif

namespace kobalt {
// A pass over a document calls this for each node. While a document is
// opened ahead and other work waits, it throws KoblazeRenderCancelled, and
// the document is dropped. In every other case it does nothing.
inline void stopPoint()
{
#ifdef KOBLAZE_CANCEL_CHECK
    static unsigned steps = 0;
    if ((++steps & 31) == 0)
        koblazeCheckCancel();
#endif
}

#ifdef KOBLAZE_CANCEL_CHECK
// True when work that is done ahead must stop: `context` is the "still
// current" function of the job, a std::function<bool()>. Called every few
// nodes, so it is also where a slowed-down engine waits.
inline bool stopRequested(void *context)
{
    const auto &current = *static_cast<const std::function<bool()> *>(context);
    if (current && !current())
        return true;
    engineSlowdownPause();
    return current && !current();
}

// While it lives, the passes of the engine over a document ask
// stopRequested() and throw KoblazeRenderCancelled when it says so.
struct StopCheck
{
    explicit StopCheck(const std::function<bool()> &current) : previous(active())
    {
        // A nested chapter load must keep the caller's cancellation too.
        stillCurrent = [this, current] {
            return (!current || current())
                    && (!previous || previous->stillCurrent());
        };
        active() = this;
        install(this);
    }
    ~StopCheck()
    {
        active() = previous;
        install(previous);
    }
    static bool cancelled() { return active() && !active()->stillCurrent(); }
    // For work that pauses on its own thread while another thread uses the
    // engine: suspend() takes the check of this thread out of the engine,
    // resume() puts it back.
    static void suspend() { install(nullptr); }
    static void resume() { install(active()); }
    bool requested() const
    {
        return stopRequested(const_cast<std::function<bool()> *>(&stillCurrent));
    }
    StopCheck(const StopCheck &) = delete;
    StopCheck &operator=(const StopCheck &) = delete;

private:
    static StopCheck *&active()
    {
        static thread_local StopCheck *value = nullptr;
        return value;
    }
    static void install(StopCheck *check)
    {
        koblazeSetCancelCheck(check ? &stopRequested : nullptr,
                             check ? &check->stillCurrent : nullptr);
    }
    StopCheck *previous;
    std::function<bool()> stillCurrent;
};
#endif
inline bool engineCancellationRequested()
{
#ifdef KOBLAZE_CANCEL_CHECK
    return StopCheck::cancelled();
#else
    return false;
#endif
}
} // namespace kobalt
