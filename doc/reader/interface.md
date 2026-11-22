# Reading interface

The reader's return control combines the arrow and optically centered title in one button.

- The arrow and title share a pressed state, with a 6-point gap between the visible arrow and text and a divider after the label.
  - The arrow and label have small optical vertical offsets. Standalone builds show Library and return to Kobalt's library. Kobo shows Exit Kobalt and returns to the stock library. Closing the book remains separate from exiting a standalone app.

- In the light theme, list rows draw their secondary lines in black, as their titles: the words and definitions in My Words, chapter page numbers, bookmark excerpts, sizes, licences and folder paths. A lighter grey is dithered on e-ink. The dark theme keeps a softer secondary ink.

## Reading menu and Navigate

- The reading menu has Chapters, Navigate, Text, Layout, Light and Stats tabs. Search is in the top toolbar.
- Navigate shows the current page and its neighbors above the book-position bar. Tapping outside the thumbnails keeps them visible. When Default menu style is Default, tapping Navigate again closes the Navigate view and leaves the basic reading menu open. Tapping the middle thumbnail opens that page and closes the controls. Selecting another tab leaves Navigate.
- Controls > Reading menu > Default menu style chooses Default, Navigate Mode or Stats Mode.
  - Default opens the basic reading menu. Navigate Mode and Stats Mode hide their matching tab and show that view whenever the basic reading menu opens, including after closing Settings. Selecting another tab temporarily replaces the default view.
  - Explicit thumbnail or pinned-Stats choices from older profiles carry over to this setting.
- Dots mark chapter starts. Minus and plus sit beside the whole-book map and zoom the detailed scrubber out and in around the current or previewed page.
- Zoom uses 1×, 2×, 4×, 8×, 16×, 24× and 36× steps. A step is available only when its range contains at least 60 pages. Books with fewer than 120 laid-out pages show only the detailed scrubber. At 120 pages, 2× zoom covers 60 pages.
- Books with more than 1,000 laid-out pages start at the first zoom step that shows at most 1,000 pages, usually between 500 and 1,000.
  - The initial zoom stops at 36× even if that still shows more than 1,000 pages. The range is centered on the reading page and stays within the book.
  - Pagination changes recalculate the initial zoom and whether the map and zoom controls are available. The visible page range and zoom scale appear below the track, with 1× zoom at the full-book range.
- The whole-book map and its zoom buttons start collapsed. An upward chevron at the right of the page-and-chapter strip expands them above the detailed scrubber. A downward chevron collapses them again. Expanding or collapsing the map keeps the zoom and reading position. The map can stay open at full zoom-out.
- Its thick, lightly patterned track represents the whole book. Thin brackets mark the visible range. Its track fill uses the paper color while the notches keep their full-book positions. The magnifier has a larger borderless circle of paper color behind it. The brackets keep a minimum grab width at high zoom.
- A separate tick below the window marks the reading page without crossing the magnifier. The window moves only while zoomed in. The labels have space below them before the detailed scrubber. Both rows have padding above and below their tracks and labels and keep the same height at every zoom level.
- Drag the window to move the range through the book, or tap elsewhere on the overview to center it there. Moving the range updates the preview without changing the reading position. Manual zoom remains while the reader is open. Pagination or chapter-position changes restore the initial zoom.

- Previous and Next sit beside the detailed scrubber, below the map and its zoom controls.
  - Double chevrons preview the previous or next chapter by default. Settings > Controls > Navigate panel > Button action switches between Page jumps and Chapter jumps. Page jumps use single chevrons. Navigate always shows page thumbnails and its navigation controls.
  - Default menu style chooses whether Navigate opens with the reading menu; Default leaves it closed until you select the Navigate tab. These settings are saved for all books.

