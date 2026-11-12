# Rendering engines

This document describes the plan to let Kobalt use more than one rendering engine. It is the result of an audit of the source. The first steps are implemented: see [Status](#status). Every statement about an engine comes from the source at the revisions below. Statements that were not checked are marked as such.

| File | Covers |
| --- | --- |
| [Dependency inventory](engines/inventory.md) | Where the application, the interface and saved data depend on the engine today |
| [Compatibility](engines/compatibility.md) | What CREngine and Crosspoint do for each requirement, the gaps and their severity |
| [Shared contract](engines/contract.md) | The proposed contract, the location format, result states, scheduling and interface policies |
| [Migration and tests](engines/migration.md) | The order of work, saved-data migration, the test strategy and the feasibility of the chapter-oriented CREngine fork |

## Status

| Step | State |
| --- | --- |
| Location format and the CREngine conversion (`src/publication/location.h`, `src/engine/blaze/adapter/reader_locations.cpp`) | Implemented and tested. Not used for saved data yet. |
| Plain engine types in `src/engine/`, outside the CREngine adapter | Done |
| `Engine` interface (`src/engine/engine.h`). `ReaderSession`, the opening job and style samples use it. CREngine is the only implementation. | Done. The interface repeats CREngine's page model; the result states of the [shared contract](engines/contract.md) are not in it yet. |
| Query states and `PageInfo` (`src/engine/query.h`), and `Engine::pageInfo`. CREngine reports exact values. | Done and tested. The interface does not read `PageInfo` yet: it still reads the page integers. |
| Repository-owned koblaze source in `src/engine/blaze` | Imported from the tested fork. See [source provenance](engines/koblaze.md). |
| Desktop, Kobo and Android build the repository engine source | Configured through one engine build definition. See [Building](building.md#local-engine-source). |
| koblaze: a window of spine items (`koblazeSetSpineWindow`), and a layout that stops when the callback asks (`OnRenderCancelRequested`) | Done in the fork, as experiments. `engine.spine-window` and `engine.layout-interrupt` check them. They skip with stock CREngine. |
| Chapter mode: Fast in the Rendering setting, the default, or `KOBALT_CHAPTER_MODE=1`, which wins over the setting. With the fork, the reader loads the unit containing the reading position and prepares nearby content on the engine worker. | Whole-book links, search, saved positions and reflow pass the focused Mac parity checks. Search loading can be cancelled, and its cursor survives replacement of the prepared document. Both Kobo SDK compile/link and adapter checks pass; physical-device validation remains pending. |
| Slices: in chapter mode one spine item is shown, laid out in slices of about 25 pages of text (`KOBALT_CHAPTER_PAGES` sets another size). Only the slice on screen has a layout. One block that is longer than a slice is laid out whole. | Works in `engine.chapter-slices` and on real books. |
| Fast in the interface: contents, bookmarks, highlights, saved words, Stats and time-left fields use estimated whole-book pages. Slice-local pages remain internal to geometry and page marks. Navigate uses a read-only indicator, chapter jumps and adjacent page previews in Fast. | Focused headless Mac checks cover saved-list jumps, estimate updates and the finish prompt at the real book end. The Rendering engine choice reloads the publication when Settings closes. The content map and other Rendering-setting follow-ups remain deferred. |
| Page estimate (`src/publication/page_estimate.h`): a page map of the whole book that becomes more precise as the user reads | Used by chapter mode for the rough page, the total, the saved percent and the value for Nickel. `engine.page-estimate` checks it. Not checked on a Kobo. See [the estimate](engines/migration.md#page-estimate). |
| Everything else in [Migration and tests](engines/migration.md) | Not started |

## Engines and revisions

| Engine | Source | Revision | Status |
| --- | --- | --- | --- |
| CREngine upstream | `koreader/crengine` | `4ce1aed17e9d7852e047e0b9b0bf6a637486fd97` | Base of the imported koblaze fork. |
| Crosspoint | `crosspoint-reader/crosspoint-reader` | `0333cbbe4649f80afafe83061bf1afe9fd372fcf` | Audited only. Not forked. |
| koblaze | `src/engine/blaze`, the repository-owned CREngine fork | Imported from `6d3946edf08ef69afe19a3c457b88af126ae64aa`; subsequent changes belong to Kobalt history. | The active engine, with Accurate and Fast modes. |

WPE WebKit is a possible later engine. The contract must not prevent it. No work on it is planned.

## Summary of the audit

1. Kobalt already has one seam. The application talks to `EngineDocument` through `ReaderSession`. Positions cross as strings, geometry as `QRect`, pages as `QImage`. The seam is not engine-neutral: its types live in the engine header, the Kobo position codec reads the CREngine DOM, and tests include CREngine headers.
2. All saved anchors are CREngine XPointer strings: the reading position, the finished position, bookmarks, highlights, reading history and the KOSync `progress` field. Each book also stores a `dom_version`. No page number is saved.
3. The application assumes one page list for the whole book. Page turns, the scrubber, one bookmark per page, chapter page labels, percent, the finish prompt and Kobo native progress read a global page index or the total.
4. CREngine parses the whole book into one DOM and lays out the whole DOM. It can lay out one spine item again, but only after one full layout, and upstream treats that state as degraded. Kobalt does not use this. CREngine cannot cancel a load or a layout.
5. Crosspoint lays out one spine item at a time, in resumable steps, and never knows the page total of the book. Its position is a spine index and a code point offset, and it already converts that offset to and from a KOReader path. It lacks several things that Kobalt has today. See [Compatibility](engines/compatibility.md#unsupported-and-limited-in-crosspoint).

## Blockers, in order

| # | Blocker | Kind | Needed before |
| --- | --- | --- | --- |
| 1 | Solved for clean XHTML: a CREngine XPointer converts to a source-based location and back. See [the test result](engines/contract.md#test-result-for-crengine). Books with malformed markup are not tested. | Adapter | Books from other tool chains |
| 2 | Crosspoint's text measurement and drawing code is not in the clone. The `freeink-sdk` submodule is empty, and it holds the FreeType adapter, the display and the storage code. Its renderer writes to a 1-bit panel buffer. | Engine work | Any Crosspoint vertical slice |
| 3 | The interface needs a page model that works without a book total: turns, scrubber, bookmarks, chapter labels, percent, finish prompt. | Shared application work | Any chapter-based engine |
| 4 | Kobo native progress drops observations when the page total is zero, and the Nickel position codec reads the CREngine DOM. | Shared application work and adapter translation | A chapter-based engine on Kobo |
| 5 | CREngine has no code path that parses and lays out one spine item on first open, and none that stops a layout. koblaze must add both, with a page list for each spine item. See [koblaze feasibility](engines/migration.md#koblaze-feasibility). | Engine work, in stages | koblaze |
| 6 | Crosspoint is built as C++20 for ESP32. Kobalt builds as C++17 for Qt 5.2 and Qt 6. The port was not compiled. | Unresolved until a host build exists | Crosspoint adapter |
| 7 | There is no fake engine. Model tests open real books through CREngine and some include CREngine headers. | Shared application work | Contract tests |

## Decisions

Decided by the user:

- When the page total is unknown, Kobalt shows no page number. This applies to the contents list and to search results, which name the chapter and no page.
- A percent can be an estimate. The same estimate is good enough for the progress display, for the KOSync `percentage` and for Nickel.
- The estimate must be fast and can be rough. The user proposed a quick layout without kerning and other detail, or an approximation of how Nickel counts pages. The method is not chosen yet.
- The order of work is the adapter, then koblaze. Crosspoint comes later.
- Kobalt uses koblaze through a symlink for now. A Git submodule is preferred later.

Still open:

1. The method for the estimate. A count of words or characters for each spine item, scaled by the chapters that already have a layout, needs no second layout pass. Kobalt already counts the words of each chapter from the source.
2. Bookmarks are one per page today. With no global page list, a bookmark can belong to a location, and its page mark can come from the layout of the chapter on screen.
3. The scrubber needs a book total today. It can move by estimated percent and by chapter.
4. For Crosspoint, later: which missing features must be built, where its fork lives, and whether the engine is a choice for each book or for the device.
