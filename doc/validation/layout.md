# Typography and page layout checks

Label `layout`. Build `mac-tests`, then run `ctest --preset mac -L layout` from `platforms/desktop/`. `ctest --preset mac -N -L layout` lists its tests.

These checks cover the CREngine layout groups, publisher markup, the document cache, advanced text, columns, page spacing, paragraph adjustments, custom CSS, book normalization and the representative literary corpus. The `engine` checks render through CREngine directly and compare pixels, geometry and source positions. The `desktop` checks apply the same settings through the production reader. `component.ui`, in [Settings and controls](settings.md), checks the paragraph sliders and continuation hints.

## Engine checks

`engine.columns` compares each half of a spread with the same page rendered at single-column width. It also checks that viewports narrower than two minimum-width columns clip the page at the screen edge in both reading directions.

`engine.epub-compatibility` opens an EPUB 2 DTBook, searches its text and follows its NCX link. It also reads a ZIP64 member past the 4 GiB offset in a seekable in-memory fixture, checks illustrated navigation labels and resolves a landmark outside the loaded chapter. The EPUB fallback checks open chapters with a missing fallback ID, a self-reference and a two-item cycle. Each contents entry must still open its original chapter. The chapter preview checks cover a local image with a short entity declaration, a wrapper with `xml:base`, an external image URL and a declaration over the accepted length. To repeat the independent EPUB 3 navigation checks, pass the W3C [image label](https://github.com/w3c/epub-tests/tree/12237fcbf91ea6cfe8908b8ebcaa840776ccbc1c/tests/nav-non-text_img) and [image title](https://github.com/w3c/epub-tests/tree/12237fcbf91ea6cfe8908b8ebcaa840776ccbc1c/tests/nav-non-text_img_title) test books as arguments to `kobalt-epub-compatibility-tests`.

Hybrid regressions keep the chapter usable when a prepared document cannot resolve its reading position. Such a document is dropped and not built again for the same appearance. The page estimate regression opens a chapter whose heading is visible by itself and hidden by a publisher CSS rule in the whole book. When pagination finishes at the Chapter 4 heading, the book page and total must match the first visible paragraph in the whole book, in portrait and in two-column landscape. Both a prepared layout and a fresh layout must leave the chapter in place and preserve the next turn. Direct navigation and saved page lookups also reject the hidden heading and keep a later visible passage in place. The reverse switch also keeps the whole book and its displayed page when publisher CSS hides that heading in the chapter. The session regression deliberately damages the prepared document and checks that the requested turn and later turns succeed in chapter mode without an error. In a long chapter, the text around a search hit must match the whole book, including when the part containing the hit has no layout. A record saved by the whole book must open chapter mode in a later slice with the page numbers of the whole book, and keep them while the reader reads on. `desktop.hybridReopensCounted` closes a book that Hybrid laid out as a whole and opens it again: the chapter must show the same book and chapter pages at once, without an estimate, for whole chapters and for slices.

The flex checks compare a text row with a right margin to the same row with a gap. They check item positions with margins during growth and in a reversed row. The flex checks also cover a label beside a taller item, a centered column with a definite height, a percentage sidebar and an inline badge at its natural width. The page layout check covers a centered verse block, sibling widths at three viewport sizes, wrapping, gaps, weighted growth, and a row taller than a page. An unused flex selector must produce the same pixels as ordinary block layout. The chapter slice check opens a long, plain flex column in slices and compares every page start and image with the whole book. The page estimate check uses short chapter slices to exercise handoff inside a chapter with 100 wrapping rows. A fixed-height column at 40 px reader text must keep the same page starts and pixels after a chapter handoff on a 320 × 360 viewport.

The saved synthetic book `overflowing-float-bookmarks.epub` checks that a page containing overflowing float text has a reading position that resolves to the same page in the whole book. `heading-after-floats.epub` compares every page start and image when a float changes the height of the heading before a slice boundary.

`KOBALT_HANDOFF_BOOK=<epub>` makes `kobalt-page-estimate-tests` walk that book in chapter mode and resolve every page anchor in the whole book. It compares pages by sequential index because a blank page can share its anchor with the following page. It fails on unresolved positions, different page totals, different page starts or different sampled pixels. Pixel comparisons sample every fifth page. Set `KOBALT_HANDOFF_ALL_PAGES=1` to compare every page. Optional `KOBALT_HANDOFF_CAPTURE_DIR` saves the first differing page pairs for inspection. Keep captures under the worktree’s `build/` directory.

The Hybrid font-size check uses a percentage column gap. It verifies that the reader adopts the whole book with the new pixel gap after changing text size.

The session checks also request an appearance edit while the worker has adopted the whole book but has not published its frame. The edit must return to chapter rendering before reflow.

The Hybrid checks also cover interrupted chapter-place reads, publisher-font reuse across the whole-book build, font axes and the publication language after a parked build resumes from a preview, parked-cache ownership and abandonment after a cached document opens. Rendering policy 5 rebuilds pagination that may contain another book's publisher-font metrics. Parsed-book data remains compatible and is retained. Search-context resolution and refusal handling do not change either cache format.

The restored `reader`, `publisher-markup`, `local-reader` and `document-word-count` checks keep the original assertions. Their focused EPUBs are generated offline in C++ with the original text, span IDs, markup and resource paths. `engine.document-cache` is a separate check, so `engine.reader` stays within the child deadline under parallel execution. `engine.reader` also links the portable Kobo position codec, but it does not load Nickel or inspect firmware symbols. Its variable-font cases need the pinned Amstelvar fixture and fail rather than skip without it.

`engine.css-class-index` compares indexed and scanned styles, including imports, equal specificity and pseudo-elements. It also checks that leaving nested stylesheet scopes restores the outer rules in order. It also compares page starts and rendered pixels for equivalent selectors.

`engine.fallback-styles` checks that small caps synthesized for a chapter-scoped embedded font use that same face. It compares the drawn capitals, confirms that missing characters use regular fallback, and checks that a font with native OpenType small caps uses its own substitutions. Rendering policy 11 rebuilds pagination with the corrected glyph metrics. It retains parsed books and saved reading data.

`engine.spine-window` checks that a chapter window does not report its own pages as exact book progress, and that whole-book layout restores the exact page numbers. It also checks that a rejected appearance change leaves the chapter’s reading font in place. The page-note tests in `engine.links` check that enabling notes cannot bypass the same validation.

`engine.engine-contract` checks that callbacks stay with their view through chapter changes and detach when the view is destroyed. Nested layouts preserve their caller's cancellation and progress. A foreground layout with nested flows polls its own callback while a whole-book layout is parked.

`engine.page-estimate` restores a bookmark outside the initial chapter window, parks whole-book pagination, jumps until the bookmarked chapter is evicted, then returns. The bookmark, adjacent pages and final handoff must match an independent whole-book rendering. Opening a second book on the same engine must use the new book's pages and count whether the first book's pagination was parked or complete.

The slice checks compare interrupted and uninterrupted readers at the start of layout, during extension of a long paragraph and at a slice boundary. Cancelling preparation of the next slice must preserve the visible page and allow both a reverse turn and a retry. `engine.layout-interrupt` also interrupts a viewport change in a later slice, returns to the original viewport and checks the page starts and pixels in both directions. The layout-interrupt check also rejects incomplete cached page lists and invalid page geometry, then restores a valid list.

`engine.page-estimate` checks the position and word count of a passage with supplementary Unicode characters before the anchor.

`engine.page-estimate` compares saved highlight end pages with whole-book pagination after the chapter leaves the window. A selection ending at the start of the next page belongs to the preceding page.

`engine.chapter-slices` checks that the text beside a floating sidebar stays outside the sidebar when Blaze formats the page for display. Rendering policy 8 refreshes saved layouts with the corrected float geometry.

Chapter slices also compare every page start and image against the whole book when publisher CSS floats a paragraph or hides it. A new slice layout key drops the old observed page numbers while retaining reading positions and the learned estimate weights. A floating drop cap at the top of a page must keep the same spacing and following lines across the slice boundary.

`engine.chapter-parity` compares chapter and whole-book navigation across hidden non-linear chapters. It checks covers, explicit entry into a hidden chapter, forward and reverse turns, adjacent preview pixels and the last readable page before a hidden appendix. It also crosses between a long chapter and a group of short chapters with hidden footnotes at both ends. After a font or line-spacing change, it compares the reading position and page pixels at slice boundaries where a container has a top border and padding. In vertical Japanese text it also checks the next passage after crossing that boundary in both directions. After reflow, forward and reverse turns across a padded container also check its bottom edge against whole-book rendering. A shaded-group fixture checks that blocks from a previous slice leave no background on the current page.

## Shared locations and the engine interface

`engine.location-round-trip` opens the real EPUB fixtures in `tests/fixtures`. For sampled text nodes of each spine item it converts three CREngine positions to a shared location and back, and requires the same place in the text. It also parses the source XHTML and requires the same element name and the same counted text at each location path. To report on other books, run `kobalt-location-round-trip-tests FOLDER` with `QT_QPA_PLATFORM=minimal`; it prints one line for each EPUB in the folder and requires nothing. See [the location format](../engines/contract.md#location-format).

`engine.engine-contract` checks each state of an engine query and that a value of another layout is not current. Through the engine interface it then requires that the whole-book engine reports exact book and chapter pages on every page, and a new layout generation after a font size change. It has no engine that leaves chapters without layout yet.

## Check a folder of EPUBs

From `platforms/desktop/`, configure the [Mac preset](../building.md#mac) if needed, then run:

```sh
cmake --build --preset mac --target kobalt-local-reader-tests
../../build/desktop-mac/kobalt-local-reader-tests --epub-folder "/path/to/books"
```

On Linux, use the `linux` preset and `build/desktop-linux/` output directory.

The scan visits subfolders and checks files ending in `.epub` regardless of case. It skips symbolic links. One headless process opens the books in path order and releases each one before moving on. The test uses the local reader with fresh settings and no saved reading state or disk cache. A pass means the reader rendered a first page.

The terminal lists each file, its result and the reason for any failure, then prints the totals. Exit status `0` means every book opened. Status `1` means at least one book failed. Status `2` means the scan could not run or finish. An opening that takes more than two minutes is cancelled and reported as a failure. The scan stops if the engine does not finish cancelling. If the process crashes, the last `Checking` line identifies the book in progress.

Intentional cache-policy and stylesheet mismatches in the negative tests can print CREngine cache warnings. The normal-book diagnostic check still rejects unexpected warnings.

The CREngine upgrade to `4ce1aed1` keeps the Kobalt patch stack and the manual optical-size override. Both normal text and Bold Reading prefixes use the upstream 12-point optical-size fallback when CSS sizing is enabled and render DPI is below 100. `engine.reader` checks manual optical sizes with CSS sizing disabled, including Bold Reading prefixes and authored font widths. `engine.vertical-writing` also checks a paragraph that inherits vertical writing. Its border encloses the background fill, and a centered background image falls inside the filled column in both vertical directions. A percentage-sized tile also repeats down the column. HarfBuzz 15.1.0 and FriBidi 1.0.18 also affect text shaping and bidirectional layout; use the font, layout and multilingual corpus checks when changing these pins.

## Figure pagination

`engine.figure-layout` covers fitting and boundary-positioned figures, oversized figures, long captions, publisher split overrides and forced breaks with retained or discarded margins. It checks one and two columns in both themes at 96 and 300 PPI, image dimensions, source order, caption links and selection, reflow, previews and cached reopening. Set `KOBALT_TEST_FIGURE_CAPTURE_DIR` to retain its focused page captures at 96 PPI. Plain images with adjacent captions keep their existing behavior. Non-block fixtures verify that the default break rule is absent and that pages match a reference without it, including floated and inline figures and block figures inside floats, inline boxes or table cells.

The regular `engine.figure-layout` check also opens the accepted `tests/fixtures/figure-caption.epub` review book. Its SHA-256 is pinned in the test. The check first confirms separation with the default disabled, then requires the image and caption together on page two.

For a dedicated before/after EPUB, run `kobalt-figure-layout-tests --review-document <output-directory>`. It writes `figure-caption.epub` and two page captures at 600 × 800 pixels and 96 PPI. Pass that EPUB as a third argument to capture the same bytes with another build. Set `KOBALT_TEST_FIGURE_EXPECT_SPLIT=1` only for the baseline build: the check requires the image on page one and the caption on page two. The candidate requires both on page two. The command uses the same checked-in fixture as the regular test. The document uses ordinary prose to reach the page boundary. It contains no fixed-height spacer.

`tests/fixtures/reporter-figure-float.epub` reproduces the CSS and figure markup from [MobileRead post 467](https://www.mobileread.com/forums/showpost.php?p=4622373&postcount=467). It retains `display:inline-block`, `float:left`, `width:100%`, authored break avoidance and viewport-relative image limits. The image is a generated portrait rectangle. The omitted surrounding prose is synthetic, and the local image path uses PNG. The regular figure check rejects image/prose overlap and caption displacement, including right floats, narrower floats with wrapped prose and a page-boundary placement. It requires selection rectangles for each visible search hit before checking for overlap. It collects time-sliced search results before checking text and geometry, so CPU scheduling does not decide the expected match count. It checks both themes, reading densities, columns, cache reopening and viewport restoration. A separate matrix checks ordinary floats at the saved-reference limit and beyond it, including cache reopening. Run `kobalt-figure-layout-tests --reporter-document <output-directory>` to capture this book, or `--reporter-only` for its focused matrix.

The default keeps a fitting figure together. It does not defer figures past later prose to fill gaps. The original MobileRead report supplied no book sample; the synthetic fixtures do not establish compatibility with every publisher layout.

## Captures

Set `KOBALT_TEST_CAPTURE_DIR` for app captures of the column checks, or `KOBALT_TEST_COLUMN_SPACING_CAPTURE_DIR` for the engine comparison of text-size and margin-relative column spacing. Set `KOBALT_TEST_SPACING_CAPTURE_DIR` to keep local before and after images of the decorative page-spacing matrix.

## Literary corpus

`corpus.multilingual` uses unchanged source JSON for Arabic, Hebrew, Hindi and Japanese from the original fixture set. `tests/fixtures/public-domain/catalog.json` keeps their original file checksums, and the test verifies the source files and chapter checksums. It packages the first eight paragraphs of each of the first two chapters in C++, with the original text, author, source revisions, rights and Wikisource contributor attribution. The source works are public domain. The transcriptions keep the CC BY-SA 4.0 terms in the accompanying `LICENSE` and the EPUB metadata. This bounded corpus is separate from the screenshot corpus in [Screenshots and gallery](gallery.md). Neither gives a full international rendering guarantee.

## Experimental book normalization

`ctest --preset mac -R "engine.beautifier|model.appearance-settings|session.(reading-book|bookmark-session|highlight-session)"` runs the book normalization checks with the saved state, bookmarks and highlights that it affects.

The normalization check compares authored, inline, inherited and pseudo-element text colors with a neutral reference in both themes. It checks publisher background removal, reversible toggles, reflow, cache reopen in both directions and unchanged raster and vector image pixels. Arabic fixtures compare joined letters at the start of a paragraph and inside an inline span. An explicit non-joining character confirms that the pixel comparison can detect broken joining. Settings checks cover the optional default, global ownership, preset selection and appearance identity.

For an authorized local EPUB, `kobalt-beautifier-tests --compare-layout <epub> <passage> <output-directory>` compares paragraph spacing, indentation and line spacing one at a time, then checks centering and expansion on up to 32 pages from that passage. It saves comparison images and a JSON record in the output directory. Each setting must restore the original page when reset, and the source EPUB must stay unchanged. A zero count for a page-spacing mode can mean that the sampled pages do not meet that mode's suitability rules. It does not show a defect by itself. `kobalt-beautifier-tests --compare <epub> <passage> <output-directory>` renders the same source passage before and after normalization, checks the source text and file hashes, and verifies that turning the mode off restores the original pixels. Keep private books and comparison output under ignored build directories. Do not add them to the public fixtures.

## Limits

These checks do not measure river reduction or device reflow performance. Synthetic fixtures do not establish compatibility with every publisher, script or font, or physical-device rendering, touch and e-ink behavior.

## Cache compatibility across engine updates

Dependency and patch updates require a cache compatibility review. The parsed-book policy retains the beta 7 identity; explicit parser and engine-format versions reject incompatible records. A separate rendering version refreshes styles and pagination without rereading publication text. Hyphenation changes refresh layout through that rendering policy.

Build `kobalt-document-cache-tests` against the previous and candidate sources with the same probe. Run the previous executable with `--write-compatibility <epub> <empty-output-directory>`, then the candidate with `--check-compatibility <epub> <same-output-directory>`, using Qt's minimal platform. The probe requires old-cache hits, compares every page and chapter with a fresh candidate parse, checks reading anchors and confirms the next unchanged opening does not rewrite the cache. It covers ordinary text, Bold Reading, variable optical sizes and vertical writing. Use an isolated directory, never a user's profile. The ordinary document-cache suite runs the same comparisons within one engine version.

The vertical-writing buffer regression covers a run measured at 57 pixels that draws 58 pixels. Rotating the old allocation read beyond its last row and could paint unrelated glyph fragments, making cached and fresh pages differ. The local buffer patch grows and zero-fills the allocation before compositing and rotation. It adapts the buffer-safety part of the upstream vertical fork's fix without its experimental glyph-origin changes.

The horizontal-rule check in `engine.beautifier` renders the reported top-border CSS at several reading densities. It checks both border heights and the gray colors.

Current-page formatting warnings use parsed-book format 12 for the added style diagnostic field. `engine.page-estimate` checks that unsupported CSS does not change fallback pixels and that cached styles retain the warning. The previous whole-book inspector remains covered as a standalone utility; it no longer determines reader warnings.

The session check turns forward from Trevelyan’s cover and backward from its title while the whole book is ready. In two columns the whole book shows the cover with the title, and the chapter shows the cover alone. Both turns stay in the chapter and keep its layout revision, so bookmark and page lookups of the chapter stay valid. The adjacent Navigate thumbnail must show the page of that turn. A numbered jump to the cover must keep it alone. A jump to the cover’s passage then adopts the whole book with the cover and title together and starts a new layout revision. A bookmark lookup queued before that jump keeps its answer, and one queued behind it is dropped. The check continues with a turn beyond that screen and a return to portrait. The chapter-parity check reads a vertical Japanese chapter with ornamental columns in both directions. It compares the slice pixels and the screen selected for handoff with Accurate pagination.

Two-column screens pair the columns of their own document: a chapter from its start, the whole book from the start of the book. The chapter-parity column-pairs check opens the second chapter of a book whose first chapter has three or four one-paragraph columns. With three, the pairs are one column apart, and no turn in either direction may switch to the whole book. With four, every turn switches to the next whole-book screen. With odd and even column counts, the last chapter screen must show the full book total and the chapter’s own last page both before and after reopening. The check opens positions saved by each layout, removes the spine-item counts from a checkpoint, and compares the first backward turn before and after reopening. `desktop.hybridColumnPairs` reads the same kind of book through the reader in landscape. Each forward turn after the build, the switch included, must start two paragraphs later. The session check jumps by page number in the second chapter with three or four columns before it. It compares the displayed image and saved passage with the same jump before handoff. The four-column case switches when both images match. On another spread, publisher CSS hides the text of the second column only in the whole book. The reference images must agree on the first column and differ on the second. That jump must stay in the chapter.

The column checks cover odd and even chapter lengths in both reading directions with chapter alignment enabled. They check the empty column, chapter jumps, turning in both directions, and switching alignment off. A contents entry inside a spine item starts a new spread only when it begins a page after a forced break.


The columns check reads a non-linear section in both page directions. It checks that hiding it clears the trailing column, explicit jumps still reach it and showing it restores the shared screen.

`engine.fast-shaping` checks that colliding words keep their own metrics after the table grows and the language changes, and that clearing a face drops every language’s records and releases the active glyph buffers. It compares fast and normal shaping after Latin Extended Additional letters enter a warm glyph cache. It checks automatic selection around the text threshold, explicit overrides, font-size changes, interleaved documents and small slices of a large chapter. It compares page counts, boundaries and pixels with the selected forced path across bundled serif and sans-serif fonts. The optional parsed-cache property `kobalt.shaping.text.250000` retains the bounded count for the same rendering policy. Older caches compute it once without reparsing. Rendering policy 7 repaginates existing layouts while retaining parsed books and reading data.

The embedded NV Palatium fixture checks native Latin small caps and synthesized Greek and Cyrillic capitals in the same face.

`engine.line-breaking` checks Japanese strict and loose line breaking. It checks that a paragraph reuses its retained break decisions only for the same text, language and CSS line-break and word-break values, and that a reflow back to the original margin, which reuses them, gives the same page starts as the layout that computed them.