- The page number, total page count and chapter title always appear in a paper-colored strip at the top of the navigation panel, in smaller uppercase monospace.
  - The disclosure cell uses the same background. Only the panel’s top edge has a heavy rule; thin rules separate the caption, map, detailed scrubber and tabs. During a preview they describe the selected page.
  - Highlight links and Return to page use buttons centered above that strip. Each button fits its icon and label in a compact row.
  - The Return button adds a plain X with a taller touch target and a hairline separator to discard the return destination. Labels use optical alignment with their icons. Hairlines also separate the map’s zoom buttons from its track.
  - Both buttons have complete borders and a small gap below them, with or without the scrubber. Moving one page in the timeline reuses the overlapping thumbnails and loads only the newly exposed page. The thumbnails update together after that load.
  - The timeline uses an opaque paper background, white in light mode and black in dark mode, so book text does not show behind the cards. Bookmarked pages show the same ribbon shape as the reading page, including in the side thumbnails.
  - The top toolbar never shows a page counter.

- Dragging the main scrubber right moves forward through its visible page range.
  - The preview uses up to 90% of the available height and keeps the page proportions. Thumbnails include saved highlights with the same colors and overlap order as the reading page. The preview renders the selected page over an opaque paper background.
  - Smaller preceding and following pages enter from the left and right screen edges, with a gap beside the middle preview. Tap a side thumbnail to bring that page into the middle.
  - Swipe left or right across the previews to preview the next or previous page. A swipe changes only the preview, not the reading position. At the start or end of the book, the missing neighbor stays hidden.
  - Tap the middle preview to go there. Tapping outside the navigation controls keeps Navigate open. Selecting a preview requests a full screen refresh when the destination page is ready to clear the preview frame. Selecting the middle preview closes the reading controls.
  - Reopen the controls to use Return to page and go back to the exact page you left. Scrubbing does not save a new position until you select the preview. Selecting another bottom tab hides the navigation panel and cancels the preview.
  - Sleeping or closing the reader also cancels the preview.

- With the Hybrid rendering engine, until it has the whole book, Navigate replaces the scrubber and book map with a read-only book-progress indicator.
  - The arrow buttons jump directly to the previous or next chapter. The side page previews and page buttons turn one page using the normal reading path, including across chapter boundaries. Side-page and chapter jumps keep Navigate open and refresh the previews.
  - Chapter dots remain on the rail, with a thin vertical rectangle showing the current position. Dragging the indicator or preview does not seek. Previous and next previews are rendered without moving the reading position.
  - More accurate rendering keeps the full scrubber behavior described above.

## Reader panels and controls

- Text and Layout panels measure their visible controls and messages, including optional font controls.
  - They keep their bottom edge above the navigation bar. When the controls exceed the space below the top toolbar, the panel reaches the toolbar and pages between complete rows, with Previous and Next below them, as the Header and footer sheet does.
  - About measures its content and leaves room for the settings tabs beside it.

- Tapping the selected Text, Layout, Stats or Light tab closes that panel and keeps the reading controls open.
  - Tapping the exposed book page outside these panels returns to reading and hides the controls. The Chapters close button and selected Chapters tab both close the list and deselect the tab in the same screen update.
  - Appearance changes finish applying before the panel closes. Panel switches and taps that hide all controls keep their separate refresh policy.

- Reader buttons keep their keyboard focus behavior without drawing a native focus frame.
  - A held bottom tab uses the same continuous fill as a selected tab. Pressing or cancelling a tab repaints only that tab and its adjoining rule, without refreshing the whole navigation bar.
  - In the light theme on MediaTek Kobo displays, the tabs keep partial refresh when a panel opens, so an unchanged selected tab does not receive a full flashing refresh.

- On Kobo Qt5 and Qt6 builds, the reading menu's status strip shows a Bluetooth icon beside the battery while Bluetooth is powered on.
  - It indicates that Bluetooth is enabled, not that a remote is connected. A Wi-Fi indicator appears beside Bluetooth only while connected with a positive signal-quality report. It stays hidden while disconnected or when signal quality is unknown or zero.
  - Radio and signal changes use Nickel notifications without scans or polling. Both indicators are hidden while the reading controls are closed. Nickel controls both radios. Android and desktop do not show these indicators.

