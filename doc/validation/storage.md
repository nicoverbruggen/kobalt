# Storage and history checks

Label `storage`. Build `mac-tests`, then run `ctest --preset mac -L storage` from `platforms/desktop/`. `ctest --preset mac -N -L storage` lists its tests.

These checks cover the profile database, record writers, saves at close and sleep, native save ordering, settings and book-state persistence, migrations, reading history and the [main-thread guard](../validation.md#main-thread-guard). Read [Storage compatibility](../storage.md) before you change saved data. The `model` and `session` checks use the same production storage code as the app, with child processes that end without cleanup, held writers and forced commit failures. The `lifecycle` checks drive the production reader with controlled storage and native services.

| Test | Checks |
| --- | --- |
| `model.profile-storage` | The SQLite profile: import, schema checks, single ownership, recovery, backups and synced commits |
| `model.record-writer` | Queue order, cancellation and limits of the record writers |
| `model.reading-history` | Reading history, its migration and its recovery |
| `session.reading-book` | Saved book state, background saves and retries, reading time and history in the session |
| `platform.nickel-event-save` | Nickel's event flush against a competing database writer |
| `platform.nickel-reading-save` | The native reading-time save worker: queued intervals, held flushes and timeouts |
| `lifecycle.closeWaitsFor*`, `lifecycle.closeRetriesRequiredNativeSaves`, `lifecycle.sleepWaitsForNativeTimeBeforePosition` | Saves and their order at close and sleep |
| `lifecycle.background*`, `lifecycle.standaloneReadingHistory`, `lifecycle.activityBatchesAndLowBattery` | Reading history and activity saves through background, resume and low battery |

`lifecycle.nativeOpeningStatus`, in [Kobo adapters](kobo.md), checks the native status update at opening.

## Notes

The `model.profile-storage` build for Kobo leaves out the Library part, the same as the application target.

The Kobo adapters reserve their own default-database transaction before they call Nickel's destructive analytics flush. Qt5 reports `readerHidden` after it ends the session, and Qt6 reports it before. Kobo history uses the shared recorder beside the native bridge, so a blocked local writer does not block the native stop call.

The profile ownership marker can be empty, because the OS-lock implementation also uses an empty marker normally. Symlink, directory and FIFO markers are refused. A failed recovery releases ownership so another process can retry. The marker stays after a normal release, so another process cannot lock a replacement inode while a contender holds the original one.

Close performance checks separate the visible return to the Library from the end of teardown. Measure normal-use timings with diagnostics disabled, because durable diagnostic events sync their log and change storage costs.

## Limits

These checks do not cut power. Durability after a power loss rests on the documented write-ahead logging guarantees of SQLite. They do not reproduce Nickel database contention on firmware, online sync, Android process termination or background delivery, or device storage latency. Process-kill checks do not validate physical power-loss durability. The Stats history interface is deferred.
