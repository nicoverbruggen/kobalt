# Progress and statistics checks

Label `progress`. Build `mac-tests`, then run `ctest --preset mac -L progress` from `platforms/desktop/`. `ctest --preset mac -N -L progress` lists its tests.

These checks cover progress percentages, chapter progress, native progress, progress choices, finish prompts, reading pace and speed, word counts and Stats. The `model` checks hold the calculations, and the `lifecycle` checks drive the production reader. `session.reading-book`, in [Storage and history](storage.md), checks the saved finish prompt, the reading-time counter and when the reader reads native progress.

On Kobo, Nickel's sync and USB signals start a progress check. A failed read repeats at the next checks, up to three times after each signal. A one-minute read is only the fallback when those signals cannot be connected. The test bridge has no change signals, so the `lifecycle` checks use that fallback.

The progress checks include a changed native position with an unchanged or older nonzero timestamp and an unread reset that clears the timestamp and location. Each check reads the current committed native row. The reader must offer a conflict choice, and choosing the local position must allow the next save. A choice from an earlier alert must not acknowledge a different current position.

Same-timestamp KEPUB span locations that drop a positive character offset to zero or omit it keep the precise local position and allow the next save without a choice. Model and session checks retain conflicts for other changed positions, timestamps and statuses, and for normalization of an unacknowledged position. A genuine same-timestamp move to the same span start has the same representation and follows this rule.

Lifecycle checks also cover closing after an offset round trip or a conflict choice, saving a kept local position at the next sleep without a page turn, and delaying a progress choice until the image viewer closes.

The `component.ui` assertions cover linear chapter lengths above the visible minimum height, one scale across graph pages, chart padding, and chart height at different densities and text sizes. The `chapterGraphLengths` gallery case shows unequal long chapters beside short ones in each supported viewport.

`engine.columns` checks a screen shared by two chapters in both reading directions. Chapter progress follows the source position at the start of the screen for both publication and supplied chapter lists. Jumping to the next chapter's source position selects that chapter even when it starts in the second column.

These checks do not establish physical Kobo or Android behavior.

`engine.large-toc` generates songbooks at 1,023, 1,024, 1,025, 9,999, 10,000 and 10,001 destinations. It checks the total bound, hierarchy, source-based titles, navigation, native-list fallback, graph lengths, word-count reuse, reflow and cached reopening. `desktop.largeToc` (label `input`) opens the real reader's last chapter-list page, selects Song 10,000 and checks that row widgets stay bounded by the visible page. Unindexed content beyond the cap retains the existing last-title fallback. These fixtures do not reproduce the original reported songbook.

## Clara BW reading time

`session.reading-book` replays a Clara BW reading-time failure with a controlled boot clock. The fix was checked on a physical Clara BW with firmware 4.46.23836. In the first run, the counter recorded 382.248 seconds at the Stats check, including 330.891 seconds of kernel standby, and Stats showed 6 minutes. The page did not change during the run. Nickel saved 385 seconds when the book closed shortly afterwards. A second run checked deliberate sleep. The counter paused at 67.537 seconds, kept that total through about 6 minutes 46 seconds asleep and counted again after wake. Stats then showed 2 minutes. The final active total was 138.575 seconds, and Nickel saved 139 seconds across the two active intervals. Both native time writes were verified.

Native saves carry typed results from each Nickel adapter through ReadingBook to the lifecycle caller. The checks distinguish verified persistence, transaction-entry contention, changed progress, unavailable services, deferred writes, rejected positions and unverified writes. Only verified saves advance the native baseline. Sleep retries transaction-entry contention within its existing budget, regardless of message wording. An unverified result with the former busy message does not trigger that retry. USB and shutdown retain their existing bounded retry policy. Checkpoint diagnostic strings remain compatible.

Native normalization is supplied by the host. The portable reconciler requires a choice for a changed position unless the host recognizes a normalization of the acknowledged baseline. The Nickel checks cover dropped KEPUB offsets in that direction only. A host without the rule keeps the conflict. Resetting the verified baseline retains the host rule. The bridge also decides whether an unmappable position permits an activity-only update; shared reading code does not classify EPUB paths for this decision.

Native activity methods report completed, not required, failed or abandoned outcomes. Adapters must implement both opening and completion obligations explicitly. A not-required opening does not change the saved native status. Revocation releases pending completion callbacks as abandoned and ignores late replies. Native cloud-sync policy and its explanation come from the host and remain available after revocation; shared code does not infer them from publication identifiers.

ReadingBook receives its reading clock from the host. Kobo supplies the suspend-inclusive boot clock; standalone hosts retain the Qt elapsed timer. Session checks verify injected time, explicit stop and resume, failed samples and recovery without changing clock origins. Shared reading code no longer selects a platform clock.

Development progress controls are supplied by the host. Lifecycle checks cover an absent capability, an unavailable book, each typed action and a failed simulation. The host supplies the description and receives the current progress; the shared reader does not select fixture paths or create native commands. Routine reports are opt-in; opening and lifecycle snapshots remain available. State reports include link geometry and reading estimates only when the host enables detailed reports.
