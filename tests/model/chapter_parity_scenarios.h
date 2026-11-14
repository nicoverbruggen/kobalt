// Scenarios of the chapter-mode parity test. Each runs its actions on the
// whole book and in chapter mode and compares what the reader shows after
// each one. Append a scenario here; the actions are in chapter_parity.cpp.
// Run one scenario: kobalt-chapter-parity-tests <part of its name>.
namespace {
Action chapterSlicePages(int pages)
{
    return { QString("setChapterSlices(%1)").arg(pages), [pages](Reader &reader) -> QString {
                if (reader.model->bookFraction() >= 0)
                    reader.model->setChapterSlices(pages);
                return {};
            } };
}

QVector<Scenario> scenarios()
{
    return {
        { "links",
          "links",
          { // A note without markup is a note only by its target. Chapter
            // mode guesses its kind until it is tapped.
            linkKinds({ "legacy note" }), followLink("short note"), followLink("long note"),
            followLink("legacy note"), followLink("oversized note"), followLink("hidden note"),
            followLink("missing passage", true), followLink("nearby passage"), back(),
            followLink("later passage"), followLink("Return through a link"), back(), back() } },
        { "border above a chapter slice",
          "trevelyan",
          { chapterSlicePages(2), gotoChapter(3), setFontSize(2), turn(-2), turn(-2) } },
        { "border before a vertical chapter slice",
          "bordered vertical text",
          { chapterSlicePages(2),
            { "rotate to landscape", [](Reader &reader) -> QString {
                 return reader.model->applyAppearance({}, { 1072, 758 }) ? QString()
                                                                        : "error: rotation failed";
             } },
            turn(4), turn(1), turn(-1) } },
        { "line spacing on the final chapter slice",
          "trevelyan",
          { chapterSlicePages(7), setFontSize(8), gotoChapter(3), turn(-1),
            changeLayout("setLineSpacing(140)", { { "line_spacing", 140 } }) } },
        { "chapters and turns",
          "trevelyan",
          { gotoChapter(9), gotoChapter(1), gotoChapter(2), turn(30), turn(-30) } },
        { "search",
          "trevelyan",
          { gotoChapter(2), search("Trevelyan"), openResult(-1), search("the"), openResult(300),
            search("wine. It was a good"), search("Tanya's protestations"), search("zzqxv"),
            search("Trevelyan", "the"), openResult(5) } },
        { "search ahead replacement", "trevelyan", { searchAfterAhead() } },
        { "search cancellation", "trevelyan", { gotoChapter(2), searchCancellation() } },
        { "exclusive endpoints", "trevelyan", { exclusiveEndPages() } },
        { "whole-book words", "trevelyan", { wholeBookWords() } },
        { "session lists", "trevelyan", { sessionLists() } },
        { "contents list", "trevelyan", { gotoChapter(4), contentsList() } },
        { "bookmarks and highlights",
          "trevelyan",
          { gotoChapter(4), turn(2), addBookmark(), gotoChapter(8), savedList("bookmark"),
            openBookmark(), gotoChapter(6), turn(1), addHighlight("the"), gotoChapter(2),
            savedList("highlight"), openHighlight() } },
        { "slice page lookup", "trevelyan", { pagesOfOtherSlice() } },
        { "font size and reopen",
          "trevelyan",
          { gotoChapter(5), turn(3), setFontSize(6), saveAndReopen() } },
        { "backward chapter boundary", "trevelyan", { backwardChapterBoundary() } },
        { "prepared chapter boundary",
          "trevelyan",
          { jumpToBoundaryChapter(), prepareChapterBoundary(), cachedBoundaryTurn(-1),
            cachedBoundaryTurn(1), cachedBoundaryTurn(-1), cachedBoundaryTurn(1) } },
        { "cold chapter boundary",
          "trevelyan",
          { jumpToBoundaryChapter(), cachedBoundaryTurn(-1, false, false),
            cachedBoundaryTurn(1, false), cachedBoundaryTurn(-1, false),
            cachedBoundaryTurn(1, false) } },
        { "grouped chapter boundaries", "trevelyan", { groupedChapterBoundaries() } },
        { "cancelled chapter preparation",
          "trevelyan",
          { jumpToBoundaryChapter(), prepareChapterBoundary(true), cachedBoundaryTurn(-1),
            cachedBoundaryTurn(1) } },
        { "prepared chapter reflow",
          "trevelyan",
          { jumpToBoundaryChapter(), prepareChapterBoundary(), setFontSize(6),
            cachedBoundaryTurn(-1), cachedBoundaryTurn(1) } },
        { "adjacent page previews", "trevelyan", { adjacentPagePreviews() } },
        { "later-slice interactions",
          "trevelyan",
          { laterSlice(), selectWord(), revisitHighlight(),
            changeLayout("setMargins(40)",
                         { { "margin_pixels", 40 },
                           { "top_margin_pixels", 40 },
                           { "bottom_margin_pixels", 40 } }),
            changeLayout("setLineSpacing(180)", { { "line_spacing", 180 } }), saveAndReopen(),
            realBookEnd() } },
        { "switch rendering mode",
          "trevelyan",
          { laterSlice(), selectWord(), switchModes(), turn(1), turn(-1) } },
        { "scrubber",
          "trevelyan",
          { gotoChapter(5), skip(scrubTo(0.5), "Percentage scrubbing is unavailable in Faster") } },
    };
}
} // namespace