- When Kobo reports a battery change, the menu, visible reading strips and open header and footer previews show the new level and charging state. Plugging or unplugging the reader updates the charging icon even when the percentage stays the same.

- Native Kobo dialogs, including software-update prompts, accept input above the open reader. Closing a native dialog returns input to Kobalt. Sleeping or closing Kobalt restores the dialog's original window state.

- Kobalt has one interface, in `src/app/reader/`.
  - Its navigation has Exit Kobalt, Search, Annotate, Bookmarks and Settings at the top, and Chapters, Navigate, Text, Layout, Light and Stats in bottom tabs. Chapters covers the top row with its own Chapters bar and keeps the status strip and the tabs.
  - Each entry shows its number and the page where it starts. The close cell on the right of the bar and the Chapters tab both close the list.
  - Settings sits in the same place as the close cell, so it ignores taps until 400 ms after the list has closed. The same applies after Bookmarks, Annotations and Settings close. Settings files from older versions can contain `ui_style` and `familiar_navigation`.
  - Kobalt ignores them and keeps them in the file.

- The interface is designed for 300 ppi screens, such as the Clara and Libra, and scales to the screen's density, so controls and text have the same physical size on every device.
  - Kobalt reads the density from the device name that the firmware exports in `PRODUCT`, as Kobo's own display plugin does: 212 ppi for the Nia and Aura, 227 for the Elipsa models and 265 for the Aura HD and H2O.
  - For an unknown device it uses the density that Qt reports, and otherwise 300. It logs the result as a `ui-density` event. Thin control borders and dividers use two pixels at 300 ppi and one pixel at the Nia's 212 ppi.
  - Heavy panel separators keep their own thickness. The Light panel marks brightness at 10% intervals while retaining 1% adjustment. Warmth uses evenly spaced 10% stops for both dragging and the step buttons. Kobalt converts those percentages to Nickel's native temperature range.
  - Native rounding is accepted and the controls retain their size during adjustment.

## Loading screen

- On Kobo, an accepted book opening shows the loading screen at 0% before inspecting the EPUB or validating the whole asset bundle.
  - The screen draws in Kobalt's interface fonts from its first frame. When Kobalt starts, a worker thread reads only those font files and checks them against the asset manifest.
  - The first opening then only registers them, and waits if the reading is still running. Later openings reuse the registered fonts. The opening failure dialog is built only when an opening fails. The remaining opening work starts after that screen paints.
  - Progress updates change the bar without repainting the full loading screen.

- The loading screen reads an existing cover on a worker as soon as opening begins.
  - Kobo uses Nickel’s cached cover; Android and desktop use the Library cover. This does not wait for the reader session or the decision to open in Nickel. The cover remains visible during that handoff, including when it arrives after the decision.
  - The publication cover replaces the cached thumbnail when available. Kobalt’s logo is visible immediately while no cover is available. The cover replaces it as soon as it arrives. A failed cover extraction never clears a cover already shown.

- The loading screen centres its visible group on the screen: from the top of the drawn cover, or of the logo when there is no cover, to the baseline of the status caption.
  - The cover keeps its own shape. The label, progress bar and caption keep fixed distances from the cover's bottom edge, so a cover that is not 2:3 moves them.
  - When a cover replaces the logo, the text moves to centre the new group. The back arrow keeps its place. Page formatting takes up most of the opening bar. The bar reaches 100% when the first page is ready to show.
  - A cached book can skip work and advance more quickly.

- On Kobo, the first loading screen reuses the selected book tile's cover when an unambiguous image is available. Missing or generated tile covers leave the normal loading icon in place. Cover files load on a worker thread. A sharper cover replaces the thumbnail when available, and the image keeps its aspect ratio.

## Screen sizes and orientation

