# Working on Kobalt

Read [Building](doc/building.md) before running a build. Keep changes scoped, and keep these instructions current in the same change as tooling updates. Do not restore retired runners wholesale.

Flag suboptimal approaches before extending them: duplicated build logic, application-side SDK workarounds, dependence on an old worktree, unnecessary rebuilds and unclear ownership. Explain the issue and propose a simpler approach.

## Repository and doc map

| Path | Owns |
| --- | --- |
| `src/app/` | Application shell, reader window, library shell, about and messages |
| `src/reading/` | Reading session, appearance settings, progress, bookmarks, highlights, activity, presets, sleep screen |
| `src/engine/` | Engine interface, worker thread, and Blaze (Kobalt's CREngine fork) |
| `src/publication/` | EPUB opening, resources, page estimates, image policy |
| `src/render/` | Page painting, refresh regions, highlights, bookmarks, tap zones |
| `src/storage/` | Profile SQLite database, JSON stores, record writer, caches, annotation export |
| `src/ui/` | Shared controls, dialogs, input handling, theme styling |
| `src/library/`, `src/dictionaries/`, `src/sync/` | Standalone Library, StarDict lookup, KOSync client and scheduling |
| `src/platform/`, `src/assets/`, `src/support/`, `src/scenario/` | Platform adapters, font/asset loading, main-thread guard/diagnostics, scenario player |
| `platforms/` | Per-platform CMake projects, presets, toolchains (`desktop/`, `kobo/`, `android/`, `launcher/`, `standalone/`) and scripts (`checks/`, `ci/`, `eval/`, `review/`) |
| `assets/` | Shared asset inputs, staged into each platform output |
| `dependencies/` | Pinned third-party URLs, checksums, patches and CMake recipes |

Read only the doc relevant to your task:

| Topic | Doc |
| --- | --- |
| Builds, presets, SDKs, formatting | [doc/building.md](doc/building.md) |
| Reader behavior by feature area | [doc/reader.md](doc/reader.md), then only the matching `doc/reader/<area>.md` |
| Tests, area labels, fixtures, guards | [doc/validation.md](doc/validation.md), then only the matching `doc/validation/<area>.md` |
| Storage schemas and cache policy | [doc/storage.md](doc/storage.md), then only the matching `doc/storage/<area>.md` |
| Rendering engine and Hybrid rules | [doc/engines.md](doc/engines.md) |
| UI review workflow | [doc/screen-review.md](doc/screen-review.md) |
| Benchmarks and release evaluation | [doc/release-evaluation.md](doc/release-evaluation.md), [doc/device-benchmarks.md](doc/device-benchmarks.md) |
| Writing and maintaining docs | [doc/documentation.md](doc/documentation.md) |

## Build ownership

- Use CMake and presets only. Do not add Python tooling or an internal build-support framework. Stage runtime bundles inside each platform output, and never read runtime assets from another checkout.
- Prepared SDKs belong to `kobuild`. Kobo and Android builds use prepared `kobuild` images, with native ARM64 Android containers on ARM64 hosts when the image supports them. Never compile Qt or any Qt module in Kobalt; fix missing SDK components in `kobuild`. Pin all four images (Qt5, Qt6, Android, Linux) in `platforms/sdk-image.sh` from one `kobuild` release tag by digest, and name that tag in [Building](doc/building.md). Application dependencies such as CREngine stay application inputs, separate from Qt SDK production.
- The desktop app is a local development and test tool. Desktop builds use an existing native Qt 6.11+ SDK; a Kobo cross SDK cannot replace it. They default to sensorless manual orientation (`KOBALT_DESKTOP_SENSORS` enables the real sensor adapter). Keep desktop builds and galleries local. Do not add desktop release packages or extra desktop CI jobs unless requested.
- Compiled third-party dependencies and downloads live in the shared dependency cache, `build/dependency-cache/` in the main checkout, keyed by content. It belongs to no worktree and is the only build state that worktrees share, so using it is not dependence on another worktree. All other outputs stay in each worktree's `build/`, and release evaluation exports use their own cache. Never edit, move or partly delete a published cache entry; change a pin, recipe or patch to produce a new key. See [Dependency cache](doc/building.md#dependency-cache).
- CI runs the full native suite in the pinned Linux image (`platforms/ci/native-tests.sh`) before Kobo and Android builds. CI does not run the sanitizer preset; run it when a release candidate is compared against the previous beta ([Evaluate a candidate](doc/release-evaluation.md#evaluate-a-candidate)). Branch CI runs representative variants. Tagged CI and release approval require `KOBALT_TEST_ALL_VARIANTS=1`, recorded separately.
- Direct Kobo builds and branch, nightly and interim CI enable verbose diagnostics by default. Tagged releases disable them; `KOBALT_VERBOSE_DIAGNOSTICS=OFF` selects release logging for a direct build. `qt5-diagnostic` and `qt6-diagnostic` always enable verbose diagnostics and use the regular reader without the scenario player; see [page-turn diagnostics](doc/validation/diagnostics.md#page-turn-diagnostics).
- The developer launcher (`./start.sh`) keeps its sources and presets under `platforms/launcher/`. Its actions delegate to the existing platform presets, Kobo build script and scenario player. Startup checks must explain missing dependencies in the terminal, keep Docker optional for desktop work, and never install tools or pull images automatically. Keep visible scenarios separate from headless assertions, and preserve run-owned cleanup.

## The page turn is sacred

While the user can turn a page or interact with the reader, **the main thread must never write storage or wait for another thread**. This includes Nickel's database, profile and preference saves, diagnostic logs, sleep snapshots and synchronous calls to the profile writer. Queue such work on a worker thread and report results back to the main thread. A slow SD card or a busy Nickel database must never delay the next tap.

- Block the main thread only where no input can arrive (sleep, USB export, closing, power-off, host checkpoint), marked with `mainthread::AllowBlocking` and a reason. Never use it to silence a reading-path violation; remove existing exceptions rather than add to them ([Main-thread guard](doc/validation.md#main-thread-guard)).
- Every operation that writes storage or waits for another thread must call `mainthread::blocking()` (`src/support/main_thread.h`), including a new storage helper or a new firmware call. Native tests run with `KOBALT_STRICT_MAIN_THREAD=1` (use `=trace` for call stacks); Kobo logs `main-thread-blocked`.
- Run `cmake -P platforms/checks/source-checks.cmake` after changes touching UI storage access, `AllowBlocking`, `reader_*` names, short timers or `reading/` headers. Any change to an allowlist in `platforms/checks/` is a decision for the user: ask and wait.

## Validation, screenshots and evaluation

Run desktop CMake/CTest commands from `platforms/desktop/` and root-level CMake scripts from the repository root. Update affected tests when behavior changes and run them before handoff.

| Loop | Command |
| --- | --- |
| Routine UI checks | `cmake --build --preset mac-light-tests && ctest --preset mac-light` |
| Area check | `cmake --build --preset mac-tests && ctest --preset mac -L <area>` |
| Group checks | `-L model`, `-L lifecycle`, `-L '^ui$'` (anchored to exclude `build`), `-L platform`, `-L corpus` |
| Full native suite (recorded) | `cmake -DPRESET=mac -P platforms/validation-record.cmake` (from repo root) |
| Kobo syntax check for Qt 5.2 and Qt 6.5 (after first shared edit) | `cmake -P platforms/kobo/syntax.cmake` (add `-DEVAL=ON` if touching eval code) |
| Kobo SDK and pewter checks (before integration) | `cmake --build --preset mac-kobo-checks` |

- Validate incrementally. Assess only what changed since the last validated build and reuse applicable results; do not rerun the full suite for minor edits. Small spacing or styling changes may need only a build and visual inspection. Do not delay an explicitly requested deployment with redundant validation. The light lane does not replace full release validation. State which checks ran, were reused or were skipped. Respect the [validation guard](doc/validation.md#validation-guard); use `KOBALT_REVALIDATE=1` only when a rerun is genuinely needed and explain why.
- Native tests run through `platforms/desktop/run-test.cmake` with Fusion style and isolated temporary profiles. Await asynchronous UI completion before sending the next action, and target exposed paper when dismissing panels. Never add fixed waits over 100 ms; wait for a condition or use an injected clock. Keep public validation independent of private runtime services. Do not restore the old test-host application or scroll-only canvas. Public Nickel compatibility checks are deferred until the user chooses an approach.
- When a Kobo adapter change alters what Kobalt asks Nickel to do, update `tests/pewter/` and its tests in the same change ([Pewter](doc/validation/kobo.md#pewter-the-fake-nickel)). Ordinary Mac builds stay independent of Docker and Kobo checks.
- Use the checked-in Trevelyan's Type Tester fixture for general reading checks and synthetic books for focused regressions. Preserve the pinned international and Gutenberg fixtures and their scenes ([Native validation](doc/validation.md#book-fixtures)). When adding a UI screen that needs screenshot coverage, add its scene in `tests/support/screenshot_gallery.h`, and in `primaryScreens` in `tests/desktop.cpp` if it is in `primaryViews`, in the same change. For targeted view fixes, use `KOBALT_GALLERY_SCENE` (`build/screenshots/mac-scenes/`) rather than regenerating the full gallery, and keep unrelated captures and review feedback. Keep device frames in the HTML viewer only and preserve filenames and checksums in `manifest.json`.
- For release evaluation and performance, use `platforms/eval/run.cmake` and `compare.cmake` (`-DGATE=ON` for the resource gate); see [Release evaluation](doc/release-evaluation.md). Keep eval scripts thin and put end-to-end performance scenarios only in `platforms/eval/scenarios.json`, never as C++ test code. The scenario player ships only in test and eval packages, never in release packages. Keep `DEVICE_RUNNER` generic: everything must also work without a private runner. Never re-record a baseline to make a report pass. Before every release, run `KOBALT_PERF_FILTER=resources. cmake -DPROFILE=release -DPARTS=perf -P platforms/eval/run.cmake` on baseline and candidate with the same diagnostic file policy, and compare with `-DGATE=ON`. Performance runs hold a cross-worktree lock. Kobo containers use at most `KOBALT_DOCKER_CPUS`. Do not lower the priority of native tests or evaluation.
- Never record fixed test, case, assertion or screenshot totals in documentation or these instructions (`ctest --preset mac -N` gives the current inventory). Keep meaningful fixture sizes, limits and measured durations.

## UI integration and review

- Before editing UI, trace the closest existing control or screen: its construction, shared styling, metrics and later presentation adjustments, not just its widget class. Preserve existing typography, caption/value alignment, padding, dividers and interaction unless a change is requested. Use established preview content when suitable.
- New layout controls must use understandable units and visibly demonstrate the setting in their preview.
- Share an early screenshot (entry point and opened state) for new or substantially redesigned controls before broad polishing or gallery generation.
- Before declaring UI complete, compare it directly with the closest existing example: actual font, size, alignment and spacing. Passing tests and generating screenshots do not establish visual consistency. When reviewers are requested, give them these comparisons and require findings or explicit verification for both the surrounding screen and the new control.
- Review visible UI changes through [Screen review](doc/screen-review.md): capture the unmodified base first with `platforms/review/baseline.cmake`, then iterate with `round`, `serve`, `notes`, `reply` and `ask`, and send the user the link. Only the user supplies review notes. Respect questions, accepted behavior and explicitly deferred ideas in the feedback. Keep review data in `build/review/` and stop the server when done.

## Execution and cleanup

- Use sanitizers for bug fixing and regression tests. For a crash or suspected lifetime error, follow [Desktop memory checks](doc/validation.md#desktop-memory-checks). Reproduce the ordinary reader action with a local fixture, fix the cause and repeat the same check. A desktop pass does not validate a Kobo package.
- Run native Qt commands outside the agent sandbox on the development Mac, because the sandbox can hide CPU capabilities that headless Qt tools need. Container commands need engine socket access. Do not open a visible app or run visible UI tests without authorization. Prefer native headless checks while editing, then Kobo and Android last.
- Kobo storage IO is expensive. Devices use eMMC or slower SD cards; measure storage-bound work such as syncs and saves on both, and judge changes by the slower one.
- Stop and reap run-owned helper processes, temporary containers and scratch profiles on exit or failure. Preserve the user's app state, active runs, SDKs and incremental caches, and keep unresolved failure evidence. Remove build and scratch outputs with `cmake -DPATHS="<path1>;<path2>" -P platforms/cleanup.cmake`, never with `rm -rf` on a variable path. It removes only paths inside the worktree's `build/` (outside `build/dependency-cache/`) or an agent scratch folder, and fails when it refuses a path.

## Storage, caches and documentation

- Read [Storage compatibility](doc/storage.md) before changing settings, book state, bookmarks, annotations, caches, backup tools or test database inspection. Never open the live exclusive profile through an external SQLite connection. Keep saved reading data separate from disposable caches. Never tie format versions to release names. Compatible optional fields keep the current version with explicit defaults, and edits preserve supported optional fields. Incompatible saved-data changes require a version bump, a migration that keeps user data, and regression tests. Never overwrite or delete unsupported or damaged saved data; profile preparation moves it into `backups/`. Do not assume older builds keep unknown fields.
- Review cache compatibility on every engine dependency or patch change. Bump `rendering_version` in `platforms/cache-policy.cmake` for repagination-only changes, `documentCacheChecksum` for parser or preprocessing changes. Cache invalidation must never clear user data.
- Keep Markdown in `doc/`, except the root `README.md` (owned by the user) and `AGENTS.md`. Follow [Documentation guide](doc/documentation.md): edit in place rather than appending, do not add no-op entries to `doc/storage/`, and do not add per-assertion test recaps to `doc/validation/*.md`.

## Git and worktrees

- Format first-party C/C++ with `.clang-format` before committing ([Code formatting](doc/building.md#code-formatting)).
- Do not stage, commit, push or publish without explicit authorization. Warn before signed operations that may trigger 1Password. Never add an agent coauthor. Preserve unrelated changes and the original checkout.
- Work in a feature worktree by default. Before the first edit, create branch `feature/<name>` in worktree `worktrees/<name>`, even for small changes. Other agents and the user can edit `develop` at any time, so only read and investigate there unless the user asks you to work there for that task. If you started in `develop` unasked, stop and move only your own changes into a worktree. If one thread asks for several features touching the same code, ask whether to use separate worktrees before editing.
- Integrate into `develop` by cherry-pick or fast-forward only, never with a merge commit. After verified integration, remove the worktree and branch unless asked to keep them. Before removal, check that the worktree has no uncommitted changes, that all intended changes are integrated, and that needed test evidence is preserved.
- Use lowercase release-stage words (`alpha`, `beta`) in commit messages; `RC` may stay uppercase.
