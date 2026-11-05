# Installation and files

## Builds and firmware

Kobalt has two Kobo builds. Each build matches the Qt version of its firmware.

| Build | Firmware | Qt | System package |
|---|---|---|---|
| `qt5` | 4.38.21908 and later 4.x | 5.2 | `KoboRoot.tgz` |
| `qt6` | 5.18 and later 5.x, and 6.x | 6.5 | `Kobo.tgz` |

The first public beta, `v1.0-beta.1`, includes only the Qt5 package. Qt6 remains a development build.

No build checks the device model or display driver. At startup, each build checks its required Nickel symbols and the registered size of Nickel's `Volume` type. The `qt5` build finds the unexported orientation singleton by its machine code and requires it to match exactly once. If a check fails, Nickel keeps its own reader. A release that keeps every symbol but changes another object size or a virtual-table slot is not detected.


## Installation

For the first public beta, download `Kobalt-qt5.zip` from the release. To build a firmware 4 package locally with release logging, run `KOBALT_VERBOSE_DIAGNOSTICS=OFF bash platforms/kobo/build.sh qt5` and use `build/kobo-qt5/Kobalt-qt5.zip`. Without that setting, the build enables verbose diagnostics and produces `Kobalt-qt5-diagnostic.zip`.

1. Extract the full ZIP on your computer and show hidden files.
2. Copy the complete `.adds` folder from the ZIP to the root of the Kobo drive. Merge folders and replace matching package files. Keep other apps, settings and reading data.
3. Copy the complete `.kobo` folder from the ZIP to the same drive root. Merge folders. It contains `KoboRoot.tgz` for firmware 4 or `Kobo.tgz` for firmware 5 and 6. Do not unpack this archive yourself.
4. Eject the device safely, disconnect USB and let Nickel install the update.

Copy every file supplied in the ZIP, keeping its folder path. Copy `.adds` first so the required resources are present before the update starts. Kobalt creates its support files and `.adds/kobalt/.initialized` installation receipt on first startup. Updates move the older `.adds/.kobalt.initialized` receipt into that folder.

The system package holds only `libkobalt.so`. Fonts, hyphenation dictionaries and the asset manifest go to `.adds/kobalt/assets/`. `manifest.json` names the release version and the size and SHA-256 of each file. If an asset is missing, damaged or belongs to another version, Kobalt keeps its native settings and book-menu reader choice available. Trying to open a book with Kobalt shows a Nickel system dialog titled “Kobalt can't start”. It explains the resource problem, asks you to reinstall Kobalt and shows the installed version. OK returns to the library without opening the book. The event log records `assets-unavailable`. If asset validation fails and the assets folder contains `FSCK0000.000`-style recovery files, the dialog explains that the filesystem may have recovered files under different names. The event log also records `asset-filesystem-recovery`. Kobalt first checks recovery files in the missing asset’s directory. If a file’s size and SHA-256 match the manifest, it renames that file to restore the missing asset and continues validation. It never replaces an existing asset or changes an unmatched recovery file. This restores filenames only; it does not repair the filesystem. An update replaces the managed asset files and keeps settings and saved positions.

Updates from early test builds that used a different plugin name require manual removal of that older plugin and its failsafe copy before installing Kobalt. The current build does not migrate or remove older plugin names.

## Removal and recovery

A normal build stays installed across restarts. To remove it, delete `.adds/kobalt/uninstall` or the whole `.adds/kobalt` folder, then restart Nickel. Removal keeps other files in `.adds/kobalt` unless you deleted the folder. On supported Kobo firmware, an initialized Kobalt watches a confirmed USB session for removal of the `uninstall` marker. After storage remounts and Nickel finishes processing, it shows “Removing Kobalt” and “Restarting your device...” and requests a normal system restart after three seconds. The message lasts ten seconds so it stays visible beyond the restart delay and into shutdown. Restarting does not depend on whether Nickel can display the message. If Kobalt is inactive, restart the device yourself.

A previously installed temporary build writes `.adds/kobalt/uninstall-now` before it activates. The next Nickel start removes the library, also after a crash.

Kobalt uses two session markers:

- `startup-session` exists during a synchronous initialization attempt. A crash during that attempt leaves the marker. Waiting for Nickel or a usable screen, or deferring a completed attempt, does not leave it.
- `active-session.json` exists while a book is open or opening. A clean exit, a cancelled opening, a fallback to Nickel, power-off and reboot remove it.

The startup marker records an unfinished Kobalt initialization attempt. One interrupted attempt permits a retry. After two consecutive interrupted attempts, NickelHook removes Kobalt before loading Nickel libraries or installing hooks. A completed or deferred initialization, or a recognized normal shutdown, breaks the streak. A crash in the stock reader after Kobalt initialized does not count. The streak does not expire while the user waits or forces a restart after a hang. This is a narrow startup safeguard, not detection of every possible reboot loop.

Removal preserves books, settings and reading data. Before removal, Kobalt tries to save `startup-removed.txt` in its support folder. This file explains the removal and how to retry by installing Kobalt again. A failed notice write does not block removal. Successful or deferred initialization clears the notice.

An empty or damaged regular startup marker counts as one interrupted attempt and permits one retry. The exact unversioned marker and valid timestamped v1 markers remain supported. An unreadable or oversized marker, a directory or a symlink blocks initialization without requesting removal. Inspect the marker first, then stop Nickel and remove it to retry. A leftover `active-session.json` does not block startup.

NickelHook also parks the library outside `imageformats` for the first three seconds after Nickel starts. A crash in that interval leaves the library parked and inactive. If the restore thread cannot start, the library stays parked for the next startup. Failed initialization restores installed hooks in their original libraries. Install a fresh package to recover. Do not move a parked library back by hand.

