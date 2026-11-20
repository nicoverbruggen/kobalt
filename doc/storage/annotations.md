# Annotations and words

`kobalt.sqlite` is the store for bookmarks, pins, highlights, notes and saved words. The legacy JSON files remain for the diagnostic JSON backend (`KOBALT_ANNOTATIONS_SQLITE=0`) and for import. See [Database and recovery](database.md).

## Book keys

- Book keys come from `BookmarkStore::bookKey`.
- Native book identities stay the same across downloads. Sideloaded books use their content fingerprint.
- A changed file can adopt the annotations of the previous record at the same path.

## Legacy JSON files

- Bookmarks and highlights share `annotations/<book-key>/`.
- `book.json` has `version: 1`, `identity`, `title` and `fingerprint`.
- `bookmarks.json` has `version: 1` and a `bookmarks` array. `highlights.json` has `version: 1` and a `highlights` array.
- The JSON backend rejects the whole list when one row has an invalid position or the list is over its limit. It never keeps part of a list.
- A failed load or changed-file adoption blocks saves and keeps the original bytes.

## Limits

- Each book holds at most 2,000 bookmarks and 2,000 annotations.
- Bookmark lists are at most 1 MiB encoded. Highlight lists are at most 8 MiB.
- An excerpt is at most 262,144 UTF-16 code units. A note is at most 65,536.
- Malformed, oversized or unsupported lists block later saves. They are never replaced with an empty list.

## Order and revisions

- Stored order values can have gaps. Negative, non-integer and duplicate values are rejected.
- Deleting an entry keeps the other order values. Appending uses the next value.
- Explicit reordering, inserting before an entry, or running out of the signed 64-bit range can assign a new sequence.
- A load reads metadata, size bounds and records from one transaction snapshot.
- A save changes only the rows that were added, changed, removed or moved.
- A revision per list rejects stale writers. A failed transaction keeps the saved list, and the session restores its list in memory.

## Bookmarks and pins

- Each bookmark stores `position`, `chapter`, `excerpt` and `created`. Positions are canonical CREngine XPointers, not page numbers. Dates are UTC ISO 8601.
- Optional `name` and `icon` are limited to 128 and 64 UTF-16 code units. Without them, the bookmark is an ordinary bookmark.
- A non-empty `icon` pins the bookmark. Each book allows eight pins.
- An unknown icon keeps its stored value and shows a star.
- Pinned bookmarks keep their exact anchors when reflow puts several bookmarks on one page. Ordinary bookmarks still merge.
- SQLite stores pin data in a format-1 `profile_state` record named `bookmark-pins:<book-key>`, keyed by bookmark row ID.
- The bookmark transaction reads and writes that record with the list and checks the list revision. A failed pin write rolls back the whole bookmark edit.
- Import and changed-file adoption keep pins.
- Older builds do not show or maintain pins. Editing pinned bookmarks with an older build is not supported.

### Custom pin icons

