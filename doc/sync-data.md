# Sync data

This document describes the reading data that Kobalt exchanges with Nickel on Kobo, what Nickel then sends to Kobo's servers or to a server that replaces them, such as BookOrbit or Grimmory, and how this compares with Nickel's own reader. It lists the mechanism for each item: the Nickel function that Kobalt calls, the source file, the moment and the conditions. Kobalt's own KOSync exchange is a separate mechanism, described in its own section at the end. Update this document in the same change as the code it describes.

Firmware 4.x runs the Qt 5 package (`platforms/kobo/nickel-qt5/`). Firmware 5.x and 6.x run the Qt 6 package (`platforms/kobo/nickel-qt6/`). Shared Kobo code is in `platforms/kobo/common/`. Firmware details that come from a specific version name that version. Recheck them when a new firmware changes behavior.

## Overview

| Data | Where Kobalt keeps it | What Kobalt writes to Nickel | What Nickel uploads |
| --- | --- | --- | --- |
| Reading position | Its own profile, after every page | `ChapterIDBookmarked`, `DateLastRead`, at lifecycle boundaries | `CurrentBookmark.Location` in the reading state |
| Book percentage | Its own profile | `___PercentRead` | `CurrentBookmark.ProgressPercent` |
| Chapter progress | Not stored | `CurrentChapterProgress` | `CurrentBookmark.ContentSourceProgressPercent` |
| Read status | Its own profile (finished state) | `ReadStatus`, `FirstTimeReading`, `ReadStateSynced` | `StatusInfo.Status` |
| Time estimates | Its reading pace | `RestOfBookEstimate`, `CurrentChapterEstimate` | `Statistics.RemainingTimeMinutes` |
| Reading time | Its reading history | `TimeSpentReading` | `Statistics.SpentReadingMinutes` |
| Reading sessions | Its reading history | Nickel's session calls and events | `OpenContent` and `LeaveContent` analytics events |
| Bookmarks, highlights and notes | Its own profile | Nothing | Nothing |

