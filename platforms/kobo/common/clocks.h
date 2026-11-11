#pragma once
#include <QtGlobal>
#include <functional>
#include <time.h>

namespace kobalt::nickel {
// Includes kernel standby. Explicit reader sleep still pauses reading time.
inline std::function<qint64()> bootTimeClock()
{
#ifdef CLOCK_BOOTTIME
    return [] {
        struct timespec now{};
        return clock_gettime(CLOCK_BOOTTIME, &now) == 0
                ? qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000
                : qint64(-1);
    };
#else
    return {};
#endif
}
} // namespace kobalt::nickel
