# Typography and page layout checks

Label `layout`. Build `mac-tests`, then run `ctest --preset mac -L layout` from `platforms/desktop/`. `ctest --preset mac -N -L layout` lists its tests.

These checks cover the CREngine layout groups, publisher markup, the document cache, advanced text, columns, page spacing, paragraph adjustments, custom CSS, book normalization and the representative literary corpus. The `engine` checks render through CREngine directly and compare pixels, geometry and source positions. The `desktop` checks apply the same settings through the production reader. `component.ui`, in [Settings and controls](settings.md), checks the paragraph sliders and continuation hints.

## Engine checks

The restored `reader`, `publisher-markup`, `local-reader` and `document-word-count` checks keep the original assertions. Their focused EPUBs are generated offline in C++ with the original text, span IDs, markup and resource paths. `engine.document-cache` is a separate check, so `engine.reader` stays within the child deadline under parallel execution. `engine.reader` also links the portable Kobo position codec, but it does not load Nickel or inspect firmware symbols. Its variable-font cases need the pinned Amstelvar fixture and fail rather than skip without it.

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

The CREngine upgrade to `f335f7a` keeps the Kobalt patch stack. `engine.vertical-writing` and `engine.reader` cover the adaptations for the upstream combined text decorations and font-width support.

## Captures

Set `KOBALT_TEST_CAPTURE_DIR` for app captures of the column checks, or `KOBALT_TEST_COLUMN_SPACING_CAPTURE_DIR` for the engine comparison of text-size and margin-relative column spacing. Set `KOBALT_TEST_SPACING_CAPTURE_DIR` to keep local before and after images of the decorative page-spacing matrix.

## Literary corpus

`corpus.multilingual` uses unchanged source JSON for Arabic, Hebrew, Hindi and Japanese from the original fixture set. `tests/fixtures/public-domain/catalog.json` keeps their original file checksums, and the test verifies the source files and chapter checksums. It packages the first eight paragraphs of each of the first two chapters in C++, with the original text, author, source revisions, rights and Wikisource contributor attribution. The source works are public domain. The transcriptions keep the CC BY-SA 4.0 terms in the accompanying `LICENSE` and the EPUB metadata. This bounded corpus is separate from the screenshot corpus in [Screenshots and gallery](gallery.md). Neither gives a full international rendering guarantee.

## Experimental book normalization

`ctest --preset mac -R "engine.beautifier|model.appearance-settings|session.(reading-book|bookmark-session|highlight-session)"` runs the book normalization checks with the saved state, bookmarks and highlights that it affects.

For an authorized local EPUB, `kobalt-beautifier-tests --compare-layout <epub> <passage> <output-directory>` compares paragraph spacing, indentation and line spacing one at a time, then checks centering and expansion on up to 32 pages from that passage. It saves comparison images and a JSON record in the output directory. Each setting must restore the original page when reset, and the source EPUB must stay unchanged. A zero count for a page-spacing mode can mean that the sampled pages do not meet that mode's suitability rules. It does not show a defect by itself. `kobalt-beautifier-tests --compare <epub> <passage> <output-directory>` renders the same source passage before and after normalization, checks the source text and file hashes, and verifies that turning the mode off restores the original pixels. Keep private books and comparison output under ignored build directories. Do not add them to the public fixtures.

## Limits

These checks do not measure river reduction or device reflow performance. Synthetic fixtures do not establish compatibility with every publisher, script or font, or physical-device rendering, touch and e-ink behavior.