KOSync is not part of this exchange; see [KOSync: a separate sync mechanism](#kosync-a-separate-sync-mechanism).

## When Kobalt writes

Kobalt saves the position in its own profile while the user reads. It writes to Nickel only at lifecycle boundaries, never on a page turn:

- When a book opens: the reading status only (`NativeProgress::markReading`).
- Before sleep, before USB export, at power-off and when the book closes: the full progress write (`NativeProgress::write`) and the reading time of the session that ends.
- A reading session starts when a book is shown or the device wakes, and ends at sleep or close.

The page turn must never wait for storage. All writes to Nickel's database use Nickel's own functions, on Nickel's interface thread at these boundaries, or on Nickel's event thread for events. See [the main-thread guard](validation.md#main-thread-guard) for the declared waits.

## What Kobalt reads from Nickel

`NativeProgress` (`platforms/kobo/common/native_progress.h`, `nickel-qt5/native_progress.cpp`, `nickel-qt6/native_progress.cpp`) reads the book row on a separate read-only SQLite connection with a 100 ms busy timeout: `ContentID`, `ChapterIDBookmarked`, `DateLastRead`, `ReadStatus`, `___PercentRead`, `ReadStateSynced`, `FirstTimeReading`, `RestOfBookEstimate`, `CurrentChapterEstimate`, `CurrentChapterProgress` and, on Qt 6, `___NumPages`.

It checks for a changed committed position after Nickel's signals: the sync manager's `finished()` and `userProfileUpdated()`, and USB `doneProcessing()` and unplug. A signal that arrives while the device sleeps is checked at wake. Only when these signals cannot be connected does a one-minute check repeat it, and Kobalt logs `native-progress-signals-unavailable`. If another device moved the position, the reader asks before it moves (see [Progress sync](reader/sync.md)).

## What Kobalt writes to Nickel

### Reading status at opening

`NativeProgress::markReading`, both packages. Under Nickel's transaction (`DatabaseAdapter::beginTransaction`), `VolumeManager::updateFields` sets `ReadStatus = 1`, `FirstTimeReading = false`, `ReadStateSynced = false` and `HideFromHomePage = false`, as Nickel's `updateReadingStatus` does on firmware 4.46 and 6.0. It does not change the position, the percentage, the estimates or `DateLastRead`. It runs once, before the reader takes input, and does not retry.

### Position and progress

`NativeProgress::write`, both packages. The sequence:

1. Stop if a USB, power or progress transition is unresolved (`NativeLifecycle::nativeProgressSaveAllowed`).
2. Convert the position to Nickel's format. Qt 5 uses `nickel_qt5::stored` (`platforms/kobo/nickel-qt5/native_location.*`). Qt 6 uses `ContentLocation::create` and accepts it only when `ContentLocation::toTENGString` gives back the same position.
3. Set the connection's busy timeout to 0 (`NativeDatabaseWait`), so a busy database fails at once instead of blocking. Begin Nickel's transaction. If either fails, keep the local position and report "Nickel database is busy".
4. Compare the stored row with the progress Kobalt read before. If another writer changed it, stop.
5. Load a fresh `Volume` with `VolumeManager::getById` and set the fields through Nickel's setters:
   - Position and `DateLastRead`: `VolumeManager::updateBookmark`. The time is the current UTC time, at least one second after the previous stored time. Nickel stores it in whole seconds, as `YYYY-MM-DDTHH:MM:SSZ`.
   - `ReadStatus`: `Content::setReadStatus`, 1 while reading, 2 when finished.
   - `___PercentRead`: `Content::setPercentRead`, the same book percentage that Kobalt shows, counted as Nickel's reader does: the page on screen counts as read and the result rounds down, so page 1 of 83 is 1 and page 82 is 98. On firmware 4 it stays at 99 while reading, as Nickel's reader does; firmware 5 and 6 store 100 on the last page. Finished books store 100.
   - `FirstTimeReading`: `Content::setFirstTimeReading(false)`.
   - `RestOfBookEstimate` and `CurrentChapterEstimate`: `Volume::setRestOfBookEstimate` and `Volume::setCurrentChapterEstimate`, in seconds, 0 when finished. Only when Kobalt has a reading pace.
   - `CurrentChapterProgress`: `Volume::setCurrentChapterProgress`, a fraction from 0 to 1. Nickel's reader computes it as (page in chapter − 1) / pages in chapter, and so does `wordEstimates::chapterProgress` (`src/reading/word_estimates.h`). Text before the first table-of-contents entry counts as its own chapter. An unknown layout writes nothing.
6. Read Nickel's values back with `Volume::getDbValues`, and write only the changed fields with `VolumeManager::updateFields`.
7. Commit, then read the row again on the separate connection and compare every written field. If anything differs, refresh Nickel's cache and report that persistence could not be verified. The local position stays.
8. Ask Nickel to upload: if `BookmarkSyncManagerBase::syncReadingState` allows it, `BookmarkSyncManager::sendBookmark`. Nickel decides eligibility, network and power. This is not proof that the server received it.

When Kobalt has no position Nickel can store, as for some plain EPUB files, it writes only the time, the status and the percentage, and skips step 8.

On Qt 5, step 5 also writes the current chapter's own row for a KEPUB book, as Nickel's firmware 4 reader does at sleep and close. Kobalt finds the chapter row (`ContentType` 9) whose ID, `<book>!<OPF folder>!<path from the OPF>`, matches the position's file, and calls `ShortcoverManager::updatePercentRead` with floor(page in chapter ÷ chapter pages × 100), inside the same transaction. Unlike `CurrentChapterProgress`, this counts the current page as read: page 5 of 9 stores 55 on the chapter row and 0.444 as `CurrentChapterProgress`, as Nickel's firmware 4 reader does. It skips this when the book is finished. Nickel's firmware 6 reader does not write that row: at close it works out the current chapter and its progress, then saves only the book row (`ReadingView::saveProgress` passes them to `updateContentLocation`, which does not use them, and nothing calls `updatePercentRead`). Kobalt's Qt 6 build does the same.

### Recent book at close

`NativeProgress::markRecent`, both packages, once when the book closes, after the final progress write. It calls `ActivityManager::makeRecentBookActivity(content ID, database, true, true)`, as Nickel's reader does when a book closes: it creates or updates the book's `RecentBook` row in the `Activity` table with the current local time, and notifies Nickel's Home screen. It uses a busy timeout of 0 and skips the write if the database is busy.

### Reading time

`addTime` in `native_reading_session.cpp`, both packages, when a session ends. Under Nickel's transaction it adds the session's seconds to `Volume::timeSpentReading` with `Volume::setTimeSpentReading`, updates only that field, and commits. A delta of 24 hours or more is rejected. A successful commit consumes the delta even when the readback is unavailable; later attempts only verify it.

### Reading sessions and events

`NativeReadingSession` (`native_reading_session.cpp`, both packages) reports sessions through Nickel's event engine, as Nickel's own reader does:

- Start: `EventEngine::startReadingSession(volume, state, {ViewType: ReadingView})` with the current percentage, then the event `readerShown`.
- Page turn: the event `pageTurned` with `wordsRead`, the estimated words on the page just left.
- End: the event `readerHidden` and `EventEngine::endReadingSession` with the current percentage and position. The order differs: Qt 6 sends `readerHidden` before ending the session, Qt 5 after, matching each firmware's reader.

Nickel turns the start and end into the analytics events `OpenContent` and `LeaveContent`. `LeaveContent` carries the progress at the end of the session, `SecondsRead`, `PagesTurned` and `IdleTime`. Nickel adds title, author, content type and format, `Origin`, `Monetization` and `ViewType`.

Nickel keeps these events in memory until `EventEngine::savePendingEvents` writes them to the `AnalyticsEvents` table. That function clears its buffer even when the save fails, and on firmware 4.46 it saves nothing unless it runs on the event engine's own thread. Kobalt calls it on that thread after a session starts and after it ends. It first takes Nickel's default-database transaction, waiting at most 2 seconds, and skips the save if it cannot, so the buffer survives until the next save (`saveNativeEvents` in `platforms/kobo/common/native_event_save.h`). If the outer commit fails, Kobalt rolls back its own transaction so later native saves can begin. This cleanup cannot restore events that Nickel already removed from its buffer. Nickel also saves the buffer on a timer and at shutdown.

## What Nickel uploads

### Reading state

During a sync, or after `sendBookmark`, Nickel sends `PUT {api_endpoint}/v1/library/{id}/state` for books that its sync carries. From the fields above:

- `CurrentBookmark.Location`: the source file and span from `ChapterIDBookmarked`. Firmware 6 drops the character offset.
- `CurrentBookmark.ProgressPercent`: `___PercentRead`.
- `CurrentBookmark.ContentSourceProgressPercent`: floor(`CurrentChapterProgress` × 100). Firmware 4.46 and 6.0 also show it on the statistics screen.
- `Statistics.SpentReadingMinutes` and `RemainingTimeMinutes`: from `TimeSpentReading` and the estimates.
- `StatusInfo.Status`: `Reading` or `Finished`.

`ReadStateSynced` goes back to true once Nickel has uploaded. Sideloaded books (`file:///` content IDs) have no reading state on Kobo's cloud.

### Session analytics

Nickel uploads `AnalyticsEvents` rows only during a sync, as `POST {api_endpoint}/v1/analytics/event` (`SubmitAnalyticsEventsCommand`). `api_endpoint` is the Nickel setting written at sign-in, not a resource from the server's `/v1/initialization` answer.

On firmware 4.46, Nickel drops `OpenContent` and `LeaveContent` when it creates them, before any save, in these cases:

- Sideloaded mode (`ApplicationPreferences/SideloadedMode`), parental control, or the setting `StatsTrackingEnabled` set to false.
- `PrivacyPermissionsManager::analyticsEnabled` is false. It is true when the setting `kobo_privacyCentre_url` is empty. When that URL is set, from the `kobo_privacyCentre_url` resource of the initialization answer, it is true only if the user's privacy permissions, from the user-profile sync, contain `analytics.platform.features`.

A replacement server that sets `kobo_privacyCentre_url` but does not give the user that permission therefore receives no session analytics, with Nickel's own reader as well as with Kobalt. These rules have not been checked on firmware 6.

Sync servers use this data differently. BookOrbit makes a reading session from each `LeaveContent` and the `OpenContent` before it for the same book (duration from `SecondsRead`, progress change from the two `progress` values), and ignores the other metrics. Grimmory accepts the analytics upload and discards it; it uses the reading state's `ProgressPercent` and `Location`.

## Nickel's own reader compared with Kobalt

These tables compare what Nickel's own reader and Kobalt write and send for the same reading: open a book, turn pages, add a bookmark and a highlight, sleep, wake, turn pages, close and sync. They come from database and upload comparisons on firmware 4.46.23836 (Qt 5) and 6.0.279225 (Qt 6), with a sideloaded KEPUB and a store book from a local sync server. Firmware 5.x was not compared. Positions differ between the two readers only because their page layouts differ.

### Qt 5, firmware 4.x

| Moment and data | Nickel's reader | Kobalt |
| --- | --- | --- |
| Open: book row | `ReadStatus` 1, `FirstTimeReading` false, `ReadStateSynced` false. No `DateLastRead`. | The same. |
| Open: chapter rows | `WordCount` on every chapter, at the first open, counted on a worker thread from the book file (KEPUB only) | Nothing; see below |
| Page turn | No database write; `pageTurned` event | The same |
| Sleep and close: book row | `ChapterIDBookmarked` (`path#kobo.X.Y`), `DateLastRead` (UTC, whole seconds), `___PercentRead`, `RestOfBookEstimate`, `CurrentChapterEstimate`, `TimeSpentReading`, `CurrentChapterProgress` | The same columns, in the same formats |
| Sleep and close: current chapter row | `___PercentRead`, KEPUB only | The same |
| Close: `Activity` | Inserts a "recent book" row for a sideloaded book; updates its `Date` for a store book | The same |
| Bookmark or highlight | A `Bookmark` row at once (`epub/...#kobo.X.Y` paths, `Type`, `Color`, `Text`, `ChapterProgress`, `ContextString`); `PATCH .../annotations` at sleep | Nothing; kept in Kobalt's profile |
| Reading state upload | `PUT .../state` at sleep and at close | The same moments and fields |
| Session events | `OpenContent` and `LeaveContent` with `ContentFormat`, `ContentType`, `Monetization`, `Origin`, `ViewType`, `author`, `progress`, `title`; `LeaveContent` adds `IdleTime`, `PagesTurned`, `SecondsRead` | The same fields; `readerHidden` after the session ends |
| Never written | `LastTimeStartedReading`, `___SyncTime` | The same |

### Qt 6, firmware 5.x and 6.x

| Moment and data | Nickel's reader | Kobalt |
| --- | --- | --- |
| Open: book row | Status fields as on Qt 5, plus the book's `WordCount` and `metaDataJson` | Status fields only |
| Open: chapter rows | `WordCount` on every chapter, at the first open | Nothing |
| Page turn | No database write; `pageTurned` event | The same |
| Sleep and close: book row | As on Qt 5; `ChapterIDBookmarked` adds a character offset (`path#kobo.X.Y:offset`) | The same columns, in the same formats |
| Sleep and close: current chapter row | Nothing; only the book row is saved | The same |
| Close: `Activity` | As on Qt 5 | The same |
| Bookmark or highlight | A `Bookmark` row at once, with `:offset` paths and a `!!` content ID; `PATCH .../annotations` at the next sync | Nothing; kept in Kobalt's profile |
| Reading state upload | `PUT .../state`, observed at the next sync; the uploaded position drops the offset | `PUT .../state` at sleep and at close; the same fields, also without the offset |
| Session events | As on Qt 5 | The same fields; `readerHidden` before the session ends, as Nickel's Qt 6 reader does |
| Never written | `LastTimeStartedReading`, `___SyncTime` | The same |

### What the differences mean

- Kobo's cloud and replacement servers receive the same reading state and the same session events from both readers. `ContentSourceProgressPercent` is now the same as well; before Kobalt wrote `CurrentChapterProgress`, Kobalt's uploads always carried 0.
- Highlights and notes never reach Nickel's `Bookmark` table. Nickel's fields are not compatible with Kobalt's annotations, so this is deliberate.
- Kobalt does not count chapter words at opening. Nickel uses those counts for its own time estimates, which Kobalt writes itself. Nickel counts them by parsing the whole book the first time its own reader opens it; doing that at every Kobalt opening would cost time and power.
- On Qt 6, the observed upload moment for Nickel's reader came from a run that started from a different reading state than Kobalt's run. Treat that one difference as unconfirmed.

## Qt 5 and Qt 6 implementation differences

| Item | Qt 5 (firmware 4.x) | Qt 6 (firmware 5.x and 6.x) |
| --- | --- | --- |
| Position conversion | `nickel_qt5::stored` and `nickel_qt5::chapter` | `ContentLocation::create` and `toTENGString` |
| Stored position | `path#kobo.X.Y` | `path#kobo.X.Y:offset` |
| `updateBookmark` | `VolumeManager::updateBookmark(Volume&, QString, int, QDateTime)` | The same with a page number, used for Tolino |
| Tolino devices | Progress writeback refused | Writes the page count (`Content::setNumPages`) and checks Tolino's bookmark row |
| Cached time check before writing | Accepts the stored time with or without milliseconds | Compares whole seconds |
| `readerHidden` | After `endReadingSession` | Before `endReadingSession` |

## Books that Nickel's sync carries

A book whose Nickel content ID is not a `file:///` path came through Nickel's sync: a Kobo store book, a Kobo Cloud upload or a book from a replacement server such as BookOrbit or Grimmory. `ReadingBook::usesNativeCloudSync` is true for it. Kobalt leaves the position exchange to Nickel for these books. On Qt6, the native Kobo Cloud volume flag identifies the copy for the Sync tab and excludes it from KOSync. USB-sideloaded copies keep their local file identity and remain eligible for KOSync.

## Verification

- `model.chapter-progress`: the chapter-progress formula.
- `platform.native-progress` and `model.reading-progress`: progress signals and position rules.
- `platform.nickel-event-save`: the event save under a competing writer.
- `platform.nickel-reading-save`, `lifecycle.closeWaitsForNativeTimeBeforePosition` and `lifecycle.sleepWaitsForNativeTimeBeforePosition`: the order and failure handling of time and position writes.
- `pewter.progress`, in both Kobo SDKs: the status at opening, the position and progress write, the recent book at close, and their failure paths, as described above. It runs the firmware adapters against [pewter](validation/kobo.md#pewter-the-fake-nickel), a fake Nickel, with `bash platforms/kobo/build.sh qt5 --check` and `qt6 --check`.
- Firmware behavior, such as the upload format and the analytics conditions, cannot be checked by the native tests or pewter. Check it on a device or an emulated Nickel after a firmware or code change, against a local sync server: the stored row after sleep and close, the `PUT .../state` body, and the `AnalyticsEvents` rows and their upload.

When a Nickel symbol that Kobalt resolves at run time is missing on a new firmware, the matching write is skipped or refused. The required ones are listed in each `Api::ready()` in the native files. Optional ones, such as `Volume::setCurrentChapterProgress`, are checked where they are used.

## KOSync: a separate sync mechanism

KOSync is Kobalt's own sync. It does not use Nickel's sync, and it works the same way on Kobo, Android and desktop. It is used only for books that Nickel's sync does not carry.

### What is sent

Kobalt talks to the KOSync server over HTTPS with the standard KOSync protocol:

- To sign in, it sends the user name and the MD5 hash of the password, in the headers `x-auth-user` and `x-auth-key`. The password itself is never stored or sent.
- To read the server's position, it sends `GET /syncs/progress/{document}`.
- To save its own position, it sends `PUT /syncs/progress` with five values:
  - `document`: the book's identifier. By default this is an MD5 hash of parts of the file. With filename matching, it is the MD5 hash of the file name.
  - `progress`: the position in the book.
  - `percentage`: how far into the book, from 0 to 1.
  - `device`: the device name, for example "Kobo Libra Colour (Kobalt)".
  - `device_id`: a random identifier for this installation.

### Where it lives in the code

- `src/sync/kosync.*`: the account, the requests and the conflict rules.
- `src/app/reader/window/reader_kosync.cpp`: when the reader syncs.
- `platforms/kobo/common/native_sync_network.*`: on Kobo, how the connection is made through Nickel's Wi-Fi.
- `kosync.json`: the saved account and its settings. See [Storage compatibility](storage.md).

### How it touches Nickel

KOSync writes nothing to Nickel's database itself. When a KOSync exchange moves the reader to another position, that position reaches Nickel through the normal position write described above, at the next sleep or close.
