# Blaze changes for CREngine maintainers

Blaze is the EPUB engine of Kobalt, a reader for Kobo e-readers. It is a hard fork of KOReader's CREngine. This file lists what Blaze changed in the code it inherited, so that CREngine maintainers can decide what, if anything, they want to take. Nothing here is a request. Take what is useful, in any form, or nothing.

Blaze starts from `koreader/crengine` commit `4ce1aed17e9d7852e047e0b9b0bf6a637486fd97`. Unless an entry says otherwise, we found the problem by reading the code and did not reproduce it in upstream. Links go to the Blaze source. In inherited files, a `Blaze:` comment marks most changes.

Each entry starts with a short summary and then gives the details. **Code:** names the Blaze source to read. **Upstream:** gives the state of upstream `master` at `a459d926` (2026-11-21):

- "Present in `master`" means that master still has the behavior that Blaze changed.
- "Partly in `master`" means that master changed the same code after Blaze's base.
- "Not in `master`" means that master has no such feature or interface.
- An entry that we did not compare with master says so.

The categories are:

- [Bug fixes](#bug-fixes): crashes, leaks, undefined behavior and wrong output.
- [Hardening](#hardening): defects that are not reachable in KOReader today, or that only matter on bad input.
- [Speedups](#speedups): less work or less memory for the same result.
- [Features](#features): CSS and EPUB support that upstream does not have.
- [Changes for Kobalt's reader](#changes-for-kobalts-reader): host interfaces that Kobalt needs. They are listed for completeness and are unlikely to fit upstream.

## Bug fixes

### Use after free when parsing fails

**Summary:** `LVParseXMLStream` and `LVParseHTMLStream` delete the document while its writer still uses it.

Both functions create `ldomDocumentWriter writer(doc)` on the stack. When `CheckFormat()` or `Parse()` fails, they `delete doc`. The writer is destroyed after that, at the end of the scope, and `~ldomDocumentWriter` reads `_document->isDefStyleSet()`. Blaze deletes the document after the writer is gone, through a small guard object declared before the writer. A simpler upstream fix is to delete the document at the end, outside the writer's scope.

**Code:** `KoblazeStoppedDocument`, `LVParseXMLStream` and `LVParseHTMLStream` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`. Confirmed by reading the master code.

### PNG decoding leaks the bitmap on error

**Summary:** a corrupt PNG leaks the whole decoded bitmap.

`LVPngImageSource::Decode` allocates `storage` for all rows, then calls `png_read_image`. A libpng error jumps back to the `setjmp` handler, which destroys the read struct but does not free `storage`. Blaze keeps the pointer in a `volatile` variable and frees it in the handler.

**Code:** `LVPngImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`. Confirmed by reading the master code.

### JPEG decoding frees an indeterminate pointer on error

**Summary:** after a libjpeg error, the JPEG decoder can free or leak its row buffer.

`LVJpegImageSource::Decode` keeps its row buffer in a plain local variable that changes after `setjmp`. After `longjmp`, the value of such a variable is indeterminate, and the error path calls `delete[]` on it. Blaze takes the row buffer from libjpeg's own pool with `alloc_sarray`, so `jpeg_destroy_decompress` frees it on every path.

**Code:** `LVJpegImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`. Whether it fails depends on the compiler and its register use.

### GIF decoding reads uninitialized bytes

**Summary:** the GIF decoder reads up to three bytes after the image data that were never written.

`LVGifFrame::DecodeFromBuffer` allocates `stream_buffer_size + 3` bytes and copies the data, but does not set the three extra bytes. The LZW decoder reads up to three bytes at a time, also at the end. Memory checkers such as Valgrind and MemorySanitizer report it. Blaze sets the three bytes to zero.

**Code:** `LVGifFrame::DecodeFromBuffer` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### The cache index is rewritten only when its length changes

**Summary:** `cr3cache.inx` keeps an old file order and old sizes when the new index has the same length.

`CacheFile::writeIndex` compares `buf.getCRC()` with the CRC of the old file to skip identical writes. `getCRC()` covers the whole buffer, including the CRC32 that `putCRC` just appended. A CRC32 over data followed by its own little-endian CRC32 is a constant for a given length. So the compare only checks the length, and a reordered index (`moveFileToTop`) or a changed size of the same byte length is not written. We confirmed the constant with zlib. Blaze takes the CRC before `putCRC`.

**Code:** `writeIndex` and `readIndex` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`. The effect is that cache eviction can remove recently used files first.

### Reading a damaged page list can allocate billions of entries

**Summary:** `LVRendPageList::deserialize` trusts the page count from the cache, and keeps a partial list after a failure.

The page count `len` comes from the cache file and goes to `reserve(len)` and the read loop without a bound. A damaged count can ask for up to 4 billion entries before the buffer error stops the reads. After a failure, the partial list stays, and `has_nonlinear_flows` keeps an old value because the function only sets it to true. Blaze checks the remaining buffer before it reads, bounds the count by the smallest page record, and clears the list and the flags on any failure. Bad CRCs already fail upstream, through the buffer's error flag.

**Code:** `LVRendPageList::deserialize` and `LVRendPageInfo::deserialize` in [lvpagesplitter.cpp](crengine/src/lvpagesplitter.cpp). Blaze's page record has extra fields, so take the checks, not the record layout.

**Upstream:** present in `master`. Confirmed by reading the master code.

### Resize gives the old screen size to media queries

**Summary:** after a resize, CSS `@media` queries see the previous screen size.

`LVDocView::Resize` calls `m_doc->setScreenSize(m_dx, m_dy)` before it stores the new `dx` and `dy` in `m_dx` and `m_dy`. Blaze passes `dx` and `dy`.

**Code:** `LVDocView::Resize` in [lvdocview.cpp](crengine/src/lvdocview.cpp).

**Upstream:** present in `master`. Confirmed by reading the master code.

### Page text can take the first line of the next page

**Summary:** `getPageText` can return the first line of the following page, because the end of the page range is found by scanning from a row that already belongs to that page.

`getPageDocumentRange` scans upward from `end_y = page->start + page->height` for the last position on the page, and accepts it when its top is at most `end_y`. The row at `end_y` is the first row of the next page. When a line starts exactly there, the scan finds that line and its top passes the check, so the page text takes it. The last page of a chapter, for example, can take the first line of the next chapter. This is not specific to one script: it happens with plain English text and with a right-to-left Arabic book.

For page text, Blaze starts the scan one row higher, on the last row of the page, and keeps the check at `end_y`. The check has to stay there: the caret after the last character of a wrapped line sits at the top of the next line, which is `end_y` for the last line of a page. An earlier Blaze version moved `end_y` itself up one pixel, and that left out the last line of every page. Link collection keeps the old scan.

Reproduced in Blaze: with upstream's scan, two tests fail, and with Blaze's scan, both pass. One is a paragraph of numbered words followed by a short chapter, which checks that each page keeps its last line and leaves out the next chapter (`pageExcerptsKeepTheirLastLine` in [page_layout.cpp](../../../tests/model/page_layout.cpp)). The other is the Arabic book (`pageExcerptsStopAtTheVisibleBoundary` in [multilingual.cpp](../../../tests/model/multilingual.cpp)).

**Code:** `getPageDocumentRange` in [lvdocview.cpp](crengine/src/lvdocview.cpp).

**Upstream:** present in `master`.

### The formatter can loop on a character that it cannot measure

**Summary:** when text measurement fits zero characters on a line, the formatter tries the same character again.

In the code that splits a word that is too long for the line, a measured length of zero leaves the position where it was. Blaze takes one character with zero width and moves on.

**Code:** the `Blaze:` comment in `LVFormatter` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Drawing reads an unset word index

**Summary:** `LFormattedText::Draw` can read `lastWordSrcIndex` before it is set.

When the leading words of a line are hidden, the first assignment is skipped, and the value is read uninitialized. The variable is also `lUInt16`, which cannot hold a "no word yet" value. Blaze uses an `int` that starts at `-1`.

**Code:** `LFormattedText::Draw` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Nodes leak when a document is destroyed before its first layout

**Summary:** a document that never reached the cache file leaks its element and text nodes.

`~ldomDocument` unregisters the document before its nodes are freed. A node then cannot find its document, so `onCollectionDestroy` returns early and frees nothing. This happens for every node that is not in the cache file, for example when a document is closed before its first layout. Blaze frees those nodes with `destroyNodes()` while the document is still registered.

**Code:** `ldomDocument::~ldomDocument` and `destroyNodes` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Fonts that fall back to themselves are never freed

**Summary:** a font whose fallback font is itself, or a font in a fallback ring, stays alive until the process ends.

`GetFallbackFont` with an empty face name returns the first fallback face. When the main font is that face at the same size, its fallback is itself. `LVFontInstanceCache::gc()` only drops fonts with one reference, so it never collects such a font. Blaze clears the font references of every cached font in `~LVFreeTypeFontManager`. This fixes the leak at shutdown only. During a run, the fonts still stay in the cache.

**Code:** `~LVFreeTypeFontManager` in [lvfntman.cpp](crengine/src/lvfntman.cpp).

**Upstream:** present in `master`. We did not reproduce the self reference.

### Obfuscated fonts fail when the identifier has line breaks

**Summary:** the IDPF font obfuscation key keeps line breaks around the unique identifier.

The IDPF algorithm hashes the unique identifier with all XML whitespace removed: U+0020, U+0009, U+000D and U+000A. Upstream calls `trim()`, which removes only spaces and tabs at the ends. An OPF that puts the identifier on its own line gives a wrong key, and the embedded fonts do not load. Blaze removes all four characters everywhere in the identifier.

**Code:** `setIdpfManglingKey` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** present in `master`. We did not check whether the XML parser already removes the line breaks in this case.

### External SVG files resolve their own references wrongly

**Summary:** an SVG file referenced from XHTML looks up its images relative to the XHTML file, not to itself.

`lunasvgGetImageSource` resolves through `doc->getObjectImageSource(url, node)`, which is relative to the node's document. For an external SVG file, Blaze resolves the reference against the folder of the SVG file. Inline SVG and data URLs keep the document lookup.

**Code:** the `Blaze:` comment near `lunasvgGetImageSource` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### SVG files that reference each other recurse without end

**Summary:** an SVG image that refers back to an image being drawn can overflow the stack.

Blaze keeps a per-thread list of the SVG sources being drawn. It stops at a repeat, by stream or by name, and at a depth of 16, which also covers unnamed data streams. Each caller keeps its source alive.

**Code:** `SVGRenderScope` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`, which has no guard in `lvimg.cpp`. We did not check whether lunasvg stops such cycles itself.

### Navigation documents with several epub:type values are missed

**Summary:** `<nav epub:type="toc landmarks">` is not found as a table of contents.

`epub:type` is a list of tokens separated by spaces. Upstream compares the whole value with `"toc"`. Blaze splits the value and matches each token.

**Code:** the navigation document code in `ImportEpubDocument` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** present in `master`.

### linear="no" is stored on the manifest item, not on the spine slot

**Summary:** an item that appears twice in the spine gets one linear flag for both places.

Upstream stores `nonlinear` on the shared `EpubItem`, so the last spine reference wins. The `EpubItem` copy constructor and assignment drop `nonlinear`, and the default constructor leaves it uninitialized. Blaze keeps one flag for each spine slot in `linearFlags`.

**Code:** `linearFlags` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** present in `master`. Rare in real books.

### Saved positions add unknown element names to the document

**Summary:** parsing an XPointer with an unknown element name registers that name in the document.

`createXPointerV1` and `createXPointerV2` call `getElementNameIndex(name)`, which adds a new id for an unknown name. A stale or foreign bookmark then changes the name table of the document. Blaze uses a lookup that does not add names, `findElementNameIndex`, and returns a null XPointer.

**Code:** `createXPointerV1`, `createXPointerV2` and `findElementNameIndex` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Collapsed table borders depend on cell order

**Summary:** with `border-collapse`, a cell compares its border with a neighbour border that an earlier cell already changed.

Upstream calls `cell->elem->setStyle(newstyle)` inside the loop over cells, so later cells see the result of earlier ones. It also ignores `border-style: hidden`, and breaks ties with a flag, not with the CSS 2.1 order. Blaze finds all winning borders first, writes them after the loop, and gives `hidden` priority. The same code also maps borders for vertical text, which upstream does not need.

**Code:** `collapse_border` and the cell pass after it in [lvrend.cpp](crengine/src/lvrend.cpp).

**Upstream:** present in `master`. We did not render a table in upstream.

### An inherited odd-pixel text-indent becomes hanging

**Summary:** an inherited `text-indent` with an odd pixel value can turn into a hanging indent.

Upstream stores the `hanging` keyword in bit 0 of `text_indent.value` and tests `value & 1`. When an `em` or `%` indent is inherited, it becomes `css_val_screen_px`, and any odd pixel count then reads as hanging. Blaze gives hanging indents their own length type, `css_val_screen_px_hanging`, and passes the flag separately to the formatter.

**Code:** `setNodeStyle` and `renderFinalBlock` in [lvrend.cpp](crengine/src/lvrend.cpp), `css_val_screen_px_hanging` in [cssdef.h](crengine/include/cssdef.h), and the indent setup in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`. We did not reproduce it with a book.

### Zero-width no-break space shows as a question mark

**Summary:** U+FEFF can draw as the default character, and it makes a line go through right-to-left processing.

`getReplacementChar` puts U+2060 in its group of characters that can be ignored, but not U+FEFF, so a font without U+FEFF draws the default character. `getCharIndex` also returns glyphs for U+200B, U+2060 and U+FEFF when a font has visible control pictures for them. Blaze adds U+FEFF to the group and returns no glyph for these three characters. Separately, `lStr_isRTL()` counts U+FEFF as right-to-left because it is in the Arabic Presentation Forms-B block. Blaze ignores it there when Blaze's shaping options treat the joiner as plain. Upstream would need its own rule for that part.

**Code:** `getReplacementChar` and `getCharIndex` in [lvfntman.cpp](crengine/src/lvfntman.cpp), and the `Blaze:` comment on U+FEFF in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Drawing an image at zero size

**Summary:** `LVColorDrawBuf::Draw` does not return early for a zero width or height.

`LVGrayDrawBuf::Draw` has this guard. The color version does not, so it makes a zero-length map and can call `qSmoothScaleImage` with a zero target size. Blaze adds the same guard.

**Code:** `LVColorDrawBuf::Draw` in [lvdrawbuf.cpp](crengine/src/lvdrawbuf.cpp).

**Upstream:** present in `master`.

### Draw buffers start with undefined colors

**Summary:** `LVBaseDrawBuf` does not set its background and text colors.

A draw buffer that does not set them reads undefined values. A comment in upstream's `FillRect` mentions this. Blaze starts with white and black.

**Code:** the `LVBaseDrawBuf` constructor in [lvdrawbuf.h](crengine/include/lvdrawbuf.h).

**Upstream:** present in `master`. We did not check which derived buffers depend on it.

## Hardening

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

### A full disk ends the process

**Summary:** a failed cache write calls `crFatalError`.

`saveNodeData`, `compact` and `swapToCache` stop the process when a write to the cache file fails, for example on a full SD card. Blaze keeps the chunk in memory, logs the error and returns it. The `saveNodeData` part is mixed with a return type change for Blaze, so take only the `crFatalError` removals.

**Code:** `saveNodeData`, `compact` and `swapToCache` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

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

When two source fragments share one text node, `getNearestCommonParent()` returns that text node, and `getStyle()` returns an empty reference for it. Blaze moves up to the parent element first. We did not confirm that upstream can reach this case.

**Code:** the nowrap check in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Compiler checks

**Summary:** small changes that make the compiler check more.

The font classes mark their overrides with `override`, so a changed virtual signature fails to compile. `NB_IMP_SLOTS` is computed from the enum, which removes a hand-kept count and its `#error`.

**Code:** [lvfntman.cpp](crengine/src/lvfntman.cpp) and [lvstyles.h](crengine/include/lvstyles.h).

**Upstream:** present in `master`.

## Speedups

### List items are numbered once

**Summary:** numbering a list is linear, not quadratic.

Each list item finds its number by counting its previous siblings, so a list of n items costs n² steps. Blaze numbers all items of a list once and keeps the numbers by data index.

**Code:** `getListItemCounters` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp) and its use in [lvrend.cpp](crengine/src/lvrend.cpp).

**Upstream:** present in `master`.

### Sibling walks find the next child fast

**Summary:** a walk over the children of a large parent is linear, not quadratic.

Looking up the index of a child scans the child list from the start. Blaze first tries the entries beside the last result for the same parent. It still compares each entry, so a wrong guess only costs the normal scan.

**Code:** `findChildIndex` in [lvtinydom.cpp](crengine/src/lvtinydom.cpp).

**Upstream:** present in `master`.

### Selector matching skips rules that cannot match

**Summary:** the style pass tests fewer selectors per node, with the same cascade order.

Upstream already checks the class of the first rule with `quickClassCheck`, but it still walks every selector. Blaze adds two filters. Active stylesheets index the selectors that require a class, so a node only visits the selectors for its own classes and the selectors without a class. Each selector also records the element names and classes that its ancestors must have, and a quick ancestor check rejects it before the full match. The merge keeps the cascade order: a tie still goes to the element chain.

**Code:** [css_ancestors.cpp](extensions/css_ancestors.cpp), [css_class_index.cpp](extensions/css_class_index.cpp), and `quickAncestorCheck` and the merge in [lvstsheet.cpp](crengine/src/lvstsheet.cpp). The index updates in `detachScope` follow Blaze's stylesheet scopes, described in the next entry.

**Upstream:** present in `master`.

### Stylesheet scopes copy selectors only when needed

**Summary:** entering and leaving a DocFragment stylesheet no longer copies every selector twice.

Upstream `LVStyleSheet::push()` copies every selector chain with `dup()`, and `pop()` copies them all again with `set()`. A book with hundreds of fragments pays this for each one. Blaze's `push()` only records the selector count. The first `parseAndAdvance` or `merge` after a push copies the chains and keeps the original, and `pop()` swaps the original back without a copy.

**Code:** `SavedScope`, `push` and `pop` in [lvstsheet.h](crengine/include/lvstsheet.h), and `detachScope` in [lvstsheet.cpp](crengine/src/lvstsheet.cpp). `ScopedPush` belongs to Blaze's cancellation support and is not needed upstream.

**Upstream:** present in `master`. Not measured on its own.

### Line breaks are kept when a paragraph reflows

**Summary:** a paragraph that is formatted again with the same text and language does not run libunibreak again.

The document keeps the break decisions of recent paragraphs, keyed by text, language, `line-break` and `word-break`. A new width or font size then reuses them. It covers paragraphs up to 2048 characters in one source fragment, in up to 127 slots.

**Code:** `paragraphBreaks` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp), declared on `ldomDocument` in [lvtinydom.h](crengine/include/lvtinydom.h).

**Upstream:** present in `master`.

### PNG images decode one row at a time

**Summary:** a PNG that is not interlaced needs one row of memory, not the whole image.

Upstream reads all rows and then hands them to the drawing callback, which uses each row before the next one. Blaze reads and hands over one row at a time. An interlaced PNG still needs all rows, and Blaze limits that allocation to 64 MB, because inline data images skip the resource size check. One behavior changes: a PNG that fails halfway has already drawn its first rows.

**Code:** `LVPngImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp).

**Upstream:** present in `master`.

### Large JPEG images decode at a smaller size

**Summary:** a JPEG that is drawn smaller than its size is decoded at a reduced scale, and its rows are scaled as they arrive.

Upstream decodes the full image into a buffer and then scales it. When the drawing target accepts a smaller raster, Blaze sets libjpeg's DCT scaling and scales each row with the filters and rounding of `qSmoothScaleImage`. It only scales when the image size is a multiple of the MCU size, so that padding cannot bleed into the last row or column. This needs two new decoder callback methods, `GetRasterTargetSize` and `OnStartRasterDecode`.

**Code:** [lvimg.h](crengine/include/lvimg.h), `LVJpegImageSource::Decode` in [lvimg.cpp](crengine/src/lvimg.cpp), the decoder callback in [lvdrawbuf.cpp](crengine/src/lvdrawbuf.cpp), and `SmoothRowScaler` in [qimagescale_p.h](crengine/qimagescale/qimagescale_p.h).

**Upstream:** present in `master`.

### Hyphenation patterns load on first use

**Summary:** a language's hyphenation patterns load when text first needs them, not when the language is seen.

Upstream's `TextLangCfg` constructor resolves the hyphenation method, which loads the patterns, for every language tag in the book. Blaze resolves it on the first call to `getDefaultHyphMethod()`, which `softHyphenateText` also uses. The method is filled lazily without a lock, so it is not safe to format the same language on two threads for the first time at once.

**Code:** `TextLangCfg::getDefaultHyphMethod` in [textlang.cpp](crengine/src/textlang.cpp).

**Upstream:** present in `master`.

### Script lookup for ASCII

**Summary:** ASCII characters get their script without a HarfBuzz call.

`measureText` and `addLine` call `hb_unicode_script` for every character. For ASCII, Blaze returns Latin for letters and Common for everything else. This is a small gain that we did not measure on its own.

**Code:** `koblazeUnicodeScript` in [lvtextfm.cpp](crengine/src/lvtextfm.cpp).

**Upstream:** present in `master`.

### Cache of shaped words

**Summary:** each font face remembers shaped words and pair kerning, so common words are not shaped again.

Each face keeps a cache of shaped words and kerning by language. Plain Latin runs are measured from that cache. A sample of them is still shaped with HarfBuzz, and the widths and flags are compared, to catch differences. The face clears its caches when font settings change. This is a large change and needs a design discussion.

**Code:** [word_cache.cpp](extensions/word_cache.cpp), [fast_shaping.inc](extensions/fast_shaping.inc) and [shaping_options.cpp](extensions/shaping_options.cpp), compiled into [lvfntman.cpp](crengine/src/lvfntman.cpp).

**Upstream:** not in `master`.

## Features

### Vertical writing

**Summary:** `writing-mode: vertical-rl` and `vertical-lr`, with pages that can each use their own writing mode.

Blaze lays out and draws vertical text for CJK books. This includes upright and rotated glyphs, ruby, emphasis marks (`text-emphasis`), tate-chu-yoko, logical CSS properties, and borders and backgrounds in vertical boxes. A horizontal fragment in a vertical book, such as an author biography, gets horizontal pages. Each page records its writing mode for drawing and hit testing. The layout follows JLReq and the JFM model of LuaTeX-ja. It is a large change.

**Code:** [extensions/vertical/](extensions/vertical/), and the `Blaze:` comments in [lvrend.cpp](crengine/src/lvrend.cpp), [lvtextfm.cpp](crengine/src/lvtextfm.cpp), [lvfntman.cpp](crengine/src/lvfntman.cpp), [lvpagesplitter.cpp](crengine/src/lvpagesplitter.cpp) and [lvdocview.cpp](crengine/src/lvdocview.cpp).

**Upstream:** not in `master`. It has no `writing-mode` support.

### Flexbox

**Summary:** common `display: flex` and `inline-flex` layouts.

Horizontal rows support wrapping, grow and shrink factors, `flex-basis`, percentage widths, gaps, main-axis alignment, cross-axis alignment for single lines, and auto margins. Forward columns stack their children and can center short content. `@supports (display: flex)` stays false, because general nested layouts and fragmentation of side-by-side items are not supported. See [Flex layout](../../../doc/engines/koblaze.md#flex-layout) for the full list.

**Code:** [flex_rows.inc](extensions/flex_rows.inc), `css_d_flex` in [cssdef.h](crengine/include/cssdef.h), and the `Blaze:` comments in [lvrend.cpp](crengine/src/lvrend.cpp) and [lvstsheet.cpp](crengine/src/lvstsheet.cpp).

**Upstream:** not in `master`.

### EPUB manifest fallbacks

**Summary:** a spine item in a format the engine cannot read uses its manifest fallback.

The importer follows `fallback` chains in the manifest until it finds a supported item, and caches the result. Each spine slot keeps its own item, also when several slots share a fallback. Links to the original manifest item still reach the chapter.

**Code:** [spine_fallback.h](extensions/spine_fallback.h) and `ImportEpubDocument` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** not in `master`.

### DTBook chapters in EPUB 2

**Summary:** spine items of type `application/x-dtbook+xml` load as chapters.

**Code:** [dtbook.h](extensions/dtbook.h) and `ImportEpubDocument` in [epubfmt.cpp](crengine/src/epubfmt.cpp).

**Upstream:** not in `master`.

### Images in navigation labels

**Summary:** table of contents entries that are only an image get a label and keep the image.

A navigation link that holds only an image takes its label from the image's `alt` or `title`, and the entry keeps the image path so that a reader can show it.

**Code:** [nav_labels.h](extensions/nav_labels.h), `_labelImage` in [lvtinydom.h](crengine/include/lvtinydom.h), and [epubfmt.cpp](crengine/src/epubfmt.cpp).

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

## Changes for Kobalt's reader

These changes serve Kobalt's reader on slow single-core devices. They add host interfaces that KOReader does not use. They are listed so that a maintainer who reads the diff knows what they are.

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

### Packed hyphenation patterns

**Summary:** the host can supply hyphenation patterns in a compact binary form, with the XML patterns as a fallback.

**Code:** [packed_hyphenation.inc](extensions/packed_hyphenation.inc) and [hyphman.cpp](crengine/src/hyphman.cpp).

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
- In-page link ids are percent-decoded when the page context records them. We have not checked whether upstream needs this, because upstream decodes when it follows a link.

**Upstream:** not in `master`. For the percent-decoding item, we did not check whether upstream needs it.

## Keeping this file current

Kobalt updates this file in the same change as each Blaze change to inherited code. An entry is removed when Blaze drops the change or upstream has it. When an entry is offered upstream, the link to the discussion or pull request goes at the end of its section.
