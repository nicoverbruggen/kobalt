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
| A8 | Audit the remaining includes of `engine/blaze/adapter` outside the adapter and add a source check for them. | `cmake -P platforms/checks/source-checks.cmake` |

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

In chapter mode only the slice on the screen has a layout. The rough page number, the page total, the saved percent and the value for Nickel come from a page map of the whole book (`src/publication/page_estimate.h`).

#### Algorithm

1. Measure. `PageEstimateMeasurer` reads the XHTML of each spine item once, in parts of 8 KB. For each block it keeps the width of its text in units of the font size, its kind (prose, verse line, table cell, heading, image or rule), its indent, and the quotations, list items and table rows that start before it. The text after a line break is a verse line. The result does not depend on the font size or the page.
2. Count. For one page, `pageEstimateFeatures` counts the lines of each kind of block: the width of the text divided by the width of the line, rounded up. It also counts paragraphs, quotations, list items, table rows, images, and the parts that start on a new page. This is arithmetic only.
3. Weigh. A linear model gives the lines of a part from these counts. Its reference weights were fitted on whole spine items of 22 Standard Ebooks. A heading line counts as about five lines, because headings have large margins. A table row adds about two and a half lines. A part that starts on a new page adds a little less than half a page, which is the empty end of its last page.
4. Correct. Each slice or spine item that gets a layout gives its real page count. The map fits the weights again with these observations, by weighted least squares that is drawn towards the reference weights. One observation does not change the estimate much. Many observations of one kind of text move the weights to that book. An observed part counts with its real pages. When every spine item has a layout, the map is exact. A slice gives the characters that it really holds, also when its layout was made longer to end on a page that starts a block. A unit of several short spine items gives the pages of each spine item. The spine item that is opened ahead adds its pages to the same map. A stopped layout gives nothing. A change of settings that changes the pages forgets the observations. Dark mode, the text colors and the outline thickness keep them.
5. Show. The page number comes from the map: the pages before its spine item, plus the pages before its slice in the spine item, plus the page in the slice, plus one. Spine items and slices with a layout count with their real pages, the others with the estimate. Only the stretches of a spine item without a layout add the empty ends of their slices. So when everything before the page had a layout, the page number is the real page, and the last page of a book read from the start is the real total. Every layout counts once, also the one at the open and the layout of a spine item without text, such as a cover image.
6. Hold. The map changes when a spine item before the page gets its real pages later, for example after a jump. The page number takes such a change at the next jump or the next start of a spine item, not while the user turns forward inside a spine item. A forward turn always adds one page, also past a spine item without pages, and at the start of a spine item the number can move up to the map. It moves down only at a jump. A turn that passes text without a layout, such as slices without a page, counts as a start too. The total takes changes at once, but is never smaller than the page number.

The page fields of the header and footer show these numbers. The chapter of the chapter page is the spine item. Until every spine item is measured or has all its pages, the total is only a guess from the bytes. In that time there is no book page number and no total, and each page field shows its percent form. That percent is the bytes of the whole book before the page: the spine items before it, and the characters before the page in its spine item. Once the map is complete, the page fields show page numbers at the next page turn or repaint that happens anyway; there is no extra repaint. A total is estimated until every spine item of the book, or every part of the spine item, had a layout. A spine item that had a layout without pages counts with zero pages.

The measuring runs on the engine worker after the pages beside the current page, in the idle time after a turn. It takes at most 32 parts in one idle time and stops when the user asks for another page. Until a spine item is measured, its pages come from its size in bytes and the pages per byte of the measured spine items.

#### Fast interactions

Chapter mode keeps one immediate spare unit beside the current document and a bounded cache of previously visited units. At the first page of a unit, cancellable background work prepares the previous unit through its last page. At the end of a unit it prepares the next one. Crossing a unit boundary retains the departed document in the spare slot, so immediate reverse turns can reuse it. The cache checks layout settings, viewport, stylesheet and slice size before reuse. A tap that arrives before preparation finishes can still pay the cold load cost.

