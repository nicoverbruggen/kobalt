# Power mechanisms

This document describes what Kobalt does on a Kobo that costs power: its timers, how it learns about changes, and when it turns Wi-Fi on or off. It also describes Nickel's own Wi-Fi rules, because Kobalt's choices depend on them. For a plain-language summary, see the [user guide](guide.md). Update this document in the same change as the code it describes.

Firmware 4.x runs the Qt 5 package (`platforms/kobo/nickel-qt5/`). Firmware 5.x and 6.x run the Qt 6 package (`platforms/kobo/nickel-qt6/`). Shared Kobo code is in `platforms/kobo/common/`. Firmware details that come from a specific version name that version.

## Principles

- **Signals before polls.** Kobalt reacts to Nickel's signals for sleep, wake, USB, sync, battery, Wi-Fi and Bluetooth. It does not poll them.
- **No regular work while idle.** While a book is open and the user does nothing, only a few timers run, each at most about once a minute.
- **Timers stop with the reader.** Timers do not fire while the kernel is suspended. Most of Kobalt's timers also stop when the reader hides or sleeps.
- **The page turn is sacred.** Kobalt never waits for storage or another thread on the main thread while the user can turn a page. Turning Wi-Fi off is the one radio action that waits. It happens before the reader takes input, at sleep or at close, except when a KOSync sync that turned Wi-Fi on ends while the reader takes input; see [When Kobalt turns Wi-Fi off](#when-kobalt-turns-wi-fi-off). See [the main-thread guard](validation.md#main-thread-guard).

## Timers that repeat while reading

While a book is open and the device is awake, Kobalt wakes once a minute, just after the wall-clock minute changes. That one tick does all of the periodic work:

| Work on the minute tick | What it does | Code |
| --- | --- | --- |
| Clock | Updates the header clock, battery and time-left fields when the page shows them, and the menu status when the menu is open | `clockTimer` and `Reader::startClock` (`src/app/reader/window/reader.cpp`, `reader_build.cpp`) |
| Reading pace | Asks Nickel for the reading pace on Nickel's worker thread, for the time estimates. At most once a minute | `ReadingBook::minuteTick` → `ReadingBridge::minuteTick` → `ReadingPaceCache::tick` (`common/reading_pace_cache.h`) |
| Reader check | Repeats the visibility checks. Reads Nickel's saved position, for a position from another device, only when Nickel has reported a sync, an account update or USB. If those signals are unavailable, it reads at most once a minute and logs `native-progress-signals-unavailable` | `ReaderWindow::minuteTicked` → `ReaderCoordinator::poll` (`common/reader_coordinator.cpp`) |

The timer is precise and armed for 50 ms after the next minute. If it ever fires before the minute changes, it only re-arms, so the minute work runs once a minute. The tick runs whether or not the page shows a clock. It stops when the reader hides or the device sleeps. The reading-time minutes in the menu need no tick of their own: opening the menu pauses the reading clock and refreshes the status.

One other timer can repeat:

| Timer | Interval | When it runs | Code |
| --- | --- | --- | --- |
| Reader check fallback | 65 s, pushed back by every minute tick | Only fires when the minute tick is not running: the reader is hidden or asleep but awake enough for timers. Stops when no book is open | `ReaderCoordinator` timer |

KOSync while reading has no timer. When the reader shows a new position, after a page turn, a jump or a link, it syncs if the chosen interval has passed since the last sync. On Kobo the interval counts real time, also while Nickel keeps the device in standby between page turns (`CLOCK_BOOTTIME`, the same clock as the reading duration): 30 seconds for every page, or 1, 5, 15 (default), 30 or 60 minutes. A book left open wakes nothing for KOSync; its last position goes up at the next page or, if those syncs are on, at sleep or close. It needs a signed-in account with automatic sync on, a book that Nickel's sync does not carry, and an interval other than never (`Reader::syncKosyncWhileReading` in `src/app/reader/window/reader_kosync.cpp`).

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
| Wi-Fi saver deadline | 1 s, once | A book opens or the device goes to sleep, and the saver waits for the Bluetooth answer |
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

These five rules are the contract.

1. **Wi-Fi that a KOSync sync turned on goes off when that sync ends.** This covers the opening sync, the first sync after wake and a sync while reading. It applies when the sync succeeds, fails, reaches its deadline or is cancelled by Skip, close or sleep. "Turned on" means that Kobalt's silent connection request found the radio off (`WirelessManager::up` false) and asked Nickel for it. A radio that was already on is not Kobalt's, even when Kobalt's request connects it.
2. **At sleep, the Wi-Fi power saver turns Wi-Fi off at once,** after the sleep sync if KOSync syncs at sleep. Nickel would otherwise do it about 20 seconds after the sleep screen.
3. **The power saver alone decides about a connection that Kobalt did not open:** when a book opens and at sleep. KOSync never turns it off.
4. **Syncs that the user asks for leave Wi-Fi as it is.** Sign in, Sync now and the manual toolbar button use Nickel's interactive `connectWireless`, and Kobalt does not turn that Wi-Fi off.
5. **While a book is open, the power saver keeps Nickel's own background connections from turning Wi-Fi on,** such as the one Nickel starts after every wake. It uses the rules for opening a book.

#### KOSync (rules 1, 3 and 4)

`Reader::prepareKosyncNetwork` marks each silent request (`kosyncReleaseWifi`). `Reader::finishKosync` and `Reader::cancelKosync` call `Reader::releaseKosyncWifi`, so every way a sync ends passes through it: success, failure, the sync deadline, Skip, close (`prepareTransition`, the exit flow and the reader's destructor), sleep (`flushKosync`) and wake. It calls `NativeSyncNetwork::release`, which decides whether the Wi-Fi is Kobalt's:

- `NativeSyncNetwork::prepare` records ownership (`turnedOn`) only for a silent request that starts a connection while the radio is off. It does not record it when a connection is already usable, when Nickel is already connecting, in airplane mode or for an interactive request.
- `release` stops a request that is still running, then calls `WirelessManager::off()` if the radio is on. Ownership ends there, so a second call does nothing.

The sleep sync uses only a connection that is already up. If a sync that turned Wi-Fi on is still sending when the device sleeps, the sleep sync waits for it, sends the final position and then releases the Wi-Fi.

`off()` waits on the main thread (see below). At close, Kobalt declares that wait as `wifi-off while closing`; at sleep and at wake, before the reader is shown, as `wifi-off at sleep`. When a sync ends while the reader takes input (a Skip, the end of a sync while reading or of a wake sync, or an opening sync that outlasts the loading screen), the main-thread guard reports the wait as `wifi-off`. A Clara BW on firmware 4.46 measured about 87 ms.

#### The Wi-Fi power saver (rules 2 and 3)

`Reader::saveWifiPower` runs as the last step of `preparePresentation`, before the book takes input. At sleep, `Reader::saveWifiPowerAtSleep` runs when `flushKosync` finishes, which is after the sleep sync or at once when there is none. Both ask the host (`ReaderHostServices::turnOffWifi`; on Kobo `saveNativeWifiPower` in `common/native_wifi_power.cpp`). Until beta 7, Kobo provides neither this service nor `holdWifiOff`, so rules 2, 3 and 5 do not apply there and only KOSync turns Wi-Fi off. Wi-Fi goes off only when all of these hold:

- The setting `turn_off_wifi_on_open` is on (Settings → System → Power management; default off). Until beta 7 the settings cleanup removes this key, so it is never on.
- Kobalt's Bluetooth indicator is off, and a fresh D-Bus query answers that the adapter is off. A stopped Bluetooth stack counts as off. No answer within 1 s counts as unknown and keeps Wi-Fi on.
- The book is not one that Nickel's sync carries (`ReadingBook::usesNativeCloudSync`).
- When a book opens only: KOSync is signed out, or automatic sync is off. Automatic KOSync expects a connection while the book is open. At sleep its last sync has ended, so this condition does not apply.
- Wi-Fi is up (`WirelessManager::up`) and Nickel is not syncing (`SyncManagerBase::isSyncing` on `N3SyncManager::sharedInstance()`).

It then calls Nickel's `WirelessWorkflowManager::turnWifiOff()`, which also respects Nickel's developer setting that keeps Wi-Fi on. The call waits on the main thread for child processes (see below), inside the declared scope `wifi-off before reader input` when a book opens and `wifi-off at sleep` at sleep. Wake deletes the request at sleep (`Reader::cancelWifiPowerAtSleep`), so a Bluetooth answer that comes after wake does not turn Wi-Fi off.

#### Nickel's background connections (rule 5)

A few seconds after every wake, Nickel's wake routine in `Nickel3Application` calls `WirelessWorkflowManager::connectWirelessSilently()` and then reconnects Bluetooth devices. It does this whether or not Wi-Fi was on before sleep, also after the power saver turned it off, and it starts no sync. Firmware 4.46.23852 and 6.0.279225 both do this. Nickel also calls `connectWirelessSilently()` for image downloads, its browser and store, and after USB. Its store sync connects through `connectWireless` instead.

Kobalt hooks `connectWirelessSilently()` for calls from libnickel (`kobalt_connect_wireless_silently` in `common/native_wifi_power.cpp`, registered in `nickel-qt5/hooks.cpp` and `nickel-qt6/hooks.cpp`). The hook is optional; without it, Nickel connects as before. The reader registers a hold through `ReaderHostServices::holdWifiOff` when it builds. The hook skips Nickel's call and logs `wifi-power-saver` with `moment` `nickel-connection` when all of these hold:

- The call comes on the GUI thread.
- The reader is still open and not closing, and `Reader::wifiPowerSaverApplies` answers yes: the setting is on, Kobalt's Bluetooth indicator is off, the book is not one that Nickel's sync carries, and KOSync is signed out or automatic sync is off.
- No Kobalt connection request is waiting (`NativeSyncNetwork::preparing`). Nickel's network dialogs for Sign in or Sync now can start background connections of their own.
- Nickel's browser is not open over the reader (`BrowserHandoff::activeFor`).

Kobalt's own silent requests resolve `connectWirelessSilently()` with `dlsym` and do not pass through the hook. Skipping the call turns nothing off, so nothing waits on the main thread. When the book closes, Nickel connects again the next time it needs to.

### What Nickel does when Wi-Fi goes off

`WirelessManager::off()` runs part of its work on the calling thread before it hands the radio to a worker:

- Firmware 4.46: `killall dhcpcd`, waited for on the calling thread.
- Firmware 6.0: `killall dhcpcd-dbus`, `killall dhcpcd`, `wpa_cli terminate` and `killall wpa_supplicant`, each waited for.

Nickel has no call that does the whole shutdown on a worker. This is why Kobalt turns Wi-Fi off before the reader takes input, at sleep or at close where it can, and reports the wait when a sync ends while the reader takes input.

### Nickel's own Wi-Fi timeouts

Kobalt's choices exist because of these. From firmware 4.46.23836 and 6.0.279225:

- **Wake.** Nickel turns Wi-Fi on a few seconds after every wake (see rule 5).
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
| Sleep | A durable checkpoint of book state, speed, history and layout (`synchronous=FULL`), the reading history, Nickel's progress (skipped when the position is unchanged; on firmware 4 it includes the current KEPUB chapter's row) and reading time, the diagnostic log with a sync, and a passive WAL checkpoint |
| Close, USB and power-off | The same as sleep, plus the sleep-screen image (`cache/sleep-screen.png`), the CREngine document cache (`.cache/*.cr3`) when it is missing or stale, and closing `kobalt.sqlite` with `wal_checkpoint(TRUNCATE)` and `journal_mode=DELETE`. Close also writes the book's recent-book row in Nickel's `Activity` table; see [Sync data](sync-data.md) |
| At 5% battery or less, before standby | A durable batch and a passive checkpoint |

### SQLite settings

`kobalt.sqlite` opens with `locking_mode=EXCLUSIVE`, `journal_mode=WAL`, `synchronous=FULL`, `fullfsync=ON` and `wal_autocheckpoint=1000` (`src/storage/profile_database.cpp`). Routine batches switch to `synchronous=NORMAL` for their transaction. If WAL is refused, the database uses a DELETE journal with `synchronous=EXTRA`.

## Sleep and standby

Kobalt can delay a suspend, never prevent it for long:

- `PowerManager::canSuspend` (hooked in `nickel-qt5/hooks.cpp` and `nickel-qt6/hooks.cpp`, decided in `native_lifecycle.cpp`) answers false while a save, a USB transition, the sleep-view check or the last KOSync upload (at most 2 s) is still running. In evaluation packages it also answers false while the scenario player runs, unless the scenario allows standby (`NativeLifecycle::holdAwake`).
- On firmware 4 only, just before Nickel writes `mem` to `/sys/power/state`, Kobalt checks for touch input that arrived during the preparation or in the last 200 ms (`common/standby_input.cpp`). If there is any, the attempt fails with `EAGAIN` and Nickel tries again later. A touch during an armed attempt takes a one-second kernel wake lock (`kobalt-input`).
- On firmware 5 and 6 Kobalt only observes the suspend.

## Verification

- `lifecycle.wifiPowerSaver`: when the reader asks to turn Wi-Fi off at opening, that the book waits for the answer, and that the reader's hold on the host's background connections follows the same rules.
- `lifecycle.wifiPowerSaverAtSleep`: the saver at sleep runs after the sleep sync, also with automatic KOSync, only when the setting is on, and wake drops its request.
- `platform.native-sync-network`: connection handling, which Wi-Fi `release()` turns off (a radio that was already on, a cancelled, failed or running request, an interactive request) and the saver's Kobo side, including Bluetooth on, unknown and no answer, Nickel syncing, and the declared wait. It also checks when the hook skips Nickel's background connections: the reader's hold, a waiting Kobalt request, the browser, another thread and a deleted reader.
- `lifecycle.kosyncWifiRelease`: opening, wake and periodic syncs release Wi-Fi when they fail, reach their deadline, are skipped or are cancelled by close, and Sync now does not.
- `lifecycle.kosyncBackgroundWifi` and `lifecycle.kosyncMoments`: when KOSync connects and releases Wi-Fi while reading, and which switch the opening and wake syncs follow.
- `lifecycle.progressChoiceWaitsForSettings`: the signal-driven position check.
- Nickel's own timeouts, the time `turnWifiOff()` and `off()` wait, and what Nickel does when `off()` follows a cancelled connection request cannot be checked by the native tests. Measure them on a device or an emulated Nickel after a firmware change, and measure the wait on both eMMC and SD-card devices.
