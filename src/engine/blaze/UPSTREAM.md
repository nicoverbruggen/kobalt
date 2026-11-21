# Blaze changes for CREngine maintainers

Hi there, KOReader/CREngine maintainers. Thank you so much for making an awesome engine.

This is Blaze. It is a hard fork of KOReader's CREngine. This file lists what Blaze changed in the code it inherited, so that CREngine maintainers can decide what, if anything, they want to take. Please take what you find useful.

Blaze starts from `koreader/crengine` commit `4ce1aed17e9d7852e047e0b9b0bf6a637486fd97`. Links go to the Blaze source. In inherited files, a `Blaze:` comment marks most changes.

Each entry says how we know about the problem. "Reproduced" means that we took the Blaze change out again, ran a small input through Blaze with AddressSanitizer, and saw the failure go away with the change. "Reproduced in stock KOReader" means that a control EPUB failed in KOReader v2026.07.1. Otherwise we found the problem by reading the code.

Each entry starts with a short summary and then gives the details. **Code:** names the Blaze source to read. **Upstream:** gives the state of upstream `master` at `a459d926` (2026-11-21):

- "Present in `master`" means that master still has the behavior that Blaze changed.
- "Partly in `master`" means that master changed the same code after Blaze's base.
- "Not in `master`" means that master has no such feature or interface.
- An entry that we did not compare with master says so.

Speed figures are single trials of one operation on one device, a Kobo Clara BW, unless the entry names another machine. They do not add up to a gain for a whole book.

Block quotes are notes from Kobalt's author.

The sections come in this order. In each section, the entries that we expect to be most useful to CREngine maintainers come first.

