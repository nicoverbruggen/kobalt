#include "platforms/kobo/common/native_startup_wait.h"
#include <cstdio>
#include <initializer_list>

#define CHECK(value)                                                 \
    do {                                                             \
        if (!(value)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
            return 1;                                                \
        }                                                            \
    } while (0)

// Checks the shared Qt5 and Qt6 startup wait: ready before 30 seconds, one timeout at 30 seconds and later recovery, with no sleeping.
int main()
{
    using kobalt::nickel::NativeStartupWait;
    using Result = NativeStartupWait::Result;
    NativeStartupWait normal;
    CHECK(normal.update(false, 0) == Result::Waiting);
    CHECK(normal.update(false, 29999) == Result::Waiting);
    CHECK(normal.update(true, 29999) == Result::Ready);
    CHECK(normal.update(true, 30000) == Result::Ready);

    NativeStartupWait boundary;
    CHECK(boundary.update(true, 30000) == Result::Ready);
    CHECK(boundary.update(true, 31066) == Result::Ready);

    // Both captured slow starts completed about 31.3 seconds into this wait.
    for (const auto completion : { 31305, 31284 }) {
        NativeStartupWait slow;
        CHECK(slow.update(false, 29999) == Result::Waiting);
        CHECK(slow.update(false, 30000) == Result::TimedOut);
        CHECK(slow.update(false, 30120) == Result::Waiting);
        CHECK(slow.update(true, completion) == Result::Recovered);
        CHECK(slow.update(true, completion + 250) == Result::Ready);
    }

    NativeStartupWait stalled;
    CHECK(stalled.update(false, 30000) == Result::TimedOut);
    CHECK(stalled.update(false, 60000) == Result::Waiting);
    CHECK(stalled.update(false, 3600000) == Result::Waiting);
    CHECK(stalled.update(true, 3600001) == Result::Recovered);
    CHECK(stalled.update(true, 3600251) == Result::Ready);
    return 0;
}
