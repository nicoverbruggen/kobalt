# Kobo adapter checks

Label `kobo`. Build `mac-tests`, then run `ctest --preset mac -L kobo` from `platforms/desktop/`. `ctest --preset mac -N -L kobo` lists its tests.

These checks cover startup recovery, the native startup wait, Nickel Screensaver discovery, native dialogs above the reader, the Nickel book menu, session and font-scale adapters and Qt5 compatibility helpers. Sleep, USB and shutdown adapters are in [Sleep, USB and power](sleep.md).

The portable platform checks keep their original assertions with path and fixture adaptations. They run the production adapters with controlled callbacks, fake native types and synthetic font wrappers in temporary directories. They do not resolve firmware symbols or run on a device. `platform.screensaver-loader` runs only on Linux.

`platform.nickel-account` checks the Qt 5 account read under a real SQLite lock. An unavailable read defers the check, while a confirmed sign-out or changed account remains a revocation. The initial account read must succeed before monitoring starts. Active checks retry a failed account read at most three times after each native change signal.

An unavailable authorization check at wake closes the hidden reader through local-only disposal. It must not write native progress or reading time without a confirmed account. Active Qt 5 account checks retry failed reads between event-loop turns with no SQLite busy wait.

## Pewter: the fake Nickel

Pewter tests the firmware adapters in `platforms/kobo/nickel-qt5/` and `nickel-qt6/` with the Kobo SDKs. Run `bash platforms/kobo/build.sh qt5 --check` or `qt6 --check` from the repository root, or `cmake --build --preset mac-kobo-checks` from `platforms/desktop/` for both. The check builds the tests with the mod library and runs them with CTest in the prepared image. CI runs the same check in each Kobo build job before the package build, and a failure stops the job before the package is built. `pewter.progress` runs in both SDKs. `pewter.qt5-refresh`, the [Qt5 refresh check](display.md#qt5-refresh-adapter), runs in the Qt5 SDK. To run them by hand in the image, configure with `-DKOBALT_BUILD_TESTS=ON`, build the `kobo-tests` target and run `ctest --test-dir ../../build/kobo-qt5` from `platforms/kobo/`.

Pewter, in `tests/pewter/`, defines the Nickel classes and functions that the adapters resolve with `dlsym`, with Nickel's names and signatures. The compiler gives them the same symbol names. `nickel.cpp` has the functions that all firmware versions share. `nickel_qt5.cpp` has the firmware 4 functions for `nickel-qt5/`, and `nickel_qt6.cpp` the firmware 5 and 6 functions for `nickel-qt6/`. `pewter.cpp` has the test interface of `pewter.h`. Each Kobo SDK builds the `pewter` object library from the shared files and its own firmware file. A test executable links it with the application objects of the mod library and exports pewter's functions, so the adapter finds them as it finds Nickel's. Pewter records each call with its arguments, its thread and whether a transaction is open, and keeps the reading data in a temporary `KoboReader.sqlite` with the columns that Kobalt reads and writes. Options make Nickel refuse a transaction, an update or a commit, report sleep, or decline the upload.

`pewter.progress` checks `NativeProgress`, the progress writeback that [Sync data](../sync-data.md) describes: the status update at opening, a save partway through a book, the last page and a finished book, a save without a position, and the recent-book call at close. It checks which functions Kobalt calls, their order, their values, that all of them run on the interface thread, that only changed fields are written, and the stored row. On Qt 5 it also checks the KEPUB chapter row. It checks that a busy database, a rejected update, a failed commit, a position changed by another writer and a sleeping device leave the row as it was and request no upload. A stale native cache releases Kobalt's transaction, refreshes once, reacquires its transaction and rechecks the committed row before saving in the same attempt. An existing native transaction must remain untouched. A cache that stays stale must release its transaction without writing or uploading. A changed committed row still refuses the save.

The tests run under `qemu-arm` with the Qt libraries of each image: firmware 4's Qt 5.2.1 and C++ library in the Qt5 image, and Qt 6.5 in the Qt6 image. The C library is the image's cross C library, because the image's `qemu-arm` crashes in the glibc 2.19 loader of firmware 4. A test run takes less than a second; a check after a small change takes a few seconds per SDK. Pewter checks what Kobalt asks Nickel to do. It does not check that Nickel behaves the same way, so its functions follow the firmware behavior documented in Sync data, never copied firmware code. Update pewter when a firmware changes a function that Kobalt calls. Tolino devices, reading sessions, reading time and Wi-Fi are not covered yet.

### What pewter is and is not

Pewter is a validation tool. It exists only to check that Kobalt's firmware adapters call Nickel correctly. It is not a replacement for Nickel, it does not reproduce Nickel's behavior beyond what these checks need, and it is never part of a Kobalt package: only the test programs link it.

Pewter contains no code from Kobo or Nickel. Its classes and functions carry the names and signatures of the Nickel functions that Kobalt's adapters call, because the adapters find those functions by name; the adapters contain the same names. The behavior of each fake function is written from the observed effects that [Sync data](../sync-data.md) documents, not from firmware code. The only Kobo code in Kobalt's toolchain is Kobo's open-source Qt release, used in the Qt6 SDK and credited in the [Acknowledgements](../acknowledgements.md#adapted-code-and-kobo-integration) under its license terms. Pewter does not use it.

## Device gaps

These checks leave device gaps. Full loader removal and restart after a failed startup need a device. Native callback delivery, settings injection and book opening after a slow startup need firmware validation. The dialog checks do not exercise firmware update downloads or installation.

## Native allocations

The Qt5 native allocations were checked against the ARM32 libraries from firmware 4.38.23828, 4.45.23697 and 4.46.23836. The constructors and native allocation sites agree with the light, dictionary and settings storage that the adapters use. ContentSettings has a 72-byte array stride, including alignment padding. This inspection found no allocation mismatch. Each adapter checks its required symbols and available type metadata before use.

`pewter.keyboard` exercises native keyboard allocation, receiver ownership, submission, language switching, placement and cleanup in both SDKs. A queued language change must not reopen a dismissed keyboard, and the next explicit opening must use the chosen language. Its synthetic widgets test the adapter contract, not Nickel’s key layout. Check letters, numbers, symbols, focus and both themes in firmware before deploying a keyboard change.
