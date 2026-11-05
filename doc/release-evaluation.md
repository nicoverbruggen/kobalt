# Release evaluation

A release evaluation compares a candidate with a baseline. It records the same tests, screenshots and timings for both, and it writes a report of the differences. The person or agent who reads the report decides whether each difference is expected from the commits between the two. Use it before a release and while you make a subsystem faster.

Run every command from the repository root on the development Mac, outside the agent sandbox. Configure the `mac` preset first, as described in [Building](building.md#mac). The evaluation uses the same Qt SDK as that build.

## Record runs

```sh
cmake -DRECORD_BASELINE=ON -P platforms/eval/run.cmake
cmake -P platforms/eval/run.cmake
cmake -P platforms/eval/compare.cmake
```

The first command records a baseline. The second command records the current working tree after your changes. The third command compares the newest run with the baseline and prints the path of `report.md`.

`run.cmake` accepts these options:

| Option | Effect |
| --- | --- |
| `SUBJECT` | `worktree` (default) measures this checkout as it is, including uncommitted changes. A commit, tag or branch name exports that revision with `git archive` into `build/eval/<label>/src` and measures the export. No checkout or branch is created. |
| `PROFILE` | `quick` (default) runs the light tests, the light screenshot set and three performance processes. `release` runs the full native suite, the full screenshot gallery, five performance processes and the device scenarios when a device runner is set. |
| `PARTS` | Overrides the profile's parts: `tests`, `screenshots`, `perf`, `device`. |
| `RUNS` | The number of performance processes. |
| `PATCH` | A patch to apply to an exported revision. It never applies to the working tree. |
| `DEVICE_RUNNER` | The external device runner. `KOBALT_DEVICE_RUNNER` in the environment does the same. |
| `DEVICE_TARGETS` | The device builds to measure. The default is `qt5`. |
| `KEEP_DEVICE` | Passed to the device runner to reuse one prepared device between runs. |
| `RECORD_BASELINE` | Makes this run the baseline for later comparisons. |

A run builds everything first and measures afterwards, so no build competes with a timing. A part that fails, such as a test, a screenshot scene or a performance process, is recorded in `meta.json` and the run continues, so the report still shows everything else. A run that stops early leaves its directory with a `.partial` suffix as evidence; delete it when you no longer need it. Each run writes `build/eval/<label>/runs/<time>/`. A working-tree label contains the commit and, for a modified tree, a hash of the changes. A run directory holds `meta.json`, `tests.xml` from CTest, `tests.log`, a copy of the screenshots and their manifest, one `perf-<n>.json` for each performance process, and `device-<target>/` for each device target. `build/eval/baseline.json` and `build/eval/latest.json` point at the recorded baseline and the newest run.

An exported revision needs its own Mac and performance builds the first time. The export copies the verified dependency archives from the shared [dependency cache](building.md#dependency-cache) and links its screenshot books, so it needs no network. The export builds its dependencies in its own `build/dependency-cache/`, for the Mac and for device builds, so a run never depends on the shared cache. An export of a revision from before the shared cache receives the same archives in its platform build directories. On the measured Mac, configuring and building a new performance directory took about a minute. Remove old exports from `build/eval/` when you no longer need them.

## Compare runs

```sh
cmake [-DBASE=baseline] [-DTARGET=latest] -P platforms/eval/compare.cmake
```

`BASE` and `TARGET` accept `baseline`, `latest`, a label or a run directory. A label selects its newest run other than the target, so the same label on both sides compares its two newest runs. The report goes to `build/eval/compare/<base>__<target>-<run>/`:

- `report.md` lists the timing changes first, then the tests, screenshots, performance, device results and changes.
- `report.json` holds the same data for tools.
- `screens/` holds base, target and difference crops for every changed screenshot.
- `commits.txt` and `changes.txt` list the commits and the changed files between the two subjects.

The report compares tests by name. It lists tests that newly fail, newly pass, were added or were removed.

Device comparisons retain failed and skipped scenario outcomes and their reasons, even when they have no timing samples. A previously completed scenario missing from a candidate device run is reported. A run that did not request or collect device results is not treated as missing scenarios.

The report compares screenshots by checksum. For each changed image, it gives the number of changed pixels and their bounding box. Some screens change on every run by design. `platforms/eval/eval.json` lists them with the reason. The About screen shows the source commit and whether the tree is modified. The annotation-export screens show the export time and a temporary path.

The report compares each timing metric as the median of the process medians. A slow process then counts once, not once per sample. The spread is the median absolute deviation between processes, relative to the median. The verdict is:

- `slower` or `faster` when the change exceeds the metric's band and its absolute floor;
- `noisy` when the change exceeds the band, but the spread of either run is larger than the band and the change is not much larger than that spread;
- `same` otherwise.

`touched_by` lists the changed files under a metric's source paths. A flagged metric that no changed file touches is reported as unexpected.

Each performance process runs under `taskpolicy -t 0 -l 0`. Without it, macOS moves whole processes between fast and slow cores, and medians of the same build differed by up to 70% on the measured Mac. With it, they stayed within about 20%, mostly within 10%. Timings compare only on the same machine. The report tells you to measure again when the machine model, the power source or Low Power Mode differs between the runs. Do not run builds or other heavy work during a run.

## Machine load

Only one evaluation runs at a time. `run.cmake` takes the lock `kobalt-load/evaluation.lock` in the Git common directory, so all worktrees of the repository share it. When another evaluation holds the lock, the run prints `Waiting for another evaluation to finish` and continues when the other run ends.

Each Kobo build holds its own lock file in `kobalt-load/builds/` while it runs. This applies to `platforms/kobo/build.sh` and to the `mac-kobo-checks` target. After its own build step, the evaluation waits until these builds end and prints `Waiting for a Kobo build to finish before the measured steps`. A Kobo build that starts while an evaluation runs prints a message and waits until the evaluation ends. The device builds of an evaluation do not wait, because the evaluation started them. To stop a wait, stop the build or the other evaluation.

The operating system releases each lock when its process ends, so a crashed run or build leaves no lock behind. A lock file that stays on disk without an owner has no effect. The fast check `build.loadLock` covers the evaluation lock, the wait for a registered build and a stale registration in a scratch lock folder, without Docker or a real evaluation.

Other heavy work does not register, for example native test runs, desktop builds, Android builds and device emulators. Do not start it during a run. Kobo build containers also have a CPU limit; see [Building](building.md#kobo).

## Calibration

The default bands in `eval.json` are 15% for micro-benchmarks, 20% for end-to-end scenarios, 25% for device scenarios and 10% for memory. Memory metrics are in KiB and use the `floors_kib` floor; the other metrics use `floors_ms`. Measure the noise on your machine before you rely on them:

```sh
cmake -P platforms/eval/run.cmake
cmake -P platforms/eval/run.cmake
cmake -DBASE=<label> -DTARGET=latest -DCALIBRATE=ON -P platforms/eval/compare.cmake
```

Both runs must measure the same subject. `CALIBRATE` adds the pair to `build/eval/calibration.json`. Each metric's band allows 1.5 times the largest change seen between same-subject runs, measured as a ratio, and at least 5%. Comparisons use these bands once three pairs are recorded; before that, they use the defaults. Record several pairs, at different times, before you treat a verdict as a gate. The evaluation does not fail a build; it only reports.

## Mac performance scenarios

`kobalt-desktop-tests --perf FILE` runs the performance scenarios in one headless process and writes raw samples to `FILE`. `cmake --build --preset mac-perf` builds the RelWithDebInfo directory `build/desktop-mac-perf` and writes `build/desktop-mac-perf/perf/perf.json`. Configure it once with `cmake --preset mac-perf -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.3/macos"`. The first run downloads the pinned War and Peace edition into `build/screenshot-books/`. `KOBALT_PERF_FILTER` selects scenarios by the start of an identifier. `KOBALT_PERF_REPETITIONS` sets the repetitions; the default is five. `KOBALT_PERF_EVENTS=FILE` keeps the reader's diagnostics events from the run in `FILE`, for audits of a single scenario. Use an absolute path, because the process runs in a temporary folder. The reader rotates its log at 128 KiB, so `FILE` holds only the newest part of a long run. `kobalt-desktop-tests --watch` runs the scenario file in a visible window at real speed and prints each scenario as it starts, so a person can follow the run. It saves no timings. It is the only mode that opens a window, so run it only when someone asked to watch.

Micro-benchmarks lay out and draw text with new content in each sample, so the layout cache cannot hide the work: a dictionary entry, a footnote, the AGPL notice, a page draw, a wrapped label and an elided title.

All other scenarios come from `platforms/eval/scenarios.json` and run through the reader's built-in scenario player, in the production desktop window. A scenario names its book, optional `setup` steps that run once, the `steps` of one sample, `after` steps that tidy up between samples and `teardown` steps that run at the end. Steps press a control by its object name, hold a word or a link found by its text, send a page key, wait for a control to appear or close, wait for the reader to settle, wait for a diagnostics event, let a fixed time pass (`wait_ms`), and open or close a book. On a Kobo, `wait_ms` counts real time, including time in standby. The player keeps the device awake for the whole run: Kobalt's `PowerManager::canSuspend` hook answers false, so Nickel's standby does not stretch waits and samples. A kernel wake lock does not work for this, because Nickel reads `/sys/power/wakeup_count` on its GUI thread before each suspend and that read waits while a lock is held, which freezes the reader and the run. A scenario that sets `"standby": true` lets the device suspend while it runs. An event step can require more fields with `where`, such as `{"operation":"wifi-off"}`. With `"earlier": true` it also matches the events that earlier steps of the same sample skipped or passed over, for work that can end before or after the step. With `as`, the sample refers to the matched event by that name, so a sample can keep several events of one kind. A `mark_end` step ends the time since `mark`, so later steps can wait for more events without adding to the sample. A `native-sync` step starts the host's own store sync, as its sync button does, and does not wait for it to end. When a sync from an earlier sample still runs, the step first waits for it to end, at most 5 minutes or the step's `timeout`. A scenario that needs it lists `native-sync` in `needs`; the Mac player skips it. A `{"wifi":"off"}` step turns Wi-Fi off on the host and waits until the radio is down, at most 15 seconds or the step's `timeout`. It does nothing when Wi-Fi is already off. On a Kobo it calls Nickel's `WirelessManager::off()`, the same call that KOSync makes after a sync that it connected for. The call waits on the main thread in its own declared scope, `scenario wifi step`, and is timed as `scenario-wifi-off`, so it is not mistaken for KOSync's `wifi-off`. A host without it skips the scenario; the Mac player is such a host. A `kosync` step sets a KOSync account in memory for the rest of the scenario. Its optional `interval_ms` replaces the time between automatic syncs while reading, so a scenario does not wait 30 seconds for each sync; the saved account is not read or written, and the account ends with the scenario, also after a failure. The reader has settled when its engine has no work queued or running, including drawing the pages beside the current one. A page key sent earlier waits for that drawing or misses the page cache, so a sample would measure the race instead of the turn. A list of names or points in a step takes the next entry in each sample. The sample is a field of a diagnostics event, the time since a `mark` step, or the process's memory from a diagnostics event. The player sends taps and holds through the window system, as real input arrives. Page keys call the reader directly and skip its filter for page turns less than 200 ms after the last one, so a sample measures the turn and not the filter. It is compiled only into the test executable and into evaluation packages.

The scenarios cover book opening and closing, page turns, a page turn back into the previous chapter, the reading menu, a text-size change, long and short dictionary entries, a long footnote, Settings, Acknowledgments, the licence notice and memory use. The word-spacing and tracking scenarios open their Styling sheets and wait for the sample to paint. Their panel-open events use panel styling and include sample_ms for construction and the wait for the rendered sample. Pending sample renders, including cancelled jobs, keep the reader busy until their results reach the GUI thread. They use the benchmark book, a small notes fixture and War and Peace. On the Mac, a substituted dictionary service gives "set" a long entry and other words a short one, as a real English dictionary does.

`book.open` closes the open book and opens it again, so the parsed book comes from the cache, and samples `reader-shown`. `book.close` turns a page and closes the book, and samples `book-close`. On the measured Mac, a close took about 125 ms while picker fonts were added to Qt's font database, because removing them made Qt rebuild that database. With private picker fonts it takes about 22 ms. `book.open.large` removes the disposable caches and opens War and Peace, so each sample parses and lays out the whole book. `reflow.font-size` taps the larger and smaller text-size buttons in the reading menu, in turn. While the menu is open, only a preview of the current chapter reflows, so the scenario times the menu's close until the reflowed book page has rendered. It also records the engine's `render_ms`. `memory.reading` records the resident size after the reading scenarios, and `memory.peak` the peak resident size at the end of the run, both in KiB. Both depend on what ran earlier in the process, so compare them only between runs with the same filter. `memory.kobalt.reading` and `memory.kobalt.peak` record the same values minus the resident size when the first book began to open. The reader logs that base as `base_rss_kib` in a `memory-base` event. On a Kobo the base is Nickel with Kobalt loaded but not yet reading, so these two show Kobalt's own share without the host, and Qt 5 and Qt 6 compare directly. On the Mac the base is the app before its first book.

The `panel.styling.widows` and `panel.styling.page-spacing` scenarios separate entry from prepared previews. The `.entry` timer starts before pressing Styling and ends when the preview has painted, including tab entry and preparation of the requested preview. Its stages also report the modal event and sample wait. The `.prepared` scenario opens and closes the requested sheet once before measuring cached reopening. Compare both metrics; timing only the modal can hide work moved into the tab.

`panel-open` events time a panel from the input that opens it until its first paint has finished. They record `total_ms`, the synchronous `open_ms` and `paint_ms`, plus `range_ms` and `lookup_ms` for the dictionary and `details_ms` for a footnote. `page-turn-painted` records the turn until the page paints. `render_ms` ends when the reader starts to paint: it covers the engine's page and the wait on the GUI thread. `paint_ms` covers the changed area, the paint and the flush. On some screens the flush only queues the update, and the screen composes it on the GUI thread later. That time is not part of the event, but a following input waits for it.

`reader-shown` times a book opening from the open request until the first reader page has painted. `total_ms` is the sum of four stages:

- `engine_ms`: until the reading engine has laid out the book. On Kobo, `prepare_ms` gives the part before the engine started.
- `window_ms`: creating and sizing the reader window. `build_ms` is the window's own construction, with one `build_<part>_ms` per part, such as `build_settings_ms`.
- `wait_ms`: waiting until the window may be shown, for example for a pending sync.
- `show_ms`: from handing the window over until its page has painted. `theme_ms` is the first theme pass inside it.

`book-close` times a close from the close request until the reader has finished closing. `sync_ms` is the time spent on storage: `checkpoint_ms` for the reading position checkpoints, `native_ms` for the progress written back to the native library, `session_ms` for ending the native reading session and, on Kobo, `profile_ms` for closing the profile database. `total_ms` covers the whole close.

`sleep_capture_ms` measures the synchronous sleep backdrop capture. `sleep_dispatch_ms` measures handing that backdrop to the platform. Kobo's committed-token lookup and persistence run on a worker and can continue after `book-close`; they are not included in `sync_ms`.

These events do not sync the log to storage, so they do not slow the reader.

## Device scenarios

A Kobo runs the same scenario file through the same built-in player. `bash platforms/kobo/build.sh qt5-eval` (or `qt6-eval`) builds an evaluation package, `build/kobo-qt5-eval/Kobalt-qt5-eval.zip`. It is the normal package plus:

- the scenario player, compiled in;
- the benchmark book in `kobalt-eval/`, a folder that the library lists;
- the scenario file in `.adds/kobalt/scenarios/`;
- `.adds/kobalt/diagnostics-enabled`, which adds the state and session reports.

The release packages never contain these. Only books marked `"package": true` in the scenario file go into the package. The other books are not used on a Kobo, because the player cannot open them there.

When the scenario file exists, the player starts two seconds after the first book opens in Kobalt, once per start of the reader. It writes one line per scenario to `.adds/kobalt/diagnostics/scenarios.jsonl`, after a header line with the target, version, firmware and model. That file does not rotate, unlike the event log. A `scenario-run-end` event marks the end. The reader then shows "Benchmark finished" or "Benchmark failed", with the number of completed, skipped and failed scenarios, the first failure and where to find the results over USB. A run fails when a scenario failed, no scenario ran or the results could not be saved. When the run lost its reader, the next opening shows the notice.

On a Kobo, books open from the library. The player can close the first book and open it again from a copy of its opening request, but it cannot open another book. The large-book and footnote scenarios are skipped with that reason. Dictionary scenarios are skipped when no dictionary is installed. The player sends its presses directly to the widget under the point: Kobalt's touch filter on Qt 5 stops translating touch after it sees a synthesized mouse press in its windows.

To measure a Kobo connected over USB:

```sh
bash platforms/kobo/build.sh qt5-eval
cmake -DPACKAGE=build/kobo-qt5-eval/Kobalt-qt5-eval.zip -DDEVICE=/Volumes/KOBOeReader -P platforms/kobo/install-eval.cmake
cmake -DDEVICE=/Volumes/KOBOeReader -DOUTPUT=<run>/device-qt5 -P platforms/kobo/collect-eval.cmake
```

The install script warns when the Kobo has no dictionary. Between the two commands, eject the Kobo, let it restart, open ★ Kobalt Benchmark in Kobalt and wait for the run to end. KOSync stays paused while the scenario file is on the Kobo, so the run does not read or send the reading position. Only a scenario with its own `kosync` account syncs, and only with that account. The collect script copies `scenarios.jsonl` and the event log into a run's `device-qt5` folder. `compare.cmake` then reports the device timings like the Mac ones. Device timings compare only with runs on the same model, storage and firmware.

The evaluation can also drive a device through an external runner, a program outside this repository. Pass it with `DEVICE_RUNNER` or `KOBALT_DEVICE_RUNNER`. `run.cmake` builds each target's evaluation package and calls:

```sh
<runner> run --package <Kobalt-qt5-eval.zip> --target qt5 --output <run>/device-qt5 [--keep-device <name>]
```

The runner installs the package, opens the test book in Kobalt, waits for the `scenario-run-end` event and copies `scenarios.jsonl` into the output folder. It exits with zero when it wrote the file, even if a scenario failed. The comparator still reads `device.json` from runs recorded by an earlier runner.

### Wi-Fi off and first open

The opt-in `kosync.wifi-off` scenario measures how long Nickel's Wi-Fi shutdown blocks the main thread after a KOSync sync that Kobalt started. It needs a device with a saved Wi-Fi network. The scenario closes the open book, then sets an in-memory KOSync account that syncs every page, at most once a second (`interval_ms`), and may turn on Wi-Fi. Its server address is on the device itself, where nothing listens (`https://127.0.0.1:9`). Each sample first turns Wi-Fi off, because a connection that is already up is not one that Kobalt turned on. It then waits 1.5 seconds and turns a page. Kobalt then turns Wi-Fi on for the sync, the request is refused at once (a periodic sync makes one attempt), and Kobalt turns Wi-Fi off again. The sample is the `main-thread-wait` of that `wifi-off`. Kobalt's KOSync request stays on the device, and the saved KOSync account is neither used nor changed. Nickel's own connection checks still run while Wi-Fi is on. The scenario needs a host that can turn Wi-Fi off through Nickel, so the Mac player skips it. A sync that succeeded would report a storage error, because the scenario account is never saved; with this server address, no sync succeeds.

Two other measurements come from the same run's event log. The book opening after the installation restart includes the bundled-asset check, which later openings skip; it is the last `reader-shown` event before `scenario-run-start`. When Wi-Fi is on and a book opens while automatic KOSync is off, the Wi-Fi power saver turns it off and logs a `main-thread-wait` with the allowance `wifi-off before reader input`; during a scenario run KOSync is paused, so this happens at that same opening. Kobo leaves the power saver out until beta 7, so until then the log has no such wait. The log can also hold earlier sessions, so always take the events just before `scenario-run-start`.

From beta 7, check before installing that Kobalt's Settings › System › Power management › Smart Wi-Fi power saving is on. It is off by default. Checking it after the restart would mean opening a book, and any opening starts the run: with another book the run is skipped, and with the benchmark book it starts before Wi-Fi is on.

Add `-DREPEAT_SCALE=0.2` to the install command for one sample instead of five, when the run only has to show that the scenario works.

For firmware 4:

```sh
bash platforms/kobo/build.sh qt5-eval
cmake -DPACKAGE=build/kobo-qt5-eval/Kobalt-qt5-eval.zip -DDEVICE=/Volumes/KOBOeReader -DSCENARIO_FILTER=kosync.wifi-off -P platforms/kobo/install-eval.cmake
```

For firmware 5 and 6:

```sh
bash platforms/kobo/build.sh qt6-eval
cmake -DPACKAGE=build/kobo-qt6-eval/Kobalt-qt6-eval.zip -DDEVICE=/Volumes/KOBOeReader -DSCENARIO_FILTER=kosync.wifi-off -P platforms/kobo/install-eval.cmake
```

Then:

1. Eject the Kobo and let it restart.
2. Turn Bluetooth off in Nickel. With Bluetooth on, the saver leaves Wi-Fi on and the scenario cannot measure.
3. Turn Wi-Fi on and wait until it is connected. If Nickel starts a sync, wait until it ends: the saver leaves Wi-Fi on while Nickel syncs.
4. Within a minute, open ★ Kobalt Benchmark in Kobalt. Nickel turns idle Wi-Fi off after about two minutes, and then the saver has nothing to measure. This must be the first book opened after the restart. The saver turns Wi-Fi off, and the run takes about four minutes.
5. Collect the results with `collect-eval.cmake` as above, into `<run>/device-qt5` or `<run>/device-qt6`.

`scenarios.jsonl` holds the five KOSync samples. If a sample fails with "no kosync-network-finished event", Wi-Fi was already on when the page turned, or it did not connect within KOSync's ten-second limit. "No main-thread-wait event" means Kobalt connected but did not turn Wi-Fi off.

### Page turns during a Nickel sync

The opt-in `native-sync.page-turn` scenario checks if a page turn waits for Nickel while Nickel saves its reading events during a sync. When a book opens or closes, Kobalt saves Nickel's pending reading events on Nickel's event thread. This save waits up to two seconds for Nickel's database. On each page turn, Kobalt reports the page to Nickel on the main thread. If the two share a lock, a turn right after an opening can wait for the save.

Each sample starts Nickel's own sync, as the sync button in Nickel's status bar does. On firmware 4.46.23836 and 6.0.279225 that is `N3SyncManager::sync()`. Kobalt looks up this function at run time. If the firmware does not have it, the step fails with that reason. The sample then closes the benchmark book and opens it again, which saves the events of the old session and of the new one, and turns five pages forward without a pause. The sample is the time from when the reopened book is ready until the fifth page has painted. It includes any time that the main thread waited between the turns. Its stages are each turn's `elapsed_ms` (`turn1_ms` to `turn5_ms`), the two event saves (`stop_save_ms` from the `operation-end` of `nickel-session-stop-flush`, `start_save_ms` from that of `nickel-session-start-flush`; each brackets one `nickel-events-save`) and the time until Nickel's sync ended (`sync_ms`, checked every 100 ms). The stages of one sample are at the same position in each list, so a slow turn can be matched to a slow save. Each sample waits for its sync to end. Then the scenario turns five pages back, so each sample starts at the same page. The event log has `native-sync-start` and `native-sync-end` around each sync, with the saves and turns between them, so it shows whether the sync was still running during the save. `native-sync-start` also has `call_ms`, the time that the call to Nickel took on the main thread.

The scenario contacts the store, so it needs a Kobo that is signed in to a Kobo account. Do not use sideload mode: there Nickel ignores page events, so a turn cannot wait for the save. Nickel turns Wi-Fi on for the sync if it is off. A sample takes about as long as one Nickel sync.

For firmware 4:

```sh
bash platforms/kobo/build.sh qt5-eval
cmake -DPACKAGE=build/kobo-qt5-eval/Kobalt-qt5-eval.zip -DDEVICE=/Volumes/KOBOeReader -DSCENARIO_FILTER=native-sync.page-turn -P platforms/kobo/install-eval.cmake
```

For firmware 5 and 6, use `qt6-eval` and `Kobalt-qt6-eval.zip`. Then eject the Kobo, let it restart, open ★ Kobalt Benchmark in Kobalt and wait for the run to end. Collect the results with `collect-eval.cmake`.

If a sample fails with "Nickel did not start a sync", Nickel refused the sync, for example because the battery is low. "No operation-end event" means that Kobalt did not save Nickel's events for one of the two sessions, for example because Nickel's storage was not available.

## Device reference

`platforms/eval/devices/` keeps benchmark runs from real devices as a reference across devices. [Device benchmarks](device-benchmarks.md) explains the files and compares the devices.

## Sleep persistence benchmark

The optional `sleep.close` scenarios compare book exit with the stock screen, the current page and the cover. They use production persistence: lossless level-1 PNGs for pages and native-cover references when available. Each mode has one warmup and three measured trials. The setup selects Chapter 1 and turns forward once, so Page measures text instead of the opening cover. In-memory mode overrides never change saved preferences. Covers must finish decoding before a sample; a missing cover fails the case instead of measuring a fallback page.

Build and install the firmware 4 evaluation package with the filter:

```sh
bash platforms/kobo/build.sh qt5-eval
cmake -DPACKAGE=build/kobo-qt5-eval/Kobalt-qt5-eval.zip -DDEVICE=/Volumes/KOBOeReader -DSCENARIO_FILTER=sleep.close -P platforms/kobo/install-eval.cmake
```

Use the actual device mount path. Eject, restart and open ★ Kobalt Benchmark as described above. Collect results with the normal `collect-eval.cmake` command. These scenarios do not run without a filter. The desktop player skips them because it has no Nickel sleep persistence path.

Each trial samples the real `book-close` event, then drains the pending save before reopening. `persistence_kind` distinguishes a cover `reference` from a `png`. Details retain PNG `encode_write_ms`, atomic `commit_ms`, file `bytes`, synchronous `prepare_ms` and `invalidate_ms`, and the remaining `drain_ms` after close. The worker's `token_ms` is split into `token_open_ms`, `token_query_ms` and `token_close_ms`. Worker timings overlap book exit; do not add them to the exit time. A missing or failed custom-screen save fails the trial. Stock has no image write. Home sleep still performs a fresh committed-token check, which this close benchmark does not time.

PNG details record the selected codec API and argument. Production uses zlib level 1: modern Qt takes compression argument 11, while Qt 5.2 takes quality 89. Both are lossless. The benchmark does not override compression or write separate test images.

Results retain warmup and measured trials, including failures, in `details`. Warmups are excluded from timing samples. Compare the same device and firmware, and repeat with reversed scenario order to check drift. Test slower SD-backed devices separately before generalizing from eMMC.

From `platforms/desktop/`, run the focused native checks:

```sh
cmake --build --preset mac --target kobalt-sleep-persistence-tests kobalt-scenario-sleep-tests
ctest --preset mac -R '^platform.(sleep-persistence|scenario-sleep)$'
```

## Evaluate a candidate

Read `report.md` from the top and decide:

1. A failed part at the top of the report, or a newly failing test, blocks the release until it is explained. A removed test needs a reason in the commits.
2. Each changed screenshot is explained by a commit, or it is listed as unexplained. Inspect the crops in `screens/`.
3. Each `slower` or `faster` metric is checked against `touched_by` and the commit messages. An intended speed-up or an accepted cost is expected. A change that no commit explains is unexpected.
4. A comparability warning, or a `noisy` verdict on a metric that matters, means measure again with more `RUNS`.
5. State the verdict as ready, not ready or needs review, with the reasons.

Never re-record the baseline to make a report pass.

Linux uses the same evaluation scripts. They select the host’s `linux` or `mac` presets, build directories and galleries automatically. Configure the corresponding desktop preset first, or pass `QT_PREFIX` for a prepared native Qt SDK. Linux records kernel, architecture, hostname, core count and initial load; power state is recorded as unknown. Keep evaluation baselines on the same host and Qt SDK.

Evaluation uses the normal logging durability policy. Older captures that disabled diagnostic-log sync are not directly comparable for storage traffic; record that policy difference when comparing them.