Fast keeps slice-local page indices for painting, selection and bookmark marks. `pagesOf()` returns -1 for a position outside the loaded slice. Display labels use `bookPages()` instead, with an estimated whole-book page or a pending value. Highlight end labels use the preceding visible character because the saved end is exclusive. Saved lists retain their anchors and remain navigable while page labels are pending.

The worker publishes completed measurement batches to the session without repainting the reading page. An open contents list updates its page labels in place. Reading activity and time-left calculations use whole-book progress; Fast pace observations use source positions so repeated frames do not count as turns. The same chunked XHTML measurement counts source words for the whole book. Source word prefixes split the counts at contents entries without laying out other chapters. Fast uses these counts for chapter bars and time estimates after measurement finishes. CSS-hidden source text can make them differ from Accurate’s styled-document counts; Fast does not write them into Accurate’s word-count cache.

Links load their target unit on demand. Missing-target classification checks the target chapter for the requested ID; unmarked legacy notes retain the accepted guessed kind. Search caches source text separately, loads matching units with cancellation, and keeps the unit origin in its cursor across background replacement. A shared content map is not implemented.

#### Measured accuracy

Measured with `kobalt-page-estimate-tests <folder>` on the 22 Standard Ebooks, at 1264 by 1680 pixels, a 44 pixel font, 150 percent line spacing and 80 pixel margins. The real pages come from a whole-book layout. For each spine item after the reading position the start page was compared with its real start. The table gives the mean over the books of the largest error, the largest error of any book, and the error of the total. The model used weights fitted on the other 21 books, so that no book was in its own fit. Only spine items before the reading position had a layout.

| Read | Method | Mean of largest error | Largest error | Mean error of the total | Largest error of the total |
|---|---|---|---|---|---|
| Nothing | First estimator | 1.04 % | 4.72 % | 17.7 % | 36.4 % |
| Nothing | Page map | 0.68 % | 2.29 % | 3.5 % | 15.1 % |
| 1 % | Bytes of the spine item on the screen | 2.48 % | 11.48 % | 11.2 % | 93.3 % |
| 1 % | Page map | 0.66 % | 2.29 % | 3.3 % | 15.1 % |
| 5 % | Bytes | 2.48 % | 11.48 % | 5.6 % | 22.9 % |
| 5 % | Page map | 0.63 % | 2.29 % | 3.2 % | 15.3 % |
| 20 % | Bytes | 2.48 % | 11.48 % | 7.1 % | 30.1 % |
| 20 % | Page map | 0.69 % | 2.29 % | 2.2 % | 15.3 % |
| 50 % | Bytes | 2.12 % | 9.52 % | 6.1 % | 22.1 % |
| 50 % | Page map | 0.49 % | 2.29 % | 1.3 % | 15.3 % |

- The largest errors of the page map are in the poems of T. S. Eliot: one spine item holds almost the whole book, so the survey has nothing to correct with. In chapter mode the slices of that spine item correct it. The survey does not model slices.
- On a six-inch page (1072 by 1448 pixels, a 32 pixel font, 48 pixel margins) with the reference weights, the mean of the largest error was 0.89 % before reading and 0.56 % after half of the book. The weights were fitted at the larger page only.
- On the checked-in Trevelyan's Type Tester, which is not in the fit, the total before any layout is 111 percent of the real one and the largest start error is 1.8 % of the book. Read forward in slices of 6 pages, the page number adds one for each turn and the last page is the total.
- Real glyph advances of the reading font were tried in place of the fixed table of character widths, in a prototype. With fitted weights they gained almost nothing (0.68 % against 0.70 % for the mean of the largest error). They also need the font on the worker, so the map uses the table.

#### Chapter mode survey

`kobalt-page-estimate-tests --chapter <book.epub>` reads one book page by page in chapter mode and compares what the reader shows with a whole-book layout, on a six-inch page (1072 by 1448 pixels, a 32 pixel font, 48 pixel margins). It reads forward from the start with the measuring done at the open (`measured`) or never (`unmeasured`), and opens the book at 5, 20 and 50 percent. It prints one CSV row for each checkpoint; the comment above `chapterSurvey()` lists the fields. Run it once per book, with `QT_QPA_PLATFORM=offscreen`, and collect the rows.

