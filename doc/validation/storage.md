# Storage and history checks

Label `storage`. Build `mac-tests`, then run `ctest --preset mac -L storage` from `platforms/desktop/`. `ctest --preset mac -N -L storage` lists its tests.

These checks cover the profile database, record writers, saves at close and sleep, native save ordering, settings and book-state persistence, migrations, reading history and the [main-thread guard](../validation.md#main-thread-guard). Read [Storage compatibility](../storage.md) before you change saved data. The `model` and `session` checks use the same production storage code as the app, with child processes that end without cleanup, held writers and forced commit failures. The `lifecycle` checks drive the production reader with controlled storage and native services.

| Test | Checks |
| --- | --- |
| `model.preview-store` | Private preview filenames, exact PNG pixels, invalidation, memory and storage limits, one-time old-cache cleanup, interrupted migration and pending scan recovery, completion persistence, failed and special marker reads, hidden PNGs, preservation of unrelated files and symlinks, and ancestor replacement at the deletion boundary |
| `model.profile-storage` | The SQLite profile: import, schema checks, single ownership, recovery, backups, synced commits and preservation of beta 8 caches during profile preparation and Blaze cache clearing |
| `model.record-writer` | Queue order, replacement of pending snapshots, delivery of each independent preference edit and its result, cancellation and wait limits |
| `desktop.settingsKeepPendingLibrarySort` | Select Author in the Library, delay its save until the Reader opens, switch on dark mode and verify both settings on disk |
| `model.save-wait` | Save waits: a report runs the waiting check from the event loop, also from a worker thread; the writer thread reports each write; the fallback timer covers a save that does not report |
| `model.reading-history` | Reading history, its migration and its recovery |
| `session.reading-book` | Saved book state, background saves and retries, reading time and history in the session |
| `platform.nickel-save-trace` | The `native-save-slow` warning of a slow Nickel save, with its stages |
| `platform.nickel-reading-save` | The native reading-time save worker: queued intervals, the hold for a close and timeouts |
| `lifecycle.closeWaitsFor*`, `lifecycle.closeRetriesRequiredNativeSaves`, `lifecycle.sleepWaitsForNativeTimeBeforePosition` | Saves and their order at close and sleep |
| `lifecycle.background*`, `lifecycle.standaloneReadingHistory`, `lifecycle.activityBatchesAndLowBattery` | Reading history and activity saves through background, resume and low battery |

`lifecycle.activityBatchesAndLowBattery` waits for standby to request the activity save after the page turn's queued refresh. It checks standby both before and after a failed background save completes. Standby may resume during retry cooldown only after pending writes end. The check retains the unsaved position until an explicit successful checkpoint.

`session.reading-book` holds the profile writer while an enlarged image returns to Fit. It reads the stored book record after the writer drains. It also delays acknowledgements across repeated choices, forces the newest write to fail, and checks that an unchanged retry reaches storage. A close that is still saving accepts a newer explicit choice. A synchronous checkpoint waits for the close write, preserves its callback and confirms the newer state.

`platform.native-lifecycle` checks native scan dispatch, rejection of a second pending request and one-shot completion through Nickel’s `doneProcessing` signal.

`model.preview-store` belongs to the `settings` label. Run it separately with `ctest --preset mac -R '^model.preview-store$'`.

`lifecycle.nativeOpeningStatus`, in [Kobo adapters](kobo.md), checks the native status update at opening.

## Notes

The `model.profile-storage` build for Kobo leaves out the Library part, the same as the application target.

The Kobo adapters reserve their own default-database transaction before they call Nickel's destructive analytics flush. Qt5 reports `readerHidden` after it ends the session, and Qt6 reports it before. Kobo history uses the shared recorder beside the native bridge, so a blocked local writer does not block the native stop call.

The profile ownership marker can be empty, because the OS-lock implementation also uses an empty marker normally. Symlink, directory and FIFO markers are refused. A failed recovery releases ownership so another process can retry. The marker stays after a normal release, so another process cannot lock a replacement inode while a contender holds the original one.

`lifecycle.slowCloseSave` holds the native reading-time callback through the close deadline. After sleep and wake, Keep reading allows a page turn while the write is pending. The late reply leaves the book open. Retry while the writer is pending closes the book once the writer finishes. Both rows check the final native position write. When only the host's records still wait, the timeout notice also offers Close without waiting; `lifecycle.closeWithoutWaitingForHost` checks that it closes at once, keeps Kobalt's saved place and writes no native position.

`lifecycle.sleepCancelsPendingExit` puts the reader to sleep while an ordinary close is waiting, both before and after the timeout notice appears. The first page turn on wake succeeds with the native save still pending. The test delivers the late reply, turns another page, then checks that a fresh Exit closes the book.

`lifecycle.retryFailedSaveAfterWake` fails the pending reading-time save before wake and after the first page turn. It holds the automatic retry while the reader turns another page. A successful retry stays silent. A failed retry offers manual recovery unless a newer position has already been saved. The test also commits a newer page before the original reply, checks that no retry starts, and verifies the final position at close. `lifecycle.preSleepCloseAcknowledgedAfterWake` holds the acknowledgement of an earlier local close across a page turn and sleep. A failed native retry still shows its warning after that acknowledgement arrives.

Close performance checks separate the visible return to the Library from the end of teardown. Measure normal-use timings with diagnostics disabled, because durable diagnostic events sync their log and change storage costs.

## Limits

These checks do not cut power. Durability after a power loss rests on the documented write-ahead logging guarantees of SQLite. They do not reproduce Nickel database contention on firmware, online sync, Android process termination or background delivery, or device storage latency. Process-kill checks do not validate physical power-loss durability. The Stats history interface is deferred.
