// Scenarios of the chapter-mode parity test. Each runs its actions on the
// whole book and in chapter mode and compares what the reader shows after
// each one. Append a scenario here; the actions are in chapter_parity.cpp.
// Run one scenario: kobalt-chapter-parity-tests <part of its name>.
namespace {
QVector<Scenario> scenarios()
{
    return {
        { "links",
          "links",
          { // A note without markup is a note only by its target. Chapter
            // mode guesses its kind until it is tapped.
            linkKinds({ "legacy note" }), followLink("short note"), followLink("long note"),
            followLink("legacy note"), followLink("oversized note"), followLink("hidden note"),
            followLink("missing passage"), followLink("nearby passage"), back(),
            followLink("later passage"), followLink("Return through a link"), back(), back() } },
        { "chapters and turns",
          "trevelyan",
          { gotoChapter(9), gotoChapter(1), gotoChapter(2), turn(30), turn(-30) } },
        { "search",
          "trevelyan",
          { gotoChapter(2), search("Trevelyan"), openResult(-1), search("the"), openResult(300),
            search("wine. It was a good"), search("Tanya's protestations"), search("zzqxv"),
            search("Trevelyan", "the"), openResult(5) } },
        { "bookmarks and highlights",
          "trevelyan",
          { gotoChapter(4), turn(2), addBookmark(), gotoChapter(8), openBookmark(), gotoChapter(6),
            turn(1), addHighlight("the"), gotoChapter(2), openHighlight() } },
        { "font size and reopen",
          "trevelyan",
          { gotoChapter(5), turn(3), setFontSize(6), saveAndReopen() } },
        { "scrubber",
          "trevelyan",
          { gotoChapter(5), skip(scrubTo(0.5), "owned by the fast-surfaces work") } },
    };
}
} // namespace
