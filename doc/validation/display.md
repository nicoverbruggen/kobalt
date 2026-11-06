# Display refresh checks

Label `display`. Build `mac-tests`, then run `ctest --preset mac -L display` from `platforms/desktop/`. `ctest --preset mac -N -L display` lists its tests.

These checks cover page animations, refresh policies and regions, pixel damage, paint batching, the Library theme surface and the BOOX frame transport. `platform.page-animation` and `platform.page-transition-native` check policy and submission with fake display calls. The `boox` checks use the Android frame transport and queue.

`ui.navigationPressKeepsDamageInItsTab` checks that pressing or cancelling each bottom tab repaints only that tab's column, with a panel open or closed and in both themes. It uses a native backing store so a graphics proxy cannot hide broad repaint requests.

`ui.navigationRefreshKeepsOverlayAndPanelPolicies` checks that a combined panel and navigation update keeps full refresh outside the visible tabs. Overlays and hidden navigation do not receive the tabs' partial policy. The Qt5 adapter check exercises the same split through both firmware policy-list ABIs. This optimization applies only to ordinary light-theme controls on MediaTek displays; dark cleanup, page turns and other backends retain their policies.

A completed frame that closes the active panel while keeping the reading controls publishes a one-shot dismissal. This includes the selected tab and the Chapters close button. The display adapter submits the restored panel area and deselected tab together. Proven text-only pages use partial mode; image and dark cleanup retain the stronger policy. The helper and Qt5 adapter checks cover combined submission, one-shot consumption and repair precedence. `lifecycle.navigationDismissalWaitsForPanel` checks immediate and delayed appearance dismissal and distinguishes closing a panel from switching tabs.

Use `KOBALT_GALLERY_CASE=androidPageAnimations` to capture only the Android page-animation views. The display-rate lookup uses the window handle with a primary-screen fallback, so it stays compatible with the prepared Qt 5.2.1 Kobo SDK.

The widget keyboard batches key release with the editor change. On MediaTek in the light theme, ordinary edits confined to the visible keyboard and its editor use one partial update. Whole-keyboard changes, unrelated panel damage and dark cleanup keep their existing policies. The model checks cover the damage boundary; `pewter.qt5-refresh` checks the combined submission through both policy-list ABIs. Physical typing latency and ghosting require a device check.

## Qt5 refresh adapter

The Kobo test `pewter.qt5-refresh`, in `tests/pewter/refresh_qt5.cpp`, drives the production Qt5 refresh adapter through both policy-list ABIs, against the firmware Qt 5.2.1 libraries of the prepared Qt5 image. `bash platforms/kobo/build.sh qt5 --check` builds and runs it with the [Kobo adapter tests](kobo.md#pewter-the-fake-nickel).

## Device results and limits

These are synthetic policy, submission and transport checks. They do not resolve firmware symbols or verify physical panels, waveforms, pixel smoothness or Android inset timing.

A Pixel 9a confirmed the light Library background and system-bar icon contrast after deployment. Dark-theme system-bar icons still need a physical-device check. A BOOX device run confirmed that the loading caption and progress bar fit the display and that the Library settles at its correct inset size. Its book-to-Library recording still shows brief blank or repositioning frames. The native handoff checks do not cover Android compositor timing, so the return transition remains an open device issue.