- A screen whose short edge is under 3.4 inches uses the phone form factor, measured from native pixels and reader density.
  - Phones and the BOOX Palma (2.75 inches) use it; the six-inch Kobo Clara (3.57 inches) does not. Detection is independent of orientation and decides defaults such as the Phone header and footer preset. The arrangement follows the current orientation instead.
  - A window narrower than 3.4 inches shows bottom navigation icons without labels.
  - Sliders and segmented choices put their captions above the controls when a side caption would leave too little width; a slider's value then sits on the caption line, at the right.
  - A window shorter than 3.4 inches, such as a phone in landscape, keeps both menu bars and puts the clock and battery in the toolbar between the exit action and the actions on the right.
  - When the exit caption nearly reaches the actions, the exit action extends to meet them. The exit action shows its right divider only when the gap before the other actions is at least one regular toolbar button wide.

- Hosts can report a safe area: a band at the top or bottom that a camera cutout or system bar covers, and the radius of rounded screen corners.
  - The page paper fills the bands and the text starts inside them. The menu puts its status line in a top band at least as tall as the status line, and starts below a shorter one.
  - Full-screen views such as Search and Acknowledgments, and a dictionary sheet at the top, start below the band. Header and footer text moves in from the sides until it clears a rounded corner. Side cutouts stay outside the reader, as margins.
  - On Android, the Library and reader both reserve the larger reported side inset on both sides.

- Reader panels and the top and bottom menu toolbars use the full screen width in both portrait and landscape.

- On firmware 4.45 and later 4.x, only Clara BW and Clara Colour use NickelRotate’s final touch orientation transform while Kobalt owns rotation.
  - Both landscape orientations use the corrected quarter-turn. Closing or suspending the reader restores native input mapping. Clara HD, Clara 2E and other models retain native mapping. The correction requires both touch driver hooks and the expected portrait panel geometry.
  - Firmware 5 and 6 use Kobo’s Qt6 touch driver, which updates its mapping when the screen orientation changes, including on Clara devices.

## Reading status

- In Hybrid, opening the reading menu lets the engine lay out the whole book at the next quiet moment. While pages are being counted, the top status row shows a centered hourglass. The indicator follows the background layout state. When the count finishes, the usual reading time returns if the session is over five minutes.
- After five minutes, the top menu shows how many whole minutes the book has been open since the last opening. On Kobo, this count includes kernel standby while the reading page remains visible. Putting the reader to sleep or hiding the reading window pauses the count. Closing the book or a USB connection resets it.

## Battery benchmark

- NickelBatteryTest, which is not included in this public repository, can select a temporary appearance profile for its dedicated benchmark EPUB.
  - That opening uses the embedded publisher font, Kobalt’s default text size and 60-pixel margins at 300 ppi, 144% line spacing, publisher alignment and light mode. Text and margin sizes follow screen density. Hyphenation, extra weight and Bold Reading are off.
  - Kobalt keeps the original book appearance values, including absent values, when it saves reading progress. The profile stays in memory and does not change global settings or other books. Settings writes are disabled for that opening.

## Dialogs and lists

- Reader confirmations use shared notice and choice modals with a required title, an optional description and an optional image.
  - Dictionary removal and restoring appearance defaults open over the settings sheet. A notice has one action. A choice has two actions. The primary action is filled and the secondary action is outlined.
  - Actions share padding and stack vertically with the primary first when their labels do not fit in one row. Long labels wrap inside the buttons. The font selector points up toward its picker panel.

- Chapters, bookmarks, dictionary choices, reading header and footer size choices, and acknowledgments share list rows and caption strips.
  - One, two and three text lines use shared row heights. Bookmark and annotation rows use the selected reading font for excerpts. The annotation list reserves two excerpt lines per row so it can paginate before requesting text.
  - It fetches only the current page of excerpts and releases those details when the list closes. Bookmarks retain their content-dependent row height. Current rows are inverted, pressed rows use the existing gray highlight, and long titles end with an ellipsis.
  - Moving a pointer over reader controls does not highlight or repaint them. Row press feedback ends on release, including when a list reopens with keyboard focus on that row. Search results also use book-font excerpts and the shared list spacing and dividers.

