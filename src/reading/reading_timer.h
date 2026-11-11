#pragma once
#include <QElapsedTimer>
#include <functional>

namespace kobalt {
// Elapsed time in one clock domain. The owner supplies a source with the
// required suspend behavior and controls the start and pause boundaries.
// A missing or initially failed source selects QElapsedTimer for that segment.
class ReadingTimer
{
public:
    // A negative result means the clock could not be read. The optional source
    // also lets regression checks replay standby without suspending the host.
    using Clock = std::function<qint64()>;
    explicit ReadingTimer(Clock clock = {});
    void start();
    void invalidate();

    bool isValid() const { return fallback.isValid(); }

    bool usesHostClock() const { return baseline >= 0; }

    qint64 elapsed() const;

private:
    Clock clock;
    QElapsedTimer fallback;
    qint64 baseline = -1;
    mutable qint64 lastElapsed = 0;
    mutable bool failed = false;
};
} // namespace kobalt
