# Building Kobalt

Application code lives in `src/` and `platforms/`. Shared assets live in root `assets/`. Pinned third-party source URLs, checksums, patches and CMake recipes live in `dependencies/`. Each platform owns its CMake project, presets and toolchain definitions. Generated files, downloaded sources and incremental objects stay under ignored `build/` in this worktree.

Install CMake 3.22 or later on the build host. The Kobo and Android launchers also use it to read source provenance before starting their prepared container.

Application builds do not use Python, compile Qt or read prepared libraries or assets from another checkout. The first configuration downloads the selected third-party archives and verifies their SHA-256 checksums. Exported source bundles use their local `application/upstream/` archives instead. Missing or changed bundled archives fail configuration without a network fallback. For another local archive directory, set the CMake `KOBALT_SOURCE_ARCHIVES` path. The build compiles those libraries before compiling Kobalt. Later builds reuse the configured sources and objects. Network access is required for uncached sources and Android Gradle packages.

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

### Headless tests and screenshots

Schedule a full native suite separately from other full suites and heavy builds on the same Mac, including work in other checkouts. Keep the preset's parallelism within that run. Test deadlines measure wall time, so competing runs can exhaust a test's budget without an assertion failure.

Run from `platforms/desktop/` after configuration:

```sh
cmake --build --preset mac-tests
ctest --preset mac
cmake --build --preset mac-screenshots
```

