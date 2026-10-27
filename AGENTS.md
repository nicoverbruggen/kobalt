# Working on Kobalt

Read [Building](doc/building.md) before running a build. This branch is rebuilding the development tooling incrementally. Do not restore retired runners wholesale.

## Build ownership

Use CMake and presets for application builds. Do not add Python tooling or an internal build-support framework. Keep platform build definitions, presets and toolchains under `platforms/`. Shared application source remains under `src/`. Shared asset inputs live under root `assets/`; stage runtime bundles inside each platform output. Do not read runtime assets from another checkout.

Kobo and Android builds use prepared `kobuild` images. Use native ARM64 Android containers on ARM64 hosts when the selected image supports them. The compiler, sysroot, Qt modules, Android SDK, NDK and JDK belong to kobuild. Fix missing SDK components in kobuild rather than building Qt modules or repairing SDKs in the application. Never compile Qt or any Qt module as part of a Kobalt build.

The Mac app uses an existing native Qt SDK. A Kobo cross SDK cannot replace the native Mac SDK. Application-specific dependencies such as CREngine remain application inputs, separate from Qt SDK production.

The Kindle prototype consumes the prepared `localhost/kobuild:kindle` ARM hard-float SDK image. Build that image in kobuild with `./build.sh kindle`; compiler, Qt, TLS and framebuffer dependencies belong there. `platforms/kindle/build.sh` only builds the application and exports its matching offscreen plugin for validation. Never mount an old SDK over the image's `/sdk`. The SDK currently runs as `linux/amd64`, including on ARM64 hosts. `platforms/kindle/sdk/prepare-runtime.sh` owns only a checksum-pinned stock firmware fixture under ignored `build/kindle-sdk/`. Run verification in the native container selected by `platforms/kindle/verify-package.sh`; x86 QEMU under Rosetta produces false crashes. Keep Kindle code under `platforms/kindle/` and reuse the standalone reader and library. Native checks and ARM userspace checks do not establish Kindle hardware compatibility. Do not launch the device supervisor on a development host.

The macOS and Linux desktop clients are tools for development and testing on desktop. They are not release versions of the app. Keep desktop builds, native tests and screenshot galleries local. Do not add desktop release packages or desktop CI jobs unless the user explicitly requests them.

