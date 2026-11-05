#pragma once
#include <QElapsedTimer>
#include <functional>

namespace kobalt {
// Kobo can suspend the kernel while leaving the reading page visible. Count
// that time, but let the reader's explicit sleep/hide boundaries pause it.
class ReadingTimer
{
public:
    // A negative result means the clock could not be read. The optional source
    // also lets regression checks replay standby without suspending the host.
    using Clock = std::function<qint64()>;
    // The suspend-inclusive clock on Kobo, or none elsewhere. KOSync's
    // intervals use it too.
    static Clock platformClock();
    explicit ReadingTimer(Clock clock = platformClock());
    void start();
    void invalidate();

    bool isValid() const { return fallback.isValid(); }

    bool includesStandby() const { return baseline >= 0; }

    qint64 elapsed() const;

private:
    Clock clock;
    QElapsedTimer fallback;
    qint64 baseline = -1;
    mutable qint64 lastElapsed = 0;
    mutable bool failed = false;
};
} // namespace kobalt
