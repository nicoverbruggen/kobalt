## v1.0-beta.4

### New

- Header and footer customization has been added with a few presets and the option to fully customize both. The _Custom_ preset is saved globally, but needs to be applied on a per-book basis. Books without a saved header and footer start with the last preset used.

- An optional Kobalt sleep screen on Kobo: turn on Use Kobalt's sleep screen in Settings > System. It shows the dimmed reading page with a card for Sleeping, the percentage read and, when known, the time left. It is off by default, and Kobo's PIN prompt is unchanged.

- Style every book with your own CSS in an optional `.adds/kobalt/userstyle.css`. Any of this CSS is loaded automatically by Kobalt. Use responsibly.

- After jumping to a bookmark, "Return to page" is now offered to take you back to where you came from when you navigated to the bookmark.

- An experimental Android debug build is now available, too.

### Improved

- New icons throughout the reader. The bottom tabs have larger icons and labels on one line, Settings opens from a gear in the toolbar, and each Settings tab shows an icon above its label.

- On Kobo, time estimates use Kobo's own reading pace, and your reading time in Kobalt counts towards Kobo's reading history.

- Interface text on Kobo is sharper at small sizes due to not using font hinting.

- Text, Layout and About panels are only as tall as their content, and paginate when their controls do not fit on screen.

- Minor UI adjustments.

### Fixed

- The header and footer sheet fits six-inch screens, keeps its height when presets or sizes change, and joins the toolbar when the gap is small.

- Showing and hiding link highlights refreshes the whole page once instead of refreshing each link separately.

- Fixed bugs in text rendering, including Arabic letters that did not join, math equations that lost their font, fallback text without real bold or italic, and visible or missing special spaces.

- Kobo custom dictionaries load when the standard dictionary is not installed, and can be chosen next to a standard dictionary for the same language.

- Kobo books that contain Kobo's original injected style hacks get their publisher spacing back.

- The bookmark ribbon now updates together with the page after a bookmark jump.

- On Android, Stats no longer counts time while the device sleeps or the app is in the background.

## v1.0-beta.3

### What's changed

- More EPUBs open correctly, including books with imperfect markup, unsupported layout features or Adobe page templates.

- New option: Hide non-linear fragments, in Settings > Advanced. Normal page turns skip the optional sections of an EPUB, while links, Chapters and page numbers can still open them. It is off by default.

- Text size now goes up to 48 pt, from 20 pt, and Thickness up to +12, from +8.

- The screen flashes less often.

- The font and layout panels open much faster.

- Bionic Reading is now called Bold Reading, and applies as soon as you tick it. Your Beta 2 setting carries over.

- Reading progress is saved to Kobo every 30 seconds and before the device sleeps or connects over USB, so the sleep screen shows where you are.

- Fixed bugs syncing reading progress to Kobo for some KEPUB books, and opening books from compatible sync servers.

- Stats shows word counts for every book, plain EPUBs included, on Kobo and Android. Very long chapters no longer draw a line through their bar in the chart.

- Automatic brightness, on Kobo devices with an ambient light sensor. Adjusting the brightness yourself turns it off.

- Kobo devices with an orientation sensor offer Portrait, Landscape and Both. Portrait is the default. Devices without a sensor keep manual rotation.

- On the Kobo Touch, the physical Home button saves your progress and returns to Kobo's Home screen.

- Page buttons turn the pages of Chapters, Bookmarks, search results and licenses.

- Fonts you add in subfolders of `fonts/`, such as `fonts/Family/static/`, appear in the font picker.

- With Publisher Fonts and a book without embedded fonts, serif text uses Libron, sans-serif uses Legible Next and monospace uses IBM Plex Mono.

- Hyphenation for Estonian, Finnish, Georgian, Hungarian, Latvian, Lithuanian, Polish, Slovak, Slovenian and Ukrainian.

- Formatting warnings appear only for layout features that are really unsupported, and no longer appear for large books just because of their size.

- The loading screen appears at once when you open a book, so opening feels faster and no longer looks frozen.

## v1.0-beta.2

### What's changed

- Fixed opening and appearance previews for EPUBs whose structural CSS selectors require styles to be recalculated after parsing. Publisher image spacing and centered text are preserved.

- Fixed opening EPUBs with repeated identical manifest items. Conflicting duplicate IDs remain unsupported.

- Fixed opening EPUBs with standard IDPF-obfuscated embedded TrueType or OpenType fonts.

- Added evenly spaced line-spacing slider marks with snapping at 100%, 110%, 120%, 130%, 140%, 150%, 175%, 200%, 250% and 300%. Plus and minus still adjust spacing by one percentage point.

- Fixed line spacing being applied twice. 150% now means 1.5 times the font size. Existing book settings are converted to preserve their approximate spacing, with 122% becoming 150% and a maximum of 300%.

- Fixed left alignment and justification moving publisher-centered text to the left.

- Fixed reversed landscape touch targets on affected Clara devices with firmware 4, using NickelRotate’s coordinate correction.

- Fixed opening and saved chapter lookup for sideloaded filenames containing `#`, `?` or literal percent escapes on firmware 4. If files fail to open, an explanation is logged.

- Fixed opening DRM-free local EPUBs regardless of their folder, including hidden folders, SD storage, Dropbox, Google Drive and third-party sync services. Protected books still open in the built-in reader.

- Fixed missing Chinese, Japanese, Korean and other bundled fallback characters in chapter titles and interface controls. CJK fallback text and CFF font previews use Kobalt’s font renderer to avoid crashes in the firmware font driver.

- Load private interface fallback fonts when text first needs them, including the CJK collection. This is more memory efficient.

- Fixed Kobalt remaining inactive when Nickel's initialization callback was missed.

## v1.0-beta.1

### What's New

First public Kobo beta, using Qt5 on firmware 4.38 and later.
