#pragma once
#include "reading/word_estimates.h"

namespace kobalt::nickel {
using wordEstimates::pageWords;
using wordEstimates::ReadingChapter;
using wordEstimates::readingChapter;
using wordEstimates::readingEstimates;

class PageReadingActivity
{
public:
    void start(const ReadingActivity &activity) { previous = activity; }

    // Preserve page activity for jumps, but give them no words to train pace.
    // A normal turn reports the chapter average for the page just left.
    bool observe(const ReadingActivity &activity, int *words)
    {
        const auto before = previous;
        previous = activity;
        if (activity.position == before.position)
            return false;
        const bool turn = activity.turnDirection != 0 && activity.pages == before.pages
                && std::abs(qint64(activity.page) - before.page) == 1;
        *words = turn ? pageWords(before) : 0;
        return true;
    }

    const ReadingActivity &current() const { return previous; }

private:
    ReadingActivity previous;
};
} // namespace kobalt::nickel
