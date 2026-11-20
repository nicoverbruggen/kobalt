# Blaze source and provenance

Blaze is Kobalt’s rendering engine, a hard fork of KOReader’s CREngine. Kobalt maintains it in `src/engine/blaze`. It is an ordinary source directory, not a submodule or a link to another checkout. Desktop, Kobo and Android builds use this directory. Kobalt’s adapter lives in `src/engine/blaze/adapter`; the imported library retains its upstream directory structure beside it. Engine objects stay in each platform build directory; the shared dependency cache supplies the external libraries.

The initial import is the library portion of the local koblaze fork at `6d3946edf08ef69afe19a3c457b88af126ae64aa`, based on `koreader/crengine` commit `4ce1aed17e9d7852e047e0b9b0bf6a637486fd97`. These are the same source bytes used by the preceding Mac and Qt5 test builds. The original checkout and its Git history remain separate and unchanged. The import copies source files, not Git history.

The import holds the library sources and headers, the image scaler, the default style sheets and the license notices. It leaves out the CoolReader applications and their packaging, the bundled third-party libraries that Kobalt does not build, and the upstream lint setup, test scripts and development notes. Kobalt supplies its enabled third-party libraries through `dependencies/`. Kobalt configures `src/engine/blaze/CMakeLists.txt` with `AUTO=OFF`. It builds the imported source and Blaze extensions into the same library.

The engine knows a fixed list of hyphenation pattern names. `src/engine/blaze/data/hyphenation-patterns.txt` holds that list. The pattern files themselves are not part of the engine source: Kobalt ships its own selection from `assets/hyph`.

Preserve the GPLv2 notice in `src/engine/blaze/README.TXT`, the license text in `src/engine/blaze/COPYING`, the image-scaler license, and the copyright headers. Keep self-contained Blaze code in `extensions/`. Edit the inherited files where the parser, formatter or font manager needs to call it. The former `dependencies/crengine-patches.txt` series and its patch files are retained in Git history. Their changes are integrated into this source; builds do not apply a second engine patch stack.

## Flex layout

Blaze runs flex layout when the computed display is `flex` or `inline-flex`. Horizontal text rows support wrapping, grow and shrink factors, flex basis, percentage item widths, length gaps and main-axis alignment. Single-line rows can center, stretch or align items at either end of the cross axis. Inline containers use the inline box formatter and measure the row for an automatic width.

A forward column stacks its children with a row gap and can align them horizontally. A short column with a definite height can center or distribute simple text items vertically. Oversized stacks use ordinary text page breaks. General nested layouts, reverse or wrapping columns, and independent fragmentation of side-by-side items remain outside the prototype. Multi-item row layout requires zero item margins and excludes percentage padding or percentage minimum and maximum widths. Blaze keeps the feature query `@supports (display: flex)` false.

## Bringing in upstream changes

Compare the upstream change with the recorded KOReader base. In `crengine/`, keep upstream file names, indentation and declaration style. Kobalt’s [.clang-format-ignore](../../.clang-format-ignore) excludes these files. Format the adapter under `adapter/` with Kobalt’s Qt style.

In inherited files, `Blaze:` marks a difference from the recorded KOReader revision. This includes changes carried over from earlier forks. Place the comment beside the code and explain what an upstream update must preserve. Find these comments with `rg -n 'Blaze:' src/engine/blaze/crengine`. Compare against the recorded revision for the full diff.

Blaze keeps its cancellation and pause callbacks and its chapter-window selection in `src/engine/blaze/extensions/`. They compile into the engine library and use the declarations in `crengine/include/koblaze.h`. The EPUB import loop still decides when parsing can stop and inserts an empty fragment for each skipped spine item so saved positions keep their fragment indices.

The document methods in [chapter_layout.cpp](../../src/engine/blaze/extensions/chapter_layout.cpp) select the blocks in a chapter slice and save and restore their layout. They use the existing private document state. The inherited block renderer calls `koblazeEnterBlock` when it reaches a block. The same file holds the view and document methods for swapping alternate layouts. [Page spacing](../../src/engine/blaze/extensions/page_spacing.cpp) classifies the lines that may move, records their gaps after page splitting, and maps between displayed and source coordinates. The renderer supplies the current float and initial-letter positions. The page splitter owns the serialized fields.

Each font face owns a word cache. The [cache](../../src/engine/blaze/extensions/word_cache.cpp) stores shaped words and kerning by language. The [fast shaping methods](../../src/engine/blaze/extensions/fast_shaping.inc) compile with `lvfntman.cpp` because CREngine defines its FreeType face inside that file. Its measurement loop selects the shaping path and checks sampled widths and flags against HarfBuzz. The face clears its caches when font settings change. Shaping overrides live in `extensions/shaping_options.cpp`. The font manager also owns an [embedded font cache](../../src/engine/blaze/extensions/document_fonts.cpp). It shares the bytes between chapter documents of the same book. The font manager still opens the resource, checks its size and registers the font with FreeType.

For a change to text layout, check both the inherited `lvtextfm.cpp` and [Blaze’s vertical formatter](../../src/engine/blaze/extensions/vertical/lvtextfm_vert.cpp). They still compile together because the vertical formatter uses the private formatter class. The [vertical box painter](../../src/engine/blaze/extensions/vertical/vertical_boxes.cpp) maps borders, fills and background images into screen coordinates. The inherited renderer selects it using the writing mode of the current box and page. Both writing modes use the image sizing, positioning and tiling code in `drawutil.cpp`. Vertical pages supply their painted rectangle and clip on all four sides. Bold Reading and ordinary text use the same [font variation settings](../../src/engine/blaze/extensions/font_selection.cpp). Changes to block layout or the DOM also need testing with a chapter window and a complete document. Use the [layout regressions](../validation/layout.md) to compare them.

The CSS parser keeps the selector grammar and cascade order. The [ancestor filter](../../src/engine/blaze/extensions/css_ancestors.cpp) records the requirements of a parsed selector and checks ancestor element names and classes before a full match. Active stylesheets also index universal selectors that require a class. The matcher merges the candidates in cascade order.

The EPUB importer resolves manifest fallback chains before building chapter fragments. Page lists use CREngine’s existing NCX and EPUB navigation parsers. Chapter windows retain the target ID for a printed page outside the loaded chapter. The reader resolves it when the page is selected.

Blaze uses libwebp for still images and for the first composited frame of an animation. Layout reads the WebP header. A still-image decoder can produce a smaller raster when the drawing destination accepts one. FreeType uses Brotli for WOFF2. The embedded font cache retains reconstructed single-face fonts so other sizes and chapter windows can share them. Font collections use FreeType’s collection handling.

## Cache compatibility

Parsed-book format 16 rebuilds chapter fragments and navigation records for DTBook chapters, manifest fallbacks, illustrated labels and landmarks.

Moving the same source into this repository does not change parsed-book or rendering cache formats. Later parser, preprocessing, shaping or layout changes must follow [Storage compatibility](../storage.md).

The retired chapter page counter and its transient package XML cache are removed. Chapter navigation retains its bounded document cache. Parsing the same package, navigation and NCX input without that XML cache does not change parsed-book content or layout, so this cleanup retains the cache formats and rendering version.

Current-page formatting warnings preserve the requested writing mode separately from rendering fallback. Computed style hashing, equality and serialization include that diagnostic state. Parsed-book format 12 rejects older style records. The rendering version stays unchanged because the fallback geometry and pixels stay the same. The draw buffer records unsupported style only when clipped text or an image is drawn, including generated content and page notes.
