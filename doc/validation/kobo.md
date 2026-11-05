# Kobo adapter checks

Label `kobo`. Build `mac-tests`, then run `ctest --preset mac -L kobo` from `platforms/desktop/`. `ctest --preset mac -N -L kobo` lists its tests.

These checks cover startup recovery, the native startup wait, Nickel Screensaver discovery, native dialogs above the reader, the Nickel book menu, session and font-scale adapters and Qt5 compatibility helpers. Sleep, USB and shutdown adapters are in [Sleep, USB and power](sleep.md).

The portable platform checks keep their original assertions with path and fixture adaptations. They run the production adapters with controlled callbacks, fake native types and synthetic font wrappers in temporary directories. They do not resolve firmware symbols or run on a device. `platform.screensaver-loader` runs only on Linux.

These checks leave device gaps. Full loader removal and restart after a failed startup need a device. Native callback delivery, settings injection and book opening after a slow startup need firmware validation. The dialog checks do not exercise firmware update downloads or installation.

## Native allocations

The Qt5 native allocations were checked against the ARM32 libraries from firmware 4.38.23828, 4.45.23697 and 4.46.23836. The constructors and native allocation sites agree with the light, dictionary and settings storage that the adapters use. ContentSettings has a 72-byte array stride, including alignment padding. This inspection found no allocation mismatch. Each adapter checks its required symbols and available type metadata before use.
