# Sleep, USB and power checks

`lifecycle.readerStandbySetting` checks the default, disabled, invalid and unsupported preference states. It verifies that explicit sleep and a hidden or closed reader are not held awake by the preference. These controlled host checks do not measure hardware standby or battery life.

Label `sleep`. Build `mac-tests`, then run `ctest --preset mac -L sleep` from `platforms/desktop/`. `ctest --preset mac -N -L sleep` lists its tests.

These checks cover sleep screens and snapshots, sleep and wake, session timeouts, USB export, shutdown, power-off screens and the native lifecycle adapter. The `lifecycle` checks drive the production reader with controlled host callbacks and an injected clock. The `platform` checks drive the production Kobo adapters with controlled Nickel signals, substitute native window types and a real profile database.

| Test | Checks |
| --- | --- |
| `platform.native-lifecycle` | The Kobo lifecycle adapter: standby refused while a book opens, the sleep-view handoff and its fallback, PIN rotation, USB entry and power-off |
| `platform.shutdown` | Power-off, battery-low power-off and reboot, and the Nickel Screensaver fixture |
| `platform.kobo-power-screen` | The native power and sleep views and their database reads |
| `platform.sleep-persistence` | The sleep-image writer and native-cover references |
| `platform.usb-removal` | Detecting that Kobalt was removed over USB, once per USB session |
| `desktop.deviceEmulation` | Sleep, wake and power-off through the desktop preview toolbar |

`platform.shutdown` loads `tests/support/screensaver_fixture.cpp` as a shared library with Nickel Screensaver's old call-through behavior and NickelHook descriptor. The fixture does not contain Screensaver's image composition or real Nickel firmware.

`platform.sleep-persistence` does not measure Kobo storage. The opt-in device benchmark is described in [Sleep persistence benchmark](../release-evaluation.md#sleep-persistence-benchmark).

These checks do not exercise physical power loss, firmware behavior, PIN behavior on hardware, sleep-screen refresh quality, actual hook load order, hardware suspend or physical coexistence with Nickel Screensaver. Physical sleep and wake on Kobo and Android need device validation.

The highlight sleep regression holds the engine at save preparation and text collection while the reader sleeps. It requires the accepted highlight to reach storage, the late write to become durable before standby, and subsequent engine work to stay paused until wake. A selection without Save remains unsaved. The engine gate checks overlapping save drains and wake during a drain. Annotation checks cover rejected saves, stale replies, destroyed owners and failed storage writes while sleep is requested.

`model.appearance-settings` checks every gallery choice, legacy backdrop and indicator combinations, changes made by older releases, invalid designs and greeting limits. `lifecycle.sleepGreetingEditor` opens Night with a long press, saves and reopens a greeting, restores the default with an empty field, and cancels an edit through the windowless desktop host. It does not verify the native Kobo keyboard.

`lifecycle.sleepDesignsRepaintOnlyTheirStatus` compares a battery-status repaint with a fresh render for each new design in both themes and orientations. `lifecycle.sleepSettingsShowWhereToAddPngs` checks gallery paging and the unavailable PNG explanation. The `sleepGallery` screenshot case captures `sleep-settings`, `sleep-gallery` and `sleep-greeting` through the production UI, including dark mode and large interface text.

`lifecycle.sleepWidgetsKeepTimeAcrossSessionTimeout` keeps the Today total across a reading-session timeout. `lifecycle.sleepWidgetsUseLocalDay` checks continuous reading across midnight, short sleep, several days asleep, expired and retained sessions, and a new day with no reading. `lifecycle.sleepWidgetsUseSavedDay` checks totals from another book, checkpoint exclusion and reopening. `model.reading-history` checks local-day persistence, reopen totals, checkpoint exclusion and failure retries, and the 23-hour and 25-hour daylight-saving days in Europe/Brussels. `lifecycle.sleepPowerOffWithoutCoverKeepsDesign` checks that coverless designs show the selected power-off screen after sleeping, using the reading-page fallback.

`lifecycle.sleepMetadataStaysInsideItsLayout` checks that long titles, chapter names and author names do not overwrite reserved screen areas. It covers unbroken words, mixed scripts, combining characters, emoji, line breaks, markup and oversized metadata in both themes and orientations. `lifecycle.sleepLetterPreservesLiteralPlaceholders` checks that percent placeholders in a book title remain literal. The status repaint check also uses long metadata and greetings.

`lifecycle.sleepHandoffKeepsPortraitDirection` checks sleep capture in all four reading orientations, including private books. It checks both values of Follow reading orientation. The default keeps all four directions; disabling it uses portrait layouts and retains inverted portrait. Both retain the reading size and page on wake. A later power-off snapshot keeps the captured direction after native handoff. `platform.kobo-power-screen` compares asymmetric image pixels through the production sleep presenter before and after native orientation changes, including inverted portrait. `lifecycle.sleepBackdropsUsePortrait` checks portrait conversion for saved cover, page, PNG and plain backdrops, including repeated conversion and preservation of the source image.

`lifecycle.sleepNativePreviewFollowsSettings` checks the native preview with covers hidden, inset covers, full-screen covers and full-screen information panels. Inactive options must not change the preview.

`platform.kobo-power-screen` checks that the native power-screen owner renders the selected design from a prepared backdrop. A sleeping reader retains ownership of its book even after its window hides for the native sleep handoff.

`lifecycle.sleepPrivateBookOverridesEveryDesign` checks a private book against each sleep design from a landscape reader, including Stock, Reading page and PNG over text. It checks each retained design and its anonymous progress, compares sleep and power-off pixels, rejects identifying metadata and cover references, checks the saved book choice and restores reading on wake. `desktop.settingsSleepPrivacy` saves through System, switches books, reopens and disables the choice without writing a global preference. `platform.kobo-power-screen` also checks that a private last-read book ignores saved images and global designs, survives a fresh screen owner and does not transfer its choice to another last-read book. These are controlled host checks, not physical firmware or e-ink evidence.

Private previews compare every design against a sanitized backdrop with anonymous progress and deliberately identifying source metadata and book pixels. `lifecycle.sleepPrivateBookCloseSavesAnonymousDesign` checks close-time handoff and reopening. The overlay check also exercises partial alpha above a private opaque page. The native power-screen check restores a private snapshot's anonymous progress through a fresh screen owner.

The sleep-screen settings check toggles Follow reading orientation and verifies both saved Boolean values and confirmation messages. Gallery previews use the reading shape when enabled and portrait when disabled. The sleep gallery captures include both rotation-button states, their confirmation popups and landscape private designs. These headless checks do not establish physical-device orientation behavior.

The sleep rotation button uses the shared `CheckCellButton`: an icon slightly left of center and a separate lower-right checkbox. `component.ui` checks its visible checked state, toggle delivery, disabled behavior and stable icon position across screen densities.