On 203 books of a private library (2026-11-12), after the fixes above:

- Read forward, the page number never kept or lost its value on a forward turn, in both modes.
- At the last page, the page, the total and the real total were the same, and the total was not estimated, in every book. Before, the page ended up to 43 pages ahead of the real page (Proust, 4113 against 4070) in 117 books, and 84 books kept an estimated total because a spine item without text was never counted.
- No total was shown before the map was complete. Before, the total at an unmeasured open was a median 175 percent too high. Measuring a whole book took a median 26 ms and at most 174 ms on the Mac.

Errors in percent of the real total, as median / 90th percentile / worst. The position error is that of the page number, or of the percent where no page number is shown.

| Checkpoint | Total error before | Total error after | Position error before | Position error after |
|---|---|---|---|---|
| Unmeasured open | 175 / 251 / 596 | not shown | 0.00 / 0.34 / 2.22 | 0.22 / 0.43 / 2.22 |
| Measured open | 2.7 / 10.6 / 50.7 | 2.7 / 10.6 / 50.7 | 0 / 0 / 0 | 0 / 0 / 0 |
| Measured, 5 % | 1.4 / 4.2 / 16.1 | 1.4 / 4.2 / 16.6 | 0 / 0 / 0 | 0 / 0 / 0 |
| Measured, 20 % | 0.7 / 2.4 / 16.6 | 0.7 / 2.2 / 16.6 | 0.00 / 0.10 / 0.26 | 0 / 0 / 0 |
| Measured, 50 % | 0.4 / 1.2 / 14.0 | 0.4 / 1.2 / 14.0 | 0.00 / 0.34 / 0.68 | 0 / 0 / 0 |
| Measured, 100 % | 0.3 / 0.6 / 1.2 | 0 / 0 / 0 | 0.26 / 0.64 / 1.19 | 0 / 0 / 0 |
| Jump to 20 %, measured | 2.1 / 7.5 / 27.1 | 2.0 / 7.7 / 27.1 | 0.44 / 1.28 / 6.24 | 0.42 / 1.28 / 6.24 |
| Jump to 50 %, measured | 1.9 / 7.7 / 43.0 | 1.9 / 7.7 / 43.0 | 1.04 / 3.49 / 23.95 | 1.02 / 3.49 / 23.95 |
| Jump to 50 %, unmeasured | 2.2 / 6.7 / 66.8 | not shown | 1.10 / 3.52 / 24.17 | 0.48 / 2.01 / 6.84 |

A scale of the text weights for each book, from the observed parts with prose, was tried. It lowered the error after a jump (total at 50 %: 3.8 against 7.7 for the 90th percentile) but not at the open, where only a cover or a title page has a layout, and it made the worst total at 5 and 20 % larger. It is not used.

#### Cost

On the Mac, measuring a whole book took 3 to 299 ms. One part of 8 KB took at most 0.4 ms, with one outlier of 1.5 ms. An observation, with the new fit and the new map, took at most 0.2 ms. Recorded layout times of a Kobo Clara BW suggest that a Kobo is 25 to 40 times slower than the Mac. That gives, as an estimate only, 10 to 16 ms for one part, and 0.1 to 12 seconds of idle time to measure a book. A tap waits for at most one part.

#### Saved

The measurement is kept for the open book only. Saving it would cost a storage write of up to about a megabyte for a large book, and measuring again after opening is short.

A small record, the book-state field `page_estimate` ([Storage compatibility](../storage.md)), keeps the page numbers stable when the book opens again. It holds the corrected weights, the page numbers at the saved position and the real pages of the spine items that were read as a whole. It is saved with the reading position, so it adds no write. When the book opens again at the same position with the same settings, the corrected weights become the starting weights, and the page on the screen keeps the saved number once the measuring is done. A save in that time keeps the saved number; after a turn the number comes from the map. With other settings, only the weights carry over. `engine.page-estimate` checks this, and also damaged, out-of-range and oversized records. `session.reading-book` checks the save and the new opening through the book state on both storage backends, and that a checkpoint over its byte limit drops the read spine items first, then the record, before image views.

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
