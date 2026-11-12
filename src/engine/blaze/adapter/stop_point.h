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
    engineSlowdownPause();
    const auto &current = *static_cast<const std::function<bool()> *>(context);
    return current && !current();
}

// While it lives, the passes of the engine over a document ask
// stopRequested() and throw KoblazeRenderCancelled when it says so.
struct StopCheck
{
    explicit StopCheck(const std::function<bool()> &current)
    {
        koblazeSetCancelCheck(&stopRequested,
                              const_cast<void *>(static_cast<const void *>(&current)));
    }
    ~StopCheck() { koblazeSetCancelCheck(nullptr, nullptr); }
    StopCheck(const StopCheck &) = delete;
    StopCheck &operator=(const StopCheck &) = delete;
};
#endif
} // namespace kobalt
