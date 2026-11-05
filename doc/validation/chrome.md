# Reader chrome checks

Label `chrome`. Build `mac-tests`, then run `ctest --preset mac -L chrome` from `platforms/desktop/`. `ctest --preset mac -N -L chrome` lists its tests.

These checks cover the header and footer, the reading strips, the status strip, battery and reading-time fields, the clock and its timers, time-remaining text, the 150% interface text layouts and the return header. `model.header-footer` holds the layout model. The `lifecycle` checks drive the production reader with a test clock, and the `desktop` and `ui` checks measure the strips and the return header in pixels.

Set `KOBALT_READING_TIME_CAPTURE_DIR` to an absolute directory to capture the status strip in `lifecycle.readingTimeStatus`. Set `KOBALT_CAPTURE_HEADER_FOOTER` to a workspace directory to save the strip images of `ui.chapterPageSurvivesLongTitles`.

`lifecycle.minuteClock` and `lifecycle.idleTimers` check timers on the main thread only. Worker-thread timers and Kobo-only timers are out of scope. `platform.nickel-reading-pace-cache` checks the pace cache, but the reader coordinator's 65-second fallback timer has no automated check.

These headless checks do not establish physical-device refresh or behavior.
