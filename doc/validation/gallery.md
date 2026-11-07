# Screenshots and gallery checks

Label `gallery`. Build `mac-tests`, then run `ctest --preset mac -L gallery` from `platforms/desktop/`. `ctest --preset mac -N -L gallery` lists its tests.

These checks cover gallery generation, the completeness and integrity checks, failed workers, the phone frame, the primary-view tour and screenshot-book preparation. The gallery itself is not a CTest test. Build `mac-screenshots` for the full gallery or `mac-light-screenshots` for the primary views; see [Headless tests and screenshots](../building.md#headless-tests-and-screenshots). Both produce PNGs, a checksum manifest and a local HTML gallery. The primary-view test has a 120-second process limit for the complete viewport and theme matrix.

## Workers and filters

Gallery workers run in separate processes with isolated profiles and temporary files. Each worker reuses immutable font setup across its cases. The default worker count uses the available CPUs, up to eight workers. Set `KOBALT_GALLERY_JOBS` to an integer from 1 to 32 to override it. Each worker has a hang guard, and individual UI actions keep their behavioral deadlines. A failed worker stops the run and keeps the previous complete gallery.

The light gallery, the full gallery and a scene-filtered run have separate output directories. A scene-filtered run writes to `build/screenshots/mac-scenes/`. `KOBALT_GALLERY_SCENE` names gallery cards, which are the `screen` field of each manifest entry, and captures all their pages in every size and theme. The full gallery's manifest records the case that captured each image, so the filter runs only those cases, and each worker stops after its last selected scene. Run the full gallery once before the first filtered run, and again after a change that moves a scene to another case. `KOBALT_GALLERY_CASE` runs named gallery cases instead. A filtered run does not replace the full gallery. No gallery captures Android device screens.

## Viewports

All scenes render the production reader or Library widget directly, without the desktop preview or a device frame. Six-inch images are 1072 × 1448 pixels and seven-inch images are 1264 × 1680 pixels, transposed for landscape. The six-inch landscape viewport is 1448 × 1072 pixels. PNG density metadata and manifest entries record 300 PPI.

The phone is 720 × 1600 pixels at 300 PPI, a representative 20:9 screen of about 61 × 135 mm at the reader's design density, not a named handset. In portrait it has a 60-pixel camera band at the top and corners with an 80-pixel radius. The reader fills the band with paper and starts the page below it. The Library starts below the band, as it starts below Android's status bar. In landscape the camera side is letterboxed on both sides, so the phone landscape capture is 1480 × 720 pixels. Phone mode supports 100% interface text only, because the UI is too dense at 150%. These are native host captures of shared widgets with a simulated safe area. They do not validate real Android system-bar and cutout insets, the Android keyboard, density reporting or physical touch input.

## Completeness

The completeness check derives the expected captures from the code. The full gallery expects every scene that has a card title in `tests/support/screenshot_gallery.h` and the three screens of each book in the international catalogue. The light gallery expects the `primaryScreens` list in `tests/desktop.cpp`, and a scene-filtered run expects the selected screens. Each expected screen needs light and dark captures at every viewport and 150% interface text at every e-reader viewport. Pages start at 1 without a gap, and light and dark have the same pages at each viewport; 150% text can add pages. A capture of an undeclared screen, viewport or variant fails the run, and a missing capture fails with its screen, viewport, variant and page. The check cannot see a paged screen that loses its last page in every variant, but the paging helper fails an inventory that does not complete its cycle. A case-filtered run and HTML-only regeneration apply the same rules to the screens that they contain.

Captures are sorted before the manifest is written, then checked for completeness, checksums, dimensions and density. Outputs replace the previous complete set only after every capture succeeds. The generator refuses to replace an unrelated directory. HTML-only regeneration verifies the raw bytes against the recorded SHA-256 and decodes each PNG before it replaces the page.

## Viewer

The gallery page shows one card per screen. Paginated captures share a card, and size and theme controls change the variant on the existing cards. It starts with six-inch light images at 300 PPI. The viewer shows device frames by default: a six-inch Kobo Clara and a seven-inch Kobo Libra. Frames change only the HTML presentation. The manifest keeps the individual files and variant metadata for automated comparisons: `scene` identifies the raw capture, and `screen` and `capture_page` group its card and pages. In the viewer, Left and Right move through the filtered captures, Space switches themes and Escape closes the viewer.

## Fixtures

The Library fixture has 37 invented books and authors with generated covers. It reads no personal library and needs no downloaded books. The export scenes simulate Nickel export support and show the isolated test profile path in the dialogs. Dictionary-management scenes use standalone host capabilities at every viewport, because that tab is hidden in Kobo device previews. Dictionary replies are controlled fixtures, not live downloads. The scanning capture resets and freezes one animation frame.

## Publication tour and screenshot books

The publication tour uses the original Wikisource chapter files for Arabic, Hebrew, Greek, Russian, Armenian, Georgian, Hindi, Bengali, Tamil, Thai, Vietnamese, German, Japanese, Korean, Simplified Chinese and Traditional Chinese. `tests/fixtures/public-domain/screenshot-catalog.json` pins the complete input files. C++ validates both file and chapter checksums before it packages deterministic EPUBs with the original text, source revisions, authors and attribution. Japanese, Korean and both Chinese editions also have vertical-writing variants. These apply writing-mode CSS and do not reproduce the source edition's layout.

`tests/fixtures/screenshot-books.json` pins the Gutenberg editions of Alice, Calculus Made Easy, Moby Dick and War and Peace. The full screenshot target prepares the first three with CMake in `build/screenshot-books/`, and the performance target prepares War and Peace there for its large-book scenario. Both verify cached bytes and reject changed downloads. Gutenberg regenerates its EPUB files, about once a month for Moby Dick and War and Peace, and a regenerated file has a new checksum even when its text is unchanged. Each reference therefore downloads the pinned bytes from an Internet Archive capture of the Gutenberg cache file. Its `archived_from` field names the captured URL. The captures are from 18 to 22 October 2026, and the captured files were generated from 1 to 13 October 2026. The 2 November 2026 regeneration of Moby Dick has the same text, markup, styles and cover, but new generated file names, identifiers and modification date. A changed download still fails preparation until its checksum is reviewed. The reviewed current mathematics and Moby Dick editions have new checksums; their beta-one checksums remain in the references for comparison. Mathematics captures the original equation passage. Moby Dick supplies the dense chapter graph. Embedded-font captures package the existing pinned Amstelvar fixture.

`tests/fixtures/perf-notes.epub` is a 2 KB synthetic book for the footnote performance scenario: one "long note" reference and the same sixty-paragraph note as the link fixture in `tests/support/book_fixtures.h`. `tests/fixtures/kobalt-benchmark.kepub.epub` is the Type Tester text with its own title, ★ Kobalt Benchmark, its own identifier and a generated cover that reads "Kobalt Benchmark". The performance scenarios use it, and the Kobo evaluation packages install it, so a device run never shares a book, reading position or KOSync document with the reader's own copy of Type Tester. Its SHA-256 checksum is in `platforms/eval/scenarios.json`. Ordinary native tests remain offline.

Firmware light, Bluetooth, native sync and operating-system dialogs and pickers stay outside the host gallery and need separate platform validation.

## Browser check

The browser regression check is `node tests/screenshot_gallery.mjs build/screenshots/mac/index.html` from the repository root. It needs an installed Playwright module and headless Chromium. Set `PLAYWRIGHT_MODULE` to an importable module name or file URL and `KOBALT_CHROMIUM` to an executable when they are not available through the normal installation. It does not run a device or open a visible browser.

## Reproducibility

The book fixture and clock are fixed. Screenshot profiles, including the light set, reload bookmarks with a fixed creation date before they capture the list and confirmation. Dictionary screenshots use a fixed 32 GB of free space instead of querying the development disk. Capture delivers pending layout and paint events before it renders the visible frame. Host Qt rendering can still affect pixels, so compare runs with the same environment. The manifest records image dimensions and SHA-256 checksums, book identity and checksum, application version, Qt version, source commit and modified status. Equal inputs should produce equal images. Compare manifests to detect changes, then inspect the affected images. No golden images are checked into the repository, and image equality does not establish functional correctness.
