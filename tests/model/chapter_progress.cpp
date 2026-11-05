#include "reading/word_estimates.h"
#include "check.h"
#include <cmath>
#include <utility>

using namespace kobalt;

namespace {
ReadingActivity activity(int page, int pages, ChapterSelection chapter,
                         QVector<ChapterLink> chapters = {})
{
    ReadingActivity result;
    result.page = page;
    result.pages = pages;
    result.chapter = chapter;
    result.chapters = std::move(chapters);
    return result;
}

bool same(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-12;
}

// Nickel's sync uploads floor(progress * 100) as ContentSourceProgressPercent.
int uploaded(double progress)
{
    return int(std::floor(progress * 100));
}
} // namespace

// Checks the chapter fractions Kobo writes to Nickel, `CurrentChapterProgress` and the chapter row value; an unknown layout writes nothing.
int main()
{
    const QVector<ChapterLink> chapters{ { "One", "a", 4, 0, 0 },
                                         { "Two", "b", 12, 0, 0 },
                                         { "Three", "c", 21, 0, 0 } };
    // Nickel's reader divides the pages before the current one by the
    // chapter's pages: page 5 of 8 is 0.5, page 6 of 7 is 0.714.
    require(same(wordEstimates::chapterProgress(activity(15, 30, { 0, 1, 5, 8 }, chapters)), 0.5),
            "Page 5 of 8 is half of the chapter");
    require(same(wordEstimates::chapterProgress(activity(17, 30, { 1, 2, 6, 7 }, chapters)),
                 5.0 / 7.0),
            "Page 6 of 7 is five sevenths of the chapter");
    require(uploaded(wordEstimates::chapterProgress(activity(28, 30, { 2, -1, 9, 9 }, chapters)))
                    == 88,
            "Page 9 of 9 uploads as 88 percent, as Nickel's reader does");
    require(same(wordEstimates::chapterProgress(activity(4, 30, { 0, 1, 1, 8 }, chapters)), 0.0),
            "The first page of a chapter is its start");
    // Nickel's firmware 4 chapter row counts the current page as read: page 5
    // of 9 is 55, page 7 of 9 is 77, as observed in Nickel's own reader.
    require(uploaded(wordEstimates::chapterReadThroughPage(
                    activity(15, 30, { 0, 1, 5, 9 }, chapters)))
                    == 55,
            "Page 5 of 9 stores 55 percent on the chapter row");
    require(uploaded(wordEstimates::chapterReadThroughPage(
                    activity(17, 30, { 0, 1, 7, 9 }, chapters)))
                    == 77,
            "Page 7 of 9 stores 77 percent on the chapter row");
    // The last page follows from the same formula; it was not observed in Nickel.
    require(same(wordEstimates::chapterReadThroughPage(activity(28, 30, { 2, -1, 9, 9 }, chapters)),
                 1.0),
            "The last page of a chapter reads it through");
    require(wordEstimates::chapterReadThroughPage(activity(30, 30, { 0, 1, 5, 9 }, chapters)) < 0,
            "An unknown layout has no chapter row value");
    // Text before the first table-of-contents entry counts as its own chapter.
    require(same(wordEstimates::chapterProgress(activity(0, 30, { -1, 0, 0, 0 }, chapters)), 0.0),
            "The first page of the front matter is its start");
    require(same(wordEstimates::chapterProgress(activity(2, 30, { -1, 0, 0, 0 }, chapters)), 0.5),
            "Page 3 of 4 front-matter pages is half of the front matter");
    require(same(wordEstimates::chapterProgress(activity(9, 10, { -1, -1, 0, 0 })), 0.9),
            "A book without chapters uses the whole book");
    // No word counts are needed.
    auto counted = activity(15, 30, { 0, 1, 5, 8 }, chapters);
    counted.totalWords = -1;
    require(same(wordEstimates::chapterProgress(counted), 0.5),
            "Chapter progress needed word counts");
    // An unknown or inconsistent layout writes nothing.
    require(wordEstimates::chapterProgress(activity(-1, 30, { 0, 1, 5, 8 }, chapters)) < 0,
            "Unknown page was accepted");
    require(wordEstimates::chapterProgress(activity(30, 30, { 0, 1, 5, 8 }, chapters)) < 0,
            "Page past the end was accepted");
    require(wordEstimates::chapterProgress(activity(0, 0, { 0, 1, 5, 8 }, chapters)) < 0,
            "Empty layout was accepted");
    require(wordEstimates::chapterProgress(activity(15, 30, { 0, 1, 0, 8 }, chapters)) < 0,
            "Page 0 of a chapter was accepted");
    require(wordEstimates::chapterProgress(activity(15, 30, { 0, 1, 9, 8 }, chapters)) < 0,
            "Page after the chapter was accepted");
    require(wordEstimates::chapterProgress(activity(15, 30, { 5, -1, 1, 8 }, chapters)) < 0,
            "Unknown chapter was accepted");
}
