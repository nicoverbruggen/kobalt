# Storage compatibility

Kobalt keeps saved reading data apart from disposable caches. Storage formats have their own versions. They do not follow app releases.

## Rules

- Never change a format version only because the release version changed.
- Hosts choose the storage directory. `storage::Paths` owns the shared file names.
- JSON files use UTF-8. Writes use `QSaveFile`, so a file is only replaced by a complete new file.
- A compatible optional field keeps the current format version. Older records load with an explicit default for it.
- Edits keep the supported optional fields of a record.
- A change to required fields, their meaning or their location needs a new format version, a migration that keeps user data, and regression tests.
- Never overwrite or delete unsupported or damaged saved data. Profile preparation moves unusable files into `backups/` and starts from defaults. Other reads reject the file and leave it in place.
- Do not assume that older builds keep unknown fields or can safely write newer schemas. The area files say what older builds do where it is known.
- Cache invalidation rebuilds disposable data only. It never clears settings, bookmarks, annotations or reading history.
- Saved data never goes in a cache directory.

## Saved-data versions

| Data | Version |
| --- | --- |
| `reader-settings.json` | `schema: 1` |
| `kosync.json` | `schema: 1` |
| `kobalt.sqlite` | `PRAGMA user_version = 4` |
| Book records (`book_state`) and `profile_state` records | format 1 |
| Legacy JSON book state | `schema: 1` |
| Legacy JSON bookmarks and highlights | `version: 1` |

Cache versions change more often. The current values are in `platforms/cache-policy.cmake` (`rendering_version`) and in `documentCacheChecksum` (parser format). See [Disposable caches](storage/caches.md).

## Storage areas

Read only the file for the area you change.

| File | Covers |
| --- | --- |
| [Settings](storage/settings.md) | `reader-settings.json`, global preferences, retired keys, `config.json` |
| [Book state](storage/book-state.md) | Per-book checkpoints, `main_text_marks`, `image_views`, `page_estimate`, sleep privacy |
| [Annotations and words](storage/annotations.md) | Bookmarks, pins, custom pin SVGs, highlights, notes, saved words, annotation exports |
| [Database and recovery](storage/database.md) | `kobalt.sqlite`, the profile connection, durability, backups, reading sessions, migration, `backups/` recovery |
| [Disposable caches](storage/caches.md) | Blaze parsed books, rendering policy, layout snapshots, sleep backdrops, preset previews, retired caches |
| [Sync and Android storage](storage/sync-and-android.md) | `kosync.json`, Android preferences, resource folders, deferred KOSync |
