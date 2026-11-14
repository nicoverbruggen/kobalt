# Reader chrome checks

Label `chrome`. Build `mac-tests`, then run `ctest --preset mac -L chrome` from `platforms/desktop/`. `ctest --preset mac -N -L chrome` lists its tests.

These checks cover the header and footer, the reading strips, the status strip, battery and reading-time fields, the clock and its timers, time-remaining text, the 150% interface text layouts and the return header. `model.header-footer` holds the layout model. The `lifecycle` checks drive the production reader with a test clock, and the `desktop` and `ui` checks measure the strips and the return header in pixels.

Set `KOBALT_READING_TIME_CAPTURE_DIR` to an absolute directory to capture the status strip in `lifecycle.readingTimeStatus`. Set `KOBALT_CAPTURE_HEADER_FOOTER` to a workspace directory to save the strip images of `ui.chapterPageSurvivesLongTitles`.

`desktop.headerFooterChapterMode` opens the book fixture in chapter mode with the book page, the chapter page, the page turn time and the chapter load time in the strips. It waits for the asynchronous page estimate after the first frame, then checks that both page fields show a page and a total, and that forward turns add one page, also across a chapter start. It checks that both time fields show milliseconds and that the chapter title does not contain the load time. Set `KOBALT_CHAPTER_PAGE_CAPTURES` to an absolute directory to save the six-inch reader at a chapter start, in the middle of the chapter and at the start of the next chapter, with normal and 150% interface text.

`lifecycle.clockRestsWithoutConsumers` checks that the minute clock rests while no strip field, menu or host needs it, and that the menu starts it. With `lifecycle.clockAsksPaceForTimeLeft` it also checks that the reading pace is asked only for a visible consumer. `lifecycle.minuteClock` and `lifecycle.idleTimers` check timers on the main thread only. Worker-thread timers and Kobo-only timers are out of scope. `platform.nickel-reading-pace-cache` checks the pace cache, but the reader coordinator's 65-second fallback timer has no automated check.

`desktop.percentageFormat` checks the Settings choice, draft and save behavior, reopening Settings, header/footer previews and unchanged page counts and progress. The model checks cover invalid saved values, whole-percent defaults, fractional progress, rounding, unknown lengths, large books and the last page. The gallery includes the Settings row, choice dialog, reading page and header/footer preview.

These headless checks do not establish physical-device refresh or behavior.