- Custom SVG icons are user files in `custom-pins/` in the profile directory on Kobo and desktop. Android uses its working copy of the chosen folder; see [Android resource folders](sync-and-android.md#android-resource-folders).
- A file name starts with a letter or digit, uses only letters, digits, `_` and `-`, and ends in `.svg`. It is at most 48 characters. Other `.svg` files are reported as failed.
- An icon ID is `custom:` and the exact file name, including `.svg`. Built-in IDs do not change.
- Renaming or removing the file keeps the saved ID, name and position. The shortcut shows a pin. The editor shows a selected question-mark cell with the missing file name.
- A replaced file with the same name shows its new image at the next opening.
- Older builds show these IDs as a star.
- The opening worker reads and rasterizes the icons once per book opening.
- Discovery reads at most 256 directory entries and 16 KiB per file. Only regular files count. The folder itself cannot be a symbolic link.
- The reader keeps at most 120 alpha masks of 96 by 96 pixels, about 4.2 MiB. Painting only scales and tints them. There is no polling, disk cache or SVG work during page turns.
- Files added while a book is open appear after it is opened again.

## Highlights and notes

- Each highlight stores `id`, `start`, `end`, `text`, `chapter` and `created`.
- Optional `color` is `gray` (default), `yellow`, `green`, `blue` or `pink`. Optional `style` is `shaded` (default), `underline` or `dotted`. Unknown values stay through edits and show a fallback.
- Optional `sentence_start` and `sentence_end` objects record endpoint provenance with `enabled` and `chosen_start` or `chosen_end`.
- Optional `note` is plain text. A missing note is empty. Notes keep Unicode and line breaks. Passage edits and appearance changes keep the note.

### Compact loading

SQLite can load highlights without excerpts, chapter labels or notes.

- A compact record holds the ID, endpoints, provenance, appearance and timestamps.
- A visible-list request fetches only its excerpts and chapter labels. Opening one record for editing fetches all of it. Both requests check the compact list's revision and reject a concurrent change.
- Compact loading checks byte bounds, metadata, IDs, count, order and stored byte totals. It does not check UTF-16 validity or the exact encoded size of payloads it did not load. A damaged payload can stay unnoticed until it is fetched.
- Fetched fields get the full checks.
- An appearance edit reads and checks the full row inside the write transaction, then keeps its excerpt and note.
- Endpoint edits need the full record. Deletion does not read the payload.
- The full-load API and the JSON backend do not use compact loading.

## Saved words

My Words stores dictionary lookups in the `saved_words` table.

- A lookup is saved when the dictionary returns a definition, `disable_words` is false and `save_looked_up_words` is true or missing.
- The dictionary sheet can save or remove a word whatever `save_looked_up_words` says. It can also save a word without an entry, such as a name. That row has an empty `definition` and `dictionary`.
- The key is the book key and the case-folded word. A moved or renamed sideloaded book keeps its words.
- A repeated lookup increments the count and updates the latest time. When it found an entry, it also updates `definition` and `dictionary`. A lookup without an entry never replaces a saved definition. The first sentence and position stay.
- Each row keeps a snapshot of the book: reading identity, content fingerprint, source key, title and author. Opening the book updates the source key, title and author.
- The sentence is at most 1,024 UTF-16 code units, the definition HTML at most 16 KiB, and other text at most 4,096 code units. Times are UTC milliseconds.
- There is no foreign key to annotations or the Library, so removing a book keeps its words.
- Reads and writes run in order on the profile writer. Results come back through the owner's event queue, so a lookup never waits for storage.
- The Android and desktop Library find a word's book by source key, then by title and content fingerprint. The fingerprint check reads only the archive directory. A local file must match the saved fingerprint. An Android document matches by key alone.
- The JSON backend has no saved words and hides the setting.

## Annotation exports

Exports are ZIP files for reading outside Kobalt. They are not backups and cannot be imported. They do not change saved-data versions or Nickel settings.

- Each ZIP holds one UTF-8 file: `annotations.md` or `annotations.html`.
- Both formats group consecutive chapters in source order, number the annotations and leave out page numbers.
- Each entry has its creation time. Markdown shows UTC. HTML starts with UTC text and shows local time when JavaScript runs.
- HTML embeds Libron Regular as base64, with a comment naming the SIL Open Font License 1.1. Without the font asset it falls back to a system serif.
- HTML has search and three sort orders: Book order, Newest first and Oldest first. Date sorts use source order for ties and keep the numbers.
- HTML draws the saved marker, underline or dotted underline in the reader's palette, with the reader's fallbacks. The Show annotation styling option hides or shows these, also when printing.
- HTML needs no external files, network or browser storage, also from a `file:` URL.
- Export asks for confirmation, naming the book and the ZIP contents.
- Export fetches complete records in one revision-checked request, including notes. It waits for the location lookup. Unresolved passages sort after resolved ones, in saved order.
- The suggested name is the sanitized title, the first 16 hex characters of the SHA-256 book identity, and `-markdown.zip` or `-html.zip`.
- Kobo writes to `/mnt/onboard/Kobalt/Annotations/`. An existing file needs a separate Replace approval, checked again before writing.
- Mac and Android use the native save picker, with overwrite confirmation. Cancelling writes nothing.
- Local files are written with `QSaveFile`. Android `content:` files are written with `QFile`. Whether that replace is atomic depends on the document provider.
