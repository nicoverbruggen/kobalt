# Power mechanisms

This document describes what Kobalt does on a Kobo that costs power: its timers, how it learns about changes, and when it turns Wi-Fi on or off. It also describes Nickel's own Wi-Fi rules, because Kobalt's choices depend on them. For a plain-language summary, see the [user guide](guide.md). Update this document in the same change as the code it describes.

Firmware 4.x runs the Qt 5 package (`platforms/kobo/qt5/`). Firmware 5.x and 6.x run the Qt 6 package (`platforms/kobo/qt6/`). Shared Kobo code is in `platforms/kobo/common/`. Firmware details that come from a specific version name that version.

## Principles

- **Signals before polls.** Kobalt reacts to Nickel's signals for sleep, wake, USB, sync, battery, Wi-Fi and Bluetooth. It does not poll them.
- **No regular work while idle.** While a book is open and the user does nothing, only a few timers run, each at most about once a minute.
- **Timers stop with the reader.** Timers do not fire while the kernel is suspended. Most of Kobalt's timers also stop when the reader hides or sleeps.
- **The page turn is sacred.** Kobalt never waits for storage or another thread on the main thread while the user can turn a page. Turning Wi-Fi off is the one radio action that waits, and it happens before the reader takes input. See [the main-thread guard](validation.md#main-thread-guard).

## Timers that repeat while reading

While a book is open and the device is awake, Kobalt wakes once a minute, just after the wall-clock minute changes. That one tick does all of the periodic work:

| Work on the minute tick | What it does | Code |
| --- | --- | --- |
| Clock | Updates the header clock, battery and time-left fields when the page shows them, and the menu status when the menu is open | `clockTimer` and `Reader::startClock` (`src/interfaces/reader/window/reader.cpp`, `reader_build.cpp`) |
| Reading pace | Asks Nickel for the reading pace on Nickel's worker thread, for the time estimates. At most once a minute | `ReadingBook::minuteTick` → `ReadingBridge::minuteTick` → `ReadingPaceCache::tick` (`common/reading_pace_cache.h`) |
| Reader check | Repeats the visibility checks. Reads Nickel's saved position, for a position from another device, only when Nickel has reported a sync, an account update or USB. If those signals are unavailable, it reads at most once a minute and logs `native-progress-signals-unavailable` | `ReaderWindow::minuteTicked` → `ReaderCoordinator::poll` (`common/reader_coordinator.cpp`) |

The timer is precise and armed for 50 ms after the next minute. If it ever fires before the minute changes, it only re-arms, so the minute work runs once a minute. The tick runs whether or not the page shows a clock. It stops when the reader hides or the device sleeps. The reading-time minutes in the menu need no tick of their own: opening the menu pauses the reading clock and refreshes the status.

One other timer can repeat:

| Timer | Interval | When it runs | Code |
| --- | --- | --- | --- |
| Reader check fallback | 65 s, pushed back by every minute tick | Only fires when the minute tick is not running: the reader is hidden or asleep but awake enough for timers. Stops when no book is open | `ReaderCoordinator` timer |

KOSync while reading has no timer. When the reader shows a new position, after a page turn, a jump or a link, it syncs if the chosen interval has passed since the last sync: 30 seconds for every page, or 1, 5, 15 (default), 30 or 60 minutes. A book left open wakes nothing for KOSync; its last position goes up at the next page or, if those syncs are on, at sleep or close. It needs a signed-in account with automatic sync on, a book that Nickel's sync does not carry, and an interval other than never (`Reader::syncKosyncWhileReading` in `src/interfaces/reader/window/reader_kosync.cpp`).

## Timers that run only for a short time

| Timer | Interval | Runs while |
| --- | --- | --- |
| Wi-Fi connection check (`NativeSyncNetwork`) | 100 ms; 1 s while a Nickel Wi-Fi dialog takes input; ends after 10 s | Kobalt waits for a connection |
| KOSync deadline | 10 s, or 2 s for the last upload at sleep or close | A KOSync exchange runs |
| KOSync reader retry | 100 ms, repeating | A KOSync result waits for a busy reader. While the reader is hidden or asleep it does not run; the wait resumes when the reader is shown |
| KOSync request retries and timeouts | 0.5, 1.5 and 3 s retries; 10 s per request | A request is in flight |
| Host name lookup (Qt 5 only) | 25 ms | A KOSync server name is being resolved |
| Wake retry (`NativeLifecycle`) | 25 ms | After resume, while Nickel's sleep, PIN or USB view is still shown |
| Light panel | 500 ms | The light panel is open |
| Startup | 250 ms, at most 30 s | Kobalt waits for Nickel to finish starting |
| Wi-Fi saver deadline | 1 s, once | A book opens and the saver waits for the Bluetooth answer |
| Save retries at sleep, USB and shutdown | 10 to 100 ms, bounded (2.5 s at sleep) | Saves drain at a lifecycle boundary |
| UI timers (resize, notices, hold gestures, search updates, orientation debounce) | 100 ms to 4 s, once | After user input |

The diagnostic log and the profile writer each use their own thread. They wait without a timeout until there is work, so they do not wake the device while idle.

## What Kobalt listens to instead of polling

| Source | Nickel signals | Code |
| --- | --- | --- |
| Sleep and wake | `N3PowerWorkflowManager::aboutToSleep`, `resumingFromSleep`, `resumedFromSleep`; the sleep view's Show event | `common/native_lifecycle.cpp` |
| USB | `PlugWorkflowManager::aboutToConnect`, `aboutToUnplug`, `startedProcessing`, `doneProcessing`; `PlugManager::plugged`, `unplugged` | `native_lifecycle.cpp`, `native_progress.cpp` |
| Sync | `N3SyncManager::finished`, `userProfileUpdated` | `common/native_progress.cpp` |
| Battery | `battery_level`, `battery_changed`, plug and AC signals | `common/native_battery.cpp` |
| Wi-Fi | `WirelessManager::wifiEnabled`, `networkConnected`, `networkDisconnected`, `linkQualityForConnectedNetwork` | `common/native_wifi.cpp` |
| Bluetooth | `BluetoothManager::adapterPowered`; the D-Bus owner of `org.bluez` (or `com.kobo.mtk.bluedroid` on firmware 4 MediaTek devices); one asynchronous `Properties.Get` of `Adapter1.Powered` at start and when the owner changes | `common/native_bluetooth.cpp` |

The reader once checked Nickel's state every 250 ms. That poll is gone; the minute tick above is what remains.

## Wi-Fi

### When Kobalt turns Wi-Fi on

Kobalt asks Nickel for a connection only for KOSync (`common/native_sync_network.cpp`):

- **Sign in, Sync now and the manual toolbar button** use `WirelessWorkflowManager::connectWireless`, Nickel's interactive workflow, which can show Nickel's network dialogs.
- **The opening sync and the first sync after wake** use `WirelessWorkflowManager::connectWirelessSilently`. Nickel does not connect in airplane mode.
- **A sync while reading** connects silently only when the Automatic sync choice is "Turn on Wi-Fi" and fewer than three background connections have failed in a row. With "Wait", it uses Wi-Fi only if it is already on.

Kobalt never connects for books that Nickel's sync carries.

### When Kobalt turns Wi-Fi off

1. **After a sync while reading that turned Wi-Fi on.** `NativeSyncNetwork::release` calls `WirelessManager::off()`, only for a silent connection that Kobalt itself started and that is still connected. This runs while the reader is visible, so the main-thread guard reports it as `wifi-off`.
2. **The Wi-Fi power saver, when a book opens.** `Reader::saveWifiPower` runs as the last step of `preparePresentation`, before the book takes input. It asks the host (`ReaderHostServices::turnOffWifi`; on Kobo `saveNativeWifiPower` in `common/native_wifi_power.cpp`). Wi-Fi goes off only when all of these hold:
   - The setting `turn_off_wifi_on_open` is on (Settings → System → Power management; default on).
   - Kobalt's Bluetooth indicator is off, and a fresh D-Bus query answers that the adapter is off. A stopped Bluetooth stack counts as off. No answer within 1 s counts as unknown and keeps Wi-Fi on.
   - The book is not one that Nickel's sync carries (`ReadingBook::usesNativeCloudSync`).
   - KOSync is signed out, or automatic sync is off.
   - Wi-Fi is up (`WirelessManager::up`) and Nickel is not syncing (`SyncManagerBase::isSyncing` on `N3SyncManager::sharedInstance()`).

   It then calls Nickel's `WirelessWorkflowManager::turnWifiOff()`, which also respects Nickel's developer setting that keeps Wi-Fi on. The call waits on the main thread for child processes (see below), inside the declared scope `wifi-off before reader input`.

### What Nickel does when Wi-Fi goes off

`WirelessManager::off()` runs part of its work on the calling thread before it hands the radio to a worker:

- Firmware 4.46: `killall dhcpcd`, waited for on the calling thread.
- Firmware 6.0: `killall dhcpcd-dbus`, `killall dhcpcd`, `wpa_cli terminate` and `killall wpa_supplicant`, each waited for.

Nickel has no call that does the whole shutdown on a worker. This is why Kobalt turns Wi-Fi off only before the reader takes input, or reports the wait when it cannot avoid it.

### Nickel's own Wi-Fi timeouts

Kobalt's choices exist because of these. From firmware 4.46.23836 and 6.0.279225:

- **Sleep.** Nickel turns Wi-Fi off about 20 seconds after the sleep screen appears (`N3PowerWorkflowManager`).
- **Firmware 6.** `WifiWatchdog` turns Wi-Fi off when there has been no network activity for 2 minutes. It checks on Nickel's 10-second battery poll.
- **Firmware 4.** `WirelessWatchdog` needs 2 minutes without network activity and also 15 minutes since the last `BluetoothHeartbeatEvent`. Nickel sends that event when its Bluetooth manager starts and when its own reader opens a book, whether Bluetooth is on or not. So Wi-Fi can stay on for about 15 minutes after start-up or after a book opened in Nickel's reader. Opening a book in Kobalt sends no such event.
- `MaxWifiTimeout` in Nickel's settings is a connection timeout, not an idle timeout.

## Bluetooth

Kobalt never turns Bluetooth on or off. It only observes it, for the status icon and the Wi-Fi saver.

When a Bluetooth remote turns pages, Kobalt posts Nickel's `BluetoothHeartbeatEvent`, at most once every 5 seconds (`reportNativeBluetoothActivity` in `common/native_bluetooth.cpp`). This keeps Nickel from turning Bluetooth off while the remote is in use, as Nickel's own reader does. It applies only to arrow-key presses with a hardware scan code while Bluetooth is powered; on Qt 6 Nickel's `AVRCPButtonFilter` must also classify the key as Bluetooth input. On firmware 4 the same event also restarts the 15-minute Wi-Fi window above, so Wi-Fi stays on longer while a remote is in use.

## Storage writes

Writing to storage costs power and time on a Kobo, most on devices with an SD card. Kobalt keeps reading state in memory and writes it in batches or at lifecycle boundaries. The formats are in [Storage compatibility](storage.md); this section is about when writes happen.

### While reading

| What | When | How |
| --- | --- | --- |
| Reading position, reading history, reading speed (`kobalt.sqlite`) | A batch after page turns, at most once every 5 minutes (once a minute at 5% battery or less). Unchanged rows are skipped. | Profile writer thread, `synchronous=NORMAL` |
| Bookmarks, highlights and notes (`kobalt.sqlite`) | At each edit | Profile writer thread, `synchronous=NORMAL` |
| Settings (`reader-settings.json`) | When a setting or the appearance changes | `QSaveFile`, profile writer thread |
| Diagnostic log (`diagnostics/events.jsonl`) | In normal mode, as in release builds, a page turn adds nothing; only failed renders and failed page animations are logged. Verbose mode adds per-turn timing lines. Routine lines are appended 30 seconds after the last one, at most 60 seconds after the first. Warnings, errors and verbose mode use 1 and 5 seconds. | Diagnostic thread, no sync unless the batch has a warning or error |
| Nickel's database | Nothing on a page turn; Nickel's `pageTurned` event stays in Nickel's memory | |

While the reader sits idle, Kobalt writes nothing to its database or settings files. Only diagnostic events, if any, are appended.

### At lifecycle boundaries

| Moment | Writes |
| --- | --- |
| Opening | `active-session.json`; Nickel's reading status; a book-state checkpoint; on a cache miss, `cache/reopen-layout.json` |
| Sleep | A durable checkpoint of book state, speed, history and layout (`synchronous=FULL`), the reading history, Nickel's progress (skipped when the position is unchanged) and reading time, the diagnostic log with a sync, and a passive WAL checkpoint |
| Close, USB and power-off | The same as sleep, plus the sleep-screen image (`cache/sleep-screen.png`), the CREngine document cache (`.cache/*.cr3`) when it is missing or stale, and closing `kobalt.sqlite` with `wal_checkpoint(TRUNCATE)` and `journal_mode=DELETE` |
| At 5% battery or less, before standby | A durable batch and a passive checkpoint |

### SQLite settings

`kobalt.sqlite` opens with `locking_mode=EXCLUSIVE`, `journal_mode=WAL`, `synchronous=FULL`, `fullfsync=ON` and `wal_autocheckpoint=1000` (`src/storage/annotation_database.cpp`). Routine batches switch to `synchronous=NORMAL` for their transaction. If WAL is refused, the database uses a DELETE journal with `synchronous=EXTRA`.

## Sleep and standby

Kobalt can delay a suspend, never prevent it for long:

- `PowerManager::canSuspend` (hooked in `qt5/hooks.cpp` and `qt6/hooks.cpp`, decided in `native_lifecycle.cpp`) answers false while a save, a USB transition, the sleep-view check or the last KOSync upload (at most 2 s) is still running.
- On firmware 4 only, just before Nickel writes `mem` to `/sys/power/state`, Kobalt checks for touch input that arrived during the preparation or in the last 200 ms (`common/standby_input.cpp`). If there is any, the attempt fails with `EAGAIN` and Nickel tries again later. A touch during an armed attempt takes a one-second kernel wake lock (`kobalt-input`).
- On firmware 5 and 6 Kobalt only observes the suspend.

## Verification

- `lifecycle.wifiPowerSaver`: when the reader asks to turn Wi-Fi off, and that the book waits for the answer.
- `platform.native-sync-network`: connection handling, `release()` and the saver's Kobo side, including Bluetooth on, unknown and no answer, Nickel syncing, and the declared wait.
- `lifecycle.kosyncBackgroundWifi` and `lifecycle.kosyncMoments`: when KOSync connects and releases Wi-Fi.
- `lifecycle.progressChoiceWaitsForSettings`: the signal-driven position check.
- Nickel's own timeouts and the time `turnWifiOff()` waits cannot be checked by the native tests. Measure them on a device or an emulated Nickel after a firmware change, and measure the wait on both eMMC and SD-card devices.
