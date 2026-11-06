# Display refresh checks

Label `display`. Build `mac-tests`, then run `ctest --preset mac -L display` from `platforms/desktop/`. `ctest --preset mac -N -L display` lists its tests.

These checks cover page animations, refresh policies and regions, pixel damage, paint batching, the Library theme surface and the BOOX frame transport. `platform.page-animation` and `platform.page-transition-native` check policy and submission with fake display calls. The `boox` checks use the Android frame transport and queue.

Use `KOBALT_GALLERY_CASE=androidPageAnimations` to capture only the Android page-animation views. The display-rate lookup uses the window handle with a primary-screen fallback, so it stays compatible with the prepared Qt 5.2.1 Kobo SDK.

## Qt5 refresh adapter

The Kobo test `display.qt5-refresh` drives the production Qt5 refresh adapter through both policy-list ABIs, against the firmware Qt 5.2.1 libraries of the prepared Qt5 image. `bash platforms/kobo/build.sh qt5 --check` builds and runs it with the [Kobo adapter tests](kobo.md#pewter-the-fake-nickel).

## Device results and limits

These are synthetic policy, submission and transport checks. They do not resolve firmware symbols or verify physical panels, waveforms, pixel smoothness or Android inset timing.

A Pixel 9a confirmed the light Library background and system-bar icon contrast after deployment. Dark-theme system-bar icons still need a physical-device check. A BOOX device run confirmed that the loading caption and progress bar fit the display and that the Library settles at its correct inset size. Its book-to-Library recording still shows brief blank or repositioning frames. The native handoff checks do not cover Android compositor timing, so the return transition remains an open device issue.
