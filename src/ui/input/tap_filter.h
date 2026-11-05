#pragma once
#include <QtGlobal>

namespace kobalt::ui {
enum class InputClock { Pointer, Key };

// Compare press times, not delivery times: a slow paint can queue both taps.
// Qt touch and key timestamps can use different clocks across suspension.
// Compare different clocks using delivery time. Rejected taps do not extend
// the window after the last accepted navigation.
class TapFilter
{
public:
    bool accept(quint32 sourceTime, qint64 deliveredTime, InputClock clock = InputClock::Pointer)
    {
        if (seen) {
            const bool sameClock = clock == previousClock && sourceTime && previousSource;
            const auto elapsed = sameClock ? qint64(quint32(sourceTime - previousSource))
                                           : deliveredTime - previousDelivery;
            if (elapsed < 200 || (sameClock && elapsed >= 0x80000000LL))
                return false;
        }
        seen = true;
        previousSource = sourceTime;
        previousClock = clock;
        previousDelivery = deliveredTime;
        return true;
    }

private:
    bool seen = false;
    quint32 previousSource = 0;
    InputClock previousClock = InputClock::Pointer;
    qint64 previousDelivery = 0;
};
} // namespace kobalt::ui
