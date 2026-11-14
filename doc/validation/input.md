# Input checks

Label `input`. Build `mac-tests`, then run `ctest --preset mac -L input` from `platforms/desktop/`. `ctest --preset mac -N -L input` lists its tests.

These checks cover touch and pointer input, holds, swipes in paged lists, press feedback, the page-turn tap filter, hardware page buttons, navigation previews, the touch pointer and word selection in the reader. The `ui` checks deliver input through the production graphics canvas to the production widgets. CTest runs `desktop.wordSelection` once for each display density and theme, and `desktop.hardwarePageButtons` once for each open view, each in its own process. The page-turn tap filter is described in [Diagnostics](diagnostics.md#page-turn-diagnostics).

`ui.holdInvalidatedTarget` checks pointer and touch contacts when a frame invalidates the lookup target: the hold threshold, queued source timestamps, timestamp wrap, small movement, swipes, cancellation and the next hold. `ui.holdInvalidationStopsTimer` uses a shortened interval to check that a late short release stays a tap and a long one is consumed. `lifecycle.pageFrameDuringHold` delivers mouse and touch contacts across a real page turn or reflow, including adjacent pages with the same text position, and verifies that a stale hold cannot start lookup or cause another turn. `lifecycle.pageImageDuringHold` decodes a deferred image during the first hold. It asserts that a token-pending frame precedes a queued marker before the token job, and that a geometry-pending frame arrives after a draft markup request but before its rectangles and handles are installed. The dictionary-open row checks the dictionary panel position and existing handles. The selection-open row checks the visible menu card position and existing handles, rather than the full-screen overlay. These checks separate gesture consumption from lookup-target validity and retain ordinary short-tap navigation.

The word-selection check drags a highlight back across a slice boundary within one chapter and checks the selected words on both pages. Longer passages must retain the same source text, sentence context and endpoints as Accurate. An existing highlight whose start is outside the slice must still prevent an overlap. Publisher-hidden text stays excluded. Source-only selection uses the original block methods in memory and keeps the parsed-book and rendering cache formats.

## Restored input and paint checks

The UI and reader-window checks keep selected assertions from `tests/android/android_ui_test.cpp` and `tests/native-ui/reader_test.cpp` at `82b8935`. The toolbar invalidation check measures the reader window directly, because a graphics proxy renders widgets into its display cache and adds paint events that do not prove that the reader invalidated them. The annotation-frame checks use the original focused EPUB text and anchors. General reading checks and screenshots still use Type Tester.

## Press feedback captures

The `pressFeedback` gallery case drives the production reader and Library canvases. Capture it alone with `KOBALT_GALLERY_CASE=pressFeedback`; see [Building](../building.md#headless-tests-and-screenshots). Its color palette uses the existing host color-display capability.

The desktop touch-pointer check also verifies that device previews suppress hover tooltips in the reader, Library and preview controls. Fill window and unrelated windows keep normal tooltip delivery.

Chapter-group checks cover separate disclosure and title hit areas, disclosure swipes through the graphics canvas, nested visibility and navigation through a synthetic EPUB. The component checks also cover destination-less headings, duplicate destinations, source numbering, paging, resizing and full-width row hit areas. The `chapterGroups` gallery case captures expanded and collapsed groups, the collapsible chapter preference, its Off mode and the Navigation and Links settings in the existing viewport and appearance variants.

`desktop.buttonMapping`, `desktop.buttonMappingActions` and `desktop.buttonMappingUnavailable` use the production Controls entry, device diagram, action chooser and asynchronous preference writer. They check Cancel, Do nothing, both buttons advancing, native direction fallback, unsupported hardware, persistence after reopening, preview paging and preservation of unknown button mappings across e-reader and phone layouts. The `buttonMapping` gallery case captures the Controls entry, mapping modal, action chooser and Do nothing draft, including all pages of cards that need paging. These host fixtures do not verify physical Kobo or Android key delivery.

`desktop.swipeChoices` checks the navigation-mode radios and independent Reverse swipe direction checkbox, mode changes and preference saves. Its `swipeChoices` gallery case captures the Controls entry, default chooser and checked reversal with Taps only in all viewports and themes. `desktop.pageSwipes` checks normal and reversed horizontal page turns with LTR and RTL spine progression, portrait and landscape, both themes, unchanged tap and logical button directions, vertical drags, disabling taps and swipes, changing direction in the same reader and restoration through a fresh controller. The appearance-settings model check covers missing and invalid values without changing layout or navigation mode.

## Limits

These headless checks do not establish physical Kobo or Android touch behavior, touch-driver behavior or physical key delivery. Native operating-system cursor display and physical touch interaction still need manual checks.

`lifecycle.sleepSettingsShowWhereToAddPngs` checks physical page-button routing through all sleep gallery pages in both directions, consumes turns at each boundary and leaves the reading position unchanged. `desktop.settingsTextPresetGallery` checks the same routing and boundary consumption in the text preset gallery. Both galleries and the paged reader panels use the standard `PagerBar`.
