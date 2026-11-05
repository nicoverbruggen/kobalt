#pragma once

#include <cstdint>

namespace kobalt::nickel {
// Stop polling a stalled Nickel startup without rejecting a later completion.
// The caller can retry from the completion callback or a book-opening hook.
class NativeStartupWait
{
public:
    enum class Result { Waiting, TimedOut, Ready, Recovered };

    Result update(bool ready, std::int64_t elapsedMilliseconds)
    {
        if (ready) {
            const bool recovered = timedOut;
            timedOut = false;
            return recovered ? Result::Recovered : Result::Ready;
        }
        if (!timedOut && elapsedMilliseconds >= 30000) {
            timedOut = true;
            return Result::TimedOut;
        }
        return Result::Waiting;
    }

private:
    bool timedOut = false;
};
} // namespace kobalt::nickel
