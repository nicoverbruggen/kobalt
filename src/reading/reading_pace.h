#pragma once
#include <QtGlobal>

namespace kobalt {
// Reading pace for the current session only: forward page turns over active
// reading time. The time and the turns reset together when the book is opened.
class ReadingPace
{
public:
    // Below this much reading, or this many forward turns, a pace is a guess.
    static constexpr qint64 minimumMilliseconds = 5 * 60 * 1000;
    static constexpr int minimumTurns = 3;

    // Count a forward turn once per page. Jumps and backward turns do not count.
    void observe(int page, bool forwardTurn)
    {
        if (forwardTurn && last >= 0 && page > last)
            ++turns;
        last = page;
    }

    int forwardTurns() const { return turns; }

    // Milliseconds to read `pages` more pages, or -1 when there is no pace yet.
    qint64 estimate(int pages, qint64 readingMilliseconds) const
    {
        if (pages < 0 || turns < minimumTurns || readingMilliseconds < minimumMilliseconds)
            return -1;
        return readingMilliseconds * pages / turns;
    }

private:
    int last = -1, turns = 0;
};
} // namespace kobalt