- Long lists wrap when paged.
  - Next on the last page returns to the first page, and Previous on the first page opens the last page. This applies to Chapters, Bookmarks, Search results, font choices, dictionary choices, downloaded dictionaries and the acknowledgments list.
  - Empty and single-page lists disable both page actions. Dictionary definitions, footnotes, license text and the chapter graph keep their page boundaries. Paging a list does not change its selected item.

## Interface text

- Use title case for named dialogs. Use sentence case for confirmation questions such as “Delete reading activity?” Keep screen names capitalized, as in “Remove from Library?” Give an action and a dialog separate string keys when they need different capitalization.

## Zen Mode

- Settings > Display > Zen Mode gives the reading menu a smaller layout. A lotus icon identifies the row. Its title reads ZEN MODE in the same style as the reading menu’s ZEN mark. It is a global setting and is off by default. Restore appearance defaults turns it off. The rest of Settings stays the same.

- The menu shows two centered groups instead of the full-width bars. Each group has rounded outer corners and a heavier rule than the regular bars.

- The top group hangs from the top edge.
  - Its status line shows a ZEN caption on the left and the clock in the center. The caption has the same space to its left as above and below it, and the battery icon the same space to its right.
  - The battery appears on the right only at 20% or lower. Zen Mode hides the minutes-open count and the Wi-Fi and Bluetooth indicators. A gray divider separates the status line from three cells, the back arrow, Reading stats and Settings.
  - The arrow has no label. Its accessible name is Exit Kobalt on Kobo and Library elsewhere.

- Reading stats shows the chart icon with a chevron.
  - It is a lasting toggle for all books. While it is on, every Zen menu opens with a panel under the cells, and the chevron points up.
  - The panel shows the time read in this session, the pages turned in this session and the time left in the chapter. Gray dividers separate the three figures and continue the cell dividers.
  - A time of an hour or more reads like 4 h 35 min. The time left uses the Stats estimate and shows ≈ while Kobalt learns the reading pace. The ≈ is smaller than the numbers and larger than the units.
  - Each turn that reaches the screen counts once, also a page button turn while the menu is open. Taps buffered during a slow render do not count. A new session after sleep sets both session counts back to zero.
  - The preferences writer thread saves the toggle, so the main thread does not wait for storage.

- The bottom group sits on the bottom edge.
  - It holds Chapters, Search, Annotate and Bookmarks as tabs in the regular tab style. Annotate turns highlight mode on and off, as the toolbar button does. Bookmarks opens the shared Annotations and Bookmarks list, and a second tap closes it.
  - Annotate appears when Annotations is enabled and Highlighting is set to Toggleable annotation mode. Quick annotation mode starts a highlight by holding a word, so it hides the toggle in both menus. Bookmarks shows when Bookmarks or Annotations is enabled.
  - Without the Bookmarks feature, the tab reads Annotations and shows the note icon, as the toolbar button does. With both features hidden, the group holds Chapters and Search.
  - Zen Mode does not show Navigate, Text, Layout, Light or Stats, and Default menu style has no effect. It also hides the manual KOSync button and the formatting warning button. Sync now stays in Settings > Sync.
  - Edge swipes for light still work.

- Bottom cells are at most 214 design pixels wide and top cells at most 250, so the stats have room.
  - Both groups keep the 36-pixel inset from the screen edges. On narrow screens the cells become narrower. The stats panel grows with large text. When one figure is wider than its column, all figures shrink together.
  - Tab labels give way to icons when they do not fit, as in the regular bar.

- While the Zen menu is open, the header and footer are blank.
  - The groups lie over the page, clipped to their rounded shape, and the text around them stays visible. The page does not reflow. Closing the menu brings the header and footer back.
  - Every way out of the Zen menu requests one full cleanup, also a jump from Chapters, Search or Bookmarks, so the groups leave no ghosting. Search opened from the Zen menu requests it when Search closes, with or without a jump.
  - Turning Zen Mode on or off requests one full cleanup when Settings closes, because the whole menu changes. Chapters, Bookmarks and Settings keep the regular top bar.
  - Below Chapters and Bookmarks, the bottom group stays with the open view's tab selected and attaches to the view. It drops its rounded frame, and the regular bar's heavy rule ends the view across the full width, directly above the tabs.
  - The tabs keep their size and place, so the tapped tab does not move. Gray dividers close the group at both ends, and paper covers the page beside it, so no book text shows next to the tabs.
  - Opening either view, closing it with its tab and returning from Settings to the Zen menu each update the screen once.

