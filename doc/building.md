# Building Kobalt

Application code lives in `src/` and `platforms/`. Shared assets live in root `assets/`. Pinned third-party source URLs, checksums, patches and CMake recipes live in `dependencies/`. Each platform owns its CMake project, presets and toolchain definitions. Generated files and incremental application objects stay under ignored `build/` in this worktree. Compiled third-party dependencies and their downloads live in the shared [dependency cache](#dependency-cache), which belongs to no worktree.

Install CMake 3.22 or later on the build host. The Kobo and Android launchers also use it to read source provenance before starting their prepared container.

Application builds do not use Python, compile Qt or read prepared libraries or assets from another checkout. The shared dependency cache is the only exception. Configuration selects the dependency set for the compiler and SDK. When the cache does not have that set yet, configuration downloads the selected third-party archives, verifies their SHA-256 checksums and compiles the libraries before it continues. Exported source bundles use their local `application/upstream/` archives instead. Missing or changed bundled archives fail configuration without a network fallback, also when the cache already has the set. For another local archive directory, set the CMake `KOBALT_SOURCE_ARCHIVES` path. Network access is required for uncached sources and Android Gradle packages.

## Developer launcher

Run `./start.sh` from the repository root to build and open the small Qt Widgets developer launcher on Linux or Mac. It needs only a native Qt 6.11+ Core/Gui/Widgets SDK, CMake, Ninja and the native compiler; it does not configure or compile the reader just to open the launcher. It reuses the desktop build’s selected Qt SDK when available. For initial SDK selection, use `./start.sh -DCMAKE_PREFIX_PATH=/path/to/qt`. Startup prints platform-specific setup instructions for missing tools, an outdated CMake or missing Qt. `./start.sh --check` performs the dependency and Qt configuration checks without building or opening the GUI. Docker readiness and the pinned Kobo SDK images are checked with bounded, read-only probes. Missing Docker, daemon/socket access or SDK images produce instructions while desktop actions remain available. Nothing is installed or pulled automatically. Ordinary repeated launches reuse the launcher’s incremental build under `build/launcher-linux/` or `build/launcher-mac/`.

The buttons configure and incrementally build the desktop app before opening it, build and verify the Qt 5 or Qt 6 Kobo package through the existing prepared-image script, build and run the full headless CTest suite, or build and watch the existing autonomous UI scenarios in a desktop window. The visible mode uses `kobalt-desktop-tests --watch` and `platforms/eval/scenarios.json`; it is not the full headless assertion suite and records no performance timings. It prepares the pinned large-book fixture when needed. Tests keep their own profiles and never drive the user’s running reader.

The launcher shows command output and exit status, runs one job at a time per worktree, and retains its latest job log under the launcher output directory. Stop or closing the launcher cancels its active process group and removes only that run’s temporary profiles. Kobo’s build script owns and removes its own container, including on interruption; unavailable cleanup retains its container ID for recovery. A desktop app opened by the launcher remains open independently. Kobo buttons require a running container engine and the prepared images documented below; the launcher does not install SDKs or pull images.

To validate the launcher without opening a window, run `cmake --preset linux`, `cmake --build --preset linux`, then `ctest --preset linux` from `platforms/launcher/`. Substitute `mac` on Mac. These checks cover action selection, command failure, cancellation, temporary-profile cleanup and Kobo container ownership.

## Mac

Use an installed native Qt 6.11 or newer Qt 6 SDK with Core, Gui, Widgets, Network, Test, GuiPrivate, Sql and its bundled libpng headers and archive. CMake 3.22 or later, Ninja, the Xcode command-line tools, make, patch and pkg-config must be available. The headless tests also use the system `zip` executable to create an offline EPUB fixture. The desktop project supports macOS and Linux.

Run these commands from `platforms/desktop/`:

```sh
cmake --preset mac -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.3/macos"
cmake --build --preset mac
```

The selected SDK stays in the CMake cache. Use a fresh output directory when changing SDKs, or clear cached Qt package paths with `'-UQt6*'` while selecting the new SDK. The build rejects mixed Qt component paths.

The app is `build/desktop-mac/Kobalt.app`. Open it manually for an interactive session. It starts in a normal 800 × 1136 window with the six-inch reader preset at 300 DPI. The text size picker sets the interface text size that Large Print sets on Kobo and the system font size sets on Android: 100%, 125% or 150%. Text grows and controls keep their size. The Library and the reader use the same size. A new size applies to the Library at once and to a book when it opens. An open book keeps its size until it closes. With either e-reader preset selected, the pointer is a 24-point translucent black circle throughout the Mac window, including its toolbar and the space around the device. It darkens while pressed. Fill window keeps the normal pointer. Building does not launch a window. Native Qt commands must run outside the agent sandbox on the development Mac, including headless builds.

A build from a linked Git worktree is a variant named after the worktree's folder. For example, the worktree `worktrees/library` builds Kobalt (library). A variant has its own bundle identifier and its own profile under `~/Library/Application Support/Kobalt/Kobalt-<variant>/`, so it runs beside the regular app and other variants without sharing settings, reading positions or the library. A variant does not register as an EPUB viewer, so Finder's Open With lists only the regular app. Open books in a variant from its library, with Command-O or by dropping a file onto the window. The configure step prints the app name and bundle identifier. Pass `-DKOBALT_MAC_VARIANT=name` to choose another name, or `-DKOBALT_MAC_VARIANT=` to build the regular app from a worktree. The main checkout builds the regular app.

### Kobo compatibility checks

Run `cmake --build --preset mac-kobo-checks` from `platforms/desktop/` to check both Kobo SDKs. This optional target uses the prepared Qt5 and Qt6 images, compiles changed production objects and links the mod library. It also builds the Kobo adapter tests and runs them under `qemu-arm` against the firmware Qt libraries of each image; see [Pewter](validation/kobo.md#pewter-the-fake-nickel). It skips installation ZIP creation and package checks. Ordinary Mac app and test builds do not run it. Workers can use it when their change needs Kobo checks. Run both targets before integration; the ordinary edit/build/test loop does not require them.

From the repository root, use `bash platforms/kobo/build.sh qt5 --check` or `qt6 --check` for one SDK. Set `KOBALT_CHECK_SOURCE_ROOT` only when using a newer check runner against another explicitly selected checkout. Checks reuse the selected checkout's incremental Kobo build directories and the shared dependency cache. A first check in a new worktree still compiles the application objects; later checks rebuild only stale inputs. The checks use the real SDKs and catch compile and link errors, including code hidden behind platform conditionals and in the test programs. The mod library and the tests share one compiled set of application objects. The tests check what the firmware adapters ask Nickel to do; they do not replace package or device validation. Missing Docker access or a prepared image fails the check without downloading or building an SDK.

### Kobo syntax check

The syntax check uses empty placeholders for generated `.moc` and `moc_*.cpp` includes. The SDK builds generate and compile the real meta-object code.

Run `cmake -P platforms/kobo/syntax.cmake` from the repository root to find Kobo compile errors on the Mac in seconds. It checks every source that the Kobo Qt5 and Qt6 builds compile, including the C files and the Kobo test programs. The Mac's clang reads each source in syntax-only mode against the headers of the prepared images. Each lane uses the flags, definitions and warnings of its Kobo build.

| Lane | Headers |
| --- | --- |
| `qt5` | Qt 5.2, the GCC 4.9 C++ library and the sysroot of the Qt5 image. |
| `qt6` | Qt 6.5, the GCC 11 C++ library and the glibc headers of the Qt6 image. |

The check finds missing C++17 library headers such as `<optional>` in GCC 4.9, Qt APIs that are newer than the SDK, Qt 6.5 deprecations such as the `QCryptographicHash::addData(const char *, qsizetype)` overload, `override` on a function that overrides nothing in the SDK, missing includes and `-Werror` warnings.

The default checks both lanes. To check one lane, put `-DSDK=qt5` or `-DSDK=qt6` before `-P`. Add `-DEVAL=ON` to also check the evaluation package of each selected lane: every source again with `KOBALT_SCENARIO_PLAYER` defined, and the scenario player. This doubles the work of a first run, so it is off by default.

The check is incremental. A later run checks only the sources whose files, headers or flags changed. To check only some sources, give their paths as arguments. Each selected lane that compiles a source checks it. A header argument checks all sources of the selected lanes again, because the recorded dependencies decide which sources include it. The Ninja file, response files and recorded dependencies stay in `build/kobo-syntax/` in each worktree.

The first run of a lane copies its SDK headers out of the prepared image into `sdk-headers/` in the [dependency cache](#dependency-cache), so all worktrees share one copy for each image. This needs a running container engine and the image from `platforms/sdk-image.sh`. Later runs do not use the container engine until that image pin or the copy recipe in the script changes. The check never pulls or builds an image.

On the development Mac, a first run of one lane, including the header copy, took 7 to 12 seconds. A first check of both lanes with copied headers took 15 seconds while other builds ran. A run with no changes takes less than a second.

The check reads Kobalt's generated headers and the dependency headers from the desktop build. Configure the `mac` preset first, as described in [Mac](#mac).

The check does not link. `mac-kobo-checks` remains the final check before integration.

### Headless tests and screenshots

Schedule a full native suite separately from other full suites and heavy builds on the same Mac, including work in other checkouts. Keep the preset's parallelism within that run. Test deadlines measure wall time, so competing runs can exhaust a test's budget without an assertion failure.

Run from `platforms/desktop/` after configuration:

```sh
cmake --build --preset mac-tests
ctest --preset mac
cmake --build --preset mac-screenshots
```

To build, run and record a full suite pass in one step, run `cmake -DPRESET=mac -P platforms/validation-record.cmake` from the repository root. The launcher's full-suite action, the release evaluation, the full gallery and Kobo packages record their own passes. A Claude Code hook refuses to repeat these runs on an unchanged tree. See [Validation guard](validation.md#validation-guard).

The Mac test presets run six tests at a time. Desktop checks that follow the gallery's screen sizes run one row per size, both themes and 150% interface text, not every combination. Set `KOBALT_TEST_ALL_VARIANTS=1` to run every combination; release approval requires this pass, and the release evaluation profile selects it. Validation records keep representative and all-variant passes separate. Gallery captures always use every combination.

For routine UI changes, use `cmake --build --preset mac-light-tests` and `ctest --preset mac-light`. Add `cmake --build --preset mac-light-screenshots` for the primary-view gallery in `build/screenshots/mac-light/`. The full commands above remain the release validation lane. Gallery workers run in isolated processes; set `KOBALT_GALLERY_JOBS` to control their parallelism. To capture only some gallery cards, run `KOBALT_GALLERY_SCENE=about cmake --build --preset mac-screenshots`; separate several names with commas. The images go to `build/screenshots/mac-scenes/`, and the full gallery stays as it is. Filtered captures include all current pages of each selected screen, even when a layout change adds or removes pages. See [Native validation](validation.md#everyday-commands) for coverage and limits. To review the captures screen by screen and collect notes, use [Screen review](screen-review.md).

To capture a new screenshot case before it has a full-gallery scene index, use `KOBALT_GALLERY_CASE=pressFeedback cmake --build --preset mac-screenshots`. The value is a comma-separated list of screenshot case names. The existing gallery workers run every device and theme row of those cases and write `build/screenshots/mac-cases/`. This selection cannot be combined with scene selection or the light gallery. It preserves the full gallery and uses the same capture, checksum and HTML generation path.

Performance measurements use their own RelWithDebInfo directory, `build/desktop-mac-perf`. Configure it once with `cmake --preset mac-perf -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.3/macos"`, then run `cmake --build --preset mac-perf`. `cmake --build --preset mac-eval` builds the comparison tool for [Release evaluation](release-evaluation.md), which records and compares tests, screenshots and timings.

The app and tests reuse the same compiled desktop implementation. They share one dependency build. Coordinate probes queue their temporary engine documents on the reader's engine worker, including construction and destruction, because CREngine shares fonts and caches between documents. The `mac` build preset builds the interactive app; `mac-tests` builds the desktop test executable and all restored standalone checks. The model checks link a static archive of existing portable core objects, so they include only the objects they use. CTest does not build the tests automatically. Configure with `-DBUILD_TESTING=OFF` for an app-only build without Qt Test or zip.

The tests always select Qt's windowless minimal platform. They check empty-library onboarding, imported-book covers, EPUB opening, rendered text, page turns, saved positions, reopening, opening errors and cancellation. Reading checks cover Fill window, the six-inch preview and the seven-inch preview in light and dark mode. General reading checks and screenshots use the checked-in public-domain Trevelyan's Type Tester v1.2, with its chapter-heading CSS fix. See [book fixtures](validation.md#book-fixtures) for provenance and checksum. Focused session and stylesheet regressions generate synthetic EPUBs offline in C++, with fixed ZIP timestamps. A diagnostic check captures native stdout and stderr to reject unexpected PNG errors and CREngine warnings during normal use. A separate storage check verifies that forced compaction does not report memory pressure and that actual pressure still does. Each case has an isolated profile. Profiles and generated fixture files stay under `build/desktop-mac/test-tmp/` and are removed when their run ends. The user's app and reading state are not used.

The screenshot preset runs the same application code headlessly with a fixed 09:00 clock. It saves PNGs under `build/screenshots/mac/`, covering the book cover, reading interface, panels, the library and important states in six-inch and seven-inch portrait, narrow phone and six-inch landscape sizes, in both themes and, for e-reader profiles, in the light theme with 150% interface text. Phone mode supports 100% interface text only. Every capture is a frameless native-resolution PNG at 300 PPI. Open `index.html` in that output directory to browse one card per screen, with readable titles and a six-inch light default. Screen-size and theme controls select the image variant without adding duplicate cards. Filter sections or search with multiple words. The page uses the same light colours, type, header, controls and favicon as [Screen review](screen-review.md). The sidebar lists the filtered screens by group, the same as in Screen review. Its button at the bottom opens it or folds it into a narrow strip. Select a name to go to its card, or a group heading to go to the group. While you scroll, the sidebar marks the first screen in view. Reset filters appears when a filter, size or variant differs from the defaults. Select an image to open the viewer. With the screenshot focused, Left and Right move through the filtered images and Space switches the same scene between themes. The 150% text control shows the light capture with larger interface text and holds the theme until it is turned off. Escape closes the viewer. Focused buttons, selectors and disclosure controls keep their normal keyboard behavior. The full-viewport viewer repeats device, theme, page, frame and Close controls at the top and bottom; on narrow screens only the top bar shows them. Its top bar also shows the position in the filtered screens with Previous and Next buttons, the same as the screen title in Screen review. The viewer has its own sidebar. In the viewer, the sidebar marks the current screen, and selecting a name shows that screen. The sidebar stays open or closed between visits. It defaults to the six-inch Kobo Clara frame. The seven-inch Kobo Libra frame includes its wider grip and page buttons. Both use the desktop preview proportions. The frame and screenshot fit together in the space between the controls, including in landscape and after resizing. Turning the frame off shows the same raw image. Capture details contain the filename, dimensions, checksum and a raw PNG download. The manifest and adjacent raw files remain available for automated comparisons. The HTML embeds every image and works without the adjacent PNGs. `manifest.json` lists image dimensions and SHA-256 checksums, Qt and application versions, source commit, modified status and fixture checksum. The full gallery also captures the original international books, CJK writing-mode variants, mathematics, embedded fonts and dense statistics. Its first run downloads the checksum-pinned Gutenberg editions into `build/screenshot-books/`; later runs verify and reuse them. Ordinary builds and tests do not download these books. A changed download fails before capture and leaves the last complete gallery in place. A failed capture leaves the last complete set in place. The generator refuses to replace a directory without its own manifest. Temporary capture directories are removed after success or failure. Inspect the generated images before judging visual correctness; these checks do not compare screenshots against golden images. See [Screenshots and gallery](validation/gallery.md) for the complete scene list and coverage.

To rebuild only the HTML controls from the existing manifest and PNGs, run `cmake --build --preset mac --target screenshot-gallery`. This does not recapture images, download books or change their recorded provenance. Before replacing the HTML, it validates every PNG checksum, decoded dimensions and density against the manifest. A rejected file leaves the previous gallery intact.

The model checks cover the library index, metadata, sorting and folder scanning, asset verification and recovery, the shared profile writer thread, standalone session history, bookmark and highlight storage, appearance settings, headers and footers, layout inspection and caching, resource caching, local resource access, ZIP validation, reading pace and speed, engine pause/resume, diagnostics, crash reports, dictionary installation, pixel damage and conversion, word counting, brightness gestures, refresh policy, plain archives, progress conversion, font-axis bounds and Kobo touch-rotation guards. They create synthetic inputs in C++ and require no external fixtures or downloads. The optional real-dictionary archive case is not part of this group. Run `ctest --preset mac -L model` for this group or `ctest --preset mac -L desktop` for the desktop checks. Each native test process gets a private working and temporary directory under `build/desktop-mac/test-tmp/`. The CMake launcher removes it after the process exits or reaches its process limit, including when a failed assertion bypasses C++ destructors. Fast checks have a 30-second process limit. Integration checks have a 120-second limit. The full screenshot tour has a 300-second limit for its additional publication and image work. Regression children have a 90-second limit within the integration process budget. CTest allows another five seconds for launcher cleanup. These are hang guards; explicit behavioral timeouts, including USB deadlines, remain unchanged.

The restored engine group checks layout, alignment, indentation, vertical text, MathML, line breaking, font fallbacks, SVG limits, whitespace, links, footnotes, Unicode search and word selection. It also preserves the original reader, publisher-markup, local-reader and document-word-count assertions with offline C++ fixtures. These check publisher and embedded fonts, real variable axes, Bionic rendering, colour and dark mode, source positions, document-cache recovery, local CSS and logical counts. The document-cache matrix runs separately to keep the reader check within its deadline. All these cases stay in the integration lane and reuse the compiled core archive. Component checks exercise production controls, footnote and dictionary panels, font parsing and registration, and acknowledgment/license paging. Desktop integration covers bookmark removal and merging after reflow, annotation creation, passage editing, recoloring, write-error recovery, settings apply and persistence, navigation preview/cancel/commit and bookmark returns across menu styles, rotation, resolution and page-button controls. Two-finger and right-click gesture checks preserve the original pointer assertions. The session group checks reading-book, bookmark, highlight and reader-session behavior with C++ EPUB fixtures. The resource integration group checks image limits and the resource-server protocol with local controlled servers. Use `ctest --preset mac-fast` for the everyday checks or `ctest --preset mac-integration` for renderer, session and broader desktop interactions. Build `mac-fast-tests` for only the fast lane, or `mac-tests` for the full suite. Neither lane downloads test books or rebuilds the SDK.

The separate reader lifecycle executable covers preview cancellation and failed apply, draft persistence through sleep and closing, orientation handoff, sleep images, stale searches, pending bookmarks, finish dialogs, progress conflicts, USB final-save recovery, complete annotation frames and toolbar invalidation. Run `ctest --preset mac -L lifecycle` for this group. It reuses production objects and does not add work to the fast build target or the fast checks. Controlled host services replace hardware callbacks; no device or firmware is needed. See [Native validation](validation.md#test-inventory) for the assertion inventory and exclusions.

The additional native integration groups reuse the same production objects. Run `ctest --preset mac -L '^ui$'` for shared canvas and paint checks, `-L platform` for portable orientation, font-source and Kobo-helper checks, or `-L corpus` for the representative literary corpus. With `KOBALT_DESKTOP_SENSORS=ON`, the orientation matrix uses the production 250 ms debounce and takes about 20 seconds; it is not part of the fast lane. No test probes device hardware. See [Native validation](validation.md) for fixture ownership and deliberate exclusions. Each test also has a feature-area label, such as `kosync` or `layout`, for a short run while you work on one area; see [Area checks](validation.md#area-checks).

## Linux

Use a prepared native Qt 6.11 or newer Qt 6 SDK with Core, Gui, Widgets, Network and Sql. The kobuild Linux image provides one; [GitHub builds](#github-builds-and-tagged-releases) describes how to run the suite in it. Headless checks also need Test, matching GuiPrivate headers, and the minimal platform plugin. Desktop builds use sensorless orientation by default. Set `-DKOBALT_DESKTOP_SENSORS=ON` with a matching Qt Sensors module to run the real sensor adapter and sensor-specific integration assertions. Manual rotation and sensorless lifecycle checks run in the default build. Kobo and Android keep real sensor support. Install CMake 3.22 or later, Ninja, a C/C++ compiler, make, patch, pkg-config, Git, zip, and libpng/zlib development packages. Never build Qt modules as part of the application build. Distribution Qt packages are supported when they meet these requirements; keep all Qt components from the same SDK.

Run from `platforms/desktop/`:

```sh
cmake --preset linux
cmake --build --preset linux
cmake --build --preset linux-light-tests
ctest --preset linux-light
cmake --build --preset linux-light-screenshots
```

Pass `-DCMAKE_PREFIX_PATH=/path/to/qt` when using a separate SDK. The executable is `build/desktop-linux/Kobalt`; verified runtime assets are under `build/desktop-linux/assets/runtime/`. Building does not open a window. Linked-worktree builds have an isolated profile named after the worktree folder. Override it with `KOBALT_LINUX_VARIANT`, or use an empty value for the regular profile. Qt selects the platform application-data directory.

Linux provides the same light, fast, integration and full test lanes, performance and evaluation tools, and screenshot galleries as Mac. Substitute `linux` for `mac` in desktop preset commands. Full validation uses `cmake --build --preset linux-tests` then `ctest --preset linux`; full captures use `linux-screenshots`. Captures go to `build/screenshots/linux/` and `build/screenshots/linux-light/`. Tests use Fusion and the minimal platform plugin without X11, Wayland or a visible window. Each platform owns its output and temporary files. Compare screenshots with the same host and Qt SDK. GUI operation under X11/Wayland requires separate validation.

## Assets

`assets/manifest.json` records the runtime file selection, version, sizes and SHA-256 checksums. Font metadata records upstream sources and revisions. The selected eBook font collection is checked in under `assets/fonts/ebook-core/`; builds do not fetch fonts. Keep the manifest consistent when changing runtime assets. All platforms include bundled hyphenation pattern names and contents in the cache policy. Pattern changes rebuild disposable book caches without changing saved reading data.

Each platform uses `assets/CMakeLists.txt` to verify and stage the selected files under its own output directory. CMake tracks these inputs and removes retired files from the staging directory. The Mac app copies the runtime bundle into its Resources directory; Kobo packages it under `.adds/kobalt/assets`; Android embeds it as Qt resources. After verifying its current extracted bundle, Android removes old digest-named bundle directories. Failed installation keeps old bundles. Cleanup preserves unrelated entries and does not follow sibling symlinks. Artwork, notices and the two Styling sample HTML files are compiled from root `assets/`. Source metadata and unused font files are not copied into the runtime bundle.

To regenerate the US English whole-word exceptions, run `cmake -P assets/convert-english-exceptions.cmake` from the repository root. It uses the checked-in upstream corpus under `tests/fixtures/hyphenation/` and preserves the general patterns and exceptions outside that corpus. Update the dictionary's size and SHA-256 in `assets/manifest.json` after a change. The `engine.publisher-markup` check compares the compiled engine's permitted breaks with all 1,753 corpus entries, including words with no permitted break. It also keeps the existing `seagull` result unchanged.

CMake generates the English string table from `assets/en.strings` and rejects missing keys in literal `strings::get` and `strings::key` calls under `src/`. Ordinary builds reconfigure when that file changes, so edits appear without updating a separate snapshot. The icon geometry in `src/ui/controls/reader_icon_vectors.h` is hand-kept C++ outline data based on the Phosphor SVGs. Editing an SVG does not update it, and its maintenance workflow has not been restored. The dictionary catalogue for downloads comes from `assets/dictionaries.json`, and the page style and search script of annotation exports come from `assets/export/`. CMake embeds them when it configures a build. Ordinary builds reconfigure after an edit to these files.

## Kobo

Kobo uses Nickel's library, so its target excludes the standalone Library model and interface. Shared controls use the C++ library APIs available in the prepared firmware SDK, including the older Qt5 target headers.

Page animations require a working display hook and a recognized hwtcon or mxcfb backend. hwtcon has normal setting text; mxcfb has a compatibility warning. The `-DKOBALT_PAGE_ANIMATIONS=OFF` build option keeps the setting unavailable on either backend. See [reader refresh behavior](reader/display.md) and the focused animation checks in [Display refresh checks](validation/display.md).

The Kobo target selects the suspend-inclusive boot clock for reading duration only. Android and desktop keep the Qt elapsed clock. This selection adds no timer or scheduled wakeup.

The `qt5` and `qt6` presets consume prepared kobuild SDKs, including Sensors. Compiler, sysroot and Qt repairs belong in kobuild. Pull the selected SDK images before building:

```sh
docker pull ghcr.io/nicoverbruggen/kobuild:qt5-v0.5.0
docker pull ghcr.io/nicoverbruggen/kobuild:qt6-v0.5.0
bash platforms/kobo/build.sh qt5
bash platforms/kobo/build.sh qt6
```

The launcher pins these images by digest. It mounts this worktree and the [dependency cache](#dependency-cache), configures CMake, which builds missing reader dependencies, and builds the application and installation ZIP. It does not pull or repair SDKs. Configure and build/package durations are printed separately. Parallelism defaults to two; set `JOBS` to change it.

Direct Kobo builds enable verbose diagnostics and produce `Kobalt-qtN-diagnostic.zip` under `build/kobo-qt5/` or `build/kobo-qt6/`, alongside `libkobalt.so` with debug symbols. Tagged CI release builds, including beta releases, set `KOBALT_VERBOSE_DIAGNOSTICS=OFF` and produce `Kobalt-qtN.zip`. Branch builds and nightly/interim tags enable verbose diagnostics. CI keeps the downloaded artifact names `Kobalt-qtN.zip` for both modes. The ZIP contains a stripped installation library and runtime assets. To reproduce release logging locally, run `KOBALT_VERBOSE_DIAGNOSTICS=OFF bash platforms/kobo/build.sh qt5` or use `qt6`. The launcher passes the logging choice on every configure, including when reusing an existing build directory. Qt5 uses `.kobo/KoboRoot.tgz`; Qt6 uses `.kobo/Kobo.tgz`. Packaging removes its staging directory and does no work when its inputs are unchanged. Both tar formats store only the library on the system partition, with numeric owner `0:0` and mode `0755`, regardless of the build user. The build launcher runs the `check-package` target after packaging to verify the member path, ownership and permissions. The same check can be run in the prepared SDK with `cmake --build --preset qt5 --target check-package` or the Qt6 preset.

The build container has a CPU limit, so other work on the machine keeps its speed. The default is half of the host's CPUs, at least two and at most the CPUs of the container engine. Set `KOBALT_DOCKER_CPUS` to use another number of CPUs, for example `4` or `1.5`. A Kobo build also registers with the evaluation lock. While a performance evaluation runs in any worktree, a new Kobo build prints a message and waits for it. The `mac-kobo-checks` target uses the same script, so the same limit and lock apply. See [Machine load](release-evaluation.md#machine-load).

`bash platforms/kobo/build.sh qt5-eval` and `qt6-eval` build evaluation packages in `build/kobo-qt5-eval/` and `build/kobo-qt6-eval/`. They add the scenario player, the scenario file, the benchmark book and the diagnostics switch; see [Release evaluation](release-evaluation.md#device-scenarios).

### Page-turn diagnostic package

Run `bash platforms/kobo/build.sh qt5-diagnostic` for firmware 4, or `qt6-diagnostic` for firmware 5 and 6. These use the regular reader with verbose diagnostics compiled on. They do not include the evaluation player or test books. Outputs stay in `build/kobo-qt5-diagnostic/` or `build/kobo-qt6-diagnostic/`; package names end in `-diagnostic.zip`. Set `KOBALT_RELEASE_VERSION` to give a test package a distinct displayed version. CI release packages keep verbose logging off unless the profile contains `.adds/kobalt/diagnostics-enabled` or `KOBALT_DIAGNOSTICS=1` is set at startup. See [page-turn diagnostics](validation/diagnostics.md#page-turn-diagnostics) for event meanings and limits.

## Android

The Android project builds a signed debug APK through Qt's standard CMake `apk` target. It uses the prepared multi-architecture kobuild image; Docker selects its native host variant. Both variants target Android `arm64-v8a`.

```sh
docker pull ghcr.io/nicoverbruggen/kobuild:android-v0.5.0
bash platforms/android/build.sh
```

The launcher pins the released image by digest. Set `KOBALT_ANDROID_IMAGE` only to select another prepared image for SDK validation. Host architectures use separate `build/android-amd64/` and `build/android-arm64/` outputs, so Qt host-tool caches cannot cross between them. Gradle downloads stay under `build/gradle/`. The launcher rejects concurrent use of one Android output and removes its container when finished. Compiler parallelism defaults to two; set `JOBS` to change it.

The APK is `build/android-HOST/android-build/build/outputs/apk/debug/android-build-debug.apk`. It appears as **Kobalt Dev** in the Android launcher; release APKs appear as **Kobalt**. It uses `com.nicoverbruggen.kobalt.debug`, the public debug key under `platforms/android/app/` and a version ending in `-debug`. Android 9 or later and ARM64 are required. Release signing uses the explicit mode described below. BOOX selects Qt's SurfaceView container before creating windows so partial refresh metadata can reach the submitted buffers. Full cleaning refreshes use BOOX's `View.refreshScreen` API on a worker after a 250 ms settling delay, with full GC16 and wait flags. Before cleaning, Kobalt attempts to suppress competing system refreshes with `View.setWaveformAndScheme`, as KOReader does. Pending cleaning requests coalesce and are cancelled when the activity loses focus or pauses. `KobaltBOOX` logs distinguish queued requests, firmware calls and failures; a successful call does not prove a physical cleaning flash. Other Android devices keep Qt's default container. Library, book-opening and reader canvases share the BOOX frame transport. Returning to the Library retains a captured reader frame until its final system-bar insets are ready, then requests GC cleanup. The Android window follows the activity's laid-out bounds at startup and after rotation. Only the Library canvas draws rules below Android's status bar and above its navigation controls; the fullscreen reader has neither rule.

## Code formatting

First-party C and C++ code uses the Qt project style in `.clang-format`. Format changed files with clang-format 21 before you commit, for example `xcrun clang-format -i FILE` on the Mac. A newer major version can format untouched lines differently. The style keeps string literals and comment text as written. `.clang-format-ignore` excludes `dependencies/`, the vendored NickelHook files and the hand-kept icon geometry. `.git-blame-ignore-revs` lists whole-tree formatting commits. Run `git config blame.ignoreRevsFile .git-blame-ignore-revs` once, and `git blame` skips them.

## Dependencies and validation

CREngine and its text/image libraries use the versions and patches recorded in this worktree. Android also builds OpenSSL and downloads the pinned BOOX compatibility JAR. These application dependencies are separate from Qt SDK production. A change to their recipes, patches or source pins selects a new dependency set; application-only changes reuse it.

### Dependency cache

All worktrees of a repository share compiled dependencies. The cache is `build/dependency-cache/` in the main checkout. Configuration finds it through Git's common directory, so every linked worktree uses the same folder. A tree without Git, such as an extracted source bundle, uses its own `build/dependency-cache/`. To use another folder, set `KOBALT_DEPENDENCY_CACHE` to an absolute path, as a CMake variable or in the environment. The environment value applies when a build directory is first configured.

| Path | Contents |
| --- | --- |
| `downloads/<name>-<SHA-256>.tar` | Verified source archives, shared by all platforms. |
| `<key>/` | One dependency set: patched sources, library build trees, the installed libraries in `install/` and the CREngine archive in `crengine/`. |
| `<key>/complete` | The publication marker. A set without it is unfinished and is never used. |
| `<key>.lock` | The lock held while the set builds. |
| `sdk-headers/<sdk>-<key>/` | The Kobo SDK headers of the [syntax check](#kobo-syntax-check). The key is a hash of the pinned image digest and the copy recipe. |

The key is a hash of the compiler, compiler flags, system, Qt SDK, toolchain contents, prepared SDK image, libpng choice, source pins, recipes and patches. It does not contain the checkout path. Worktrees with the same pins and patches share one set. A change to a pin, recipe or patch selects a new key, and other worktrees keep their set. The Mac `mac` and `mac-perf` builds share one set, and so do each Kobo SDK's regular, evaluation and diagnostic builds. Application objects stay in each worktree.

When a set is missing, configuration takes its lock, builds it in place and then publishes the `complete` marker by rename. Another configuration that needs the same set waits for the lock and then uses the result. A failed or interrupted build never writes the marker. The next attempt continues it when it comes from the same checkout, and starts again otherwise. A published set is never changed. A cache hit never skips a checksum check: the key contains the pinned checksums, a set is built only from verified archives, a cached download is verified again on every use and bundled archives are checked on every configuration. On the development Mac with two dependency jobs, a second worktree configured in 2 seconds for the Mac and in 1 second for each Kobo SDK. Building a missing set took one to one and a half minutes, depending on download speed.

Do not move or rename the cache. Installed CMake and pkg-config files contain its absolute path. To use another location, set `KOBALT_DEPENDENCY_CACHE` and let configuration build the sets there. Delete a `<key>/` folder that no branch uses to free space, but only while no build runs. An interrupted download leaves a `*.partial` file in `downloads/`; it is never used and can be deleted at the same time. Never edit files inside a published set.

The syntax check copies SDK headers into a `*.partial` folder in `sdk-headers/` and then renames it into place. A concurrent run sees a whole copy or none. When two runs copy the same headers at the same time, the first rename wins and the other run removes its copy. An interrupted copy leaves its `*.partial` folder; it is never used. Delete it, or an `sdk-headers/` entry that no branch uses, only while no syntax check runs.

The Kobo and Android launchers create the cache on the host, so it belongs to the host user. They mount it at the fixed container path `/dependency-cache`, so all container builds store the same paths. Container builds lock each other out. File locks do not reach across the bind mount between the Mac and a container, measured with OrbStack. The Mac and a container never build the same key, because the compiler and system are part of it. They share only `downloads/`, where each archive is downloaded under a temporary name, verified and then renamed into place without a lock.

Release evaluation exports build their dependencies in their own cache; see [Release evaluation](release-evaluation.md). The fast check `build.dependencyCache` covers the cache location, locking, publication, failure handling and archive verification.

Stop and reap run-owned helper processes and remove temporary profiles, staging directories and containers after each run. Keep current outputs, useful measurements and incremental caches. Native desktop, model, renderer and session checks cover the paths listed above. [Native validation](validation.md#feature-coverage-and-limits) records the remaining corpus, gesture and platform gaps. These checks do not establish firmware compatibility or physical-device display behavior.

## GitHub builds and tagged releases

`.github/workflows/ci.yml` builds Qt5, Qt6 and a signed Android release APK on branch pushes and tags. Helper checks validate version inputs, source pins and missing-key failures. Before the package builds, the `native-tests` job builds and runs the full native suite on Linux with `platforms/ci/native-tests.sh`, in the pinned kobuild Linux image `linux-v0.5.0`. Branch runs use representative variants. Tagged runs, including a manual run on a tag, require all variants in the same job. The Kobo and Android jobs wait for it. Each Kobo job then runs the [Kobo adapter tests](validation/kobo.md#pewter-the-fake-nickel) before it builds its package, and checks the system archive member, numeric root ownership and executable permissions. Desktop application builds and screenshot galleries run locally through the Mac or Linux presets; they are not CI jobs.

To run the CI suite locally in the same image, run `bash platforms/ci/native-tests.sh` from the repository root. It pulls the pinned image. Set `KOBALT_LINUX_IMAGE` to use another prepared image without pulling, such as a local kobuild build, and `JOBS` to set the parallelism. Outside GitHub, set `KOBALT_TEST_ALL_VARIANTS=1` to measure the tagged-release coverage locally. Outputs go to `build/desktop-linux/` and dependencies to the shared dependency cache.

A tag such as `v1.0-beta.4` supplies the displayed version and Android version name. Package filenames stay fixed across releases so download links remain compatible: `Kobalt-qt5.zip`, `Kobalt-qt6.zip`, `Kobalt-android-arm64.apk` and `Kobalt-sources.tar.gz`. Branch builds use the version in `assets/manifest.json`. CI checks that a triggering tag identifies the checked-out commit. Local builds can set `KOBALT_RELEASE_VERSION` to test the same path without editing the tracked asset manifest.

Add these repository secrets under GitHub Settings → Secrets and variables → Actions:

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | Base64 encoding of the release keystore file. |
| `ANDROID_KEYSTORE_PASSWORD` | Keystore password. |
| `ANDROID_KEY_ALIAS` | Signing-key alias in that keystore. |
| `ANDROID_KEY_PASSWORD` | Password for that signing key. |

Use repository secrets for this workflow. Environment secrets are available only to jobs that select that environment; the Android job does not select one. Variables hold configuration that does not need secrecy. The certificate fingerprint is public information, so it belongs in a repository variable.

Existing PKCS#12 (`.p12`) keystores are supported. Use the same signing key for future updates. For a P12 with one shared password, set both password secrets to that password. The key alias identifies the entry inside the keystore; it is not the application ID. The release application ID is already `com.nicoverbruggen.kobalt`.

On macOS, install Java if needed and make its tools available in the current terminal:

```sh
brew install openjdk@21
export PATH="$(brew --prefix openjdk@21)/bin:$PATH"
```

From the folder containing the keystore, inspect its alias and certificate. Replace the example filename and alias with your own. `keytool` prompts for the password; do not put it on the command line.

```sh
keytool -list -v -storetype PKCS12 -keystore release.p12
keytool -list -v -storetype PKCS12 -keystore release.p12 -alias YOUR_ALIAS
```

Copy the encoded P12 to the clipboard, then paste it into the `ANDROID_KEYSTORE_BASE64` repository secret:

```sh
base64 < release.p12 | tr -d '\n' | pbcopy
```

In the certificate output, use `SHA256`, not `SHA1`. Create `ANDROID_CERT_SHA256` under Actions → Variables → New repository variable. Its value must be 64 hexadecimal characters without colons or the `SHA256:` label. This command copies the fingerprint without colons:

```sh
keytool -list -v -storetype PKCS12 -keystore release.p12 -alias YOUR_ALIAS |
  awk '/SHA256:/ {gsub(/:/, "", $2); printf "%s", $2}' |
  pbcopy
```

Never put passwords or the encoded keystore in tracked files, build arguments or workflow logs. GitHub can report secret names but cannot return their values. A successful signing run verifies the actual keystore, passwords, alias and certificate match; the presence of the settings alone does not.

The workflow uses the prepared SDK's `zipalign` and `apksigner`. It verifies the signature, compares the certificate with `ANDROID_CERT_SHA256` and checks 16 KiB alignment before retaining an APK. Missing secrets or a mismatched certificate fail the job. Release builds use `com.nicoverbruggen.kobalt` and have no `-debug` suffix. CI uses its increasing run number as the Android version code. Keep it higher than any previously distributed version code for the same application ID and key. Changing the key prevents normal updates to existing installations.

For a local release build, set `KOBALT_ANDROID_RELEASE=1`, `KOBALT_RELEASE_VERSION`, `KOBALT_ANDROID_VERSION_CODE`, `KOBALT_ANDROID_KEYSTORE`, `KOBALT_ANDROID_KEY_ALIAS`, `KOBALT_ANDROID_KEYSTORE_PASSWORD`, `KOBALT_ANDROID_KEY_PASSWORD` and `KOBALT_ANDROID_CERT_SHA256`, then run `bash platforms/android/build.sh`. The keystore is mounted read-only. Passwords pass through the environment. The output is `build/android-HOST/Kobalt-android-arm64.apk`, with a certificate verification report beside it. Ordinary local builds keep the signed debug APK described above.

`platforms/ci/source-archive.sh` archives tracked application sources at `HEAD`, pinned application dependency archives, font sources and the exact Qtbase and QtSensors sources used by the Android SDK. The bundle records exact prepared SDK image digests and links to the corresponding `kobuild` recipe commits and upstream source pins. Compiler and tool sources remain referenced through `kobuild`; `dependencies/android-runtime.json` records the LLVM runtime source and recipe revisions. `dependencies/release-sources.json` adds release-source inputs to the application dependency manifest. Each downloaded or cached input must match its SHA-256. Ignored files, local profiles, signing keys and build outputs are excluded. Commit release changes before using this archive for a release, because uncommitted edits are not included.

Extract the archive and run the documented build commands from its `application/` folder. `source-info.json` retains the source commit and release version without a Git checkout. An explicit `KOBALT_RELEASE_VERSION` still overrides that version. The `upstream/` folder is inside the application tree so container builds can use it through their normal mount. Install the prepared SDK or pull its pinned image first; the bundle does not include a compiled SDK or Android Gradle packages. Run `bash platforms/ci/check-source-archive.sh` to verify a generated archive's Qt6 configuration with networking disabled. CI runs this check before retaining the source artifact.

Android uses dynamically linked Qt libraries. To use a modified compatible Qt SDK, prepare it through `kobuild`, then rebuild Kobalt with that SDK and sign the APK with your own key. A different key cannot update an official installation. Use the separate debug application ID for testing, or back up app data before removing the official installation.

Successful tag builds assemble a draft GitHub release with the three packages, source archive, build metadata and `SHA256SUMS`. They do not publish it. An existing published release cannot be overwritten. Review the draft and its signing report before publishing. Branch builds retain workflow artifacts only.

## Widget keyboard

Desktop hosts use `ui::makeWidgetKeyboard` from `src/ui/controls/keyboard.h`. It implements the existing `KeyboardFactory` interface. The supplied parent determines placement, the supplied editor receives text, and the controller owns its panel. It selects a Latin layout from the host's default Qt locale: German uses QWERTZ, French uses AZERTY except in Canada, and Dutch and other languages use QWERTY. It provides printable ASCII symbols, Space, Delete and submission. Tap Shift for one uppercase letter or accented character. Double-tap Shift to enable Caps Lock; its arrow gains a bar underneath. Tap Shift again to return to lowercase. Space consumes one-shot Shift. Delete and the symbol page preserve the letter case. A new editor starts in lowercase. Hold a Latin letter to choose an accented variant or a ligature. Shift also applies to these choices. Other writing systems and input methods are not implemented. `setSubmitText` changes the submit label. Android keeps its native system keyboard. Other adapters select this factory through their host services. The panel follows the reader theme and publishes its bounds so dictionary and account editors stay above it. Kobo uses the native keyboard adapter in `platforms/kobo/common/native_keyboard.h` and the selected Nickel adapter. It retains Nickel’s key layout, input receiver and language switching. The optional key-repaint hook keeps its rendering compatible with the reader theme.
