# Viewport and rotation

## Viewport transitions

Rotation, desktop preset changes and window resizing immediately replace the page text with grey placeholder blocks at half the current font height. The blocks use a subdued grey in both themes. A short word sample from the displayed page is prepared in the background after the page paints. The transition repeats that sample as word segments, with widths estimated from Unicode character counts and the current text size. Line spacing, margins and destination columns follow the current settings. Until the sample is ready, the transition uses continuous placeholder lines. They do not predict publisher styling or the final line breaks. Text reflows after the viewport has been stable for 100 ms. A static Updating page layout notice appears after one second if the replacement is still pending. The replacement page and its marks appear together. Page gestures and page buttons are ignored until the replacement is ready so they cannot use the old page coordinates.

## Layout reuse when rotating

The first switch to a new reading viewport lays out the book. Returning to the previous viewport can reuse its layout while the book and text settings stay unchanged. The source reading position stays the same across both switches. The cache uses actual dimensions and column geometry, so another window size or safe area can require layout again.

The reader retains one alternate layout within a memory budget. A text-setting change, an idle book or a layout above the budget uses normal layout on the next switch. A rotation that changes the engine’s document structure also needs normal layout. Page painting still runs for the selected viewport. Layout reuse does not keep page pictures for both orientations or preload the other orientation.