- In Zen Mode, Settings shows one short page instead of the section tabs.
  - A ZEN caption sits at the right of the Settings heading. Display holds Dark mode, which applies at once, as on the Display tab.
  - On a device with a frontlight, Light appears when the edge swipes for light are off, because Zen Mode has no other light control. Light closes Settings and opens the Light panel.
  - Sync holds Sync now, with the last sync time as its description, and Automatic sync with its schedule. It appears only when a KOSync account is signed in, KOSync is not hidden and the open book can use KOSync.
  - About Kobalt shows the version as its description and opens the About page with a back cell. All other settings stay hidden until you leave Zen Mode. Turning Zen Mode on stays on the Display tab.

- To leave Zen Mode, hold Hold to leave Zen Mode.
  - The button inverts and fills three steps, one every half second. Each step updates only the button, so the screen shows no animation. Letting go early clears the steps and keeps Zen Mode.
  - At the third step, Zen Mode turns off, Settings shows all its tabs on Display and the screen gets one full refresh. Closing Settings applies the change, as turning Zen Mode off on the Display tab does.
  - In portrait, the button sits in a bar along the bottom edge, with a note below it. In landscape, it is a cell at the right of the heading, so the page keeps its height.

## Header and footer

- Header and footer text defaults to NV Jost at Medium (26 pixels at 300 PPI). Saved font and size choices still apply. In fonts that support tabular digits, the numbers take equal space, so the clock, page counts and percentages can change without shifting neighboring characters.

- Layout has a Header and footer row.
  - Its button shows the preset in force, left-aligned and vertically centered, and opens the Header and footer view. The header and the footer have three slots each: left, center and right. A center slot is centered on the page.
  - When the slots do not fit, the side slots get less space first.

- Each slot shows one field.
  - The field picker shows these groups in order.
    - Nothing.
    - The book title and the author.
    - The chapter.
    - The page in the chapter, the pages left in it, the percent of the chapter read and the chapter with its page (such as Chapter 1 · 5 of 33).
    - The page in the book, the percent of the book read and the title with the book page.
    - The time, the battery with its percentage, the battery percentage only and the battery.
    - Battery warning and the time left in the chapter and in the book. Each has a description below its name.
    - Page turn time, Chapter load time and Book status in debug mode.
  - Without debug mode the list offers one of these only when the slot shows it. A layout that has them keeps them; see [Settings](settings.md).

- Battery warning shows the battery and its percentage only below 20%.
- Percentages count the page being read, so the last page reads 100%. A page count on its own reads 5 / 33 with half-width spaces around the slash, in the strips, the preview and the field list.
- The combined chapter and page field keeps the page count visible when its title is too long. Only the chapter title ends with an ellipsis. If no title fits beside the count, the field shows the count alone.

- Time left uses the same estimate as Stats.
  - On Kobo, it uses Nickel's reading pace as soon as source word counts are ready. Android and desktop use saved word-based reading history. They show a rough estimate with `≈` while history is limited.
- The Battery fields stay empty in Fill window on Mac; device previews show a full battery. The time always shows. Battery percentage only uses the strip’s normal text size and shows no icon, including while charging.
  - An unavailable battery level leaves this field empty. With an icon, the percentage matches the battery icon’s visible height and is centered beside it. Both scale with the selected strip size. Changing the percentage does not change its font size.

- Page turn time shows how long the last page turn took, such as 12 ms.
  - It counts from the accepted tap or button press to the moment the reader receives the new page image, before the page is painted. Each page shows the time of the turn that brought it.
  - A turn queued behind a running turn keeps the time of the first one and does not update the field. Before the first turn the field is empty, and the field list shows 12 ms as an example.

