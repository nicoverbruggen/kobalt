# Book state

Each book has one record in `kobalt.sqlite`: `book_state(identity, format, revision, payload)`. The payload is a JSON object of at most 16 KiB. The record format is 1.

- The `book` and `fingerprint` fields identify the publication.
- The record stores the canonical reading position, book-specific text preferences, dictionary choices and native progress reconciliation fields.
- The Android and desktop libraries read main-text progress from `percent`, regardless of the scope selected for Stats.
- Global layout preferences stay in `reader-settings.json`.
- Bookmarks and annotations live in their own tables, not in this record.
- Legacy JSON book state uses a host prefix, the SHA-256 of the UTF-8 book identity and `.json`. Its `schema` is 1. Profile preparation imports these files; see [Database and recovery](database.md#profile-migration-and-cleanup).

## Checkpoints

- Saves go through the background checkpoint writer.
- An unchanged checkpoint does not write its row.
- A changed checkpoint replaces only that book's object and increments `revision` in the same transaction. The revision counts changes. It is not a stale-writer check in the current checkpoint API.
- A changed checkpoint keeps extra fields of the supported record loaded at opening and replaces the fields the reader owns. It keeps the dictionary choice already in storage, even if the snapshot was taken before a later choice was saved. Changing a dictionary choice updates only its field in an existing record. Clearing the choice removes the field, as does a native unread reset.
- Display and session values are not saved: chapter titles, page counts, current and next page numbers, the resolved display font, cached-page counts, Fast source-word counts, estimated bookmark and highlight page labels, and the loaded slice start. The next save removes them from older records. Diagnostic snapshots still include them.
- Older annotations without a chapter label get a display label from their position. The record is not rewritten.

## Mark as unread and reopening

- When Nickel marks a book as unread, Kobalt keeps the book identity and the fields listed below.
- It clears the reading position, book-specific text preferences, `page_estimate` and `suppress_finish_prompt`.
- It keeps annotations, `hide_book_on_sleep_screen`, `sleep_positions` and `main_text_marks` (with `fingerprint`). When a named preset is selected for new books, the reopened book uses that preset even if it has saved main-text marks.
- Reopening a finished book resets only the reading position and the finished status. Book-specific preferences and annotations stay.

## Optional fields

All fields keep record format 1, legacy JSON schema 1 and database schema 4. Missing or invalid values use the default.

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `hide_book_on_sleep_screen` | Boolean | `false` | True keeps the selected sleep and power-off design and anonymous progress, but leaves out the cover, title, author, chapter name and page text. Reading-page and PNG modes use an opaque background. Kobo's native screen uses a safe Kobalt fallback. Not inherited by other books. Older builds ignore it and do not hide the book. |
| `suppress_finish_prompt` | Boolean | `false` | True suppresses this book's finish prompt only. |
| `text_preset` | string | none | ID of the text preset last applied to this book. |
| `sleep_positions` | object | global `sleep_positions` | Same format as the global setting. Wins over it for this book. |
| `main_text_marks` | object | publication defaults | See [Main text marks](#main-text-marks). |
| `stats_main_percent`, `stats_volume_percent` | numbers | none | Saved progress for both `stats_word_scope` values, so the private sleep screen can follow the scope without opening the book. |
| `native_writeback_failure` | object | none | The latest native save or mapping failure: result, failure flags, safe error codes and UTC time. No book text or positions. It stays through later successful saves and reopening. |
| `image_views` | array, at most 64 | Fit | See [Image views](#image-views). |
| `page_estimate` | object | reference weights | See [Page estimate](#page-estimate). |

### Main text marks

- The manual `start` and `end` each store the TOC entry’s zero-based `index`, `title`, source `position` and resolved `anchor`. For an older choice without an anchor, the reader checks the index and title. It ignores the choice if the entry no longer matches. If the entry matches, the reader saves the resolved anchor or removes the choice if it cannot resolve the boundary.
- If the boundaries cross on load, the reader drops the manual end first. It drops the manual start only if the range is still invalid.
- The start points to the first included passage. The end points to the first excluded passage, or the end of the spine.
- The reader recalculates page numbers after reflow. The resume position and bookmarks keep their source locations.

### Image views

- Each entry holds an image ID, `zoom` from 1 to 8, and the image center `x` and `y` from 0 to 1.
- The ID is a SHA-256 of the decoded dimensions and opaque pixels, computed on the engine worker. Reflow and repeated references keep the same ID. Changed image content gets a new ID.
- Fit removes the entry.
- The reader keeps the 64 most recently adjusted views. It drops the oldest when a checkpoint would go over 16 KiB.
- Older builds do not apply the views. Whether they keep them depends on their book-state handling.

### Page estimate

`page_estimate` keeps the estimated page numbers of chapter mode stable when the book opens again. It is derived data. Losing it only loses the stable numbers, never reading data. It is saved with the reading position and adds no write. Its `version` is 1.

- `weights`: the ten corrected weights for this book. They apply to every later opening. A missing weight, or one outside -1000 to 1000, means the reference weights are used.
- `layout`: a digest of the settings, viewport, slice size, rendering policy and registered font contents that decide the pages. Replacing a font file's contents invalidates it, even with the same family name.
- `spine_items`: the number of spine items.
- `place`: the spine item, slice start character, page in the part and pages in the part at the saved position. `page` and `item_page` are the book page and the spine-item page shown there. An invalid place is dropped.
- `read`: runs of spine items laid out as a whole: the first item, then the real page count of each. Empty items count 0. At most 500 items.
- `parts`: for items laid out in several slices, the item, its character count, then the start, end and page count of every slice. At most 2,000 slices.
- A run stops at its first invalid page. A read item keeps its saved pages until all its slices have a layout.
- `whole_book`: true when a whole-book layout saved the record, as Hybrid and More accurate do. Then `read` holds every spine item and `place` counts from the start of the spine item. The book opens in chapter mode with whole-book page numbers.
- `kept`: the estimated pages of each spine item as first shown, to two decimals, so the numbers do not move on reopening. Left out above 500 spine items, and once every item has real pages.
- `whole_spreads`: in two-column mode, the total screens of the completed whole-book layout. A chapter saves the page of its first column, so a backward turn after reopening keeps the same number. At the end of the book the number is the whole-book total.
- `place`, `read`, `parts`, `kept` and `whole_spreads` apply only when `layout` and `spine_items` match.
- When a checkpoint goes over 16 KiB, `read` goes first, then the whole record, then image views.
- Older builds ignore the record. Whether they keep it depends on their book-state handling.
