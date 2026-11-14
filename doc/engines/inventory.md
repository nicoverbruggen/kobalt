# Dependency inventory

This file lists where Kobalt depends on the rendering engine today. Each row separates the requirement of the user from the CREngine mechanism that serves it now. Line numbers refer to `develop` at `0f7c9813`. See [Rendering engines](../engines.md) for the context.

## The seam today

- `EngineDocument` (`src/engine/blaze/adapter/engine_document.h`) is the only engine class. Its public interface uses Qt types.
- `ReaderSession` (`src/reading/reader_session.h`) owns the `EngineDocument`. It runs each engine call on one worker thread and publishes a `ReaderFrame`. The interface reads the page, the page total and the chapter page from the frame.
- `ReadingBook` (`src/reading/reading_book.h`) owns saved data and native progress. It does not call the engine.
- The Library does not use the engine. It reads the OPF itself (`src/library/book_metadata.h`). This was checked by a search, not by a full read.

## Engine types outside `src/engine`

A wrapped or renamed type is still a dependency. These remain.

| Leak | Where |
| --- | --- |
| `engine_document.h` is included directly | `src/reading/reader_session.h:2`, `annotation_draft.h:2`, `style_sample.h:2`, `src/app/reader/panels/search_panel.h:7`, `src/app/reader/views/reading_page.cpp:8` |
| Shared types are defined in the engine header: `ChapterProgress`, `SearchHit`, `SearchCursor`, `TextRange`, `OpeningStage`, `AnnotationGeometry` | `src/engine/blaze/adapter/engine_document.h:50-70`, used in `reader_session.h` and `annotation_draft.h` |
| `reader_links.h` (`PageLink`, `LinkDetails`, `linkAtPoint`) | `src/app/reader/panels/link_panel.h:6`, `src/app/reader/reader.cpp:1744` |
| `engine_worker.h` and `engine_activity.h` | `src/reading/reader_session.cpp:7,10`, `opening_job.cpp:3`, `style_sample.cpp:2-3`, `platforms/kobo/common/native_lifecycle.cpp`, `reader_coordinator.cpp` |
| `PositionCodec` functions take `const EngineDocument &` | `src/platform/api/position_codec.h:12-13` |
| The Kobo codec is a friend of `EngineDocument` and walks `ldomNode` and `ldomXPointer` | `platforms/kobo/common/engine_positions.cpp`, `engine_document.h:346` |
| `<lvfntman.h>` for `LVFontManager::MaxEmbeddedFontBytes` | `src/publication/publication.cpp:13,308,320` |
| `<lvtinydom.h>`, `lUInt32` in a header | `src/engine/blaze/adapter/document_cache.h:7,18,59` |
| Static `EngineDocument::setPageFormat` | `platforms/kobo/nickel-qt6/refresh.cpp:71` |
| The engine reads the saved book record (`position`, `dom_version`) | `src/engine/blaze/adapter/engine_document.cpp:528,667-672` |
| CREngine capability rules outside the engine (`inspectLayout`) | `src/publication/layout_compatibility.h`, `src/reading/opening_job.cpp:288-313` |
| The engine header includes application headers and calls `strings::get` | `engine_document.h:2-18`, `reader_links.cpp:638,715-719` |
| CREngine XPointer strings and `dom_version` in saved data | See [Saved data](#saved-data) |
| Tests include `lv*` headers and `engine_document.h` | `tests/model/`, `tests/desktop.cpp`, `tests/support/` (from a search, not read) |

## Opening and publication

| Requirement | CREngine mechanism | Evidence | Assumes a full layout |
| --- | --- | --- | --- |
| Open a book and show the stages | `OpeningJob` runs on the worker. The engine parses the whole book and makes all pages before `Ready`. | `src/reading/opening_job.cpp:246-331`, `OpeningStage` at `engine_document.h:50` | Yes. `layoutReady_` means that pages exist (`reader_session.cpp:585,2286`). |
| Title, author, cover, language | `getTitle`, `getAuthors`, `coverImage`, `state()["language"]` | `engine_document.cpp:721-797,1871` | No |
| Resources and identity | `Publication::inspect` hashes the ZIP directory. `ResourceCache` serves resources. | `src/publication/publication.h:13-44`, `opening_job.cpp:126,318` | No |
| Table of contents | `tocChapters` walks `LVTocItem` and resolves each entry to a page. A host can supply `ChapterTarget` values. | `engine_document.cpp:1651-1707`, `reader_positions.cpp:96-118` | Partly. `ChapterLink.page` accepts -1 for an unknown page. |
| Spine order and non-linear items | There is no spine object. `getPageFlow(page)` is read for each page. | `engine_document.cpp:818-835` | Yes |
| Word counts for Stats | `wordCounts` reads the source at the end of opening. | `opening_job.cpp:380-381` | No |

## Navigation and progress

| Requirement | CREngine mechanism | Evidence | Behavior with an unknown total |
| --- | --- | --- | --- |
| Page turn | `turnTarget(oldPage, delta)` then `go(int)`. The end of the book is "next page equals page". | `reader_session.cpp:766-787,2212-2232`, `engine_document.cpp:804-835` | Breaks. The global index is the turn primitive. |
| Jump to a page, scrubber | `go(target)` with `target < pages()`. The scrubber takes the page, the total and the chapters. | `reader_session.cpp:789-798`, `reader_build.cpp:884-906` | Breaks |
| Go to a chapter | `goTo(chapter.position)` | `reader.cpp:2498-2517` | Works. The page column needs pages. |
| Current chapter | `chapterAround` compares XPointers in source order. | `engine_document.cpp:1727-1738` | Works |
| Chapter page and chapter pages | Computed from global pages. The last chapter ends at `pages() - 1`. | `engine_document.cpp:1751-1832`, `reader_session.cpp:42-50` | Wrong or empty |
| Header and footer fields | `headerfooter::Values` from the session | `reader_settings.cpp:87-93`, `header_footer.cpp:41-75,358-412` | Empty. Each formatter returns nothing when the total is zero. |
| Book progress bar and percent | `(page + 1) / pages` | `reading_progress.cpp:8-20` | Zero. No progress from words or bytes exists. |
| Time remaining | Chapter words times the page fraction in the chapter. The fallback uses pages left. | `word_estimates.h:14-121`, `reading_book.cpp:280-297` | Empty |
| Stats graph | Words, or chapter lengths in pages | `reader_refresh.cpp:762-785`, `chapter.h:33-52` | The fallback needs all chapter pages. |
| Finish prompt | `page * 10 >= pages * 9`, or the last page | `reading_book.cpp:636-648` | Never shown |
| Return after a link | A list of `{position, page, chapter}` in memory. The return uses the position. | `reader_p.h:616-624`, `reader_build.cpp:510-517` | Works. Only the label needs a page. |

## Saved data

One format is used everywhere: the string from `ldomXPointer::toString()`, at most 4096 characters.

| Item | Stored as | Evidence |
| --- | --- | --- |
| Reading position | `position` in the book record, with `dom_version` | `reading_book.cpp:845-871`, `engine_document.cpp:799-815,1872,1911` |
| Finished position | `finished_position`, compared as a string | `reading_book.cpp:641-659,858` |
| Bookmarks | `Bookmark{position, chapter, excerpt, created}` | `src/storage/bookmark_store.h:10-15`, `doc/storage.md` |
| Highlights | `start`, `end`, `chosen_start`, `chosen_end`, plus text, chapter, color, style, note | `src/storage/highlight_store.h:9-25`, `highlight_store.cpp:47-52`, `profile_database.cpp:233-237` |
| Saved dictionary words | The start of the range | `reader.cpp:1910`, `src/storage/saved_words.h:20` |
| Reading history | Start and end positions | `src/storage/reading_history.cpp:65-107` |
| Library percent | An integer from pages, written only when the total is known | `reading_book.cpp:865-866` |
| Nickel location | A separate format, converted by the Kobo codec | `platforms/kobo/common/engine_positions.cpp` |

Page numbers and chapter pages are removed before the book record is saved (`reading_book.cpp:889-891`).

Bookmarks need the page of every bookmark: there is one bookmark per page, bookmarks on the same page are merged, and the layout key includes the page total (`src/reading/bookmark_session.cpp:16-24,80-116,151-197`). Bookmarks do not work when the total is zero.

## Sync

| Requirement | Mechanism | Evidence |
| --- | --- | --- |
| KOSync upload | `progress` is the raw XPointer string. `percentage` is `page / (pages - 1)`. | `src/sync/kosync.cpp:172-205,886-918`, `doc/sync-data.md` |
| KOSync download | `goTo(position)` on the remote XPointer. The choice dialog needs the page of that position. It uses the remote percentage when the page is unknown. | `reader_session.cpp:816-903`, `reader_kosync.cpp:587-652` |
| Nickel progress | `ReadingActivity{position, percent, page, pages, chapters, chapter}`. Observations are dropped when the total is zero. | `src/reading/reading_bridge.h:11-58`, `reading_bridge.cpp:81` |
| Native save, duplicate check | Skips a save when the anchor, the page and the total did not change. | `reader.cpp:761-778` |

## Links and notes

| Requirement | Mechanism | Evidence |
| --- | --- | --- |
| Tap a link | Each frame carries the links of the page with rectangles. The hit test runs in the application, with no engine call. | `reader_session.cpp:149`, `reader_links.cpp:553-606`, `reader_links.h:42-80` |
| Link kind, target and note text | `linkDetails` on the worker. It reads DOM markup and style hints and returns HTML from the DOM. The application draws the note sheet. | `reader_session.cpp:1466-1476`, `reader_links.cpp:81-104,436-481,654-721` |
| Valid passage target | The target must have a global y coordinate of zero or more. | `reader_links.cpp:679-681` |
| Web links | The application asks before it opens them. | `doc/reader/navigation.md` (not read in full) |

## Selection, highlights, dictionary and search

| Requirement | Mechanism | Evidence |
| --- | --- | --- |
| Word and token at a point | `getNodeByPoint` on the worker | `reader_text.cpp:859,986,1048` |
| Word, sentence and punctuation rules for a range | Inside the engine wrapper | `reader_text.cpp:546-850,1104-1260` |
| Range rectangles and handles | `getSegmentRects`, with flags for right-to-left and vertical text | `reader_text.cpp:443,1417-1450,1546` |
| Highlights on the page | The application gives the saved ranges to the session. The worker keeps the visible ones and resolves rectangles. Painting is in the application. | `reader_session.h:283-286`, `reader_session.cpp:90-136` |
| Overlap and order of ranges | XPointer comparison in the engine | `highlight_session.cpp:208,483` |
| Dictionary | `textRange` returns the text and its sentence. Lookup does not use the engine. | `reader_text.cpp:1240`, `reader.cpp:1881-1910` |
| Search | The worker builds a text index over the whole DOM and searches in batches. A hit has XPointers and a page. A new layout drops the index. | `reader_text.cpp:1570-1653`, `engine_document.cpp:989-992` |

## Typography and settings

- Settings go to the engine as one JSON patch (`engine_document.cpp:871-968`). `text_presets` and `appearance_settings` do not include the engine.
- A full layout follows a change to the font, the size, spacing, margins, alignment, hyphenation, weight, the beautifier rules and similar settings (`engine_document.cpp:972-982`). A change to the viewport or the columns also makes a new layout (`:983,1084-1091`).
- Dark mode, outline thickness and "hide non-linear" make no new layout.
- One other viewport layout is kept for rotation (`engine_document.cpp:1005-1017,1104-1112`).
- The position survives a layout through an anchor: the XPointer of the page start is read before and applied after (`engine_document.cpp:993,1113-1114`).
- Previews and style samples open more `EngineDocument` objects (`reader_session.cpp:514-560`, `style_sample.cpp:244-312`).
- The font lists, the weight axes and the fallback face list come from the engine. The font manager is one object for the process.

## Rendering, input and refresh

- A page is one `QImage` of the body. The application draws the header and the footer (`engine_document.cpp:1507-1573`).
- Refresh data travels as text keys on the image: image regions, color tiles, text only, image only (`engine_document.cpp:1556-1565`, `src/render/page_content.cpp:83-93`).
- Turn damage for the neighbor pages is computed on the worker (`engine_document.cpp:1310-1320`).
- The engine keeps the current, previous and next page images (`engine_document.cpp:1357-1374`).
- The pixel density is a process value and is part of the cache identity (`src/reading/reading_density.h:10`).

## Threads, cancellation and caches

- One worker thread runs every engine call. The main thread never waits for it.
- Cancellation is by stale counters (`newest`, `newestLayout`, `newestPreview`), not by interruption (`reader_session.h:468-472`). `open` and `applyAppearance` have a progress callback and no cancel token.
- A closed book stays in memory and is used again when the same book opens (`reader_session.cpp:202-258`, `opening_job.cpp:244-271`).
- The parsed-book cache is named after the publication fingerprint. Its checksum covers the layout policy, the format version and the DOM version (`document_cache.h:54-70`). `rendering_version` forces new pages without a new parse (`platforms/cache-policy.cmake`).

## Existing test seams

- A fake `ReadingBridge` exists (`tests/support/reading_bridge_fixture.h`).
- The KOSync HTTP client can be replaced (`reader_kosync.cpp:32-35`).
- Progress, pace, speed, header and footer, bookmark store and sync logic have tests without an engine.
- There is no fake engine and no fake `ReaderSession`.

## Not read

The audit did not read these files in full: `src/engine/blaze/adapter/reader_source.cpp`, `reader_text.cpp` bodies, `column_view.h`, the scrubber and chapter graph widgets, `src/dictionaries`, `src/storage` beyond the stores named above, `src/sync/sync_coordinator.cpp`, the Android and desktop hosts, and most of `platforms/kobo`. More users of the page total can exist in `src/ui` and `src/app/reader/views`.