- Chapter load time shows how long the engine took to load and lay out the spine items or the part of a spine item on the screen.
  - A part that was prepared before the reader needed it shows 0 ms. When the engine has the whole book, this field is empty.
  - These two fields add no repaint and no screen refresh: their values are filled in with the rest of the strips.

- Book status shows the last page turn time and the layout, such as turn: 22ms | layout: fast (37%).
  - Before the first turn it shows the layout alone. The layout is fast while the engine lays out a chapter at a time and full once it has the whole book.
  - With Hybrid, fast shows how far the background layout is: the page estimate measures the spine items up to 15%, the whole-book build goes on to 100%, and 100% waits for the next turn, which shows the whole book.
  - Fast (failed) means the build gave no usable book.
  - Full shows how long the whole book took from the opening or the last new appearance or page size, such as full (55s), with one decimal below ten seconds.
  - A failed build also logs a `whole-book-failed` warning with the step that failed. Book status updates every 5 seconds while it is in a shown header or footer, so it can refresh the strips between page turns.
  - Without the field in a strip, nothing wakes the reader for it.

- With the Hybrid rendering engine, the page fields show Calculating… while the book has no page numbers, for example just after it opens or after a text change.
  - They then show the estimate as soon as every chapter is measured, marked with ~ before the total, and the exact count once the whole book is laid out.
  - Both appear without a page turn, and the header and footer refresh only for those two steps. The percentage fields always show a percentage.

- Customize header and footer fills the space below the reader status bar. The font button beside Close selects the typeface for both strips and saves the same preference as Settings. Close returns to Layout. The desktop preview toolbar remains available.

- The panel shows Kobalt, Bookish, Ember and Custom.
  - Narrow phone screens also offer Phone. It has no header and keeps chapter pages left and book percentage in the footer. Phone is the initial choice when no header/footer choice has been saved. Existing choices remain unchanged.
  - Short screens page between complete rows so preset names, slot targets and sizing controls keep their height. The selected miniature has a heavy border and an inverted name; a held miniature already looks selected. Choosing one applies it at once.

- Tap a slot in the full-width preview to change what it shows. This updates the Custom preset for every book that uses it. Turning off a header or footer hides its slots and keeps their assignments. Turn it back on to edit them.

- The preview uses the selected typeface, size and alignment. Shaded bands show the space each strip takes, with dotted lines where the page text begins and ends. The preview keeps the same height as you adjust the strips.

- Progress bar follows the slot configuration and remains part of the selected preset.
  - It offers Off, Bottom and Top. The bar sits 8 design pixels from the screen edge. Its 3-pixel rule and 4-pixel fill leave 4 pixels on the text side, for a total of 19 design pixels at 300 PPI.
  - The rule is ink up to the reading position and gray after it. Top draws it along the top edge and moves the header text down by the same gap that the footer keeps above a bar at the bottom.
  - The footer then needs no room for the bar, so the page keeps its height. Off remembers the edge, so turning the bar on again restores it. The presets draw it at the bottom.

- A ruler button to the right of Top adds chapter ticks: 4-pixel gaps cut through the rule and the fill where chapters start, on the same scale as the fill.
  - They use the shallowest level of the contents that has three chapter starts inside the main text, between its first and last page, or else the level with the most, so a book that nests its chapters under a part or a half-title still gets them.

- Each tick cuts a gap in the bar, so a level may have at most one tick for every 12 design pixels of the bar's width, about 89 on a six-inch screen. A level with more would cut the bar away, so the search stops there, and a book with only such levels shows no ticks.

- They come from a layout of the whole book, so they never move: More accurate shows them at once, and Hybrid once it has prepared the whole book, without another turn.
  - The book saves them with its page estimate, so it shows them at once when it opens again with the same layout. The button is disabled while the bar is off. The preview above shows the book's real progress and the ticks.

- Header size, footer size and padding apply to every book and preset. Adjusting them keeps the selected preset.

