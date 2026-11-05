#include "reading_timer.h"
#include "support/diagnostics.h"
#include <time.h>
#include <utility>

namespace kobalt {
ReadingTimer::Clock ReadingTimer::platformClock()
{
#if defined(KOBALT_READING_CLOCK_BOOTTIME) && defined(CLOCK_BOOTTIME)
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

ReadingTimer::ReadingTimer(Clock source) : clock(std::move(source))
{
    // A default QElapsedTimer is not initialized on Kobo's Qt 5.2.
    fallback.invalidate();
}

void ReadingTimer::start()
{
    fallback.start();
    baseline = clock ? clock() : -1;
    lastElapsed = 0;
    failed = false;
    if (clock && baseline < 0)
        diagnostic("reading-clock-fallback", {}, false);
}

void ReadingTimer::invalidate()
{
    fallback.invalidate();
    baseline = -1;
    lastElapsed = 0;
    failed = false;
}

qint64 ReadingTimer::elapsed() const
{
    if (!isValid())
        return 0;
    if (baseline < 0)
        return fallback.elapsed();
    const auto current = clock();
    if (current < baseline) {
        if (!failed)
            diagnostic("reading-clock-read-failed", {}, kobalt::DiagnosticSeverity::Error);
        failed = true;
        // Keep the last valid duration until this clock recovers. Switching to
        // the fallback here would mix clock origins and discard standby time.
        return lastElapsed;
    }
    failed = false;
    lastElapsed = qMax(lastElapsed, current - baseline);
    return lastElapsed;
}
} // namespace kobalt