- [Speedups](#speedups): less work or less memory for the same result.
- [Features](#features): CSS and EPUB support that upstream does not have.
- [Bug fixes](#bug-fixes): crashes, leaks, undefined behavior and wrong output that KOReader users can meet.
- [Hardening](#hardening): defects that need damaged or crafted input, or that we could not reach in KOReader.
- [How Blaze opens a book in parts](#how-blaze-opens-a-book-in-parts): how Blaze loads part of a book, lays out part of a chapter, stops work and builds the whole book in the background.
- [Changes for Kobalt's reader](#changes-for-kobalts-reader): host interfaces that Kobalt needs. They are listed for completeness and are unlikely to fit upstream.

## Speedups

### Large JPEG images decode at a smaller size

**Summary:** a JPEG that is drawn smaller than its size is decoded at a reduced scale, and its rows are scaled as they arrive.

Upstream decodes the full image and then scales it down. When the drawing target accepts a smaller raster, Blaze asks libjpeg for 1/2, 1/4 or 1/8 of the size, whichever is still at least the target size, and scales each row as it arrives with the filters and rounding of `qSmoothScaleImage`. It does this only when the image size is a multiple of the MCU size, so that padding cannot bleed into the last row or column. This needs two new decoder callback methods, `GetRasterTargetSize` and `OnStartRasterDecode`. We took the idea from web engines such as Firefox, which decodes images at the size they are drawn ([Downscaler.h](https://searchfox.org/mozilla-central/source/image/Downscaler.h)).

It helps only images that are drawn at half their size or less. Covers and photos in EPUBs are often that large. Decoding and scaling such a JPEG took roughly a quarter of the CPU time. The output is not the same pixel for pixel, so check the quality on real images.

**Code:** [lvimg.h](crengine/include/lvimg.h), `LVJpegImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp), the decoder callback in [lvdrawbuf.cpp](crengine/src/lvdrawbuf.cpp), and `SmoothRowScaler` in [qimagescale_p.h](crengine/qimagescale/qimagescale_p.h).

**Upstream:** present in `master`.

### PNG images decode one row at a time

**Summary:** a PNG that is not interlaced needs one row of memory, not the whole image.

Upstream reads all rows into one buffer and then hands them to the drawing callback, which uses each row before the next one. Blaze reads and hands over one row at a time. An interlaced PNG still needs all rows, and Blaze limits that buffer to 64 MB, because inline data images skip the resource size check.

In a desktop test, the peak memory fell from 61 to 30 MiB, and the output was the same pixel for pixel. Decoding and scaling took about a sixth less CPU on a Clara BW, but almost no less in a Kobo emulator. The memory is the main gain.

Two behaviors change. Without smooth scaling, a PNG that fails halfway has already drawn its first rows. With smooth scaling, rows are buffered, and `OnEndDecode` with an error now skips the draw, where upstream drew the partial image.

**Code:** `LVPngImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### Packed hyphenation patterns

**Summary:** hyphenation patterns can load from a compact binary file, with the XML patterns as a fallback.

Loading the German XML patterns is slow on an e-reader. `HyphMan::compilePatterns` turns a pattern file into a packed file ahead of time, and the loader then reads it without parsing XML. Loading German took about 95% less CPU on a Clara BW. Eight more dictionaries took 70 to 93% less in a Kobo emulator, with the same breaks. The format holds patterns whose letters fit in Latin-1, so it is for Latin-script languages. The packed file records the checksum of its source, and a missing, damaged or old packed file falls back to the XML.

In Kobalt the host supplies the packed files. Upstream could build them with its pattern files instead.

**Code:** [packed_hyphenation.inc](extensions/packed_hyphenation.inc) and [hyphman.cpp](crengine/src/hyphman.cpp).

**Upstream:** not in `master`.

### Selector matching skips rules that cannot match

**Summary:** the style pass tests fewer selectors per node, with the same cascade order.

Upstream already checks the class of the first rule with `quickClassCheck`, but it still walks every selector. Blaze adds two filters. Active stylesheets index the selectors that require a class, so a node only visits the selectors for its own classes and the selectors without a class. Each selector also records the element names and classes that its ancestors must have, and a quick ancestor check rejects it before the full match. The merge keeps the cascade order: a tie still goes to the element chain.

Both filters are the same idea as in browser engines. Blink's [RuleSet](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/css/rule_set.h) groups rules by id, class and tag, and WebKit's [SelectorFilter](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/css/SelectorFilter.h) rejects descendant selectors with a Bloom filter of ancestor names and classes.

The gain depends on the stylesheet. On a fixture with many class rules, opening took about a seventh less CPU. Ordinary books gain little.

**Code:** [css_ancestors.cpp](extensions/css_ancestors.cpp), [css_class_index.cpp](extensions/css_class_index.cpp), and `quickAncestorCheck` and the merge in [lvstsheet.cpp](crengine/src/lvstsheet.cpp). The index updates in `detachScope` follow Blaze's stylesheet scopes, described in the next entry.

**Upstream:** present in `master`.

### Stylesheet scopes copy selectors only when needed

**Summary:** entering and leaving a DocFragment stylesheet no longer copies every selector twice.

Upstream `LVStyleSheet::push()` copies every selector chain with `dup()`, and `pop()` copies them all again with `set()`. The style pass does this for each DocFragment, so a book with hundreds of fragments and a large stylesheet pays it hundreds of times. Blaze's `push()` only records the selector count. The first `parseAndAdvance` or `merge` after a push copies the chains and keeps the original, and `pop()` swaps the original back without a copy.

**Code:** `SavedScope`, `push` and `pop` in [lvstsheet.h](crengine/include/lvstsheet.h), and `detachScope` in [lvstsheet.cpp](crengine/src/lvstsheet.cpp). `ScopedPush` belongs to Blaze's cancellation support and is not needed upstream.

**Upstream:** present in `master`. We did not measure this change on its own.

### Sibling walks find the next child fast

**Summary:** a walk over the children of a large parent is linear, not quadratic.

Looking up the index of a child scans the child list from the start. Blaze first tries the entries beside the last result for the same parent. It still compares each entry, so a wrong guess only costs the normal scan. This matters for flat books, where one chapter body holds thousands of paragraphs. We did not measure it on its own.

**Code:** `findChildIndex` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Line breaks are kept when a paragraph reflows

**Summary:** a paragraph that is formatted again with the same text and language does not run libunibreak again.

The document keeps the break decisions of recent paragraphs, keyed by text, language, `line-break` and `word-break`. A new width or font size then reuses them. It covers paragraphs of one text node with up to 2048 characters, in 127 slots, so a document keeps at most 127 × 2048 characters. A reflow of a test fixture took about 6% less CPU, a steady 10 ms. On an ordinary book (Trevelyan's Type Tester) the cache was used, but the gain was too small to see.

**Code:** `paragraphBreaks` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp), declared on `ldomDocument` in [lvtinydom.h](crengine/include/lvtinydom.h).

**Upstream:** present in `master`.

### List items are numbered once

**Summary:** numbering a list is linear, not quadratic.

Each list item finds its number by counting its previous siblings, so a list of n items costs n² steps. Blaze numbers all items of a list once and keeps the numbers by data index. Ordinary lists of a few dozen items gain nothing that you can measure. Lists with thousands of items, such as an index or a glossary, gain the most. We did not measure it.

**Code:** `getListItemCounters` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp) and its use in [lvrend.cpp](crengine/src/lvrend.cpp).

**Upstream:** present in `master`.

### Hyphenation patterns load on first use

**Summary:** a language's hyphenation patterns load when text first needs them, not when the language is seen.

Upstream's `TextLangCfg` constructor resolves the hyphenation method, which loads the patterns, for every language tag in the book. A book that tags a few foreign words then loads their patterns too. Blaze resolves it on the first call to `getDefaultHyphMethod()`, which `softHyphenateText` also uses. The method is filled lazily without a lock, so it is not safe to format the same language on two threads for the first time at once.

**Code:** `TextLangCfg::getDefaultHyphMethod` in [textlang.cpp](crengine/src/textlang.cpp).

**Upstream:** present in `master`.

### Script lookup for ASCII

**Summary:** ASCII characters get their script without a HarfBuzz call.

`measureText` and `addLine` call `hb_unicode_script` for every character. For ASCII, Blaze returns Latin for letters and Common for everything else. This is a small gain that we did not measure on its own.

**Code:** `koblazeUnicodeScript` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Cache of shaped words

**Summary:** each font face remembers shaped words and pair kerning, so common words are not shaped again.

Each face keeps a cache of shaped words and kerning by language, with up to 50,000 words for each language. Plain Latin runs are measured from that cache. A sample of them is still shaped with HarfBuzz, and the widths and flags are compared, to catch differences. The face clears its caches when font settings change.

The cache must fill before it saves time, so it only pays off in large books. On a Mac, with the cache turned off, a small book opened a little faster and a font size change took about a quarter less time, but a large book took about half again as long to open (330 to 485 ms). Kobalt therefore turns it on only for books with at least 250,000 characters of text (`fastShapingMinimum` in [engine_document.h](adapter/engine_document.h)). The engine itself has no such limit: the host decides. This is a large change and needs a design discussion.

> We also tried seeding the cache with common words. It was not useful, but it probably deserves another look.

**Code:** [word_cache.cpp](extensions/word_cache.cpp), [fast_shaping.inc](extensions/fast_shaping.inc) and [shaping_options.cpp](extensions/shaping_options.cpp), compiled into [lvfntman.cpp](crengine/src/lvfntman.cpp).

**Upstream:** not in `master`.

## Features

### Vertical writing

**Summary:** `writing-mode: vertical-rl`, with pages that can each use their own writing mode.

Blaze lays out and draws vertical text for Japanese books. This includes upright and rotated glyphs, ruby, emphasis marks (`text-emphasis`), tate-chu-yoko, logical CSS properties, and borders and backgrounds in vertical boxes. A horizontal fragment in a vertical book, such as an author biography, gets horizontal pages. Each page records its writing mode for drawing and hit testing. The layout follows JLReq and the JFM model of LuaTeX-ja. `vertical-lr` and the sideways modes are not supported yet. It is a large change.

Vertical runs use fallback fonts at their nominal size, without x-height matching, and the font manager keeps one such font for each place in the fallback chain. Reusing it cut the paint CPU time of mixed-script vertical text by about a quarter.

**Code:** [extensions/vertical/](extensions/vertical/), `GetNominalFallbackFont` in [lvfntman.cpp](crengine/src/lvfntman.cpp), and the `Blaze:` comments in [lvrend.cpp](crengine/src/lvrend.cpp), [lvtextfm.cpp](crengine/src/lvtextfm.cpp), [lvfntman.cpp](crengine/src/lvfntman.cpp), [lvpagesplitter.cpp](crengine/src/lvpagesplitter.cpp) and [lvdocview.cpp](crengine/src/lvdocview.cpp).

**Upstream:** not in `master`. It has no `writing-mode` support.

### Flexbox

**Summary:** common `display: flex` and `inline-flex` layouts.

Horizontal rows support wrapping, grow and shrink factors, `flex-basis`, percentage widths, gaps, main-axis alignment, cross-axis alignment for single lines, and auto margins. Forward columns stack their children and can center short content. `@supports (display: flex)` stays false, because general nested layouts and fragmentation of side-by-side items are not supported. See [Flex layout](../../../doc/engines/blaze.md#flex-layout) for the full list.

**Code:** [flex_rows.inc](extensions/flex_rows.inc), `css_d_flex` in [cssdef.h](crengine/include/cssdef.h), and the `Blaze:` comments in [lvrend.cpp](crengine/src/lvrend.cpp) and [lvstsheet.cpp](crengine/src/lvstsheet.cpp).

**Upstream:** not in `master`.

### EPUB manifest fallbacks

**Summary:** a spine item in a format the engine cannot read uses its manifest fallback.

The importer follows `fallback` chains in the manifest until it finds a supported item, and caches the result. Each spine slot keeps its own item, also when several slots share a fallback. Links to the original manifest item still reach the chapter.

**Code:** [spine_fallback.h](extensions/spine_fallback.h) and `ImportEpubDocument` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** not in `master`.

### Thai line breaks without a dictionary

**Summary:** emergency wrapping of Thai text keeps leading vowels and attached marks with their consonant.

When a Thai line has no legal break, Blaze breaks between letter groups, not inside one. It still uses breaks that the book gives, such as zero-width spaces. Without word segmentation, a word can still be split.

**Code:** `insideThaiLetterGroup` and the wrapping code in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** not in `master`.

### Small caps by script

**Summary:** native small caps are used for each character's own script, and synthesized small caps keep the selected face.

Upstream master added `c2sc` from `scmp` in `8d29401c` after our base, which covers part of this. Blaze still adds these points:

- A character uses native `smcp` or `c2sc` only when the font has the feature for that character's script and language.
- A capital that the font does not have falls back at its original size.
- Combining marks stay in the small caps run with their base.
- Synthesized small caps use the face that is already selected, with all its variations, so an embedded face is not lost.

For all small caps with `smcp` but no `c2sc`, master folds capitals to lowercase and uses native glyphs, where Blaze synthesizes smaller capitals. Master's way is better, so this change must be rebased on master's code first.

**Code:** `hasNativeCaps`, `hasOwnGlyph` and the small caps code in [lvfntman.cpp](crengine/src/lvfntman.cpp).

**Upstream:** partly in `master`, through `8d29401c`, as described above.

### Images in navigation labels

**Summary:** table of contents entries that are only an image get a label and keep the image.

A navigation link that holds only an image takes its label from the image's `alt` or `title`, and the entry keeps the image path so that a reader can show it.

**Code:** [nav_labels.h](extensions/nav_labels.h), `_labelImage` in [lvtinydom.h](crengine/include/lvtinydom.h), and [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** not in `master`.

### DTBook chapters in EPUB 2

**Summary:** spine items of type `application/x-dtbook+xml` load as chapters.

**Code:** [dtbook.h](extensions/dtbook.h) and `ImportEpubDocument` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** not in `master`.

### Reader defaults for paragraphs

**Summary:** user agent styles can set a paragraph indent and a page break rule that the book's own styles override.

`default-text-indent` sets an indent for unstyled prose that gives way to an indent the book sets on an ancestor. `-cr-avoid-normal-block` asks the page splitter to avoid breaks inside plain blocks as a reader default. The `hr` border width scales with the screen DPI.

**Code:** `DEFAULT_TEXT_INDENT` and `avoid_normal_block` in [cssdef.h](crengine/include/cssdef.h), [lvstsheet.cpp](crengine/src/lvstsheet.cpp) and [lvrend.cpp](crengine/src/lvrend.cpp).

**Upstream:** not in `master`.

### Optical size setting

**Summary:** a reader setting can fix the optical size of variable fonts, in points.

Zero follows CSS. Ordinary text and Bold Reading use the same width and optical size.

**Code:** `LVRendSetFontOpticalSize` in [lvrend.h](crengine/include/lvrend.h) and [font_selection.cpp](extensions/font_selection.cpp).

**Upstream:** not in `master`. It follows the CSS `font-optical-sizing` property and has no reader setting.

## Bug fixes

### Obfuscated fonts fail when the identifier contains whitespace

**Summary:** the IDPF font obfuscation key keeps whitespace inside the unique identifier, so the embedded fonts do not load.

The [EPUB 3.3 obfuscation key](https://www.w3.org/TR/epub-33/#sec-resource-obfuscation-key) is the SHA-1 of the unique identifier after all XML whitespace (U+0020, U+0009, U+000D and U+000A) is removed. Upstream calls `trim()`, which removes only spaces and tabs at the two ends. The XML parser already turns a line break into a space, so an identifier on a line of its own still works. An identifier with a line break or a space inside it gives a wrong key. Blaze removes all four characters everywhere in the identifier.

Reproduced in stock KOReader: a minimal EPUB showed plain letters and logged a font load error. With only the line break removed, the same font bytes drew the font's test glyphs. Kobalt's regression is `checkObfuscatedFonts` in [tests/model/reader.cpp](../../../tests/model/reader.cpp), which uses the identifier `" test-\tiden\n tifier\r "`.

**Code:** `setIdpfManglingKey` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** present in `master`.

### External SVG files resolve their own references wrongly

**Summary:** an SVG file referenced from XHTML looks up its images relative to the XHTML file, not to itself.

`lunasvgGetImageSource` resolves through `doc->getObjectImageSource(url, node)`, which is relative to the node's document. An SVG in another folder then shows a blank area where its image should be, or, when a file of the same name exists in the wrong place, the wrong image. For an external SVG file, Blaze resolves the reference against the folder of the SVG file. Inline SVG and data URLs keep the document lookup.

Reproduced in stock KOReader with two control EPUBs. In the first, the image area stayed blank, while the same PNG embedded in the SVG drew correctly. In the second, images with the same name in several folders showed that KOReader can silently draw the one at the package root. Kobalt checks the colors of an SVG-relative PNG in [tests/model/reader.cpp](../../../tests/model/reader.cpp) and the size of an external SVG image in [tests/model/image_proportions.cpp](../../../tests/model/image_proportions.cpp).

**Code:** the `Blaze:` comment near `lunasvgGetImageSource` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### An inherited odd-pixel text-indent becomes hanging

**Summary:** an inherited `text-indent` with an odd pixel value can turn into a hanging indent.

Upstream stores the `hanging` keyword in bit 0 of `text_indent.value` and tests `value & 1`. That works for parsed values, which are stored × 256. But when a block inherits an `em`, `ex`, `ch` or `%` indent, `inheritLength` turns it into plain pixels of type `css_val_screen_px`, and then every odd pixel count reads as hanging. For example, `<div style="font-size: 21px; text-indent: 1em"><div>…</div></div>` gives the inner block a 21 px indent on every line but the first. Publisher CSS that sets `text-indent` on a container of paragraphs does this. Blaze gives hanging indents their own length type, `css_val_screen_px_hanging`, and passes the flag separately to the formatter.

Reproduced with that example: with upstream's bit test, the first line started at x = 0 and the second at x = 21; with Blaze's change, at 21 and 0.

**Code:** `setNodeStyle` and `renderFinalBlock` in [lvrend.cpp](crengine/src/lvrend.cpp), `css_val_screen_px_hanging` in [cssdef.h](crengine/include/cssdef.h), and the indent setup in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Use after free when parsing fails

**Summary:** `LVParseXMLStream` and `LVParseHTMLStream` delete the document while its writer still uses it.

Both functions create `ldomDocumentWriter writer(doc)` on the stack. When `CheckFormat()` or `Parse()` fails, they `delete doc`. The writer is destroyed after that, at the end of the scope, and `~ldomDocumentWriter` reads `_document->isDefStyleSet()`. The EPUB importer uses `LVParseXMLStream` for `container.xml`, the OPF, the NCX and the navigation document, so a damaged EPUB can reach this. Blaze deletes the document after the writer is gone, through a small guard object declared before the writer. A simpler upstream fix is to delete the document after the writer's scope ends.

Reproduced: `LVParseXMLStream` on a stream that is not XML gives a heap-use-after-free report in `~ldomDocumentWriter`.

**Code:** `KoblazeStoppedDocument`, `LVParseXMLStream` and `LVParseHTMLStream` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### PNG decoding leaks the bitmap on error

**Summary:** a corrupt PNG leaks the whole decoded bitmap.

`LVPngImageSource::Decode` allocates `storage` for all rows, then calls `png_read_image`. A libpng error, or a short read in `lvpng_read_func`, jumps back to the `setjmp` handler, which destroys the read struct but does not free `storage`. A truncated PNG is enough, and every failed decode leaks again. Blaze keeps the pointer in a `volatile` variable and frees it in the handler.

Reproduced: decoding a 512 × 512 PNG cut in half leaked about 1 MB on each attempt.

**Code:** `LVPngImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### The cache index is rewritten only when its length changes

**Summary:** `cr3cache.inx` keeps an old file order and old sizes when the new index has the same length.

`ldomDocCacheImpl::writeIndex` compares `buf.getCRC()` with the CRC of the old file to skip identical writes. `getCRC()` covers the whole buffer, including the CRC32 that `putCRC` just appended. A CRC32 over data followed by its own little-endian CRC32 is a constant for a given length, and the fixed magic in front does not change that. So the compare only checks the length, and a reordered index (`moveFileToTop`) or a changed size is not written. We confirmed the constant with zlib. Blaze takes the CRC before `putCRC`, and `readIndex` records the same CRC of the file it read.

The index is written again only when a new cache file changes its length. Until then, reopening books only reorders the list in memory, and the next session reads the old order. `reserve()` deletes from the end of that list when the cache is full, so a book that was opened recently but added early can lose its cache first, and the next open parses the book again. Nothing is lost but time.

Reproduced: create caches for A and then B, close, open A again, close. Upstream's index still lists B first. With Blaze's change, A is first.

**Code:** `writeIndex` and `readIndex` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Images with a width and a height are stretched when a maximum applies

**Summary:** an `img` with both `width` and `height` set keeps one of them when `max-width` or `max-height` clamps the other.

In `getStyledImageSize`, a set width is clamped to `max_width`, but a set height stays as it is. With `enforce_page_constraints`, the maximum is also the page size. So `<img style="width: 1200px; height: 800px">` on a 600 px wide page is drawn 600 × 800. Blaze scales the other side by the same factor, rounds once, and keeps at least one pixel on each side. Minimum sizes still win over the ratio. SVG viewports and automatic sizes keep upstream's rules.

Browsers follow CSS 2.1 here and stretch such an image when the author's own `max-width` applies, which is why authors write `height: auto`. Blaze keeps the ratio in that case too. Upstream may want to keep the ratio only when the page size is the limit.

Kobalt's regression is [tests/model/image_proportions.cpp](../../../tests/model/image_proportions.cpp).

**Code:** the `Kobalt:` comment in `getStyledImageSize` in [lvrend.cpp](crengine/src/lvrend.cpp).

**Upstream:** present in `master`.

### Collapsed table borders lose the right and bottom edges of inner cells

**Summary:** with `border-collapse`, a border that is set only on the right or bottom of an inner cell disappears.

Upstream compares each cell edge only with the row, the row group and the table. It does not compare it with the adjacent cell. Then it removes the right and bottom borders of every cell that is not on the right or bottom edge of the table, so that each shared edge is drawn once. A border that only the left cell or the upper cell sets is lost. Upstream also ignores `border-style: hidden` and columns. Blaze also compares adjacent cells, columns and column groups, gives `hidden` priority, then the width, then the owner (cell, row, row group, column, column group, table), as in CSS 2.1. Because a cell now reads its neighbours' original styles, Blaze writes the new styles after the loop. The same code also maps borders for vertical text, which upstream does not need.

**Code:** `collapse_border` and the cell pass after it in [lvrend.cpp](crengine/src/lvrend.cpp).

**Upstream:** present in `master`. We found this by reading the code and did not render a table in upstream.

### Navigation documents with several epub:type values are missed

**Summary:** `<nav epub:type="toc landmarks">` is not found as a table of contents.

`epub:type` is a list of tokens separated by spaces. Upstream compares the whole value with `"toc"`, `"landmarks"` and `"page-list"`. Blaze splits the value and matches each token.

**Code:** the navigation document code in `ImportEpubDocument` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** present in `master`.

### linear="no" is stored on the manifest item, not on the spine slot

**Summary:** an item that appears twice in the spine gets one linear flag for both places.

Upstream stores `nonlinear` on the shared `EpubItem`, so the last spine reference wins. The `EpubItem` copy constructor and assignment drop `nonlinear`, and the default constructor leaves it uninitialized. Blaze keeps one flag for each spine slot in `linearFlags`. Real books rarely list an item twice.

**Code:** `linearFlags` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** present in `master`.

### Zero-width no-break space shows as a question mark

**Summary:** U+FEFF can draw as the default character, and it makes a line go through right-to-left processing.

`getReplacementChar` puts U+2060 in its group of characters that can be ignored, but not U+FEFF, so a font without U+FEFF draws the default character. `getCharIndex` also returns glyphs for U+200B, U+2060 and U+FEFF when a font has visible control pictures for them. Blaze adds U+FEFF to the group and returns no glyph for these three characters. Separately, `lStr_isRTL()` counts U+FEFF as right-to-left because it is in the Arabic Presentation Forms-B block. Blaze ignores it there when Blaze's shaping options treat the joiner as plain. Upstream would need its own rule for that part.

**Code:** `getReplacementChar` and `getCharIndex` in [lvfntman.cpp](crengine/src/lvfntman.cpp), and the `Blaze:` comment on U+FEFF in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Resize gives the old screen size to device media queries

**Summary:** after a resize, `device-width`, `device-height` and `device-aspect-ratio` media queries see the previous screen size.

`LVDocView::Resize` calls `m_doc->setScreenSize(m_dx, m_dy)` before it stores the new `dx` and `dy` in `m_dx` and `m_dy`. Blaze passes `dx` and `dy`. Only the `device-*` features read the screen size. `width`, `height`, `aspect-ratio` and `orientation` read the page size and are not affected, so few books see this.

**Code:** `LVDocView::Resize` in [lvdocview.cpp](crengine/src/lvdocview.cpp).

**Upstream:** present in `master`.

### Page text can take the first line of the next page

**Summary:** `getPageText` can return the first line of the following page, because the end of the page range is found by scanning from a row that already belongs to that page.

`getPageDocumentRange` scans upward from `end_y = page->start + page->height` for the last position on the page, and accepts it when its top is at most `end_y`. The row at `end_y` is the first row of the next page. When a line starts exactly there, the scan finds that line and its top passes the check, so the page text takes it. The last page of a chapter, for example, can take the first line of the next chapter. This is not specific to one script: it happens with plain English text and with a right-to-left Arabic book.

For page text, Blaze starts the scan one row higher, on the last row of the page, and keeps the check at `end_y`. The check has to stay there: the caret after the last character of a wrapped line sits at the top of the next line, which is `end_y` for the last line of a page. An earlier Blaze version moved `end_y` itself up one pixel, and that left out the last line of every page. Link collection keeps the old scan.

Reproduced in Blaze: with upstream's scan, two tests fail, and with Blaze's scan, both pass. One is a paragraph of numbered words followed by a short chapter, which checks that each page keeps its last line and leaves out the next chapter (`pageExcerptsKeepTheirLastLine` in [page_layout.cpp](../../../tests/model/page_layout.cpp)). The other is the Arabic book (`pageExcerptsStopAtTheVisibleBoundary` in [multilingual.cpp](../../../tests/model/multilingual.cpp)).

**Code:** `getPageDocumentRange` in [lvdocview.cpp](crengine/src/lvdocview.cpp).

**Upstream:** present in `master`.

### In-page footnotes miss links with percent-encoded ids

**Summary:** a footnote link such as `href="notes.xhtml#n%C3%A9"` does not find its footnote for the page.

When the renderer records footnote links for the page splitter, it takes the text after `#` as it is. `convertHref` does not decode that part either, while the footnote's own `id` holds the plain characters. So the ids do not match, and the footnote is not placed on the page. Blaze decodes the id with `DecodeHTMLUrlString` in the five places that call `addLink`.

**Code:** the `addLink` calls in [lvrend.cpp](crengine/src/lvrend.cpp).

**Upstream:** present in `master`. We found this by reading the code and did not reproduce it.

### Saved positions add unknown element names to the document

**Summary:** parsing an XPointer with an unknown element name registers that name in the document.

`createXPointerV1` and `createXPointerV2` call `getElementNameIndex(name)`, which adds a new id for an unknown name. A stale or foreign bookmark then changes the name table of the document, which the cache file also stores. Blaze uses a lookup that does not add names, `findElementNameIndex`, and returns a null XPointer.

**Code:** `createXPointerV1`, `createXPointerV2` and `findElementNameIndex` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Drawing reads an unset word index

**Summary:** `LFormattedText::Draw` can read `lastWordSrcIndex` before it is set.

When the first word of a line is hidden or has `color: transparent`, the loop skips the first assignment, and the value is read uninitialized. The variable is also `lUInt16`, which cannot hold a "no word yet" value. When the garbage value matches, an underline or strike-through can be drawn back over the gap before the word. Blaze uses an `int` that starts at `-1`.

**Code:** `LFormattedText::Draw` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Fonts that fall back to themselves are never freed

**Summary:** a font whose fallback font is itself, or a font in a fallback ring, stays alive until the process ends.

`GetFallbackFont` with an empty face name returns the first fallback face. When the main font is that face, at the same size, weight and style, the instance cache returns the same object, so the font's fallback is itself. This happens when a user picks one of the fallback fonts as the main font. `LVFontInstanceCache::gc()` only drops fonts with one reference, so it never collects such a font. Blaze clears the font references of every cached font in `~LVFreeTypeFontManager`. This fixes the leak at shutdown only, so KOReader, which keeps the font manager until it exits, gains little. During a run, the fonts still stay in the cache.

We checked that `GetFallbackFont` returns the main font object itself when the main face is the first fallback face. We did not measure the leak.

**Code:** `~LVFreeTypeFontManager` in [lvfntman.cpp](crengine/src/lvfntman.cpp).

**Upstream:** present in `master`.

## Hardening

### SVG files that reference each other recurse without end

**Summary:** an SVG image that refers back to an image being drawn can overflow the stack.

An SVG `<image>` that points to the same SVG file is drawn by `lunasvgDrawImageHelper`, which decodes that SVG again, which reaches the same `<image>`. LunaSVG has no guard: its image element calls the host's draw helper each time. Blaze keeps a per-thread list of the SVG sources being drawn. It stops at a repeat, by stream or by name, and at a depth of 16, which also covers unnamed data streams. Each caller keeps its source alive.

Reproduced: an EPUB whose `a.svg` draws `a.svg` overflowed the stack without the guard and drew normally with it. Only a crafted or broken book does this.

**Code:** `SVGRenderScope` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### A full disk ends the process

**Summary:** a failed cache write calls `crFatalError`.

`saveNodeData`, `compact` and `swapToCache` stop the process when a write to the cache file fails, for example on a full SD card. Blaze keeps the chunk in memory, logs the error and returns it. The `saveNodeData` part is mixed with a return type change for Blaze, so take only the `crFatalError` removals.

**Code:** `saveNodeData`, `compact` and `swapToCache` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Reading a damaged page list can allocate billions of entries

**Summary:** `LVRendPageList::deserialize` trusts the page count from the cache, and keeps a partial list after a failure.

The page count `len` comes from the cache file and goes to `reserve(len)` and the read loop without a bound. The loop keeps allocating a page record for each count after the buffer error, so a damaged count can ask for up to 4 billion records. After a failure, the partial list stays, and `has_nonlinear_flows` keeps an old value because the function only sets it to true. Blaze checks the remaining buffer before it reads, bounds the count by the smallest page record, and clears the list and the flags on any failure.

`CacheFile::read` checks a hash of each block before this runs, so random damage on disk does not get here. Only a writer bug or a crafted cache file does.

Reproduced at a smaller scale: a count of 5 million in a 76-byte buffer took 746 MB of memory before the reads failed. Blaze returns at once.

**Code:** `LVRendPageList::deserialize` and `LVRendPageInfo::deserialize` in [lvpagesplitter.cpp](crengine/src/lvpagesplitter.cpp). Blaze's page record has extra fields, so take the checks, not the record layout.

**Upstream:** present in `master`.

### Mutable nodes leak when a document is destroyed

**Summary:** element and text nodes that are still mutable when the document is destroyed are never freed.

`~ldomDocument` unregisters the document before `~tinyNodeCollection` frees the nodes. A node then cannot find its document, so `onCollectionDestroy` returns early and frees nothing. Blaze frees those nodes with `destroyNodes()` while the document is still registered.

This matters less than it looks. The parser persists each element when it closes it, and text goes straight to persistent storage, so a parsed document has no mutable nodes. We parsed and deleted an OPF-sized document 50 times with the fix taken out and saw no growth. The leak needs nodes that are changed later and not persisted again. We did not search KOReader for such a path.

**Code:** `ldomDocument::~ldomDocument` and `destroyNodes` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### The formatter can loop on a character that it cannot measure

**Summary:** when `measureText` fits zero characters, the measuring loop in `LVFormatter` starts again at the same character, without end.

The loop measures each text run against a width of `0x7FFF`. When the first character alone is wider, `measureText` returns 0, the loop sets `i = start`, and it reaches the same measure again. Blaze takes one character with zero width and moves on. In horizontal text this needs one character wider than 32,767 pixels, and our attempt with `letter-spacing: 32700px` at 200 px did not hang. Blaze needs the fix because vertical text measures against the page height.

**Code:** the `Blaze:` comment in `LVFormatter` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### GIF decoding reads uninitialized bytes

**Summary:** the GIF decoder reads up to three bytes after the image data that were never written.

`LVGifFrame::DecodeFromBuffer` allocates `stream_buffer_size + 3` bytes and copies the data, but does not set the three extra bytes. `CLZWDecoder::ReadInCode` always reads three bytes, also at the end. In a valid GIF the mask drops the extra bits. In a truncated GIF, the last code can take bits from those bytes, so the last pixels can depend on old memory. Blaze sets the three bytes to zero. We did not reproduce it: MemorySanitizer does not run on our Mac, and AddressSanitizer does not report reads of uninitialized memory.

**Code:** `LVGifFrame::DecodeFromBuffer` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### The JPEG error path depends on an indeterminate pointer

**Summary:** after a libjpeg error, the C standard does not promise that the JPEG decoder frees the right row buffer.

`LVJpegImageSource::Decode` keeps its row buffer in a plain local variable that changes after `setjmp`. After `longjmp`, the standard makes the value of such a variable indeterminate, and the error path calls `delete[]` on it. In practice the code passes `&buffer` to `jpeg_read_scanlines`, so compilers keep the variable in memory. We made a stream fail halfway through a 512 × 512 JPEG and built upstream's code with Apple clang at `-O2` and AddressSanitizer. It freed the buffer correctly, with no leak. Blaze takes the row buffer from libjpeg's own pool with `alloc_sarray`, so `jpeg_destroy_decompress` frees it on every path, and the question goes away.

**Code:** `LVJpegImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### Drawing an image at zero size

**Summary:** `LVColorDrawBuf::Draw` does not return early for a zero width or height.

`LVGrayDrawBuf::Draw` has this guard. The color version does not, so it makes a zero-length map and can call `qSmoothScaleImage` with a zero target size. Blaze adds the same guard.

**Code:** `LVColorDrawBuf::Draw` in [lvdrawbuf.cpp](crengine/src/lvdrawbuf.cpp).

**Upstream:** present in `master`.

### Draw buffers start with undefined colors

**Summary:** `LVBaseDrawBuf` does not set its background and text colors.

`LVGrayDrawBuf` and `LVColorDrawBuf` do not set them in their constructors either. `LVDocView::Draw` sets both before it draws, so pages are fine. `LVInkMeasurementDrawBuf` never sets them, and `LFormattedText::Draw` and `LVDrawStateSaver` read them and set them back, so they copy undefined values. We found no output that changes. A comment in upstream's `FillRect` mentions the missing value. Blaze starts with white and black.

**Code:** the `LVBaseDrawBuf` constructor in [lvdrawbuf.h](crengine/include/lvdrawbuf.h).

**Upstream:** present in `master`.

### Cache file name buffer

**Summary:** the cache file name suffix fills its 16-byte buffer exactly.

The suffix `.%08x.%d.cr3` is written with `sprintf` into `char s[16]`. `getPersistenceFlags()` returns 0 or 1 today, so the result is 15 characters and the terminating zero. Any other flags value overflows. Blaze uses 32 bytes and `snprintf`, with the same name format.

**Code:** the cache file name code in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Node chunks on the stack

**Summary:** `saveNodeData` puts a variable-length array of about 64 KB on the stack.

The array holds `TNC_PART_LEN` nodes, which is 4096. Variable-length arrays are not standard C++, and 64 KB is a lot on a small thread stack. Blaze reuses one `std::vector` for all chunks.

**Code:** `tinyNodeCollection::saveNodeData` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Document fonts of a document without an index

**Summary:** `~ldomDocument` can unregister the system fonts.

A document that got no index has `_docIndex` -1, which is also the index of the system fonts. Upstream calls `UnregisterDocumentFonts(_docIndex)` without a check. Blaze skips the call for -1. It needs 16 open documents to happen.

**Code:** `ldomDocument::~ldomDocument` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Integer overflow in the smooth scale buffer

**Summary:** the buffer size for smooth scaling is computed in `int`.

`new lUInt8[src_dy * (src_dx * 4)]` overflows for images over about 536 million pixels. Blaze uses `size_t`.

**Code:** the decoder callback in [lvdrawbuf.cpp](crengine/src/lvdrawbuf.cpp).

**Upstream:** present in `master`.

### Embedded font size check

**Summary:** `RegisterDocumentFont` opens the font before it checks its size, and narrows the size to 32 bits.

Blaze reads the size from the container entry first, keeps it as `lvsize_t`, and opens the stream only when the size is below the limit.

**Code:** `RegisterDocumentFont` in [lvfntman.cpp](crengine/src/lvfntman.cpp).

**Upstream:** present in `master`.

### Nowrap check on a text node

**Summary:** the nowrap check can call `getStyle()` on a text node.

When two source fragments share one text node, `getNearestCommonParent()` returns that text node, and `getStyle()` returns an empty reference for it. Blaze moves up to the parent element first. Upstream adds one source for each text node, so we do not think that it can reach this case. Blaze can, because Bold Reading splits a text node into several sources.

**Code:** the nowrap check in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Compiler checks

**Summary:** small changes that make the compiler check more.

The font classes mark their overrides with `override`, so a changed virtual signature fails to compile. `NB_IMP_SLOTS` is computed from the enum, which removes a hand-kept count and its `#error`.

**Code:** [lvfntman.cpp](crengine/src/lvfntman.cpp) and [lvstyles.h](crengine/include/lvstyles.h).

**Upstream:** present in `master`.

## How Blaze opens a book in parts

This is the main reason Blaze exists, and the part of the fork that changes CREngine the most. This section explains how it works, for maintainers who know CREngine and are curious. The entries in [Changes for Kobalt's reader](#changes-for-kobalts-reader) list the same pieces one by one.

Stock CREngine opens an EPUB in one go. It parses every spine item into one `ldomDocument`, runs the style pass over the whole DOM, lays out the whole book and splits it into pages. Only then can it draw the first page. On a desktop that is quick. A Kobo Clara BW has one core and is roughly 25 to 40 times slower than a Mac for layout. On a Mac, *War and Peace* (393 spine items, about 3,500 pages) took about 570 ms to lay out as a whole, and about 12 ms for the spine item in the middle of the book and its two neighbours. Scaled by that factor, that is an estimated 14 to 23 seconds on the Clara, against well under a second. A font size change costs almost as much as the first layout, so it has the same problem.

Blaze makes that work divisible. A host can load only some spine items, lay out only part of a spine item, stop any of this work when the user does something, and build the whole book in the background without making a page turn wait for it. These pieces do not depend on each other. Kobalt uses all of them together in its Hybrid mode.

The interface is in [koblaze.h](crengine/include/koblaze.h). Koblaze was the working name of the fork, and the prefix stayed.

### Loading a window of spine items

`koblazeSetSpineWindow(first, last)` makes the next EPUB load parse only the spine items from `first` to `last`. Every other spine item still gets a DocFragment, but an empty one. So the DocFragment indices are the same as in a full load, and an XPointer such as `/body/DocFragment[37]/body/p[12]/text().120` points to the same place in the partial document and in the whole book. A negative `first` loads everything, which is the default.

Kobalt opens a book with only the spine item of the saved position. Two smaller changes make each such load cheaper. With document fonts turned off, the book's fonts are listed but not read, unless a style asks for one of them (`koblazeReadFontsFor`). And the font manager keeps the unpacked font data, so the next document of the same book does not read the font files again.

### Laying out a slice of a spine item

Some books have a few very large spine items. *Ulysses* has 25. Loading one spine item does not help much there, so Blaze can also lay out part of one. `ldomDocument::setKoblazeLayoutSlice(first, last)` takes a range in characters of the spine item's text. During layout, the block renderer asks `koblazeEnterBlock()` about each block. A block outside the range gets no layout and is not drawn. A container is entered when some of its text is in the range. The slice starts on a new page, and the page splitter drops the lines before the first block of the slice, so the first page of a slice starts the same way as that page does in the whole spine item.

Each node's character counts are computed once and kept (`koblazeTextLength`, `koblazeTextBefore`), because every slice asks for them again. Kobalt sizes a slice at about 25 pages of text, estimated from the font size and the page size, with a minimum of 2,000 characters. When the reader turns past the end of a slice, Blaze lays out the next one with the styles it already has (`koblazeRelayout`), without another style pass.

### Stopping work

The parser, the style pass and the layout can all stop. The host installs a check with `koblazeSetCancelCheck()`. The parser and the style pass call it every few dozen nodes. The layout asks after each final block, through `OnRenderCancelRequested()`, a new method of the render callback. When the check says stop, the engine throws `KoblazeRenderCancelled`, which unwinds out of `ldomDocument::render()` or `LVDocView::Render()`.

After a stopped layout, the document keeps its DOM and styles, drops the unfinished pages, and starts again from the beginning on the next render. A document whose parse or style pass was stopped is not usable, and the host destroys it.

An exception that unwinds through code not written for one needs some care:

- The parser reads many kinds of files: chapters, the package file, hyphenation dictionaries. It may only stop in a chapter, so its check does nothing unless a `KoblazeParserCanStop` scope is open.
- A destructor must not throw. Destructors that do work with checks in them open a `KoblazeNoCancel` scope. The check also does nothing while another exception is already unwinding.
- The parsers delete a stopped document after its writer, because the writer's destructor still uses the document.

In Kobalt, the check asks whether other work waits for the engine. That is how a tap during background work gets the engine back within about one paragraph of layout.

### Keeping a layout while trying the next one

`koblazeSaveLayout()` keeps the current layout aside: the geometry and layout method of every element, the pages and the slice. Styles are not part of it. `koblazeRestoreLayout()` brings it back. Kobalt uses this near the end of a slice. It lays out the next slice in idle time, and if the user turns a page meanwhile, that layout stops and the current slice comes back. The turn does not wait for a layout that it did not need yet.

### Building the whole book in the background

Chapter pages have one weakness: the book has no real page numbers. So while the reader shows the chapter, Kobalt builds the whole book in the background. This is an ordinary full load and render.

CREngine's globals are not thread-safe: the font manager, the text language manager and the render settings, among others. So the background build runs on its own thread, but never at the same time as the thread that serves the reader. The two hand the engine to each other:

- The build installs a stop check that never cancels. When other work waits, the check blocks instead, and the build thread parks inside it, in the middle of `render()`, with its stack intact.
- While the build is parked, the reader's thread uses the engine for its chapter documents. When that thread is idle, it wakes the build, which goes on where it stopped. No work is lost, and no second busy thread competes for the one core.
- Global state that belongs to the parked document is saved when it parks and put back when it resumes. This covers the main language and its open quote levels, the base font weight and optical size, the shaping options and the open cancel scopes. The page context of a layout in progress is `thread_local`, so a chapter layout on the other thread cannot reach it. While the build is parked, other documents cannot see its parsed-book cache (`ldomDocCache::koblazeSuspend`).
- Some places can wait but must not stop, such as between two blocks that a cache save writes. `koblazePausePoint()` marks them.

Saving the parsed-book cache works in steps too. A save with a time limit goes on from the stage and node block where it stopped.

When the build is done, the reader switches to the whole book on a page turn, at the same XPointer. Idle time prepares that turn first, so it costs about as much as an ordinary one. [Rendering engines](../../../doc/engines.md) describes the Kobalt side.

### Limits

- The engine has room for 16 documents, because the document index in a node handle has 4 bits. A seventeenth document shares the nodes of another one without any error. Kobalt keeps several chapter documents open, so Blaze reports the free slots (`koblazeFreeDocumentSlots`), and the parsers refuse to create a document without one.
- Some content needs the whole book, even in chapter mode. Footnotes that the reader shows below the text that links to them are one example.
- In chapter layout, margins do not collapse across a spine item border, and each spine item starts on a new page.

### What upstream could use on its own

- Stopping work is the smallest piece and the most general one. A host could stop a layout that a second font size change has already made useless, instead of waiting for it to finish.
- The spine window gives a quick first page for a book that is not in the cache yet. Page numbers stay unknown until a full load is done.
- Counting characters once per node is a small speedup for any code that maps character offsets to nodes.

## Changes for Kobalt's reader

These changes serve Kobalt's reader on slow single-core devices. They add host interfaces that KOReader does not use. They are listed so that a maintainer who reads the diff knows what they are. [How Blaze opens a book in parts](#how-blaze-opens-a-book-in-parts) explains how they work together.

### Chapter layout

**Summary:** Blaze can lay out one slice of a spine item at a time, instead of the whole book.

The host selects a character range of each spine item. The block renderer calls `koblazeEnterBlock` and leaves out blocks outside the slice, and the page splitter starts the first page of a slice at the top of its first block. The EPUB importer can load a window of spine items and inserts an empty fragment for each skipped item, so saved positions keep their fragment indices.

**Code:** [chapter_layout.cpp](extensions/chapter_layout.cpp), [spine_window.cpp](extensions/spine_window.cpp), [koblaze.h](crengine/include/koblaze.h), and the `Blaze:` comments in [lvrend.cpp](crengine/src/lvrend.cpp), [lvpagesplitter.cpp](crengine/src/lvpagesplitter.cpp) and [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** not in `master`.

### Saved and swapped layouts

**Summary:** a document can keep its layout aside, restore it, or swap between two layouts without a new render.

`koblazeSaveLayout` keeps the geometry, the layout method of every element, the pages and the slice. `koblazeRestoreLayout` brings it back after a stopped or unwanted layout. Alternate layouts also keep their styles. A full style pass drops what was kept.

**Code:** `koblazeSaveLayout`, `koblazeRestoreLayout` and `LayoutSnapshot` in [lvtinydom.h](crengine/include/lvtinydom.h), and [chapter_layout.cpp](extensions/chapter_layout.cpp).

**Upstream:** not in `master`.

### Stopping and pausing work

**Summary:** the host can stop parsing, styling, layout and cache saves, and can pause a whole-book layout while another document uses the engine.

Parsing, the style pass and layout check a host callback at regular steps and throw to stop. The parsers free their objects when they stop. A stopped layout discards its unfinished pages. A whole-book layout on its own thread can park while a chapter layout runs, so the page context is per thread, quote levels are saved per language, and the parked document's cache is hidden from other documents. Cache saves have pause points between node blocks, and time-limited saves record each finished stage.

**Code:** [layout_control.cpp](extensions/layout_control.cpp), [koblaze.h](crengine/include/koblaze.h), and the `Blaze:` comments in [lvtinydom.cpp](crengine/src/lvtinydom.cpp), [lvxml.cpp](crengine/src/lvxml.cpp), [lvpagesplitter.cpp](crengine/src/lvpagesplitter.cpp) and [textlang.cpp](crengine/src/textlang.cpp).

Note for upstream: in `saveChanges`, `_mapSavingStage` is set at the start of each stage, and `setCacheFileStale(true)` does not reset it. A time-limited save after a complete save would then skip everything. KOReader saves without a time limit, so it does not reach this today.

**Upstream:** not in `master`.

### Document slots

**Summary:** the engine reports how many documents it can still create, and refuses a new one without a free slot.

The 4-bit document index wraps after 15 documents, and a new document then shares the nodes of another. Blaze keeps several chapter documents open, so `koblazeFreeDocumentSlots` reports the free slots, and the parsers return NULL when there are none.

**Code:** `koblazeFreeDocumentSlots` in [lvtinydom.h](crengine/include/lvtinydom.h), and the `Blaze:` comments in [lvtinydom.cpp](crengine/src/lvtinydom.cpp) and [chmfmt.cpp](crengine/src/chmfmt.cpp).

**Upstream:** not in `master`.

### Page spacing

**Summary:** after page splitting, Blaze can spread the free space of a page over the gaps between its lines.

Each block is classified before its lines are tagged. After page breaks are final, the page splitter records the lines that may move and their gaps. Drawing and hit testing map between displayed and source coordinates. Cached pages store these fields.

**Code:** [page_spacing.cpp](extensions/page_spacing.cpp) and the `Blaze:` comments in [lvrend.cpp](crengine/src/lvrend.cpp) and [lvpagesplitter.cpp](crengine/src/lvpagesplitter.cpp).

**Upstream:** not in `master`.

### Bold Reading

**Summary:** an optional mode draws the first part of each word in bold.

**Code:** [font_selection.cpp](extensions/font_selection.cpp), [lvbionic.h](extensions/lvbionic.h), and `m_readingTexts` in [lvtextfm.h](crengine/include/lvtextfm.h).

**Upstream:** not in `master`.

### Document fonts

**Summary:** with document fonts turned off, fonts that a style asks for are still read, and chapter documents share font bytes.

The document lists its embedded fonts without reading them. `koblazeReadFontsFor` reads a listed font when a style names its family. The font manager keeps reconstructed font bytes in a bounded cache, so chapter documents of the same book share them.

**Code:** `koblazeReadFontsFor` in [lvtinydom.h](crengine/include/lvtinydom.h), [document_fonts.cpp](extensions/document_fonts.cpp) and [embedded_font_cache.h](extensions/embedded_font_cache.h).

**Upstream:** not in `master`.

### Reports to the host

**Summary:** the engine tells the host about drawn images and unsupported formatting, and can take image sources from the host.

The draw buffer reports when an image puts visible ink on the page, and when clipped text or an image uses formatting that Blaze does not support. A resource container can supply an image source, so that the host gives image sizes before it reads the pixels. Tables report when they are drawn.

**Code:** [lvdrawbuf.h](crengine/include/lvdrawbuf.h), [lvdrawbuf.cpp](crengine/src/lvdrawbuf.cpp), `CreateImageSource` in [lvstream.h](crengine/include/lvstream.h), and [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** not in `master`.

### Smaller host changes

**Summary:** changes that only Kobalt's reader needs.

- Pages without height, from two forced breaks in a row, can be left out.
- `koblazeRemove` removes every cache file of a document, for Kobalt's debug mode.
- The user agent pseudo element `-cr-existing-first-letter` styles a first letter that the book already has, for night mode.
- Cached page lists keep the writing mode and the page spacing fields.
- `crsetup.h` comes from the build directory.

**Upstream:** not in `master`.

## Keeping this file current

Kobalt updates this file in the same change as each Blaze change to inherited code. A new entry goes in the section for its kind, at the place that matches how useful it is likely to be. An entry is removed when Blaze drops the change or upstream has it. When an entry is offered upstream, the link to the discussion or pull request goes at the end of its section.
