# Migration and tests

This file gives the order of work for the [shared contract](contract.md), the migration of saved data, the test strategy and the feasibility of koblaze, the chapter-oriented CREngine fork. It is a plan. [Rendering engines](../engines.md#status) lists what is implemented.

## Order of work

Each step leaves a build that works and passes the native suite. CREngine keeps its whole-book layout through all of stage A.

### Stage 0: prove the location format

1. Write the round-trip test for CREngine XPointers and shared locations on the book corpus (`ctest --preset mac -L corpus`). This uses test code only.
2. Record a performance baseline with [Release evaluation](../release-evaluation.md) before any refactor.

The location format is fixed only after step 1 passes or its failures are understood.

### Stage A: CREngine behind the contract

| Step | Change | Check |
| --- | --- | --- |
| A1 | Move the plain types (`ChapterProgress`, `SearchHit`, `SearchCursor`, `TextRange`, `OpeningStage`, `AnnotationGeometry`, `PageLink`, `LinkDetails`) out of the engine headers into shared headers. No behavior change. | Build, source checks, the model group |
| A2 | Add the contract types and an engine interface. `EngineDocument` implements it. `ReaderSession` holds the interface. | Full native suite |
| A3 | Add a fake engine for tests. It leaves chapters without layout on purpose and can return each result state. | New contract tests |
| A4 | Replace the page integers in `ReaderFrame` with `PageInfo`. Move each user (header and footer, progress, scrubber, finish prompt, chapter labels, bookmarks, Stats) to the query states. CREngine returns exact values, so the pixels do not change. | Full native suite, gallery comparison |
| A5 | Add the shared location next to each saved XPointer. See [Saved data](#saved-data). | Storage tests, upgrade tests |
| A6 | Change `PositionCodec` to work on shared locations and the source XHTML. Remove the friend access of the Kobo codec. | Kobo adapter tests with pewter, both SDK checks |
| A7 | Move the CREngine capability rules of `inspectLayout` into the adapter as capabilities. | Layout tests |
| A8 | Audit the remaining includes of `engine/crengine` outside the adapter and add a source check for them. | `cmake -P platforms/checks/source-checks.cmake` |

A source check with a new allowlist is a decision for the user, as the project rules say.

Gate: CREngine through the adapter passes the full native suite, the gallery has no pixel change, the upgrade tests pass and the evaluation shows no regression against the baseline. Crosspoint work on the application side starts after this gate.

The user set the order after stage A: koblaze first, Crosspoint later.

### Stage B: Crosspoint

1. Record the upstream revision, the licenses and the notices in the fork. Decide where the fork lives.
2. Make a host build of the smallest extraction. Replace the storage layer, logging and time calls. Remove content protection.
3. Write the drawing back end for a `QImage` and the font back end. This needs the code of the `freeink-sdk` submodule or new code on Kobalt's FreeType.
4. Vertical slice: open a real EPUB, show the full contents, go to a chapter without layout, draw the page, turn pages, change the font size, save and restore the location.
5. Add the essential features that the decisions require. Report the others through capabilities.

Steps 1 to 3 do not depend on stage A and can start earlier in the fork.

### Stage C: koblaze

See [koblaze feasibility](#koblaze-feasibility). It starts after stage A, because it needs the page model of step A4.

## Saved data

The rules of [Storage compatibility](../storage.md) apply. The migration adds data and removes nothing.

- Each saved anchor keeps its XPointer field with the same name and meaning. A shared location is added as a new optional field next to it. Older records load with the field absent.
- A new optional field keeps the current format version, if an older build keeps unknown fields when it saves. This was not checked for each store. If a store drops unknown fields, that store needs a format version bump and a migration.
- The location is written when the CREngine adapter has the book open and the conversion is exact. It is not written from a guess.
- The conversion can run again at any time with the same result. A record that already has a location with the current version is left as it is.
- The XPointer is never removed or rewritten by the migration. To go back to an older build, nothing must be undone: the older build reads the XPointer.
- An anchor that does not convert keeps its XPointer and gets no location. The interface can list such anchors. They still work in CREngine. In another engine they are reported as unresolved and stay in the list.
- An anchor that is made in another engine has a location and no XPointer. An older build cannot use it. This is the reason to gate other engines behind a setting.
- KOSync keeps the XPointer as `progress`, because that is what KOReader uses. An engine without native XPointers converts the location to that form and reports the precision.

Tables that need the new field: the book record (`position`, `finished_position`), bookmarks, highlights (four anchors), saved words and reading history.

## Test strategy

| Check | How |
| --- | --- |
| CREngine behavior through the adapter | The current native suite, unchanged, plus the upgrade and restart tests |
| Each result state | Contract tests against the fake engine: exact, estimated, pending, unsupported, failed, and cancelled or superseded outcomes |
| Contents stay stable | The fake engine fills page data chapter by chapter. The contents list keeps the same entries and order while the page of an entry goes from pending to exact. |
| Location round trips | Convert, change the font size and the viewport, convert back. On the corpus, for each engine. |
| Change of engine | Open in one engine, switch, check the location and the reported precision. Legacy records stay readable by an older build. |
| Stale results | Fast settings changes, close and open, engine switch, with the fake engine holding results back |
| Features | The audited behavior for links, selection, search and annotations in each engine, with the unavailable ones shown as such |
| Performance | `platforms/eval/scenarios.json` scenarios for cold and warm open, first page, chapter change, page turn, font size change, peak memory and cache size, for each engine on the same books |

Fixtures: the checked-in Trevelyan's Type Tester book, the corpus, and focused synthetic books for one very large spine item, links between spine items, and notes in another spine item. Slow cases are reported, not only averages. Desktop results are labeled as desktop results.

## koblaze feasibility

koblaze is a fork of CREngine that lays out the spine item that the user reads, and not the whole book. It keeps CREngine's shaping, kerning, ligatures, hyphenation and page composition. Stock CREngine stays as the baseline.

### What the source allows

CREngine has `partialRender`, which lays out one `DocFragment` again. It is the nearest existing code. It has these conditions (`S/src/lvtinydom.cpp:5485-5745`):

- A cache file exists and the book has more than one fragment.
- The book had one full layout before. The code reads the old rectangle of the fragment and of the fragment before it.
- After it, cached data such as contents pages can be stale, and the cache is not saved.

There is no code path that lays out one fragment on first open.

### What depends on the whole DOM

| Dependency | Evidence | Can it be scoped |
| --- | --- | --- |
| Parsing builds one DOM for all spine items | `S/src/epubfmt.cpp:1612-1668` | Not in a first stage. The DOM, the XPointers, the id map and the link prefixes need it. Parse time stays whole-book. |
| Absolute y of a node is the sum of all heights before it | `S/src/lvtinydom.cpp:21035-21069` | Needs estimated heights for fragments without layout, or page lists for each fragment |
| One page list for the book. The total, the page of a position and contents pages read it. | `S/src/lvdocview.cpp:2350-2357,5794-5805` | Needs a page list for each fragment and the page model of the contract |
| Styles: the document sheet and the root styles apply to each fragment | `S/src/lvtinydom.cpp:5805-5811,21177-21214` | Shared. The sheet and fonts of a fragment are already scoped. |
| Notes: a link and its note are matched in one page context | `S/src/lvtinydom.cpp:5620-5637` | A note in another fragment is missed today. Needs a rule. |
| List numbering and margins between fragments | `S/src/lvtinydom.cpp:5556-5565,5753` | Approximated today |
| The cache holds layout for the whole DOM and cannot be saved from a partial state | `S/src/lvtinydom.cpp:5742-5745` | Needs a cache format with layout for each fragment |
| The kept rotation layout refuses partial mode | `S/src/lvtinydom.cpp:19826,19871` | Needs new code or is unavailable in koblaze |
| A static page context and process-wide font manager | `S/src/lvpagesplitter.cpp:51` | One layout at a time. No change needed. |
| Layout can add boxing nodes to the DOM | `S/src/lvtinydom.cpp:5797-5799` | Must be checked for each fragment, because XPointers skip these nodes |

### Goal and boundary

The goal is set by the user: koblaze works like Kobo's own reader does for KEPUB books, and prepares more in the background.

- To open a book, koblaze reads the package file and the contents. It does not parse or lay out the text of the book.
- It parses and lays out only the spine item that the user reads.
- It prepares the next and the previous spine item in the background.
- When the user acts, the background work stops at once and the engine serves the user.
- It has no layout of the whole book and no cache of the whole book.

This goes further than `partialRender`. The earlier proposal in this audit kept one parsed DOM for the book and scoped only the layout. That proposal does not meet the goal, because the first open would still parse every spine item.

The boundary that follows from the goal:

- One document object for the book stays, with one `DocFragment` for each spine item, in spine order. A fragment is empty until its spine item is needed. CREngine already keeps an empty fragment for a spine item that fails to parse, so that the indexes of later fragments do not change (`S/src/epubfmt.cpp:1650-1655`). XPointers and shared locations of loaded fragments stay as they are.
- Parsing, the style pass, the layout and the page list are for one fragment. Each fragment has its own page list.
- The contents come from the package and navigation files. A contents entry names a spine item and an anchor. Its page is a query that can be pending.
- A link to another spine item resolves to that spine item and anchor from the link text. The target fragment is loaded when the user follows the link.

Consequences:

- The book page and the page total are unknown until each fragment before the position has a layout. The contract reports them as pending or estimated. An estimate can come from the byte size or the word count of each spine item.
- Search needs the text of every spine item. It must read spine items that are not loaded, from the source and not from the DOM, or load them in the background.
- The word counts for Stats read the source today and stay.
- A note whose text is in another spine item needs that item parsed. The rule must be explicit.
- The document cache of CREngine holds the whole DOM and its layout. It does not fit. Whether a small cache for each spine item is worth its storage writes on a Kobo is a question for measurement, not an assumption.
- The kept rotation layout holds the layout of the whole book and needs a version for each fragment, or is not available.
- A construct that needs layout across spine items is reported, and the book falls back to stock CREngine with a message.

### Stopping background work

CREngine cannot stop a load or a layout today. Its callbacks report progress and return nothing (`OnLoadFileProgress`, `OnNodeStylesUpdateProgress`, `OnFormatProgress` in `include/lvtinydom.h`). Kobalt uses these points to pause the engine worker for sleep and USB (`EngineActivity::checkpoint`). It cannot abandon the work.

koblaze must add this:

- A cancel request that the progress points check. When it is set, the parse or the layout of the background fragment ends early and leaves the fragment as not loaded. The fragments that have a layout stay valid.
- The document must stay usable while the layout of another fragment is abandoned. The page that the user reads comes from a fragment that the background work does not touch.
- The time between two progress points is the longest wait for the user. During layout, CREngine reaches a progress point after each final block, which is about one paragraph (`updateRenderProgress(1)` in `src/lvrend.cpp`). It calls the callback at most once in 300 ms and one percent step (`src/lvpagesplitter.cpp`). The cancel check can use the point for each block, without that limit. The style pass and the parser have coarser points. The wait must be measured on a Kobo, for a very large spine item too.
- CREngine is built with C++ exceptions. The layout calls are recursive, so an exception from the progress point is one way to leave them. Whether every layout object is safe to unwind is not checked.

The engine worker is one thread. A page turn waits for the worker, so a pause is not enough: the background work must give the worker back.

### Measured on the desktop

These numbers come from the Mac, in a debug build of Kobalt with the release build of CREngine, on Standard Ebooks editions at 1264 by 1680 pixels. They show proportions, not Kobo times.

| Book | Spine items | Pages | Parse (ms) | Layout and pages (ms) | New layout after a font size change (ms) |
| --- | --- | --- | --- | --- | --- |
| Dr Jekyll and Mr Hyde | 14 | 154 | 4 | 22 | 19 |
| Pride and Prejudice | 65 | 738 | 17 | 106 | 90 |
| Moby-Dick | 145 | 1313 | 26 | 186 | 163 |
| Middlemarch | 102 | 1864 | 42 | 281 | 246 |
| Ulysses | 25 | 1748 | 111 | 327 | 284 |
| War and Peace | 393 | 3526 | 77 | 487 | 429 |
| Les Misérables | 426 | 3581 | 105 | 504 | 451 |
| Decline and Fall of the Roman Empire | 102 | 10170 | 282 | 1848 | 1545 |

- Layout is the larger part: about four to eight times the parse time in these books.
- Both grow with the size of the book. One spine item is a small part of either. War and Peace has 393 spine items.
- A font size change costs almost as much as the first layout.
- A book with few, large spine items gains less. Ulysses has 25.

The Kobo times for the same split are not measured.

### Constraint profile

The profile lists what koblaze takes from Crosspoint's model. Each entry has a reason. Small fixed limits from the embedded device are not copied.

| Constraint | Purpose | Visible effect | Policy |
| --- | --- | --- | --- |
| Parsing and layout are for one spine item | The work is bounded by the chapter | Book page numbers can be unknown | Report as pending or estimated |
| No layout state crosses a spine item border | A fragment can be laid out alone | Margins do not collapse across the border. A spine item starts on a new page. | Check at open. Fall back to stock CREngine if the book needs otherwise. |
| The position is kept as a location, not as a page | A new layout of one chapter restores it | None | Always |
| Caches are for each spine item and each settings set | A font change makes a layout of the visible chapter only | Other chapters build when reached | Old pages are never shown as exact |
| Preparation of the next item has a time and memory limit | The interface stays responsive | The first turn into a chapter that was not prepared can wait | Show the loading state |

Not taken from Crosspoint: the CSS subset, the table limits, the font limits, the link and note maximums and the 16-bit format fields. They do not make the layout incremental. Restricting CSS or tables alone does not change how CREngine makes pages.

### Measured with the fork on the desktop

Same conditions as above. "Three spine items" loads the item in the middle of the book and its two neighbors into the open book, parsed and laid out.

| Book | Whole book (ms) | Three spine items (ms) | The three around the largest item (ms) | One spine item (ms) | Font size change in three (ms) |
| --- | --- | --- | --- | --- | --- |
| Pride and Prejudice | 126 | 7 | 11 | 6 | 4 |
| Moby-Dick | 216 | 7 | 16 | 6 | 3 |
| Middlemarch | 325 | 12 | 23 | 5 | 7 |
| War and Peace | 566 | 12 | 15 | 10 | 4 |
| Les Misérables | 611 | 14 | 25 | 10 | 5 |
| Ulysses | 440 | 68 | 138 | 28 | 36 |
| Decline and Fall of the Roman Empire | 2132 | 51 | 193 | 28 | 35 |
| The Divine Comedy | 592 | 260 | 262 | 97 | 177 |

- The pages of the loaded spine items are the same as in the whole book. `engine.spine-window` requires this on a generated book, also for a read from the first page to the last and back through moving windows.
- One page takes about 5 ms to draw, which is as much as one ordinary spine item takes to load.
- The time follows the pages of the loaded spine items. The Divine Comedy has three parts of several hundred pages each. For such a book, the user proposed to lay out a limited number of pages first and the rest in the background. This is not built.
- A stopped layout returns in under a millisecond after the request, and the next layout gives the same pages (`engine.layout-interrupt`).

### Page estimate

`measurePageEstimate` reads the XHTML of a spine item once and keeps the width of each block of text in units of the font size. `estimatePages` then counts lines and pages for a page size with arithmetic only. It uses typical character widths, not the reading font.

On the same books, the estimated start of each spine item was compared with its real start, in percent of the book:

- The largest error was under 1.5 percent for most books and 4.7 percent for the worst (The Divine Comedy). A plain character count had a largest error of 5.2 percent and was worse in most books.
- The estimated page total was 82 to 87 percent of the real total for prose, and 64 to 76 percent for verse. One factor from the spine items that have a real layout can correct it for prose.
- The measuring pass took 3 to 293 ms for a book, which is about one sixth of a whole-book open. It reads every spine item, so it belongs in the background. Its result does not depend on the font or the page and can be kept.
- A new estimate after a font size change took under a quarter of a millisecond.

### Stages

The fork exists: the branch `kobalt` in the koblaze checkout is upstream CREngine at the pinned revision with each Kobalt patch as one commit. Its tree is the same as the patched tree that Kobalt builds. On the Mac, Kobalt adds one line to CREngine's `CMakeLists.txt` after the patches (a rename of two FreeType functions); that step is not in the fork yet.

1. Build Kobalt from the fork in place of the pin and the patches, with the same output. Move the FreeType rename into a CMake option of the fork.
2. Measure on a Kobo: the split of a cold open and of a font size change into parse, style, layout and page split, and the time between two progress points.
3. Add the cancel request at the progress points.
4. Load and lay out one fragment on demand, with empty fragments for the others. A complete spine item is the unit.
5. A page list for each fragment, and the page model of the contract in Kobalt for unknown book pages.
6. Background preparation of the next and the previous fragment, stopped by user input.
7. Smaller steps inside one very large fragment, if the measurements show that it is needed.

Each stage is compared with stock CREngine on the same books, fonts and settings: text order, line breaks, glyph positions, page borders inside a chapter, images and links. Differences are investigated and the allowed ones are listed.

An earlier prototype of a chapter layout mode exists outside this repository. It was not read for this audit, by the user's choice.