For routine UI changes, use `cmake --build --preset mac-light-tests` and `ctest --preset mac-light`. Add `cmake --build --preset mac-light-screenshots` for the primary-view gallery in `build/screenshots/mac-light/`. The full commands above remain the release validation lane. Gallery workers run in isolated processes; set `KOBALT_GALLERY_JOBS` to control their parallelism. To capture only some gallery cards, run `KOBALT_GALLERY_SCENE=about cmake --build --preset mac-screenshots`; separate several names with commas. The images go to `build/screenshots/mac-scenes/`, and the full gallery stays as it is. See [Native validation](validation.md#everyday-commands) for coverage and limits.

Performance measurements use their own RelWithDebInfo directory, `build/desktop-mac-perf`. Configure it once with `cmake --preset mac-perf -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.3/macos"`, then run `cmake --build --preset mac-perf`. `cmake --build --preset mac-eval` builds the comparison tool for [Release evaluation](release-evaluation.md), which records and compares tests, screenshots and timings.

The app and tests reuse the same compiled desktop implementation. They share one dependency build. Coordinate probes queue their temporary models on the reader's engine worker, including construction and destruction, because CREngine shares fonts and caches between models. The `mac` build preset builds the interactive app; `mac-tests` builds the desktop test executable and all restored standalone checks. The model checks link a static archive of existing portable core objects, so they include only the objects they use. CTest does not build the tests automatically. Configure with `-DBUILD_TESTING=OFF` for an app-only build without Qt Test or zip.

The tests always select Qt's windowless minimal platform. They check empty-library onboarding, imported-book covers, EPUB opening, rendered text, page turns, saved positions, reopening, opening errors and cancellation. Reading checks cover Fill window, the six-inch preview and the seven-inch preview in light and dark mode. General reading checks and screenshots use the checked-in public-domain Trevelyan's Type Tester v1.2, with its chapter-heading CSS fix. See [book fixtures](validation.md#book-fixtures) for provenance and checksum. Focused session and stylesheet regressions generate synthetic EPUBs offline in C++, with fixed ZIP timestamps. A diagnostic check captures native stdout and stderr to reject unexpected PNG errors and CREngine warnings during normal use. A separate storage check verifies that forced compaction does not report memory pressure and that actual pressure still does. Each case has an isolated profile. Profiles and generated fixture files stay under `build/desktop-mac/test-tmp/` and are removed when their run ends. The user's app and reading state are not used.

The screenshot preset runs the same application code headlessly with a fixed 09:00 clock. It saves PNGs under `build/screenshots/mac/`, covering the book cover, reading interface, panels, the library and important states in both six-inch and seven-inch sizes, in both themes and in the light theme with 150% interface text. Every capture is a frameless native-resolution PNG at 300 PPI. Open `index.html` in that output directory to browse one card per screen, with readable titles and a six-inch light default. Screen-size and theme controls select the image variant without adding duplicate cards. Filter sections or search with multiple words. Select an image to open the viewer. With the screenshot focused, Left and Right move through the filtered images and Space switches the same scene between themes. The 150% text control shows the light capture with larger interface text and holds the theme until it is turned off. Escape closes the viewer. Focused buttons, selectors and disclosure controls keep their normal keyboard behavior. The full-viewport viewer repeats device, theme, page, frame and Close controls at the top and bottom. It defaults to the six-inch Kobo Clara frame. The seven-inch Kobo Libra frame includes its wider grip and page buttons. Both use the desktop preview proportions. The frame and screenshot fit together in the space between the controls, including in landscape and after resizing. Turning the frame off shows the same raw image. Capture details contain the filename, dimensions, checksum and a raw PNG download. The manifest and adjacent raw files remain available for automated comparisons. The HTML embeds every image and works without the adjacent PNGs. `manifest.json` lists image dimensions and SHA-256 checksums, Qt and application versions, source commit, modified status and fixture checksum. The full gallery also captures the original international books, CJK writing-mode variants, mathematics, embedded fonts and dense statistics. Its first run downloads the checksum-pinned Gutenberg editions into `build/screenshot-books/`; later runs verify and reuse them. Ordinary builds and tests do not download these books. A changed download fails before capture and leaves the last complete gallery in place. A failed capture leaves the last complete set in place. The generator refuses to replace a directory without its own manifest. Temporary capture directories are removed after success or failure. Inspect the generated images before judging visual correctness; these checks do not compare screenshots against golden images. See [Native validation](validation.md) for the complete scene list and coverage.

To rebuild only the HTML controls from the existing manifest and PNGs, run `cmake --build --preset mac --target screenshot-gallery`. This does not recapture images, download books or change their recorded provenance. Before replacing the HTML, it validates every PNG checksum, decoded dimensions and density against the manifest. A rejected file leaves the previous gallery intact.

The model checks cover the library index, metadata, sorting and folder scanning, asset verification and recovery, the shared profile writer thread, standalone session history, bookmark and highlight storage, appearance settings, headers and footers, layout inspection and caching, resource caching, local resource access, ZIP validation, reading pace and speed, engine pause/resume, diagnostics, crash reports, dictionary installation, pixel damage and conversion, word counting, brightness gestures, refresh policy, plain archives, progress conversion, font-axis bounds and Kobo touch-rotation guards. They create synthetic inputs in C++ and require no external fixtures or downloads. The optional real-dictionary archive case is not part of this group. Run `ctest --preset mac -L model` for this group or `ctest --preset mac -L desktop` for the desktop checks. Each native test process gets a private working and temporary directory under `build/desktop-mac/test-tmp/`. The CMake launcher removes it after the process exits or reaches its process limit, including when a failed assertion bypasses C++ destructors. Fast checks have a 30-second process limit. Integration checks have a 120-second limit. The full screenshot tour has a 300-second limit for its additional publication and image work. Regression children have a 90-second limit within the integration process budget. CTest allows another five seconds for launcher cleanup. These are hang guards; explicit behavioral timeouts, including USB deadlines, remain unchanged.

The restored engine group checks layout, alignment, indentation, vertical text, MathML, line breaking, font fallbacks, SVG limits, whitespace, links, footnotes, Unicode search and word selection. It also preserves the original reader, publisher-markup, local-reader and document-word-count assertions with offline C++ fixtures. These check publisher and embedded fonts, real variable axes, Bionic rendering, colour and dark mode, source positions, document-cache recovery, local CSS and logical counts. The document-cache matrix runs separately to keep the reader check within its deadline. All these cases stay in the integration lane and reuse the compiled core archive. Component checks exercise production controls, footnote and dictionary panels, font parsing and registration, and acknowledgment/license paging. Desktop integration covers bookmark removal and merging after reflow, annotation creation, passage editing, recoloring, write-error recovery, settings apply and persistence, navigation preview/cancel/commit and bookmark returns across menu styles, rotation, resolution and page-button controls. Two-finger and right-click gesture checks preserve the original pointer assertions. The session group checks reading-book, bookmark, highlight and reader-session behavior with C++ EPUB fixtures. The resource integration group checks image limits and the resource-server protocol with local controlled servers. Use `ctest --preset mac-fast` for the everyday checks or `ctest --preset mac-integration` for renderer, session and broader desktop interactions. Build `mac-fast-tests` for only the fast lane, or `mac-tests` for the full suite. Neither lane downloads test books or rebuilds the SDK.

The separate reader lifecycle executable covers preview cancellation and failed apply, draft persistence through sleep and closing, orientation handoff, sleep images, stale searches, pending bookmarks, finish dialogs, progress conflicts, USB final-save recovery, complete annotation frames and toolbar invalidation. Run `ctest --preset mac -L lifecycle` for this group. It reuses production objects and does not add work to the fast build target or the fast checks. Controlled host services replace hardware callbacks; no device or firmware is needed. See [Native validation](validation.md#test-inventory) for the assertion inventory and exclusions.

The additional native integration groups reuse the same production objects. Run `ctest --preset mac -L ui` for shared canvas and paint checks, `-L platform` for portable orientation, font-source and Kobo-helper checks, or `-L corpus` for the representative literary corpus. With `KOBALT_DESKTOP_SENSORS=ON`, the orientation matrix uses the production 250 ms debounce and takes about 20 seconds; it is not part of the fast lane. No test probes device hardware. See [Native validation](validation.md) for fixture ownership and deliberate exclusions.

## Linux

Use a prepared native Qt 6.11 or newer Qt 6 SDK with Core, Gui, Widgets, Network and Sql. Headless checks also need Test, matching GuiPrivate headers, and the minimal platform plugin. Desktop builds use sensorless orientation by default. Set `-DKOBALT_DESKTOP_SENSORS=ON` with a matching Qt Sensors module to run the real sensor adapter and sensor-specific integration assertions. Manual rotation and sensorless lifecycle checks run in the default build. Kobo and Android keep real sensor support. Install CMake 3.22 or later, Ninja, a C/C++ compiler, make, patch, pkg-config, Git, zip, and libpng/zlib development packages. Never build Qt modules as part of the application build. Distribution Qt packages are supported when they meet these requirements; keep all Qt components from the same SDK.

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

CMake generates the English string table from `assets/en.strings` and rejects missing keys in literal `strings::get` and `strings::key` calls under `src/`. Ordinary builds reconfigure when that file changes, so edits appear without updating a separate snapshot. Icon geometry and the dictionary catalogue still use checked-in C++ snapshots. Their maintenance workflow has not been restored.

## Kobo

Kobo uses Nickel's library, so its target excludes the standalone Library model and interface. Shared controls use the C++ library APIs available in the prepared firmware SDK, including the older Qt5 target headers.

The Kobo target selects the suspend-inclusive boot clock for reading duration only. Android and desktop keep the Qt elapsed clock. This selection adds no timer or scheduled wakeup.

The `qt5` and `qt6` presets consume prepared kobuild SDKs, including Sensors. Compiler, sysroot and Qt repairs belong in kobuild. Pull the selected SDK images before building:

```sh
docker pull ghcr.io/nicoverbruggen/kobuild:qt5-v0.2.3
docker pull ghcr.io/nicoverbruggen/kobuild:qt6-v0.2.2
bash platforms/kobo/build.sh qt5
bash platforms/kobo/build.sh qt6
```

The launcher pins these images by digest. It mounts this worktree, configures CMake and builds the reader dependencies, application and installation ZIP. It does not pull or repair SDKs. Configure and build/package durations are printed separately. Parallelism defaults to two; set `JOBS` to change it.

Outputs are `build/kobo-qt5/` and `build/kobo-qt6/`. Each contains `libkobalt.so` with debug symbols and `Kobalt-qtN.zip` with a stripped installation library and runtime assets. Qt5 uses `.kobo/KoboRoot.tgz`; Qt6 uses `.kobo/Kobo.tgz`. Packaging removes its staging directory and does no work when its inputs are unchanged. Both tar formats store only the library on the system partition, with numeric owner `0:0` and mode `0755`, regardless of the build user. The build launcher runs the `check-package` target after packaging to verify the member path, ownership and permissions. The same check can be run in the prepared SDK with `cmake --build --preset qt5 --target check-package` or the Qt6 preset.

`bash platforms/kobo/build.sh qt5-eval` and `qt6-eval` build evaluation packages in `build/kobo-qt5-eval/` and `build/kobo-qt6-eval/`. They add the scenario player, the scenario file, the benchmark book and the diagnostics switch; see [Release evaluation](release-evaluation.md#device-scenarios).

## Android

The Android project builds a signed debug APK through Qt's standard CMake `apk` target. It uses the prepared multi-architecture kobuild image; Docker selects its native host variant. Both variants target Android `arm64-v8a`.

```sh
docker pull ghcr.io/nicoverbruggen/kobuild:android-v0.3.0
bash platforms/android/build.sh
```

The launcher pins the released image by digest. Set `KOBALT_ANDROID_IMAGE` only to select another prepared image for SDK validation. Host architectures use separate `build/android-amd64/` and `build/android-arm64/` outputs, so Qt host-tool caches cannot cross between them. Gradle downloads stay under `build/gradle/`. The launcher rejects concurrent use of one Android output and removes its container when finished. Compiler parallelism defaults to two; set `JOBS` to change it.

The APK is `build/android-HOST/android-build/build/outputs/apk/debug/android-build-debug.apk`. It uses `com.nicoverbruggen.kobalt.debug`, the public debug key under `platforms/android/app/` and a version ending in `-debug`. Android 9 or later and ARM64 are required. Release signing uses the explicit mode described below. BOOX selects Qt's SurfaceView container before creating windows so firmware refresh metadata can reach the submitted buffers. Other Android devices keep Qt's default container. Library, book-opening and reader canvases share the BOOX frame transport. Returning to the Library retains a captured reader frame until its final system-bar insets are ready, then requests GC cleanup. The Android window follows the activity's laid-out bounds at startup and after rotation. Only the Library canvas draws rules below Android's status bar and above its navigation controls; the fullscreen reader has neither rule.

## Kindle prototype

The Kindle target is a standalone application with its own library. It targets the ARM hard-float userspace used by firmware 5.16.3 and later. The initial hardware target is Paperwhite 5 with firmware 5.19.2. Physical display, touch, suspend and stock-UI restoration require device validation.

Build the SDK separately in [kobuild](https://github.com/nicoverbruggen/kobuild) with `./build.sh kindle`. Then run these commands from the Kobalt repository root:

```sh
bash platforms/kindle/build.sh
bash platforms/kindle/sdk/prepare-runtime.sh
bash platforms/kindle/verify-package.sh
```

The prepared `localhost/kobuild:kindle` image contains Qt 6.8.3, Qt Sensors, Qt SQL with its bundled SQLite driver, OpenSSL 3.5.9, zlib and FBInk using the KOReader `kindlehf` toolchain from release 2026.08. Qt uses its linked OpenSSL backend. The compiler, source pins, patches, license inputs and SDK checks belong to kobuild. The image runs as amd64 Linux, including on Apple Silicon. Docker must already be running. Set `KOBALT_KINDLE_SDK_IMAGE` to select another prepared image. Application builds mount this checkout at `/work` and keep dependencies and objects under `build/kindle/`; they do not rebuild Qt or mount over `/sdk`.

The application uses a native `hwtcon_v2` framebuffer backend on Paperwhite 11. It does not link or package FBInk. The backend preserves the firmware framebuffer mode, writes grayscale damage using the kernel stride, and submits updates through the matching firmware ioctl ABI. A worker waits for controller completion while Qt continues to handle input. The queue retains immutable image snapshots, combines pending damage, and cancels stale animation hints. Completion near the firmware's silent five-second timeout is treated as uncertain and returns through launcher recovery.

The existing Page turn animations preference controls native swipe requests. The adapter enables this capability only when the configured waveform file matches the kernel's loaded waveform name and its header reports TL support. If firmware has unmounted the waveform volume, the probe reads its header through a temporary read-only mount and removes that mount immediately. The mount lives beneath the supervisor's run directory so recovery can unmount it if the app is killed during the probe. It never calls the stock waveform mount helper, which can format the partition on failure. Sleep, full refresh, coalesced pages and exit feedback suppress swipe. The firmware rotates swipe regions and directions. Set `KOBALT_KINDLE_DISPLAY_TRACE=1` when launching for submission and completion timings. Emulator timings do not establish physical panel latency.

Runtime preparation downloads Amazon's checksum-pinned PW5 firmware 5.19.2 image and extracts its matching glibc 2.20 libraries with KindleTool and read-only `debugfs`. It does not mount or modify the image. The fixture stays under `build/kindle-sdk/firmware-runtime/` and is excluded from the installation ZIP. Verification uses a container that matches the Docker engine's architecture. On Apple Silicon, x86 QEMU running under Rosetta produced false JIT and application crashes; use native ARM64 QEMU for this check. Qt has a narrow patch that skips redundant library unloading during process cleanup on old glibc, where `dlclose` otherwise asserts after a library's finalizer has run. Explicit unload calls retain their normal behavior.

The package is `build/kindle/Kobalt-kindle-prototype.zip`. It contains `kobalt/` and `extensions/kobalt/` for the Kindle USB root. It includes private Qt libraries and the `kindle` platform plugin; it does not replace system libraries. The verification command checks the packaged ARM ABI, loader, runtime dependencies and glibc symbol requirements, then runs the packaged app against the extracted stock runtime under QEMU userspace emulation with Qt's offscreen platform. It saves `build/kindle/arm-smoke.log` and removes its temporary profile. This checks the application and runtime libraries, not the Kindle kernel or e-ink controller. Do not install it until a supervised device test is planned. The launcher expects a hard-float Kindle, a supported extension launcher and a running `lab126_gui` service. It refuses to run with a passcode enabled, another prototype instance active, or a volume daemon already paused by another reader. It stops the stock UI while the app runs and attempts to restart it after exit or application failure. An uncatchable supervisor failure still needs manual recovery.

The deployment target is a standard Paperwhite 11/PW5 with firmware 5.19.2 and KUALA. Extract the ZIP at the Kindle USB root, preserving the `kobalt/` and `extensions/kobalt/` folders. Keep existing `kobalt/data/` when updating. Copy DRM-free EPUB files to `documents/`. Kobalt scans this folder recursively and starts in its library. Use Back to library to close a book, then Exit Kobalt to return to the stock UI. Exit paints “Returning to Kindle…” and waits for its refresh acknowledgment before closing. The stock interface can take time to restart; a running service alone does not prove that Home is ready. The keyboard supports printable ASCII, including uppercase letters and password punctuation. Other keyboard languages and folder picking remain unsupported. Annotation export uses `data/config/exports/` beneath the installation folder, or `config/exports/` beneath an explicit profile, and uses the reader's confirmation dialog instead of a system file picker.

The Kindle adapter reads battery percentage, charging state, Wi-Fi connection state and Bluetooth connection state from stock services. The Library shows text status. The reading menu shows battery percentage and connected-radio indicators. Fresh profiles also show the battery in the reading header and select cover sleep screens. Existing profiles keep their settings. Brightness and warmth use the stock power service's native levels with readback. Canceled adjustments restore the original level. Kindle owns persistence and its warmth schedule; Kobalt does not change that schedule or expose automatic brightness on the standard model. Network features use Qt's TLS backend and the stock certificate store. Downloadable dictionaries use the shared dictionary manager.

The power helper publishes framework readiness while the stock GUI is stopped. The reader checkpoints before sleep and rejects sleeping contacts. Its overlay marks the painted frame, and the display plugin waits for refresh completion before suppressing later output. A late sleep image can replace that frame; native dialogs cannot draw over it. Wake requires a fresh contact and redraws the full screen. The supervisor pauses the current volume daemon while Kobalt maps files from USB storage. A USB storage request closes the reader before the supervisor resumes that daemon and restores the stock UI. USB networking stays available. The supervisor runs from system storage and keeps its open log there; cleanup copies the log to `kobalt/logs/launcher.log`. TERM allows ten seconds for shutdown before forcing exit. Physical standby, USB export and hardware recovery still require supervised device testing.

For read-only service diagnostics over SSH, run `LD_LIBRARY_PATH=/mnt/us/kobalt/lib /mnt/us/kobalt/bin/kobalt-device-info`. It does not stop the stock UI or open the framebuffer. Add `--https https://github.com/` to check an HTTPS handshake with certificate validation. A failed request exits with a nonzero status. This tool does not change lights or radios.

If the supervisor itself is killed, keep its recovery directory until its application and power helper have stopped. `/var/tmp/kobalt-kindle-prototype/volume.pid` identifies the paused volume daemon; resume that exact process with CONT. Reload the original screensaver module if `screensaver.loaded` contains `yes`, then run `start lab126_gui`. Remove the recovery directory only after confirming those services and processes. Reboot is another recovery path because Kobalt does not install a boot-time launcher. The launcher continues to refuse passcode-enabled devices.

Use the launcher as the device entry point. To change its folder or saved data, pass `--library DIRECTORY` or `--profile DIRECTORY` to `sh /mnt/us/kobalt/launcher/launch.sh`. The launcher sets the Qt and library environment, supplies `--assets /mnt/us/kobalt/assets`, and handles stock-UI restoration. Running the packaged binary directly requires that environment and asset path and bypasses restoration. `--book FILE` opens an EPUB at startup. The development `--smoke-test` option also requires `--book FILE` and an isolated `--profile DIRECTORY`; it changes reading progress in that profile.

For a headless native check of the same application code, run from `platforms/kindle/` with the installed Mac SDK:

```sh
cmake --preset kindle-native -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.3/macos"
cmake --build --preset kindle-native
ctest --preset kindle-native --output-on-failure
```

The native check opens the checked-in Type Tester EPUB, turns a page, saves, closes and reopens it, and verifies its saved position and rendered page pixels. It also checks legacy JSON migration into SQLite, optional book-field preservation, queued taps and holds, and canceled contacts through the real library and reader canvases. Sleep/wake checks restore the PW5 viewport and test a different control after an interrupted contact. Keyboard checks enter printable ASCII, delete and dismiss without intercepting reader editors. Device checks cover status parsing, bounded property calls, failed readback, rollback, initial sleep state and USB mode handling. Launcher checks use mocked services and cover failed restoration, interrupted ownership acquisition and an app that ignores TERM. The Linux package check also exercises the real power helper with a mock LIPC library. These checks remove their own temporary state. They do not open a visible window or access a Kindle.

After building `kindle-native`, run `bash platforms/kindle/preview.sh` from the repository root to open the interactive Mac preview. It keeps the PW5 reader layout at 1236 × 1648 and scales it into a resizable window. The default library contains the checked-in Type Tester EPUB. Pass an EPUB folder as the script's only argument to use your own books. Settings and reading progress stay under `build/kindle-preview/profile/`, separate from the regular Mac app and device data. Click to use the reader's touch controls. Close the window to save and exit. The preview uses native Qt 6.11.3; the Kindle package uses Qt 6.8.3. It does not emulate Kindle firmware, e-ink refresh, power management or touch hardware.

Run `bash platforms/kindle/capture.sh` to capture the library, keyboard pages, cover, body text and settings headlessly at PW5 resolution. It uses a temporary profile, removes that profile after the run, and writes grayscale PNGs, an overview and a checksum manifest to `build/screenshots/kindle/`. It replaces a previous capture set only after the new run completes and refuses an unrelated output directory. Native status is unavailable unless a test fixture supplies it; those screenshots do not show real device readings. Inspect the images before judging the UI. The native `kindle.preview` test also checks that scaling keeps the reader's PW5 layout dimensions and preserves rendered pages through save and reopen.

For stock PW5 firmware emulation, prepare the headless emulator separately:

```sh
bash platforms/kindle/emulator/prepare.sh
bash platforms/kindle/emulator/run.sh pw5-test
```

Preparation builds a container for the Docker engine's native architecture and reuses its cached QEMU build. It applies `dependencies/emulator/qemu-lut-frames.patch` to retain each prepared display region until its waveform slot commits. The preparation step runs the snapshot regression with address and undefined-behavior sanitizers, then lets Ninja check the cached emulator build. It verifies pinned [eink-emulator](https://github.com/katadelos/eink-emulator/tree/81ae873f57ea5a8aba71a00774118dc3ad8462e7), QEMU, KindleTool, Amazon firmware 5.19.2 and jb.sh downloads. Firmware, sources, guest disks and caches stay under ignored `build/` folders. Set `JOBS` to change the default four QEMU build workers. The scripts use the selected Docker context or `DOCKER_HOST` and keep Docker configuration under `build/kindle-sdk/docker-config/` unless `DOCKER_CONFIG` is set.

The runner accepts a new lowercase guest name and starts PW5 firmware 5.19.2 with upstream `--jbsh`. Jailbreak installation runs during the first boot; this does not install KUALA or Kobalt. The runner prints its container name and SSH command. QEMU binds SSH to container loopback, so use `docker exec` rather than a host SSH port. In another terminal, query its QMP socket with `docker exec kobalt-kindle-emulator-pw5-test python3 /emu/scripts/eink-qmp.py --machine pw5-test status`. Stop the runner with Ctrl-C or TERM. It stops its QEMU process and removes only its own container and guest directory, including the guest disk. It preserves prepared firmware and shared base images. Existing guest names are refused. The guest is temporary; do not use this runner for state that must survive shutdown.

Allow several minutes for first boot and stock-framework restarts. SSH availability and `lab126_gui start/running` do not prove that Home is interactive. Check a screenshot, open Library, cancel the registration dialog if shown, and open Search to confirm that its keyboard appears. Use the current scanout's pixel coordinates with QMP `tap`; PW5 uses 1236 × 1648 pixels. An early unchanged screenshot is not enough to diagnose a failed touch. Reader `page-turn-painted` and `neighbours-end` diagnostics also precede graphics-proxy presentation. Before comparing checkpoint screenshots, allow the framebuffer to settle and confirm matching body pixels in repeated captures without input; exclude the changing clock. In the emulator, a capture five seconds after neighbour preparation showed the new page while the immediate capture still showed the old page.

After Home is interactive and SSH is ready, deploy the built package and fixture from the container. First-boot firmware 5.19.2 left the emulator's user storage mounted with noexec. The command below enables execution only for that temporary guest mount. Keep large ZIPs on `/mnt/us`; `/var/tmp` is a small RAM filesystem. These commands use the emulator's generated test key:

```sh
container=kobalt-kindle-emulator-pw5-test
docker exec "$container" bash -euc '
  key=(-F /dev/null -i /emu/build/ssh/id_ed25519 -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
  scp -O "${key[@]}" -P 2222 /work/build/kindle/Kobalt-kindle-prototype.zip root@127.0.0.1:/mnt/us/
  scp -O "${key[@]}" -P 2222 /work/tests/fixtures/trevelyans-type-tester.kepub.epub root@127.0.0.1:/mnt/us/documents/
  ssh "${key[@]}" -p 2222 root@127.0.0.1 "mount -i -o remount,exec /mnt/us && cd /mnt/us && unzip -o Kobalt-kindle-prototype.zip >/dev/null && sh kobalt/launcher/launch.sh"
'
mkdir -p build/kindle-emulator/results
docker exec "$container" python3 /emu/scripts/eink-qmp.py --machine pw5-test screendump /work/build/kindle-emulator/results/manual.png
docker exec "$container" python3 /emu/scripts/eink-qmp.py --machine pw5-test tap 170 420
```

Wait for the library before sending the cover tap. Use `--hold 1` with `tap` for a held contact. SSH launch tests the supervisor directly. Testing the user launch path also requires an extension launcher, such as KUALA, installed in the guest. Emulator checks exercise the firmware kernel, framebuffer and input stack. They do not establish physical panel quality, real touch timing, suspend or USB handoff behavior.

## Dependencies and validation

CREngine and its text/image libraries use the versions and patches recorded in this worktree. Android also builds OpenSSL and downloads the pinned BOOX compatibility JAR. These application dependencies are separate from Qt SDK production. Build directories are separated by compiler, SDK, source manifest and recipe identity. This initial setup invalidates the dependency set when its recipes, patches or source pins change; application-only changes reuse it.

Stop and reap run-owned helper processes and remove temporary profiles, staging directories and containers after each run. Keep current outputs, useful measurements and incremental caches. Native desktop, model, renderer and session checks cover the paths listed above. [Native validation](validation.md#feature-coverage-and-limits) records the remaining corpus, gesture and platform gaps. These checks do not establish firmware compatibility or physical-device display behavior.

## GitHub builds and tagged releases

`.github/workflows/ci.yml` builds Qt5, Qt6 and a signed Android release APK on branch pushes and tags. Helper checks validate version inputs, source pins and missing-key failures. Kobo jobs check the system archive member, numeric root ownership and executable permissions. Desktop application builds, native tests and screenshot galleries run locally through the Mac or Linux presets; they are not CI jobs.

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

Kobo and desktop hosts use `ui::makeWidgetKeyboard` from `src/interfaces/reader/ui/controls/keyboard.h`. It implements the existing `KeyboardFactory` interface. The supplied parent determines placement, the supplied editor receives text, and the controller owns its panel. It provides a Latin QWERTY layout, printable ASCII symbols, persistent Shift, Space, Delete and submission. Hold a Latin letter to choose an accented variant or a ligature. Shift also applies to these choices. Other writing systems and input methods are not implemented. `setSubmitText` changes the submit label. Android keeps its native system keyboard. Other adapters select this factory through their host services. The panel follows the reader theme and publishes its bounds so dictionary and account editors stay above it. Kobo keys use the existing text-refresh policy; no Nickel keyboard constructors or repaint hooks are required.
