#pragma once
#include "reading/reading_bridge.h"
#include <cmath>
#include <limits>
#include <utility>

namespace kobalt::wordEstimates {
struct ReadingChapter
{
    qint64 words = -1;
    int page = 0, pages = 0;
};

inline ReadingChapter readingChapter(const ReadingActivity &activity)
{
    const auto &selection = activity.chapter;
    if (activity.totalWords < 0 || activity.pages <= 0 || activity.page < 0
        || activity.page >= activity.pages)
        return {};
    if (selection.current < -1 || selection.current >= activity.chapters.size()
        || selection.next < -1 || selection.next >= activity.chapters.size())
        return {};
    if (selection.current >= 0 && selection.current < activity.chapters.size()) {
        if (selection.pages <= 0 || selection.page < 1 || selection.page > selection.pages)
            return {};
        return { activity.chapters[selection.current].words, selection.page, selection.pages };
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
    return { words, qBound(1, activity.page + 1, pages), pages };
}

// The current page within its chapter, from 1, and the chapter's pages. The
// page is 0 when the layout is unknown. Unlike readingChapter, it does not
// need word counts.
inline std::pair<int, int> chapterPage(const ReadingActivity &activity)
{
    const auto &selection = activity.chapter;
    if (activity.pages <= 0 || activity.page < 0 || activity.page >= activity.pages)
        return { 0, 0 };
    if (selection.current < -1 || selection.current >= activity.chapters.size()
        || selection.next < -1 || selection.next >= activity.chapters.size())
        return { 0, 0 };
    if (selection.current >= 0) {
        if (selection.pages <= 0 || selection.page < 1 || selection.page > selection.pages)
            return { 0, 0 };
        return { selection.page, selection.pages };
    }
    const int pages = selection.next >= 0 && selection.next < activity.chapters.size()
            ? qMax(1, activity.chapters[selection.next].page)
            : activity.pages;
    return { qBound(1, activity.page + 1, pages), pages };
}

// The part of the current chapter before this page, from 0 to 1, or -1 when
// the layout is unknown. Nickel's reader stores the same value as
// CurrentChapterProgress, and its sync uploads it as the chapter percentage.
inline double chapterProgress(const ReadingActivity &activity)
{
    const auto [page, pages] = chapterPage(activity);
    return page < 1 ? -1 : double(page - 1) / pages;
}

// The part of the current chapter up to the end of this page, from 0 to 1, or
// -1 when the layout is unknown. Nickel's firmware 4 reader stores this, as a
// whole percentage, on the KEPUB chapter's own row: page 5 of 9 is 55.
inline double chapterReadThroughPage(const ReadingActivity &activity)
{
    const auto [page, pages] = chapterPage(activity);
    return page < 1 ? -1 : double(page) / pages;
}

inline int pageWords(const ReadingActivity &activity)
{
    const auto chapter = readingChapter(activity);
    if (chapter.words < 0 || chapter.pages <= 0)
        return 0;
    return int(qMin<qint64>(chapter.words / chapter.pages, std::numeric_limits<int>::max()));
}

// Apply a supplied pace to source counts and the current layout. Learning and
// filtering belong to the caller.
inline ReadingEstimates readingEstimates(const ReadingActivity &activity, double secondsPerWord)
{
    ReadingEstimates result;
    const auto current = readingChapter(activity);
    if (current.words < 0 || current.pages <= 0 || !std::isfinite(secondsPerWord)
        || secondsPerWord <= 0)
        return result;
    const auto seconds = [secondsPerWord](qint64 words) -> qint64 {
        const double value = std::ceil(double(words) * secondsPerWord);
        return words >= 0 && std::isfinite(value) && value <= std::numeric_limits<int>::max()
                ? qint64(value)
                : -1;
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

} // namespace kobalt::wordEstimates
