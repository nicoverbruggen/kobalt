# Rendering engines

Blaze is Kobalt’s EPUB rendering engine. It is a hard fork of KOReader’s CREngine, which grew out of CoolReader. Kobalt maintains the fork in this repository and builds it for Kobo, Android and desktop. See [source provenance and licenses](engines/koblaze.md).

## Status

The reader opens books through the [Engine interface](../src/engine/engine.h). The [Blaze adapter](../src/engine/blaze/adapter/) translates reader operations into library calls. The [library](../src/engine/blaze/crengine/) owns the EPUB document, fonts, shaping and page layout. Its file names, build target and XPointer format reflect its CREngine ancestry.

The Rendering engine setting selects how Blaze lays out the book.

| Mode | What happens when a book opens |
| --- | --- |
| Hybrid (default) | Blaze opens at the saved passage using chapter layout, then prepares the whole book in the background. The reader adopts the completed layout on a page turn. Another appearance or a rotation goes back to the chapter, and the whole book follows again. |
| More accurate | Blaze lays out the whole book before it opens. The reader has exact page numbers from the start. |
| Fast (debug override) | Blaze keeps using chapter layout. This exercises reading, search and navigation while whole-book pages are unavailable. |

Some content needs whole-book layout even in chapter mode, including linked footnotes when the reader shows them below the text. See [reader settings](reader/settings.md), [navigation](reader/navigation.md) and [layout validation](validation/layout.md).

## Hybrid on Kobo

Most Kobo models use one processor core, including the Clara BW. KOReader’s device table marks only the Elipsa, the Sage, the Elipsa 2E, the Libra Colour and the Clara Colour as multi-core. On one core, the reader window, the engine worker and the whole-book build share that core with each other and with Nickel. A Kobo is about 25 to 40 times slower than the Mac for layout work (see [Cost](engines/migration.md#cost)). Hybrid stays fast on these devices because of the rules below. Keep them when you change Blaze, its adapter or the reading session.

### The reader comes first

- Background work runs in short steps and stops when other work waits. The engine worker counts each request from another thread, `engineWorkRequests()` in [engine_worker.h](../src/engine/engine_worker.h). Idle work checks that count between steps. A tap waits for one step at most: one 8 KB part of the page estimate, one 20 ms step of the parsed-book cache write, or the time to the next stop check of the whole-book build.
- The whole-book build runs on its own thread, but never at the same time as the engine worker. The two threads hand the engine to each other, so the build adds no second busy thread. Any queued engine work parks the build at its next stop check, not only a page turn. The next quiet moment continues the build where it stopped, so no work is lost. See `FullBuild` in [reader_source.cpp](../src/engine/blaze/adapter/reader_source.cpp).
- A step that is not needed for the next page waits for idle time. The documents that a turn or an appearance change replaced are destroyed after the neighbour pages, because their teardown can write the parsed-book cache. While the reader shows the whole book, the parsed-book cache is written in idle steps, so closing has little left to write.
- A change of appearance or a rotation goes back to the chapter. A new layout of the whole book would hold up the reader. The whole book follows in the background.
- A next slice that a tap stops keeps the pages that it drew, so the turn does not draw them again.
- When the whole book is ready, idle time prepares the turn that adopts it. The whole book goes to the page on screen and draws the page after it, with the changed rectangle against the page on screen, the targets of its links and the pages of its contents. The turn then only changes documents. In two columns the whole book can pair the columns into screens otherwise than the chapter. A turn stays in the chapter if switching would repeat or skip a column. A numbered jump uses the chapter’s page number and switches only if the whole book shows the same spread. A jump to a passage can switch directly. A turn or jump that stays in the chapter keeps its layout, so lookups such as bookmark pages stay valid. They start again when a frame of the whole book reaches the reader. The whole book keeps the chapter files that the Nickel position codec parsed for the chapter, because that cache belongs to the open book. A turn in the chapter moves the page on screen, and the next idle moment prepares again. A backward turn draws its page itself.

### Do each piece of work once

- The text measurements of the page estimate, search and links do not depend on the layout. A new document of the same book takes them over, so an appearance change does not measure the book again.
- The contents place a spine item without a layout at its start, or by its characters. Neither needs a read of its XHTML.
- The whole-book build finds the start of each spine item while it builds, in one walk over the fragments. A page turn does not wait for that later.
- The Nickel position code parses a chapter source once and keeps its text once, with a range for each ID. The reader asks for the place of every page, mostly in the same chapter.
- Search rules out chapters by their runs of text before it searches them.
- The user style sheet is hashed once per change, not on each turn. To find whether a link goes to a note, the reader reads each attribute of the target’s ancestors once, not once for each note keyword.
- Large inputs load on first use. The bundled CJK collection opens when text requests a CJK face. Hyphenation patterns for a language load when text in that language first needs them.

### Start from what is already known

- Before it measures, Hybrid tries the whole book from the parsed-book cache. That needs no parsing and has the real pages. Without a cache file the try ends at once.
- The `page_estimate` record keeps the real pages of each spine item for the settings of chapter mode, so a book that opens again shows its counts at once. After an appearance change, the slices keep the opening slice size, so that the saved record still fits. See [Saved](engines/migration.md#saved).
- The engine has room for 16 documents. The whole-book build goes on only while three are free. It drops the kept units first, and without room it waits for a later idle moment.

### Repaint only for a visible change

Each refresh costs processor time and an e-ink update. Estimate rounds publish to the session without drawing the page. A round that changes nothing that the reader shows does not refresh it. Once the page fields show numbers, the reader refreshes for them twice at most: when Calculating… changes to an estimate, and when the counts become exact. Other rounds wait for the next page turn.

### Build interface parts in quiet time

The reader builds the Text and Layout panels first, then Settings, one panel at a time and only after 2 seconds without input. A panel that opens before then is built when it opens. Text preset tiles are drawn on a fixed canvas without a sample document. A tile that is drawn in the background gives way when reader work is queued, such as a chapter jump, and is drawn again after that work.

### Nickel needs the processor too

Nickel's event thread runs with idle priority (`SCHED_IDLE`). While Kobalt keeps the processor busy, for example with the whole-book build, that thread gets no time. Never wait for it on a path that the user waits for. [Sync data](sync-data.md#reading-time) explains how the close avoids it.

### Measure as on a device

`KOBALT_KOBO_EMULATION=20` makes each engine job on a desktop take about 20 times as long. The worker sleeps for the added time instead of using the processor. So it shows when work stops and how long a tap waits, but not two threads that compete for one core. Check that on a Kobo, or in VirtualNickel limited to one CPU. The Book status field of the header and footer shows the progress of the background layout and the last turn time. Diagnostics record each background step as `engine-work-start` and `engine-work-end`, and each panel opening as `panel-open` (see [Release evaluation](release-evaluation.md)).

## Engines and revisions

Blaze was imported from the local `koblaze` revision `6d3946edf08ef69afe19a3c457b88af126ae64aa`. Its upstream base is `koreader/crengine` at `4ce1aed17e9d7852e047e0b9b0bf6a637486fd97`. Changes since the import are in Kobalt’s Git history.

The original audit also examined Crosspoint at `0333cbbe4649f80afafe83061bf1afe9fd372fcf`. The [dependency inventory](engines/inventory.md), [compatibility comparison](engines/compatibility.md), [proposed contract](engines/contract.md) and [migration plan](engines/migration.md) record the design work that preceded Blaze. Kobalt ships Blaze as its only engine.
