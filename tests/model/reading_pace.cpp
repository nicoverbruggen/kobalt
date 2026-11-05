#include "reading/reading_pace.h"
#include <cstdio>
#include "check.h"

int main()
{
    kobalt::ReadingPace pace;
    constexpr qint64 minute = 60 * 1000;
    require(pace.estimate(10, 30 * minute) == -1, "A session without turns has a pace");
    pace.observe(40, false);
    pace.observe(41, true);
    pace.observe(42, true);
    require(pace.forwardTurns() == 2 && pace.estimate(10, 30 * minute) == -1,
            "Two turns gave a pace");
    // A jump, a backward turn and a repeated frame do not count as reading.
    pace.observe(80, false);
    pace.observe(79, true);
    pace.observe(79, true);
    require(pace.forwardTurns() == 2, "A jump or a backward turn counted as a read page");
    pace.observe(80, true);
    require(pace.forwardTurns() == 3, "A forward turn after a jump was not counted");
    require(pace.estimate(10, 5 * minute - 1) == -1, "Less than five minutes gave a pace");
    require(pace.estimate(10, 6 * minute) == 20 * minute,
            "Pace is not reading time per forward turn");
    require(pace.estimate(0, 6 * minute) == 0 && pace.estimate(-1, 6 * minute) == -1,
            "Remaining pages were not bounded");
    std::puts(
            "PASS: session pace from forward turns, with the five-minute and three-turn threshold");
}
