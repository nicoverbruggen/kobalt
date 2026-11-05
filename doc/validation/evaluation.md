# Performance and evaluation checks

Label `evaluation`. Build `mac-tests`, then run `ctest --preset mac -L evaluation` from `platforms/desktop/`. `ctest --preset mac -N -L evaluation` lists its tests.

These checks cover the performance-mode timing events, the release-evaluation result formats, the evaluation lock and the scenario player's sleep, KOSync and Wi-Fi steps. `platform.scenario-sleep` runs the scenario player with a controlled host and no reader window. [Release evaluation](../release-evaluation.md) describes the comparison workflow.

## Performance and release evaluation

`kobalt-desktop-tests --perf FILE` measures text layout and drawing, book opening and closing, page turns, panel openings, a text-size reflow and memory use in the production desktop window. `cmake --build --preset mac-perf` runs it from the RelWithDebInfo directory. The `panel-open` diagnostics events time each panel from its input to its first finished paint, and `reader-shown` and `book-close` time book opening and closing in stages. [Release evaluation](../release-evaluation.md) records tests, screenshots and these timings for a baseline and a candidate, and it reports the differences.

Set `KOBALT_LAYOUT_TIMINGS=1` for the alternate-layout executable to run an engine micro-measurement with the EPUB path supplied as its argument. It reports loading once and then separate cold and reused layout and visible-page paint samples for each viewport. The probe uses the production model and evicts the alternate snapshot before each cold sample. Reused samples report their full layouts in `formats`, so a missed reuse shows as 1. Set `KOBALT_LAYOUT_SETTINGS` to a JSON object of saved settings for every opening, for example `{"two_columns_landscape":true,"hanging_punctuation":true}`. Set `KOBALT_LAYOUT_DISK_CACHE=1` as well to prepare the existing disk document cache before sampling; its one-time save has its own timing. The probe then saves the landscape layout and reports five `landscape-cached-open` samples, with the opening time, the full layouts and whether the book came from the cache. Use the headless test launcher for isolated temporary state. This is not an end-to-end rotation scenario or a device benchmark.

## Measuring

Measure with `/usr/bin/time -p` around the commands above. Report the first build of restored targets separately from an unchanged build. A dependency recipe or patch change selects a new dependency set; application-only changes reuse it. A new worktree takes its set from the shared [dependency cache](../building.md#dependency-cache) when another worktree already built it, so its first build compiles only application objects. Do not compare different test inventories or SDKs as a measured speedup. Linux uses the same native targets and cleanup launcher through the `linux` presets; see [Linux builds](../building.md#linux). Keep host and Qt version consistent when comparing timings or image checksums.

## Measured Mac loop

Measured locally with Qt 6.11.3 and two build/test workers, using the existing dependency cache. These are development measurements, not cold SDK or dependency build times. The table identifies earlier measurements where the workload has since changed.

| Operation | Elapsed | Scope |
| --- | --- | --- |
| Unchanged fast test build | 0.04 s | Current native targets already compiled |
| UI source edit and fast-lane rebuild | 1.64 s | One UI file compiled, desktop test executable linked |
| Fast checks | 9.12 s | Fast lane |
| Earlier integration checks | 36.19 s | Before additional engine coverage |
| Earlier full native suite | 70.32 s | Before lifecycle restoration |
| Previous full native suite | 135.76 s | Overlapped the Qt5 application build |
| Full native suite | 141.13 s | Includes the selected UI, platform and literary regressions |
| Unchanged full test build | 0.04 s | Existing application, dependency and test objects reused |
| Initial shared UI test build | 8.03 s | New test source and executable; existing reader objects reused |
| Earlier lifecycle checks | 46.35 s | Before additional annotation and paint coverage |
| Lifecycle test source rebuild | 4.31 s | Only the new test executable rebuilt; application objects and position codec reused |
| Restored engine checks | 15.84 s | Reader, document cache, publisher markup, local reader and document word count |
| Screenshot repeat | 15.62 s | PNGs, gallery and manifest, using corrected Type Tester |
| App relink from current objects | 0.21 s | Interactive app linked; no window opened |
| Qt5 build and package | 121.50 s | Prepared kobuild SDK and existing application dependencies |
| Qt5 unchanged rebuild | 1.55 s | Same SDK and dependency cache; no application recompilation |

The touch-rotation regression failed on the reported Clara HD firmware before the fix and passed afterward. Moving the document-cache matrix into its own check reduced the reader process from a borderline 20-second deadline to about 13 seconds without changing the limits. The earlier engine-restoration build took 28.43 seconds with existing application objects and dependencies; it compiled test sources and refreshed build metadata rather than rebuilding the engine or Qt. A stalled nested child was killed and reaped in 20.85 seconds, and its temporary profile was removed.

Three consecutive screenshot runs with the corrected Type Tester fixture, fixed bookmark dates and fixed dictionary free space produced identical manifests and image checksums. About captures change with the source commit.

The Qt5 package passed ZIP integrity checks and contains the ARM32 library at `usr/local/Kobo/imageformats/libkobalt.so` in `KoboRoot.tgz`. The prepared SDK prints a CMake developer warning for `COPY_ONLY` in its resource macro; this did not prevent compilation or packaging. This check does not run the package on a device or establish firmware compatibility. Neither the Mac nor the Qt5 validation rebuilt Qt.
