# Progress and statistics checks

Label `progress`. Build `mac-tests`, then run `ctest --preset mac -L progress` from `platforms/desktop/`. `ctest --preset mac -N -L progress` lists its tests.

These checks cover progress percentages, chapter progress, native progress, progress choices, finish prompts, reading pace and speed, word counts and Stats. The `model` checks hold the calculations, and the `lifecycle` checks drive the production reader. `session.reading-book`, in [Storage and history](storage.md), checks the saved finish prompt, the reading-time counter and when the reader reads native progress.

On Kobo, Nickel's sync and USB signals start a progress check. A failed read repeats at the next checks, up to three times after each signal. A one-minute read is only the fallback when those signals cannot be connected. The test bridge has no change signals, so the `lifecycle` checks use that fallback.

The progress checks include a changed native position with an unchanged or older nonzero timestamp and an unread reset that clears the timestamp and location. Each check reads the current committed native row. The reader must offer a conflict choice, and choosing the local position must allow the next save. A choice from an earlier alert must not acknowledge a different current position.

Same-timestamp KEPUB span locations that drop a positive character offset to zero or omit it keep the precise local position and allow the next save without a choice. Model and session checks retain conflicts for other changed positions, timestamps and statuses, and for normalization of an unacknowledged position. A genuine same-timestamp move to the same span start has the same representation and follows this rule.

Lifecycle checks also cover closing after an offset round trip or a conflict choice, saving a kept local position at the next sleep without a page turn, and delaying a progress choice until the image viewer closes.

These checks do not establish physical Kobo or Android behavior.

## Clara BW reading time

`session.reading-book` replays a Clara BW reading-time failure with a controlled boot clock. The fix was checked on a physical Clara BW with firmware 4.46.23836. In the first run, the counter recorded 382.248 seconds at the Stats check, including 330.891 seconds of kernel standby, and Stats showed 6 minutes. The page did not change during the run. Nickel saved 385 seconds when the book closed shortly afterwards. A second run checked deliberate sleep. The counter paused at 67.537 seconds, kept that total through about 6 minutes 46 seconds asleep and counted again after wake. Stats then showed 2 minutes. The final active total was 138.575 seconds, and Nickel saved 139 seconds across the two active intervals. Both native time writes were verified.
