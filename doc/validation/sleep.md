# Sleep, USB and power checks

`lifecycle.readerStandbySetting` checks the default, disabled, invalid and unsupported preference states. It verifies that explicit sleep and a hidden or closed reader are not held awake by the preference. These controlled host checks do not measure hardware standby or battery life.

Label `sleep`. Build `mac-tests`, then run `ctest --preset mac -L sleep` from `platforms/desktop/`. `ctest --preset mac -N -L sleep` lists its tests.

These checks cover sleep screens and snapshots, sleep and wake, session timeouts, USB export, shutdown, power-off screens and the native lifecycle adapter. The `lifecycle` checks drive the production reader with controlled host callbacks and an injected clock. The `platform` checks drive the production Kobo adapters with controlled Nickel signals, substitute native window types and a real profile database.

| Test | Checks |
| --- | --- |
| `platform.native-lifecycle` | The Kobo lifecycle adapter: the sleep-view handoff and its fallback, PIN rotation, USB entry and power-off |
| `platform.shutdown` | Power-off, battery-low power-off and reboot, and the Nickel Screensaver fixture |
| `platform.kobo-power-screen` | The native power and sleep views and their database reads |
| `platform.sleep-persistence` | The sleep-image writer and native-cover references |
| `platform.usb-removal` | Detecting that Kobalt was removed over USB, once per USB session |
| `desktop.deviceEmulation` | Sleep, wake and power-off through the desktop preview toolbar |

`platform.shutdown` loads `tests/support/screensaver_fixture.cpp` as a shared library with Nickel Screensaver's old call-through behavior and NickelHook descriptor. The fixture does not contain Screensaver's image composition or real Nickel firmware.

`platform.sleep-persistence` does not measure Kobo storage. The opt-in device benchmark is described in [Sleep persistence benchmark](../release-evaluation.md#sleep-persistence-benchmark).

These checks do not exercise physical power loss, firmware behavior, PIN behavior on hardware, sleep-screen refresh quality, actual hook load order, hardware suspend or physical coexistence with Nickel Screensaver. Physical sleep and wake on Kobo and Android need device validation.
