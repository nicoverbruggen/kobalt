#pragma once
#include "reading/reading_bridge.h"
#include <cmath>
#include <limits>

namespace kobalt::nickel {
struct ReadingChapter {
    qint64 words = -1;
    int page = 0, pages = 0;
};

inline ReadingChapter readingChapter(const ReadingActivity &activity) {
    const auto &selection = activity.chapter;
    if (activity.totalWords < 0 || activity.pages <= 0 || activity.page < 0 || activity.page >= activity.pages)
        return {};
    if (selection.current < -1 || selection.current >= activity.chapters.size() || selection.next < -1 ||
        selection.next >= activity.chapters.size())
        return {};
    if (selection.current >= 0 && selection.current < activity.chapters.size()) {
        if (selection.pages <= 0 || selection.page < 1 || selection.page > selection.pages)
            return {};
        return {activity.chapters[selection.current].words, selection.page, selection.pages};
    }
    // The source total includes text before the first table-of-contents entry.
    // Keep that text in the estimate even when it has no chapter of its own.
    qint64 words = activity.totalWords;
    for (const auto &chapter : activity.chapters) {
        if (chapter.words < 0 || chapter.words > words)
            return {};
        words -= chapter.words;
    }
    const int pages = selection.next >= 0 && selection.next < activity.chapters.size()
                          ? qMax(1, activity.chapters[selection.next].page)
                          : activity.pages;
    return {words, qBound(1, activity.page + 1, pages), pages};
}

inline int pageWords(const ReadingActivity &activity) {
    const auto chapter = readingChapter(activity);
    if (chapter.words < 0 || chapter.pages <= 0)
        return 0;
    return int(qMin<qint64>(chapter.words / chapter.pages, std::numeric_limits<int>::max()));
}

// `secondsPerWord` comes from Nickel. This only applies that pace to Kobalt's
// source counts and current layout; it does not learn or filter reading pace.
inline ReadingEstimates readingEstimates(const ReadingActivity &activity, double secondsPerWord) {
    ReadingEstimates result;
    const auto current = readingChapter(activity);
    if (current.words < 0 || current.pages <= 0 || !std::isfinite(secondsPerWord) || secondsPerWord <= 0)
        return result;
    const auto seconds = [secondsPerWord](qint64 words) -> qint64 {
        const double value = std::ceil(double(words) * secondsPerWord);
        return words >= 0 && std::isfinite(value) && value <= std::numeric_limits<int>::max() ? qint64(value) : -1;
    };
    const qint64 chapter = seconds(current.words);
    if (chapter < 0)
        return result;
    const double progress = double(current.page - 1) / current.pages;
    const qint64 left = chapter - qint64(std::ceil(double(chapter) * progress));
    qint64 book = left;
    const int next = activity.chapter.current < 0 ? 0 : activity.chapter.current + 1;
    for (int i = next; i < activity.chapters.size(); ++i) {
        const auto remaining = seconds(activity.chapters[i].words);
        if (remaining < 0 || remaining > std::numeric_limits<int>::max() - book)
            return result;
        book += remaining;
    }
    result.chapterMilliseconds = left * 1000;
    result.bookMilliseconds = book * 1000;
    return result;
}

class PageReadingActivity {
public:
    void start(const ReadingActivity &activity) {
        previous = activity;
    }
    // Preserve page activity for jumps, but give them no words to train pace.
    // A normal turn reports the chapter average for the page just left.
    bool observe(const ReadingActivity &activity, int *words) {
        const auto before = previous;
        previous = activity;
        if (activity.position == before.position)
            return false;
        const bool turn = activity.turnDirection != 0 && activity.pages == before.pages &&
                          std::abs(qint64(activity.page) - before.page) == 1;
        *words = turn ? pageWords(before) : 0;
        return true;
    }
    const ReadingActivity &current() const {
        return previous;
    }

private:
    ReadingActivity previous;
};
}
