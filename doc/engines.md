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

## Engines and revisions

Blaze was imported from the local `koblaze` revision `6d3946edf08ef69afe19a3c457b88af126ae64aa`. Its upstream base is `koreader/crengine` at `4ce1aed17e9d7852e047e0b9b0bf6a637486fd97`. Changes since the import are in Kobalt’s Git history.

The original audit also examined Crosspoint at `0333cbbe4649f80afafe83061bf1afe9fd372fcf`. The [dependency inventory](engines/inventory.md), [compatibility comparison](engines/compatibility.md), [proposed contract](engines/contract.md) and [migration plan](engines/migration.md) record the design work that preceded Blaze. Kobalt ships Blaze as its only engine.