| Setting | Off | S | M | L | XL |
| --- | --- | --- | --- | --- | --- |
| Header or footer text | Hidden | 24 px | 26 px | 28 px | 48 px |
| Extra padding per enabled strip | 0 px | 12 px | 24 px | 36 px | 48 px |

- Values are design pixels at 300 PPI. The + and − buttons adjust the value by one pixel. With padding off, Small and Medium each reserve 48 pixels per strip. Large reserves 56. Text is centered vertically, with separate space for the progress bar. A strip with three empty slots takes no space.

- Kobalt, the default, shows the time on the left and the chapter title centered in the header. The title is uppercase. The footer shows pages left in the chapter on the left and whole-book completion percentage on the right, without a progress bar.
- Bookish shows the time and the chapter with its page in the header, and the title with the book page in the footer, in uppercase, with the progress bar.
- Ember shows the time in the center of the header, the pages left in the chapter at the bottom left and whole-book completion percentage at the bottom right, without uppercase and without the progress bar.
- Custom is your own layout.
  - Any edit starts from the layout in force and switches the book to Custom, so a preset can be adjusted. Custom counts as modified when it differs from Kobalt; its card and the Layout row then read Custom *.
  - When a preset is in force and Custom is modified, an edit or a tap on a slot first asks whether to replace your custom layout. Cancel keeps both; Replace applies the edit to the preset and saves the result as Custom. An unmodified Custom is replaced without asking.
  - Tap a slot to customize it: Customize slot and the slot name head its fields, which fill the reading area like Chapters, each with its current value in the selected header or footer typeface, size and added weight.
    - Rows are as tall as Chapters rows, and the described fields take a second line. Each row has its radio on the left, as in Refresh the screen; a held row turns gray and fills its radio.
    - A choice applies and returns, as choosing a chapter does; the close cell returns without one.
  - Fields that do not fit go to further pages with the Chapters pager, and page buttons turn them. Opening a saved slot goes straight to the page containing its field.
  - The Uppercase checkbox on the right of the bar sets that slot's case and keeps the list open. Close returns from the field list to the sheet, then from the sheet to Layout.

- Each book saves its preset.
  - A book without a saved header and footer starts with the preset of the book opened or changed last. Settings > Defaults can disable this inheritance independently of text and layout. With it off, new books start with Kobalt.
  - The Custom layout is shared by every book that uses Custom; an edit replaces it. Choosing Custom before any edit starts from the preset in force.

- Restore appearance defaults selects Kobalt for the book.
  - A book saved before presets keeps its old header, footer, clock and progress bar: it uses Kobalt when they match the old defaults, and Custom otherwise.
  - Custom then uses the saved Custom layout, or those old settings when no Custom layout is saved. An old clock without a header becomes a small header with only the time.

## Keyboard

- On the desktop development app, keyboard characters use the larger IBM Plex Sans interface face. The `123` key opens numbers and punctuation. Its label changes to `ABC` to return to letters.

- Kobo uses Nickel’s built-in keyboard for text entry, including its numbers, symbols and language layouts.
- Android uses its native system keyboard.
- The desktop development app uses Kobalt’s widget keyboard.
  - Kobalt’s keyboard follows the current light or dark theme and provides letters, numbers, symbols, Shift, Space and Backspace. Keys have rounded corners and thicker outlines.
  - Shift and Backspace use symbols. On a window narrower than 3.4 inches, the symbol rows of ten keys are as wide as the letter rows; only the punctuation row uses narrower keys.
  - Backspace sits above the wider submit key, which has a Return symbol beside the current action label. The submit key is black in light mode and white in dark mode, with opposite text and symbol colors. Pressing it reverses those colors.
  - Symbols invert with the keyboard, including pressed keys and selected Shift. Hold a letter to choose an accent or a ligature. Shift also applies to these choices. Release, then tap the choice, or drag onto it and release.
  - Tap another key or press Escape to dismiss the choices. Dictionary and account editors stay above the keyboard.
