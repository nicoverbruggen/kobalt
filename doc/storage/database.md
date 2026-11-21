# Database and recovery

## Profile database

`kobalt.sqlite` is in the profile directory, next to `reader-settings.json` and outside every cache. It stores bookmarks, annotations, book state, the standalone Library catalogue, recent-book metadata, reading speed, reading sessions and saved words. Global settings stay in JSON.

- The application ID is `KAN1`. The encoding is UTF-8. The schema version is 4, in `PRAGMA user_version`. It does not follow JSON format versions or app releases.
- Strings are bound as explicit UTF-8 bytes and read back with an explicit length. This keeps embedded NUL characters and a leading U+FEFF with every Qt SQLite provider.
- Unknown, foreign or damaged databases are rejected without changing their schema. Profile preparation then moves them into `backups/`.
- A future incompatible schema needs an explicit transactional migration before its version changes.

### Tables

| Table | Contents |
| --- | --- |
| `books`, `lists`, `entries` | Bookmarks and highlights: one row per book, one per list with its revision and size, and one per entry. See [Annotations and words](annotations.md). |
| `book_state(identity, format, revision, payload)` | One JSON object per book, at most 16 KiB. See [Book state](book-state.md). |
| `profile_state(name, format, revision, payload)` | App records: `reading-speed`, `last-book` (recent-book metadata), `bookmark-pins:*`, `activity-hidden-books`, `reading-day:*`, `reading-hours:*`. `last-book` can be 256 KiB and `activity-hidden-books` 64 KiB. Other records are at most 16 KiB. |
| `library_books` | One row per discovered book: source key, folder, metadata, cover file name, timestamps, progress and read status. An unchanged book does not rewrite its row. |
| `library_folders` | Folder keys, labels and display order. |
| `reading_sessions` | Reading intervals. See [Reading sessions](#reading-sessions). |
| `saved_words` | My Words. See [Saved words](annotations.md#saved-words). |
| `imports(source, digest, record)` | Completed JSON imports. |

- Kobo uses its native library, not the Library tables.
- `book_state` and `profile_state` payloads are compact UTF-8 JSON, checked by the app without SQLite JSON extensions. Their record format is 1, apart from the schema version.
- An unsupported record format, or a malformed or oversized payload, blocks updates.
- Optional fields, nested values, Unicode and embedded NUL characters survive import and edits.

### Diagnostic JSON backend

- `KOBALT_ANNOTATIONS_SQLITE=0` selects the legacy JSON backend for diagnostics. So does a build with `-DKOBALT_SQLITE_ANNOTATIONS_DEFAULT=OFF`.
- `KOBALT_ANNOTATIONS_SQLITE=1` selects SQLite, also in that build.
- Each store captures the backend when it is created. Do not switch backends while a book is open.

## Profile connection

- One writer thread owns `kobalt.sqlite`. Every read and write runs on that thread, one at a time, on one connection. No other code opens the file while it is open.
- Calls that a caller waits for run before queued background writes. A background write that has waited two seconds runs next.
- If the system refuses to start the writer thread, database inspection reports unavailable storage and leaves the database alone. Preferences still load, normalized in memory. A later read can retry the preference migration.
- If USB export starts before the first profile operation, database access is blocked without starting the writer. The first operation after USB disconnects can open the profile.

### Opening

- The connection takes an exclusive lock before its first read and holds it until it closes.
- Before its first change to the file, opening checks ownership, version, encoding, page size and every table's columns, order, types and primary keys. It also runs SQLite's `quick_check` and `foreign_key_check`.
- A rejected file keeps its content. Only a log left by a crash is folded into it.
- Opening refuses a read-only file.
- The connection requests a 2 MiB page cache and turns off memory mapping. This is not a limit on process memory.
- `max_page_count` keeps the main file under FAT32's 4 GiB limit. It does not limit the log or free disk space. A database already over the limit is rejected unchanged.
- A new database is created in a temporary sibling file under an initialization lock, then published by rename and a directory sync. A leftover staging file does not block a retry.

### Journal and durability

- The connection uses write-ahead logging. Commits append to `kobalt.sqlite-wal`. A checkpoint folds the log into the main file. With the exclusive lock, SQLite creates no `-shm` file.
- Synced commits use `synchronous=FULL`.
- Reading activity and immediate bookmark, highlight and progress-choice writes use `synchronous=NORMAL`. They do not sync on their own. A later synced commit or checkpoint makes them durable.
- SQLite's automatic checkpoint, after about 1,000 pages, stays on.
- If SQLite refuses write-ahead logging, the connection uses a `DELETE` rollback journal with `synchronous=EXTRA`.
- A process crash loses no completed commit, only data still in app memory.
- A power loss can lose `NORMAL` commits that were not yet synced. It cannot lose a synced commit and does not corrupt the database.
- A process that ends without closing leaves `kobalt.sqlite-wal`. The log belongs to the database. Keep it with the main file until the next opening replays it. Never delete it to clear a lock. Earlier builds also replay it.

### Write batching

- Book state, bookmark, highlight and reading-speed writes run in the background. Each owner keeps at most one waiting write. A newer write of the same record replaces it.
- While reading, position, history and speed stay in memory. A batch is due five minutes after the last successful batch. The next eligible activity (a page turn, jump or layout change) queues one snapshot in one transaction. There is no save timer.
- A failed batch gets three short attempts, then a five-minute cooldown. Activity after the cooldown retries it. Failures never move the last-success time.
- Layout memory saves on its own, durably, when the appearance changes. A failure gets three short attempts. The latest snapshot then waits through a five-minute cooldown. New edits replace the snapshot but do not reset the attempts. A due activity batch retries it. Synchronous and close checkpoints can save it during the cooldown.
- Bookmark and highlight edits show at once. If the save fails, the session restores the last saved list and reports the error.
- A process crash can lose the pending batch. Inactivity or repeated failures can make that batch older than five minutes. Together with power loss, there is no strict five-minute limit on lost reading data.
- An in-flight write cannot be dropped safely only because a deadline passed.

### Sleep, close, USB and power-off

- Sleep checkpoints the log. Sleep and host checkpoints wait for their save result.
- Ordinary standby does not force a batch or a checkpoint.
- At 5% battery or less, while not charging, activity makes a durable batch due after one minute. Standby first queues dirty reading data and a checkpoint. After a failed checkpoint, standby waits five minutes before asking again. These cooldowns never block standby. The reader uses the existing battery observation and adds no polling.
- The connection closes when a book closes, when the reader is disposed for a transition, and at USB export.
- An ordinary close drains queued writes. The profile checkpoints, switches back to a `DELETE` journal and closes the file, even when nothing changed. A copy, a computer over USB and older builds then see one plain file.
- USB saves the reader for at most three seconds, then drops queued reader writes. A running transaction finishes before the connection closes and blocks new transactions. After the close at USB export, every operation fails until the partition is mounted again or USB disconnects before the export. The firmware unmounts lazily before export, so no file may stay open.
- Power-off and reboot save the reader for at most ten seconds. The writer then closes and blocks the profile after the writes already waiting there, and Nickel continues. The reader thread does not wait for this close. When the ten seconds pass first, Kobalt only blocks new transactions, so a running write can keep the file open while Nickel shuts down. The firmware freezes Nickel at power-off. The `-wal` file then holds any unfinished transaction, and the next opening replays or discards it.
- Only the current USB request can close and block the profile. A cancelled drain cannot block it again. Unplugging cannot reopen it during power-off.
- The first operation after a close opens the file again.
- When a manual transition fails three times, the reader offers Retry or quitting without saving. USB, power-off and reboot make the same attempts within their time limits, log the failure and continue without a dialog.

## Backups and external access

- The exclusive lock is part of the storage contract. No other SQLite connection can read or back up a running profile. That includes read-only connections and SQLite's backup API.
- A busy timeout does not limit the backup API's retry loop. A longer deadline in an external tool does not release the lock.
- For a backup, stop the app or finish its storage handoff first: wait for pending writes and the connection to close, and stop anything from reopening the profile until the copy is done.
- A quiet interface, an idle writer or a finished checkpoint is not a backup barrier. Closing a book does not stop later database operations.
- Copying only the main file while the writer runs can miss committed log data. Copying the main file and the log at different moments can mix two states.
- After a crash or a failed close, copy the main file with its `-wal` or `-journal` file while the writer is stopped. Recover and verify a separate copy. Never repair or checkpoint the original.
- An online backup feature must go through the profile writer, with a consistent snapshot and an explicit end before writes resume.
- Diagnostic snapshots must stop file changes during the capture, keep the journal files, recover only the copy and check its integrity before querying it.

## Reading sessions

Android, desktop and Kobo record foreground reading intervals in `reading_sessions`. The JSON backend does not record them.

### Rows

- Each row has a UUID, a book key, the reading identity, content fingerprint and source key, a title and author snapshot, start and last-seen UTC milliseconds, active milliseconds, start and end positions, and a status: `active`, `completed` or `interrupted`.
- Text and positions are at most 4,096 UTF-16 code units each.
- The book key is the annotation book key. Matching fingerprints share a key. Changed editions stay separate.
- Android folder books also keep their document-provider key.
- Indexes cover book and start time, and start time.
- There is no foreign key to annotations or the Library. Removing a book or marking it unread keeps its sessions.
- Migration creates no rows for reading before sessions existed.

### Timing

- Duration comes from the reading timer. UTC timestamps give the order and never go backward within an interval.
- A session starts when a usable reading page shows. Reading menus count. Sleep never counts. There is no inactivity timeout.
- Sleep or app suspension stops the clock and saves the session without completing it. Repeated sleep or checkpoint notices keep the first sleep boundary.
- Returning after at least the New session after sleep time (`session_timeout_minutes`; see [Settings](settings.md#controls-navigation-and-dictionaries)) completes the old session at its last observation before sleep and starts a new UUID. Shorter sleeps keep the UUID and timer.
- A backward clock change does not end a session.
- Hiding an active reader, closing the book, USB handoff and shutdown complete the session.
- A host checkpoint during suspension stops the clock before saving, even when the host callback runs first.

### Saving

- New intervals and updates stay in memory until the activity batch or a commit point. They join position and speed in one transaction.
- Each save writes the absolute duration under the same UUID.
- A failed transaction keeps the pending intervals while the book is open. Repeated failures followed by a crash can still lose time.

### Ownership

- Before its first book opens, the host takes profile ownership through `.reading-history.lock` and recovers unfinished intervals as `interrupted` at their last saved observation. Downtime does not count.
- Controllers in one process share the owner. Another process cannot start a reader on the same profile while the owner lives. Separate profile directories are independent.
- Ownership is an OS file lock, released when the process dies. The file is a persistent marker, not a PID file. An empty file or a crash while creating it does not block the next owner. Do not delete it while an app owns the profile.
- A failed recovery releases ownership so a later opening can retry.
- On Kobo, ownership stays across book closes. It is released after the storage drain before USB export or power-off.

### Kobo and Nickel

- Kobo also reports sessions to Nickel. Nickel's session ends at close, sleep, USB export and power-off, so its time leaves out sleep. It stays open while the reader is hidden without one of those, as Nickel's own session does under a dialog. See [Reading time](../sync-data.md#reading-time).
- The New session after sleep timeout groups Kobalt history and resets Kobalt's session timer. It does not change Nickel's reading-time estimates.

### Activity records

[Activity](../reader/progress.md#activity) reads sessions, daily totals, book records and saved words on the writer thread. Its records are format-1 `profile_state` records.

- `activity-hidden-books` holds a `books` array of book keys. It is deleted when empty. A missing, damaged or newer record hides nothing and is not rewritten.
- `reading-day:<ISO date>:<opening UUID>` holds one opening's active milliseconds for one local date. Newer records also hold the book key.
- `reading-hours:<session UUID>:<ISO date>` holds active milliseconds per local hour.
- The reader writes both in the same transaction as the session. A repeated save replaces the opening's total and does not add to it. Unchanged totals are not rewritten.
- The reader loads other openings off the GUI thread and adds its own total from memory, so queued saves cannot count time twice.
- Local dates keep 23-hour and 25-hour daylight-saving days. Midnight splits the time without a new session.
- Older builds keep these records but do not update them. Totals from before daily records existed cannot be rebuilt exactly.
- Deleting a book's activity deletes its sessions and subtracts their time from `reading-day` records. Records with a book key are removed by that key.
- Older records without a book key are reduced by an estimate: each session is split at midnight, counting back from its last observation. A date total never drops below the sessions that remain on it. A record that reaches zero is deleted.
- The records of the open book's current opening never change, because its reader rewrites them.
- Activity cover thumbnails are a disposable cache in `activity-covers/` under the previews folder. Activity checks the EPUB fingerprint before reusing one.

## Profile migration and cleanup

### Schema upgrades

- Versions 2 and 3 upgrade to version 4 in one transaction. Kobalt checks the old schema and its integrity after taking the write lock. It checks the upgraded database before committing.
- Version 2 gets the session and saved-word tables. Version 3 gets the saved-word table. Then `user_version` changes. A failure rolls back.
- Other versions and partial schemas are rejected without repair.
- Builds that support only version 2 or 3 cannot open an upgraded profile.

### JSON import

- Profile preparation imports hashed book-state files, annotation folders and Library data, also for books not opened since the upgrade. Beta 3 bookmarks and version-1 JSON are supported.
- On Kobo it runs after Nickel has started: when Kobalt prepares the last book, or else at the first book opening after the loading view paints. It never runs inside Nickel's startup.
- A book's bookmarks and highlights import in one transaction. Fields, IDs, timestamps and order stay.
- Each import is recorded in `imports` inside its data transaction. A retry uses the committed record, never stale JSON, and does not import twice.
- After the commit, the digest of the source is checked and the source moves to `migration-backup/`. Annotation folders and their path index go to `migration-backup/annotations/`. A failed move can retry.
- Invalid or unsupported sources block the import. Unknown files, symbolic links, malformed sources and unsupported formats are left alone.
- Annotation imports keep source timestamps, so a changed sideloaded book adopts the newest edition, not the last folder migrated.
- Fingerprint adoption checks committed records and unmigrated JSON. Retained JSON of an imported book never replaces newer database edits.
- Library migration rejects duplicate identities, missing folders and unsupported versions instead of dropping records. A failed Library import blocks catalogue saves and shows an error. It never creates an empty catalogue.
- After import, SQLite is the source of truth. `migration-backup/` is a snapshot, not an export. Keep it until the upgrade is validated. An older build can edit those files without updating SQLite, so reinstalling an older build does not bring database edits back.

### Diagnostics folders

- Kobo always writes the bounded event log in `diagnostics/` and crash reports in `crash-reporting/`.
- It writes inspection reports only when `KOBALT_DIAGNOSTICS=1` is set or the profile has a `diagnostics-enabled` file.
- Older inspection reports and logs move to `migration-backup/diagnostics/`.
- `active-session.json` and `startup-session` keep their recovery behavior.
- None of these files are reading data.

## Unusable saved data

Profile preparation checks saved data before anything uses it. On Kobo it runs when the first book opens. On Android and desktop it runs when the reader controller starts, and again before a book opens if storage failed.

- `reader-settings.json` and `kosync.json` are unusable when they are not valid JSON, are over 16 KiB or have a `schema` other than 1. Settings without a `schema` are upgraded, not moved.
- `kobalt.sqlite` is unusable when it has Kobalt's application ID and an unsupported version, or is damaged, empty, foreign or in another encoding.
- A lock or an I/O error does not make a file unusable.
- Kobalt renames an unusable file into `backups/` in the profile directory, so the bytes stay the same. A newer file gets `.bak`. A damaged file gets `.corrupted.bak`. A number keeps backups from replacing each other, for example `reader-settings.json.corrupted.2.bak`.
- The database's `-wal`, `-journal` and `-shm` files move with it. A readable database first folds in a log left by a crash. A file that is not a database loses its log when SQLite opens it.
- `backups/` holds only these recovery copies. `migration-backup/` holds import snapshots.
- Settings then start from defaults, the sync account is signed out and the database starts empty. The next reader shows a notice after its first page. It names what was reset and where the backups are.
- When a file cannot be read or moved, Kobalt does not use the profile. Kobo shows an alert that recommends a file system repair or a reset, and offers Kobo's own reader. Android and desktop refuse to open the book with the same advice. The next opening checks again.
- Settings that become damaged, too large or newer while a book is open stay in place. Close and synchronous checkpoints then skip layout memory and still save the position. A read error still fails those saves, so closing offers Retry.