Desktop development accepts Qt 6.11 or newer Qt 6 from one prepared native SDK. Linux uses the matching `linux`, `linux-light-tests`, `linux-fast-tests`, `linux-tests` and screenshot presets in `platforms/desktop/`; keep outputs under `build/desktop-linux/` and `build/screenshots/linux*/`. Desktop builds default to sensorless manual orientation; enable `KOBALT_DESKTOP_SENSORS` only when validating the real sensor adapter with a matching prepared Qt Sensors module. Mac development uses the `mac` presets. From `platforms/desktop/`, build `mac-tests` and run `ctest --preset mac` for native headless checks. Build `mac-screenshots` for the current captures in `build/screenshots/mac/`. Both paths use the same compiled desktop implementation as the app. Keep test profiles isolated and remove temporary fixture and capture state when a run ends. See [headless tests and screenshots](doc/building.md#headless-tests-and-screenshots) for scope and commands.

Restore portable regression assertions from `develop` with only necessary path, fixture and SDK adaptations. The `mac-tests` preset builds desktop, model, engine, session, resource and component checks; use `ctest --preset mac -L model` for the restored model group. Reuse compiled application objects. Native checks run through `platforms/desktop/run-test.cmake` so failure and timeout cleanup does not depend on test destructors. Keep the Fusion style used by the app and the original component suite. Await asynchronous UI completion before sending the next action. A page center can be covered by a panel; target exposed paper when dismissing it.

Use the checked-in Trevelyan's Type Tester fixture for general reading checks and screenshots. It includes the chapter-heading CSS fix documented in [Native validation](doc/validation.md#book-fixtures). Keep synthetic books for focused regressions, not as a replacement for the general book fixture. The full screenshot gallery also uses the checksum-pinned international chapters and Gutenberg editions documented in Native validation. Keep those fixtures, their provenance and their scenes when extending the tour. Gallery captures are frameless production reader or Library images at 300 PPI, at six-inch and seven-inch sizes, in the light theme, the dark theme and the light theme with 150% interface text. Each gallery card represents one screen; size, variant and page choose its raw file. Require all three variants at both sizes for each screen, with matching light/dark pages at each size; 150% text may add pages. Keep device frames in the HTML viewer only, and retain filenames and checksums in the manifest for comparisons.

Use [Native validation](doc/validation.md) for the test inventory, feature coverage and explicit gaps. The routine UI loop is `mac-light-tests` followed by `ctest --preset mac-light`; `mac-light-screenshots` captures the primary views. Keep the light lane focused on primary views and features. It does not replace full release validation. `mac-fast-tests` and `ctest --preset mac-fast` retain the broader fast checks; `mac-tests` and `ctest --preset mac` run the full native set. `mac-screenshots` produces the current image set, checksum manifest and local gallery. Set `KOBALT_GALLERY_SCENE` to capture only the named gallery cards into `build/screenshots/mac-scenes/`; it never replaces the full gallery. Use `ctest --preset mac -L lifecycle` for reader lifecycle and annotation-frame checks, `-L ui` for shared canvas and paint checks, `-L platform` for portable adapter checks and `-L corpus` for the representative literary corpus. All four groups belong to the integration lane. Keep them in the integration lane and link the existing production objects; do not restore the old test-host application or scroll-only canvas. Keep public validation independent of private runtime services. Public Nickel compatibility checks are deferred until the user chooses an approach.

Use [Release evaluation](doc/release-evaluation.md) to compare a candidate with a baseline: `cmake -DRECORD_BASELINE=ON -P platforms/eval/run.cmake` on the baseline, `cmake -P platforms/eval/run.cmake` on the candidate, then `cmake -P platforms/eval/compare.cmake`. The quick profile is the default; `-DPROFILE=release` runs the full suite and gallery. Keep these scripts thin: they call the presets and the performance mode, and they never replace them. End-to-end performance scenarios live only in `platforms/eval/scenarios.json` and run through the built-in scenario player on every platform; add a scenario there, not as C++ test code. The player is compiled only into the test executable and into evaluation packages (`build.sh qt5-eval` or `qt6-eval`), never into release packages. Device timings come from an evaluation package on a Kobo, collected with `platforms/kobo/install-eval.cmake` and `collect-eval.cmake` or through an external runner passed with `DEVICE_RUNNER`. Keep that contract generic: everything must also work without a private runner. Never re-record a baseline to make a report pass. Real-device runs kept for comparing devices live in `platforms/eval/devices/`; see [Device benchmarks](doc/device-benchmarks.md).

Flag suboptimal approaches before extending them. Identify duplicated build logic, application-side SDK workarounds, dependence on an old worktree, unnecessary rebuilds and unclear ownership. Explain the issue and propose a simpler approach.

Do not record fixed test, case, assertion or screenshot totals in documentation or these instructions. Describe coverage and verification commands instead. Use `ctest --preset mac -N`, with optional `-L` or `-R` filters, for the current test inventory. Keep meaningful fixture sizes, limits and measured durations.

When adding a UI screen that needs screenshot coverage, include its scenes in the full gallery in the same change. When adding or removing screenshot scenes, update the full-gallery expected count in `tests/desktop.cpp` in that change. Run `cmake --build --preset mac-screenshots` from `platforms/desktop/` to verify that the complete collection matches that count.

The root `./start.sh` builds and opens the Qt developer launcher on Linux and Mac. Keep launcher sources and presets under `platforms/launcher/`. Startup checks must explain missing dependencies in the terminal, keep Docker optional for desktop work, and never install tools or pull SDK images automatically. Its actions delegate to the existing platform presets, Kobo build script and autonomous scenario player. Keep visible scenarios separate from the full headless assertion suite. Preserve run-owned process, container and scratch cleanup when changing launcher actions.

## The page turn is sacred

While the user can turn a page or otherwise interact with the reader, the main thread must not write storage or wait for another thread. This includes Nickel's database, profile and preference saves, the diagnostic log, sleep snapshots and synchronous calls to the profile writer thread. Queue such work on a worker thread and report the result back to the main thread. A slow SD card or a busy Nickel database must never delay the next tap.

Block the main thread only where it is absolutely necessary: where no input can arrive before the wait ends, such as sleep, USB export, closing, power-off and a host checkpoint. Mark each such scope with `mainthread::AllowBlocking` and state the reason. Do not use it to silence a violation on a path that runs while the user reads. The current known exceptions are listed in [the main-thread guard](doc/validation.md#main-thread-guard); remove them rather than add to them.

`src/support/main_thread.h` enforces this rule. Every operation that writes storage or waits for another thread must call `mainthread::blocking()`, including a new storage helper or a new firmware call. The native tests run with `KOBALT_STRICT_MAIN_THREAD=1` and abort on a violation. Run them with `KOBALT_STRICT_MAIN_THREAD=trace` to list every violation with its call stack. Kobo logs a `main-thread-blocked` event instead. See [the main-thread guard](doc/validation.md#main-thread-guard).

## UI integration and verification

Before changing UI, identify the closest existing control or screen. Trace its construction, shared styling, metrics and later presentation adjustments. Reusing its widget class alone is not enough.

Preserve the existing typography, caption/value alignment, padding, dividers and interaction behavior unless the user requests a change. Use established preview content when suitable.

For new layout controls, make the units understandable without reading implementation details. The preview must visibly demonstrate the setting.

Share an early screenshot for a new or substantially redesigned control. Include both its entry in the surrounding UI and its opened state. Do this before broad polishing and gallery generation.

Before declaring the UI complete, compare it directly with the closest existing example. Verify the actual font, size, alignment and spacing. Passing functional tests and generating screenshots do not establish visual consistency.

When reviewers are requested, give them these specific comparisons. Require findings or explicit verification for both the surrounding screen and the new control.

## Execution and cleanup

For repeated development deployments, it is imperative to assess only the changes since the last validated build and reuse applicable validation results. Do not automatically rerun the full test suite or a smoke test for every minor change. Small UI spacing or styling adjustments may need only a build and visual inspection. Use focused checks when the change warrants them, and do not delay an explicitly requested deployment with redundant validation. Keep full release validation separate. State which checks were run, reused or skipped.

Run native Qt commands outside the agent sandbox on the development Mac. Headless Qt tools still need CPU capabilities that the sandbox can hide. Container commands need access to the engine socket. Keep outputs and caches in the authorized workspace.

Do not open a visible app or run visible UI tests without authorization. Prefer native headless validation while editing, then validate Kobo and Android integrations last. Reuse prepared SDKs and verified dependencies.

Treat storage IO on Kobo as expensive. Devices store data on eMMC or, on older models, an SD card, and their speed differs widely. Measure storage-bound work, such as syncs and saves, on both kinds, and judge changes by the slower one.

Stop and reap run-owned helper processes and remove scratch profiles, staging directories and temporary containers when a run ends, including on failure or interruption. Preserve the user's app state, active runs, SDKs and incremental caches. Retain useful current results and unresolved failure evidence rather than every successful raw log indefinitely.

## Repository practice

Read [Storage compatibility](doc/storage.md) before changing settings, book state, bookmarks, annotations or rendering caches. This also applies to backup tools and test database inspection: follow the backup contract and never open the live exclusive profile through an external SQLite connection. Keep saved reading data separate from disposable caches. App releases must not reset either format merely because the release version changed. Compatible optional data fields keep the current format version and must load older records with explicit defaults. Preserve supported optional fields when editing records. An incompatible saved-data change requires a format-version bump, a migration that preserves existing user data, and regression tests before it ships. Never overwrite or delete the original files of unsupported or damaged saved data. Profile preparation moves them into `backups/` and starts from defaults; other reads reject them and leave them in place. Do not assume older builds preserve unknown fields or can safely write newer schemas.

Bump the format version in `documentCacheChecksum` when parser or preprocessing changes make parsed-book caches incompatible. Bump the inspection-cache format separately when verdict rules change incompatibly. Cache invalidation must rebuild disposable data only; it must never clear settings, bookmarks or annotations.

Keep Markdown in `doc/`, except the root `README.md` and `AGENTS.md`. The user owns root README edits. Keep these instructions and build commands current in the same change as tooling updates.

Do not stage, commit, push or publish without explicit authorization. Signed operations require an advance warning for 1Password prompts. Never add an agent coauthor. Preserve unrelated changes and the original checkout.

Work on a feature branch by default. Before the first edit, create a dedicated branch and a Git worktree for it. The branch `feature/name-of-feature` always lives in the worktree `worktrees/name-of-feature` in the repository: the folder name is the branch name without the `feature/` prefix. The `feature/` prefix is required. Git ignores the `worktrees/` folder. This applies to small changes too, and also when no other worker seems active. Work directly in the shared `develop` checkout only when the user says so for that task. Otherwise, other agents and the user can edit `develop` at any time: only read and investigate there. If you find that you started work in `develop` without being asked, stop and move only your own changes into a worktree. Leave every other change in `develop` as it is. When authorized to integrate a feature into `develop`, cherry-pick its commits or merge it as a fast-forward. Never create a merge commit.

One conversation thread usually means one worktree and one branch. When the user asks for several features in one thread, check whether they would change the same code. If they would, ask whether the user wants a separate worktree for each feature before the first edit.

After successful integration, normally remove the worker's worktree and feature branch unless the user asks to keep them. Before removal, verify that the worktree has no uncommitted changes and that all intended changes are integrated. Preserve any needed test evidence or other artifacts before removing the worktree.

Use lowercase release-stage words in commit messages, including `alpha` and `beta`, even when part of a release name. The abbreviation `RC` may remain uppercase.
