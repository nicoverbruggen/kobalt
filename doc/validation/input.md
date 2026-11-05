# Input checks

Label `input`. Build `mac-tests`, then run `ctest --preset mac -L input` from `platforms/desktop/`. `ctest --preset mac -N -L input` lists its tests.

These checks cover touch and pointer input, holds, swipes in paged lists, press feedback, the page-turn tap filter, hardware page buttons, navigation previews, the touch pointer and word selection in the reader. The `ui` checks deliver input through the production graphics canvas to the production widgets. CTest runs `desktop.wordSelection` once for each display density and theme, and `desktop.hardwarePageButtons` once for each open view, each in its own process. The page-turn tap filter is described in [Diagnostics](diagnostics.md#page-turn-diagnostics).

## Restored input and paint checks

The UI and reader-window checks keep selected assertions from `tests/android/android_ui_test.cpp` and `tests/native-ui/reader_test.cpp` at `82b8935`. The toolbar invalidation check measures the reader window directly, because a graphics proxy renders widgets into its display cache and adds paint events that do not prove that the reader invalidated them. The annotation-frame checks use the original focused EPUB text and anchors. General reading checks and screenshots still use Type Tester.

## Press feedback captures

The `pressFeedback` gallery case drives the production reader and Library canvases. Capture it alone with `KOBALT_GALLERY_CASE=pressFeedback`; see [Building](../building.md#headless-tests-and-screenshots). Its color palette uses the existing host color-display capability.

## Limits

These headless checks do not establish physical Kobo or Android touch behavior, touch-driver behavior or physical key delivery. Native operating-system cursor display and physical touch interaction still need manual checks.
