# Settings

`reader-settings.json` holds the global preferences. Its schema is 1.

- Settings without a `schema` are older settings. Kobalt upgrades them atomically.
- Recognized retired keys are converted or removed. Invalid known values use their defaults. Unknown keys are kept.
- A file with a newer schema blocks writes until profile preparation moves it aside. See [Unusable saved data](database.md#unusable-saved-data).
- The Reader and the standalone Library share the file. Each applies only its changed fields, on the profile writer. Removing a setting removes its field.
- Independent settings and unknown keys survive edits, also inside the `library` object. That object holds the Library view, sort and filter preferences.
- `config.json` is the small Kobo enable and disable configuration.

Every key below is optional and keeps settings schema 1. Missing or invalid values use the default. Older builds ignore keys they do not know, unless a row says otherwise.

## Display, theme and orientation

| Key | Values | Default | Notes |
| --- | --- | --- | --- |
| `ui_theme` | `classic`, `playful`, `notebook`, `custom` | see notes | A missing value selects Playful for the prototype's `playful_theme: true`, Custom for a primary font override or `ui_colorful_icons: true`, and Classic otherwise. Saving replaces the prototype flag and keeps the custom primary typeface. The monospace typeface and the header and footer typeface and weight do not depend on the theme. |
| `ui_icon_style` | `monochrome`, `color`, `pastel`, `rings` | `monochrome` | A missing value uses `pastel` when the prototype's `ui_colorful_icons` is true. |
| `ui_palette` | `grayscale`, `blue`, `sage`, `sepia` | `grayscale` | Editing a theme preset replaces the saved custom font, weight and icon values together. |
| `dark_mode` | Boolean | `false` | Dark theme for the reader and its pages. |
| `ui_font_face` | font name | theme font | Sans-serif interface typeface, used by the Custom theme. This is the primary font override that `ui_theme` checks. At most 512 characters, as for every font key. |
| `ui_mono_font_face` | font name | theme font | Monospace interface typeface. |
| `header_footer_font_face` | font name | theme font | Header and footer typeface. |
| `header_footer_added_weight` | `0`, `2`, `4`, `6`, `8` | `2` | Extra weight for header and footer text. |
| `hide_tab_labels` | Boolean | `false` | The tab bar shows icons only. |
| `screen_refresh` | `smart`, `1`, `chapter`, `6`, `10`, `20`, `manual` | `smart` | When a page turn flashes the screen. Smart flashes at a chapter start after at least 10 turns. Numbers flash every that many turns. Manual never flashes on a turn. Closing the menu with a tap on the page always flashes. |
| `show_opening_cover` | Boolean | `true` | False shows the Kobalt logo while a book opens or reopens, also at native-reader handoff. Progress and status messages stay visible. |
| `page_animations` | Boolean | `false` | Page turn animations on hosts that use the display's own animation. The saved value is the same in both themes. On the older i.MX display backend, dark mode turns animation off at runtime without saving a different value. |
| `page_turn_animation` | `off`, `sweep` | `off` | Page turn animations on Android devices other than ONYX, which draw the animation in software. The earlier values `curl` and `fade` read as `sweep`. |
| `zen_mode` | Boolean | `false` | Zen Mode: a simpler reading menu. |
| `zen_stats` | Boolean | `false` | Zen Mode shows its reading stats panel. |
| `edge_light_gestures` | Boolean | `true` | Swipes along the left edge change brightness and along the right edge change warmth. |
| `show_light_gesture_feedback` | Boolean | `true` | False hides the brightness and warmth popup and its cleanup refresh. `edge_light_gestures` still turns the gestures on or off. |
| `orientation` | `1`, `2`, `4`, `8` | host default | The saved reading orientation. |
| `auto_rotate` | Boolean | `false` | Follow the orientation sensor. |
| `locked_orientation` | `0`, `1`, `2`, `4`, `8` | `0` | Zero leaves rotation unlocked. Other values lock that orientation. `orientation` and `auto_rotate` choose the mode that resumes after unlocking. Older builds use their own rotation behavior. |
| `disable_reader_standby` | Boolean | `false` | True stops automatic standby on supported Kobo hosts while the reader is visible and active. Explicit sleep, closing, USB export and shutdown still work as usual. Other hosts keep the value but do not apply it. |

## Typography, layout and book normalization

These keys are global. When a book opens, they replace stale values in its book snapshot. They are not part of text inheritance or text presets.

The text values of a book (`font_face`, `font_pixels`, `line_spacing`, `text_alignment`, `weight_steps`, `font_weight`, `font_optical_size`, `hyphenation`) live in its book state. `last_text_layout` keeps the last values used, for `new_book_text: "last"`, with `line_spacing_version` and the `reading_ppi` they were saved at. Sizes are converted when the density differs.

| Key | Values | Default | Notes |
| --- | --- | --- | --- |
| `default_font_face` | font name | `Libron` | The reading font for books without their own choice. |
| `serif_font_face`, `sans_font_face`, `monospace_font_face` | font name | empty | Fallbacks for the generic CSS families. Empty keeps the bundled default. |
| `page_margins` | object | none | `margin_pixels`, `top_margin_pixels` and `bottom_margin_pixels` in 300 PPI pixels, `0` to `120`. The same physical margins apply at every density. |
| `word_spacing` | `80`, `100`, `120` | `100` | Space width as a percentage of the font's normal space. |
| `justification_tracking` | `0`, `5`, `15` | `0` | Extra letter spacing allowed in justified text: Normal, Subtle or More. |
| `widows_orphans` | `publisher`, `avoid`, `allow` | `publisher` | Whether single paragraph lines may sit at a page edge. |
| `bold_reading` | `off`, `bold` | `off` | Bold Mode. The retired `bionic_reading` key is copied here when `bold_reading` is missing. |
| `hide_nonlinear_fragments` | Boolean | `false` | Page turns skip non-linear sections. Links, Chapters and page numbers can still open them. |
| `two_columns_landscape` | Boolean | `false` | Two columns in landscape. |
| `chapter_starts_spread` | Boolean | `false` | A chapter that starts on a new page starts in the first column. |
| `column_spacing_percent` | `100` to `300` | `150` | Gap between the two columns. |
| `hanging_punctuation` | Boolean | `false` | Punctuation and line-end hyphens can extend into the margins and the gap between columns. Part of appearance and preview identity. |
| `inpage_footnotes` | Boolean | `false` | Turns on CREngine page notes for explicit EPUB note markup. Part of appearance identity, reflow and preview snapshots. A change repaginates. Parsed text stays reusable. |
| `vertical_justification` | `off`, `expand`, `center` | `off` | The Boolean of early development builds maps to `expand` or `off`. Needs parser format 6 or later. |
| `vertical_spacing_scope` | `all`, `protection` | `all` | Used with `vertical_justification`. |
| `paragraph_spacing` | `-1`, or `0` to `200` | `-1` | `-1` keeps the publisher spacing. Other values set the gap as a percentage of the text size. Needs parser format 7 or later. |
| `paragraph_indent` | `-1`, or `0` to `300` | `-1` | `-1` keeps the publisher indent. Other values set the first-line indent as a percentage of the text size. `0` removes it. Uses the same paragraph markers as `paragraph_spacing`. |
| `beautifier_initials`, `beautifier_spacing`, `beautifier_indent` | Boolean | `false` | Part of appearance and preview identity. Cached books keep their earlier initial markers until their cache is rebuilt. |
| `normalize_page_margins`, `reduce_chapter_spacing` | Boolean | `true` | These replaced `avoid_wasteful_styling`. Migration copies its value to each missing rule. |
| `normalize_text_colors` | Boolean | `false` | Sets publisher text to black in light mode and white in dark mode, and clears publisher backgrounds. Part of appearance and preview identity. |
| `book_normalization_preset` | `recommended`, `full`, `custom` | matching preset | Recommended turns on only the two spacing rules. Full turns on every rule. A missing value selects the preset that matches the saved rules, or Custom. An explicit Custom stays Custom. |
| `text_presets` | array, at most 8 | 4 bundled examples | See [Text presets](#text-presets). |
| `new_book_text` | `book`, `last` or a preset `id` | follows `inherit_text_layout` | Kobalt keeps writing `inherit_text_layout` for older builds. |
| `text_presets_open` | Boolean | `false` | Whether the Text panel shows the preset cards. |

### Text presets

- Each entry stores `id`, `name`, `font_face`, `half_points`, `line_spacing`, `text_alignment`, `hyphenation`, `weight_steps`, `font_weight` and `font_optical_size`. Bundled examples also store `example`.
- Sizes are in half points, so a preset looks the same at every screen density.
- A preset never stores margins or Styling values.
- The array order is the order in the Text panel. New custom presets go first. Loading puts custom presets before examples and keeps the order in each group. Use, rename and update do not change the order.
- A missing array means the four examples. An empty array means the user deleted all presets.
- `id` is at most 64 characters and `name` at most 24. `font_face` is at most 512 characters, and empty means Publisher fonts. `half_points` is 12 to 96.
- Cleanup drops entries that break these rules, duplicate IDs and entries after the eighth. Invalid optional values use their defaults.
- Books keep their own text values. Changing or deleting a preset never changes a book. The book-state field `text_preset` records the preset last applied; see [Book state](book-state.md).

## Headers, footers and progress

| Key | Values | Default | Notes |
| --- | --- | --- | --- |
| `header_footer` | `default`, `alternative_a`, `alternative_b`, `phone`, `custom` | see notes | Phone has no header and the usual chapter-pages-left and book-percentage footer. It is the default only on screens with a short edge under 3.4 inches, and only when nothing about headers and footers was saved. Explicit choices survive device detection and rotation. Older builds do not know `phone` and can fall back to their default preset. |
| `header_footer_custom` | object | none | The Custom layout: the six slots (`top_left` to `bottom_right`), `progress_bar`, `uppercase`, and the header and footer sizes. Used when `header_footer` is `custom`. |
| `header_footer_text` | object | legacy layout | Holds `header_size`, `footer_size`, `header_font_size` and `footer_font_size` apart from the preset. The first preset or size edit saves the current sizes. Preset changes then keep them, and size changes do not overwrite Custom. |
| `header_font_size`, `footer_font_size` | `24` to `48` (300 PPI pixels) | legacy size | Stored in Custom layouts and in `header_footer_text`. A missing value uses the legacy Small, Medium or Large size (24, 26 or 28). Off hides the strip and keeps its size. Saving also writes the legacy size string, or Off. Older builds may drop the numeric fields when they save. |
| Custom slot values | includes `page_turn_time`, `chapter_load_time`, `battery_percent_only` | | `battery_percent_only` shows the percentage without the icon. `battery_percent` keeps the icon. An older build shows an unknown slot as Nothing and keeps the rest of the layout. It saves `nothing` for that slot only when the user edits the Custom layout in that build. |
| `header_footer_percentage_format` | `whole`, `decimal` | `whole` | Decimal shows one decimal place in the header, the footer and their previews. Both count the current page and round down. |
| `time_remaining_format` | `detailed`, `approximate` | `detailed` | Display only. Applies to the header, the footer and Kobalt sleep screens. Does not change the stored pace or cache identity. |
| `secondary_page_numbers` | `roman`, `arabic`, `letters`, `prefixed`, `upper_roman` | `upper_roman` | Front matter and back matter each count from the start. |
| `stats_word_scope` | `main`, `volume` | `main` | Main uses the main-text boundaries for the Book figures in Stats. Volume uses the full publication. The lock screen follows this choice. The Android and desktop libraries show main-text progress from the book’s `percent` field in either mode. |
| `activity_welcome_seen` | Boolean | `false` | Activity showed its first-opening alert. |
| `dense_stats_chart` | Boolean | `false` | Written through the profile writer. If the write fails, the graph goes back to the saved mode. |

## Controls, navigation and dictionaries

| Key | Values | Default | Notes |
| --- | --- | --- | --- |
| `page_navigation` | `taps_and_swipes`, `taps_only`, `swipes_only`, `buttons_only` | `taps_and_swipes` | How pages turn. `buttons_only` needs page buttons; without them the saved value acts as the default. One-handed tap zones turn Taps only and Buttons only into Taps and swipes. |
| `tap_zones` | `left_right`, `reversed`, `top_bottom`, `one_handed` | `left_right` | Where taps turn pages. The middle zone always opens the menu. |
| `navigation_bar_buttons` | `chapters`, `pages` | `chapters` | Whether the jump buttons of the Navigate page bar move by chapter or by page. |
| `default_menu_view` | `none`, `seek`, `stats` | `none` | View shown when the reading menu opens. Older profiles map `pin_stats` to `stats` and `show_navigation_thumbnails` to `seek`. |
| `annotation_interaction` | `toggle`, `quick` | `toggle` | Toggle: holding text highlights only in Annotate mode, and otherwise looks it up. Quick: holding text always offers highlight, note and lookup, and the Annotate button is hidden. |
| `minimal_annotation_mode` | Boolean | `false` | Hides the annotation bar. |
| `marks_sort` | `position`, `newest`, `oldest` | `position` | Shared sort order of the Bookmarks, Annotations and My Words lists. |
| `button_actions` | object: control ID to `previous`, `next` or `none` | native direction | `page_upper` and `page_lower` name the page buttons by their position as the device is held, so turning the device upside down swaps them. Volume IDs name physical buttons. Unknown IDs and values stay when a known button is edited, so a profile can move between devices. |
| `reverse_page_swipes` | Boolean | `false` | Reverses horizontal page-turn swipes against the page progression. Independent of navigation mode and tap zones. |
| `tappable_links` | `off`, `footnotes`, `all` | `footnotes` | `off` is shown as Always require long press. |
| `chapter_groups` | `current`, `all`, `off` | `current` | Whether chapter groups can collapse and which open expanded. Does not change layout, chapter statistics or positions. |
| `dictionary_font_face` | font name | empty | Empty follows the open book's body font, including its fallback. |
| `dictionary_font_size` | `80`, `100`, `120`, `140` | `100` | Percent. Applies to definition text only. |
| `save_looked_up_words` | Boolean | `true` | Invalid values are removed. See [Saved words](annotations.md#saved-words). |
| `session_timeout_minutes` | `15`, `60`, `240`, `360`, `720`, `0` | `15` | New session after sleep. `0` is Never. See [Reading sessions](database.md#reading-sessions). |

## Sleep screens

- The default sleep screen is the book cover with Bookmark.
- A one-time cleanup changes `sleep_screen: "stock"` to `"cover"` and sets `sleep_defaults_migrated`. A later explicit choice of Kobo's screen then stays.
- The removed `band` style falls back to Bookmark. Explicit `card` and `none` stay. `overlay` reads the user's PNG files, which are not saved data.
- `sleep_design` selects the design: `stock`, `custom`, `page`, `bookmark`, `card`, `cover`, `seal`, `medallion`, `pill`, `caption`, `widgets`, `night`, `ring`, `framed`, `lines`, `letter` or `overlay`.
- A missing or invalid `sleep_design` uses the legacy `sleep_screen` and `sleep_tile` pair. Choosing a design also writes that pair for older builds. When a later change to the pair no longer matches `sleep_design`, the pair wins.
- Older builds show new designs as the cover with Bookmark. Page and Overlay keep the saved indicator.
- `sleep_greeting` is the Night greeting, at most 40 UTF-16 code units. Missing, invalid or empty text uses Goodnight.
- `sleep_custom` holds the Custom design. `same` is a Boolean, default true. `sleep` and `off` each hold:
  - `background`: `cover`, `page`, `overlay`, `wallpaper` or `plain`. Default `cover`.
  - `status`: `none`, `random`, `bookmark`, `card`, `seal`, `medallion`, `pill` or `caption`. Default `bookmark`.
  - `wallpaper`: an optional file name in `.adds/kobalt/wallpapers`. Empty means a random wallpaper. A name with a path separator, or longer than 255 characters, is ignored.
- Each invalid `sleep_custom` field uses its own default. A `sleep_custom` value that is not an object is removed.
- With Custom selected, the legacy pair stays `cover` and `bookmark`. Older builds that support Custom show an unknown status as Bookmark.
- `random` is chosen per layer and per session. A saved snapshot records the status it shows.
- `sleep_positions` maps a status (`bookmark`, `card`, `seal`, `medallion`, `pill`, `caption`) to `left`, `right`, `top`, `bottom`, `top-left`, `top-right`, `bottom-left` or `bottom-right`. Missing and invalid entries use the status's own place. A book can override this map in its book state.
- `sleep_follow_orientation` is a Boolean, default true. True keeps the reading orientation on sleep and power-off screens. False uses portrait, or inverted portrait when reading that way. Kobo controls its own Stock screen.

## Feature toggles

- `disable_navigate`, `disable_stats`, `disable_annotations`, `disable_bookmarks`, `disable_kosync`, `disable_words`, `disable_highlight_links` and `disable_activity` are Booleans, default false. They hide parts of the interface.
- `disable_kosync` also stops KOSync activity. `disable_words` also stops saving looked-up words.
- `disable_highlight_links` hides manual link discovery. It does not change `tappable_links`, long-press Follow link or saved annotations.
- Hiding Stats also hides Activity. The Activity value itself does not change.
- Hiding a part resets the settings that depend on it:
  - Navigate or Stats as the default menu view resets to None.
  - Hiding Navigate resets its button action.
  - Hiding Stats resets Compact chapter graphs.
  - Hiding Annotations resets Minimal annotation mode.
  - Hiding Bookmarks, Annotations and My Words together resets their shared sort order.
  - Hiding My Words resets `save_looked_up_words`.
- Saved reading data, reading history, time estimates and sync credentials stay.

## Rendering, debugging and power

- `rendering_mode` is `hybrid`, `fast` or `accurate`. Default Hybrid.
  - Settings offers Hybrid and More accurate, and Fast only in debug mode. Without debug mode, a saved `fast` stays saved and opens as Hybrid.
  - A changed mode reopens the book on the engine worker with fresh source and resource caches. The position and session stay. That opening skips the parsed-book cache but does not delete it.
  - Older builds ignore the key and keep it.
- `debug_mode` is a Boolean, default false. System > Developer > Debug mode sets it. It shows the Debug tab and lets `rendering_mode` select Fast.
- `turn_off_wifi_on_open` is a Boolean, default false. It is shown as Smart Wi-Fi power saving. While the feature is disabled, `cleanPreferences` in `src/reading/appearance_settings.cpp` removes the key on every read and write, so the power saver stays off.

## Retired keys

| Key | Handling |
| --- | --- |
| `native_hide_status_bar` | Removed during normalization. The Mac screen preset controls the status bar. |
| `light_dark_mode_shortcut` | Removed during normalization. Light always shows Dark mode on hosts with a frontlight. Dark mode stays in Settings on every host. |
| `book_beautifier` | Removed during normalization. False turns every `beautifier_*` rule off. True keeps the selected rules, and missing rules become true. |
| `avoid_wasteful_styling` | Copied to `normalize_page_margins` and `reduce_chapter_spacing`, then removed. |
| `show_link_indicator` | Ignored. A saved value stays in the file. |
| `playful_theme`, `ui_colorful_icons` | Prototype keys. Read to choose `ui_theme` and `ui_icon_style` when those are missing. |
| `bionic_reading` | Copied to `bold_reading` when that is missing, then removed. |
| `show_header`, `show_footer`, `show_clock`, `show_progress_bar`, `header_text_size`, `footer_text_size` | Converted to a `header_footer` preset, or a Custom layout, when `header_footer` is missing. Then removed. |
| `last_header_footer` | Copied to `header_footer` when that is missing, then removed. |
| `pin_stats`, `show_navigation_thumbnails` | Read to choose `default_menu_view` when that is missing, then removed. |
| `inherit_header_footer`, `show_navigation_bar`, `columns`, `column_spacing`, `smooth_images`, `heavier_interface_text`, `kobalt_sleep_screen`, `highlight_help_seen` | Removed during normalization. |
