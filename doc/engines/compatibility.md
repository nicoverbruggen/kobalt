# Compatibility

This file compares CREngine and Crosspoint with what Kobalt needs. The revisions are in [Rendering engines](../engines.md#engines-and-revisions). Nothing was built or run for this audit: every statement comes from reading source. Paths that start with `S/` are in the patched CREngine source. Paths that start with `C/` are in the Crosspoint clone. Other paths are in Kobalt.

The CREngine source that was read is the patched tree in the dependency cache. It was not confirmed that this is the tree that the current pins select, because the worktree had no configured build.

## Gap classes

| Class | Meaning |
| --- | --- |
| Adapter | The adapter converts between the shared contract and the engine. |
| Shared | The application or the interface must change, for every engine. |
| Fallback | The interface shows less, and says why. |
| Engine | The engine must get new code. |
| Blocker | Not solved. A decision or a test is needed first. |

Severity: high stops the engine from being offered, medium loses a feature that users have today, low is a visible difference.

## Matrix

| UI requirement | Current Kobalt dependency | CREngine behavior | Crosspoint behavior | Evidence | Gap and proposed handling | Severity |
| --- | --- | --- | --- | --- | --- | --- |
| Open a book | The whole book is parsed and laid out before the first page | One DOM for all spine items. A valid cache skips the parse. | Parses the OPF and the contents once, then one spine item when it is shown | `S/src/epubfmt.cpp:1173-1189,1612-1668`; `C/lib/Epub/Epub.cpp:584-602`, `C/lib/Epub/Epub/Section.h:17-22` | Shared: the opening stages must allow "first page ready, more work pending". | Medium |
| Metadata and cover | Title, author, language, cover from the engine | Supported | Supported. Metadata text is cut at 512 bytes. Covers become BMP files. | `engine_document.cpp:721-797`; `C/lib/Epub/Epub/parsers/ContentOpfParser.cpp:42`, `C/lib/Epub/Epub.cpp:113-155` | Shared: read metadata in the shared publication model, not in each engine. | Low |
| Contents | Entries with a position and a page | From the DOM, with a page for each entry after layout | Nav first, NCX second. Entries point to a spine item and an anchor. No page. | `engine_document.cpp:1651-1707`; `C/lib/Epub/Epub.cpp:584-602` | Shared: contents come from the publication model. The page of an entry is a separate query that can be pending. | High |
| Spine and non-linear items | Page flows over the full page list | Flow for each page | Spine list in the book cache | `engine_document.cpp:818-835`; `C/lib/Epub/Epub/BookMetadataCache.h` | Shared: a spine model with a linear flag. | Medium |
| Page turn | Global page index | Supported | Next page in the section, then the next spine item | `engine_document.cpp:804-835`; `C/src/activities/reader/EpubReaderActivity.cpp` | Shared: turns become "next" and "previous" on a location, not on an index. | High |
| Jump to a page, scrubber | Global index and total | Supported after layout | No global page. Book progress is from bytes. | `reader_session.cpp:789-798`; `C/lib/Epub/Epub.cpp:1290-1300` | Shared and fallback: the scrubber works on book progress and chapters when the total is unknown. Needs a decision. | High |
| Chapter page labels | From global pages | Supported after layout | Supported for the current section only. The count is an estimate while the section builds. | `engine_document.cpp:1751-1832`; `C/lib/Epub/Epub/Section.h:110-113` | Shared: chapter-local page data is its own query, apart from global data. | Medium |
| Book percent and progress bar | `(page + 1) / pages` | Supported after layout | Bytes of spine items before, plus the page fraction in the chapter | `reading_progress.cpp:8-20`; `C/lib/Epub/Epub/BookMetadataCache.cpp:312-339` | Shared: a progress value with its basis (pages, words, bytes) and exact or estimated. Needs a decision. | High |
| Time remaining | Chapter words and the page fraction | Supported | Not checked | `word_estimates.h:14-121` | Shared: use the chapter-local fraction. | Low |
| Saved reading position | XPointer string and `dom_version` | Stable across font and viewport. Depends on the DOM version. | Spine index, page and a code point offset in the spine item | `engine_document.cpp:799-815`; `S/src/lvtinydom.cpp:11849-11937`; `C/src/activities/reader/EpubReaderActivity.cpp:287-306` | Blocker 1, then adapter: a shared location, with the XPointer kept as a hint. See [the location format](contract.md#location-format). | High |
| Position after a new layout | Anchor before, `goTo` after | Supported | The page is found from the offset, with a page fraction as fallback | `engine_document.cpp:993,1113-1114`; `C/src/activities/reader/EpubReaderActivity.cpp:2040-2060` | Adapter | Low |
| Bookmarks | One per page. Needs the page of every bookmark. | Supported after layout | Bookmarks exist in the firmware application, not in the library | `bookmark_session.cpp:80-197`; `C/src/BookmarkEntry.h` | Shared: a bookmark belongs to a location. Needs a decision. | High |
| Highlights | XPointer ranges, character precise | Character rectangles from `getSegmentRects` | Word precise only. Lines store the x position and the source range of each word. | `S/src/lvtinydom.cpp:14344`; `C/lib/Epub/Epub/blocks/TextBlock.h:21-50` | Adapter and fallback: the saved range stays exact. Crosspoint draws it to word edges and reports the geometry as approximate. | Medium |
| Selection at a point | Token hit test on the worker | `getNodeByPoint` | `wordAt(x, y)` is in the firmware application | `reader_text.cpp:859`; `C/src/activities/reader/ClipSelectionActivity.cpp:267` | Engine: move the word hit test into the Crosspoint fork. Character selection is unsupported there. | Medium |
| Dictionary | Text of a range and its sentence | Supported | Word text is available. The sentence is not checked. | `reader_text.cpp:1240` | Adapter | Low |
| Search | Index over the whole DOM | Supported. Does not need layout. | Not found in the source | `reader_text.cpp:1570-1653,1850`; search of `C/src` and `C/lib` | Engine or shared: search the spine item text in shared code, and ask the engine only for the location of a hit. Needs a decision. | Medium |
| Internal links | Rectangles in each frame, target as XPointer | Targets through one id map for the book | Rectangles for each page (at most 32). The target is a spine item and an anchor. | `reader_links.cpp:553-606`; `S/src/lvtinydom.cpp:3963`; `C/lib/Epub/Epub/Page.h:130-165`, `C/lib/Epub/Epub.cpp:1302-1326` | Adapter: targets become shared locations. The fixed limit is an embedded limit and must go. | Medium |
| Notes in a sheet | Note HTML from the DOM | Supported, with in-page footnotes as an option | Notes are normal content in the flow. No sheet. | `reader_links.cpp:436-481,701-721`; `C/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:2012-2013` | Shared: read the note text from the source XHTML in shared code. Needs a decision. | Medium |
| Web links | Ask, then open | Supported | Dropped at parse time | `C/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:132-135,2023-2026` | Engine: keep them in the fork. | Low |
| Return after a link | Position list in the application | Supported | A stack of three in the firmware application | `reader_build.cpp:510-517`; `C/src/activities/reader/EpubReaderActivity.h:117` | None. This is application code. | Low |
| Fonts | FreeType and HarfBuzz, embedded fonts, fallback faces, weight and optical size axes | Full shaping | Bitmap fonts at fixed sizes. TrueType only on boards with more memory. No HarfBuzz. Kerning and pair ligatures from a font table. | `engine_document.cpp:393`; `S/src/lvfntman.cpp:2587`; `C/lib/EpdFont/VectorFontSupport.h:12-16`, `C/lib/EpdFont/EpdFontData.h:165-231` | Engine and blocker 2: a host font back end is needed. Shaping quality is lower. Not equal to CREngine. | High |
| Publisher CSS | Full cascade, user CSS, Kobo style fixes | Supported | A fixed property list. No `@font-face`, `@import`, media queries, sibling or attribute selectors, pseudo-classes. No `line-height`, `letter-spacing`, `color`, `text-transform`, `font-family`. | `C/lib/Epub/Epub/css/CssParser.h:17-29`, `CssParser.cpp:949-1113` | Fallback: report as limited. Needs a decision on embedded fonts. | Medium |
| Reader settings | One JSON patch. Many settings make a full layout. | A change to a style makes a layout of the whole book | A fixed set: font, line compression, paragraph spacing, indent, alignment, viewport, hyphenation, embedded style, image rendering, character and word spacing | `engine_document.cpp:972-983`; `C/lib/Epub/Epub/Section.cpp:160-173` | Adapter and fallback: each engine reports which settings it supports. Others are shown as unavailable. | Medium |
| Hyphenation | Patterns for many languages | Supported | Patterns for eleven languages | `C/lib/Epub/Epub/hyphenation/generated/` | Fallback. Some pattern licenses need a check before they ship. | Low |
| Right-to-left text | Warning at open for right-to-left page progression | Bidi levels for each paragraph | Bidi with MiniBidi and Arabic shaping | `S/src/lvtextfm.cpp:1926`; `C/lib/MiniBidi/BidiUtils.h:27-31` | None for bidi. Quality was not compared. | Low |
| Vertical writing | Integrated into the repository fork. Unsupported modes warn on the current page. | Supported by the fork | Not found | [Current vertical formatter](../../src/engine/blaze/crengine/src/lvtextfm_vert.cpp); historical search of `C/lib/Epub` | Fallback: unsupported in Crosspoint. | Medium |
| Images | Deferred loading, an image viewer, a 16 megapixel limit | Supported, with SVG | JPEG, PNG, GIF, dithered into a 1-bit buffer. An 8 megapixel limit. No SVG. No WebP. | `src/publication/image_policy.h:10`; `C/lib/Epub/Epub/converters/ImageDecoderFactory.cpp:29-47` | Engine: decode through Kobalt's image code and draw to a `QImage`. | High |
| Tables | CREngine table layout | Supported | A grid for at most four columns and small rows. Other tables become stacked text. | `C/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h:152-155` | Fallback: limited. Decide if the limit is needed off the embedded device. | Medium |
| MathML | Bundled math face | Supported | Not found | Search of `C/lib/Epub` | Fallback: unsupported. | Low |
| Malformed XHTML | Tolerant HTML parser | Supported | An Expat error fails the spine item | `C/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:2928-2937` | Engine or fallback: a failed item must show an error, and the rest of the book must stay readable. | Medium |
| Page image | `QImage` of the body, with refresh data | Supported | A 1-bit panel buffer, with gray as extra passes | `engine_document.cpp:1507-1573`; `C/lib/GfxRenderer/GfxRenderer.h:37-62` | Engine and blocker 2: a new drawing back end. Refresh regions must come from the page model. | High |
| Layout time and cancellation | Stale counters. Progress callback. | No cancel hook. Progress only. | `buildSomeMore(maxPages, yieldNow)` returns after a bounded step. `requestStop` stops the parser. | `S/include/lvtinydom.h:290-333`; `C/lib/Epub/Epub/Section.h:99-122`, `ChapterHtmlSlimParser.cpp:2951-2955` | Shared: the contract reports whole-book work for CREngine and never promises to cancel it. | Medium |
| Threads | One worker for all engine calls | Process-wide font manager, cache and a static page context | Static state in the image decoders, the hyphenator and the image extractor | `S/src/lvpagesplitter.cpp:51`; `C/lib/Epub/Epub/converters/` | Adapter: one worker for each engine process state. No two layouts at the same time. | Low |
| Layout cache | Named after the publication fingerprint | DOM, rectangles, pages, contents and fonts in one file | Files for each spine item. The folder name is a hash of the path, not of the content. | `document_cache.h:54-70`; `C/lib/Epub/Epub.cpp:29`, `Section.cpp:85-173` | Engine: use Kobalt's fingerprint and cache policy in the fork. | Medium |
| KOSync position | XPointer as `progress` | Native | Converts its offset to and from a KOReader path | `kosync.cpp:172-205`; `C/lib/KOReaderSync/ChapterXPathResolver.h:8-36` | Adapter. The match with CREngine's own XPointer was not tested. | Medium |
| KOSync percentage | `page / (pages - 1)` | Supported after layout | From bytes | `reader_kosync.cpp:40-46` | Needs a decision. | Medium |
| Nickel progress and location | `ReadingActivity` with pages. The codec reads the CREngine DOM. | Supported | No equivalent | `reading_bridge.cpp:81`; `platforms/kobo/common/engine_positions.cpp` | Shared and blocker 4: the codec must work on the shared location and the source XHTML. | High |

## Unsupported and limited in Crosspoint

Unsupported means that no code for it was found. Limited means that code exists and its result is lower than CREngine's.

Unsupported:

- `@font-face` and embedded publisher fonts, `font-family`
- `line-height`, `letter-spacing`, `color`, `text-transform`
- `@import`, media queries, sibling and attribute selectors, pseudo-classes
- SVG, MathML, WebP
- Vertical writing
- Text search in a book
- Character-level hit test and selection
- Notes in a separate sheet
- A page total for the book
- Web links (dropped at parse time)

Limited:

- Text shaping: no HarfBuzz and no OpenType substitution beyond a pair table
- Font sizes: a scale between 0.5 and 3.0, mapped to prepared sizes
- Tables: a small grid, else stacked text
- Images: 8 megapixels at the source
- Links and notes on one page: fixed maximums
- XHTML must be well formed

## Limits in Crosspoint

Some limits exist because of the small memory of the ESP32. A fork for Kobalt does not need them:

- A parse buffer of 1 KB, free-memory checks before work
- A maximum CSS file size, a maximum number of rules, styles and selector length
- A maximum word length, CSS ancestor depth, table size, links and notes for each page

Other limits are part of the cache file format. To remove them, the format must change:

- 16-bit page count for each spine item, 16-bit spine and contents indexes
- 16-bit text length and x position for each line, 16-bit page coordinates
- 256 bytes for a link target

No maximum chapter size was found. Large chapters build in steps and keep partial files.

## Licenses and platform

- Crosspoint's own code is MIT. The firmware is GPLv3 only because it links wolfSSL. The EPUB and layout code needs wolfSSL only for content protection (`C/lib/Epub/BookKey.cpp`, `ContentProtection.cpp`), which a fork can leave out.
- Third-party code in the render path: Expat, miniz, uzlib, PNGdec, JPEGDEC, AnimatedGIF, FreeType, stb_truetype, MiniBidi, and code from atomic14 and EPDiy with notices to keep (`C/licenses/README.md`, `C/licenses/borrowed-code.txt`, read only in part).
- Hyphenation patterns have a license for each language. Crosspoint itself calls the fit of three of them an open question (`C/licenses/README.md:67-70`).
- The build is PlatformIO with the Arduino framework, as C++20. Kobalt builds as C++17.
- The `freeink-sdk` submodule is empty in the clone. It holds the FreeType adapter, the display, the storage layer and the content protection library.
- Platform ties in the render path: `HalStorage` and `HalFile` for every cache read and write, Arduino `Print`, `millis`, `delay` and free-memory checks, one FreeRTOS mutex, and image decoders that write into the panel buffer.
- A host test build exists (`C/test/CMakeLists.txt`). It compiles the chapter parser, the text layout and the CSS parser with stub headers. The stub renderer uses a fixed glyph width, so it does not prove real text measurement. `Section.cpp`, `Epub.cpp` and `GfxRenderer.cpp` are not in that build.

The smallest extraction for a host build is `C/lib/Epub` without content protection, the ZIP and inflate code, `Utf8`, `MiniBidi`, `Serialization`, `EpdFont` and `GfxRenderer`. The storage layer, logging and time calls need replacements. The drawing back end, the font registry and the image decoders need new code for a `QImage` target.

## What CREngine does today

- One DOM for the book. Each spine item becomes a `DocFragment` under one root (`S/src/epubfmt.cpp:1612-1625`). Ids and links get a prefix for each fragment.
- A full layout drops all styles, applies styles to the whole tree, lays out the root and makes one page list (`S/src/lvtinydom.cpp:5688-5859`).
- The page total is the length of that list. The page of a position is found from its absolute y, which needs the height of everything before it (`S/src/lvdocview.cpp:5794-5805`, `S/src/lvtinydom.cpp:21035-21069`).
- `partialRender` lays out one fragment again and moves the rest (`S/src/lvtinydom.cpp:5548-5686`). It needs a cache file, more than one fragment and one earlier full layout. Upstream notes that contents and other cached data can be stale after it, and the cache is not saved in that state. Kobalt does not call it, and Kobalt's kept rotation layout refuses to work with it.
- A change to the font size fails the render context check and starts a full layout. The DOM is kept (`S/src/lvtinydom.cpp:19949-19990`).
- The XPointer is a path from the root with element names, an index among siblings of the same name, a text step and an offset. Boxing nodes are skipped. It does not depend on the font or the viewport. It depends on the DOM version.
- The callbacks report progress and cannot stop a load or a layout (`S/include/lvtinydom.h:290-333`).

Not read in CREngine: the bodies of most patches, `renderBlockElement`, the line breaker, the text storage, and the cache load code. The unit of a text offset (UTF-32) is taken from one bounds check and is not proven.