To install a new build over a temporary build, copy the new package, then delete `.adds/kobalt/uninstall-now` before you eject. If you leave it, the next start removes the new library.

Kobalt waits for Nickel to finish initialization before creating its reader controller. If Kobalt misses the completion callback, it can recover by checking the existing Power Manager and Nickel's application connection. The check does not create the manager. An unrecognized native layout cannot enable recovery. If native readiness is still unconfirmed after 30 seconds, Kobalt records `startup-deferred` and stops periodic checks. The built-in reader remains available. A later completion callback or book-opening attempt can still initialize Kobalt. Recovery records `startup-recovered` with reason `late-native-initialization` and resumes bounded retries if the screen or native services need more time. A book already opened in the built-in reader stays there until it is closed and opened again.

## Files on user storage

Support files are in `.adds/kobalt/`. Annotation exports are in `Kobalt/Annotations/` at the root of the Kobo drive.

| File | Contents |
|---|---|
| `assets/` | Bundled fonts, hyphenation dictionaries and `manifest.json` |
| `config.json` | `enabled`, the Open books in Kobalt choice |
| `reader-settings.json` | Settings that apply to every book: display options, Bold Reading, default reading font, interface preferences, new-book inheritance options and the last text/layout and header/footer settings |
| `userstyle.css` | Optional user CSS for every book; create and edit this file yourself |
| `<hash>.json` | One file per book: saved position, typography, header and footer preset, orientation, dictionary choice and the Nickel progress baseline |
| `annotations/<key>/` | One folder per book with `bookmarks.json`, `highlights.json` and `book.json`, see Bookmarks and annotations |
| `annotations/.index/paths.json` | Disposable bookmark lookup index, at most 4 MiB; rebuilt from book folders when missing or invalid |
| `reopen-layout.json` | Up to 20 layout-check results, keyed by archive digest, at most 8 KiB |
| `.cache/` | Parsed books and their pages, so a book that was opened before opens without parsing. At most 128 MiB; the oldest books are removed first. Kobalt deletes any other file in this folder |
| `crash-reporting/` | One report for each of the last 20 crashes of Nickel, see below |
| `diagnostics/events.jsonl`, `diagnostics/events.jsonl.previous` | Event log, two files of at most 128 KiB each. With verbose diagnostics, the current file and five older files of at most 16 MiB each, and `session-context.json`. See [Page-turn diagnostics](../validation/diagnostics.md#page-turn-diagnostics) |
| `scenarios/scenarios.json`, `scenarios.jsonl` | Evaluation packages only: the performance scenarios that Kobalt runs after the first book opens, and their results. See [Release evaluation](../release-evaluation.md#device-scenarios) |
| `diagnostics/state.json`, `diagnostics/session.json` | Only when a `diagnostics-enabled` file exists or the build has verbose diagnostics: reader state from the last close, sleep or shutdown, and session status. Create `live-state` to write `state.json` on every save instead |
| `startup-session`, `startup-removed.txt`, `active-session.json` | Recovery markers, see above |
| `uninstall`, `uninstall-now` | Removal control, see above |
| `doc` | Short user instructions |

The event log records startup checks, opening stages, timings, memory and save results. The startup event includes the firmware version and hardware UUID when available. The hardware UUID identifies the model, not an individual device. Startup events also record hook availability, entry and return of Nickel initialization, periodic wait state and book-opening retries. Archive inspection failures include the specific `error_code` in the `native-resource-failed` event with stage `publication-inspection`. It does not record book text, titles, file paths, serials or account data. A position fallback records the name of the chapter file inside the book. A `descriptor-exhaustion` error names only system files, files in mod folders such as Kobalt's on Kobo, and the files directly in Nickel's `.kobo` folder; any other open file, including store books and imported books, appears as its extension, such as `other file .epub`. `state.json` and `session.json` can contain book identifiers, so keep them private. The log is always on. A separate thread writes it, so logging never delays a page turn. It shows the last recorded operation, except for events still queued when a crash occurs; a crash report shows where a crash happened.

Kobalt installs a handler for fatal signals in Nickel when it starts. Each crash of Nickel, in Kobalt or elsewhere, writes `crash-reporting/crash-<date>-<time>-<pid>.log`. The `.log` extension keeps Nickel from importing reports as TXT books; firmware 6 imports `.txt` files even in `.adds`. The date and time come from the device clock. The report has the Kobalt version and commit, the firmware and model, the signal, the registers, a backtrace, the return addresses found on the stack and the executable mappings. Each address shows its library and the offset in that library. A last section names the nearest exported symbol where one exists. Kobalt's own functions are not exported: resolve their offsets with `addr2line` and the unstripped `build/kobo-qt5/libkobalt.so` or `build/kobo-qt6/libkobalt.so` of the same commit. Kobalt keeps the newest 20 reports and removes older ones. After the report, Nickel's own crash handling continues as before. Kobalt writes no report while the storage is exported over USB. A report contains no book text or titles.

On Kobo models with a physical Home button, pressing it closes Kobalt and returns to Nickel's Home screen. Kobalt saves reading progress before leaving. Any progress or finished-book choice shown during a normal exit still applies. Sleep cancels a pending return to Home and restores any exit choice after wake. In sideload mode, Nickel returns to My Books.


## Hardware validation

Device testing includes installation and reading on Clara Colour, Clara BW with firmware 4.45 and Libra 2 with firmware 4.38. Manual rotation and touch targets were confirmed on Clara BW with firmware 4.45. Page animations with NickelDissolve installed were confirmed on Clara BW with firmware 4.45. Dark-mode refresh quality, sleep with a PIN, rotation on other models and firmware versions, page buttons on Libra Colour, the Home button on Touch models and server sync still need hardware validation.
