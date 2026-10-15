## Unreleased

- Add optional `userstyle.css` for book styling. Edits apply when reopening a book and do not affect installation checks.

## v1.0-beta.4

- Refresh the page and bookmark ribbon together when jumping to a bookmark or returning to the previous passage.

- Update bundled FreeType, FriBidi and Android libpng to include upstream security fixes.

- Limit SVG nesting and resource-reference depth to bound recursive renderer work.

- Pause Android reading time while the device sleeps or the app is inactive, so Stats does not count that time as reading.

- Keep missing zero-width spaces invisible and preserve thin and hair spacing when a font lacks those characters.

- Bookmark jumps now offer Return to page in the reading menu, using the same return history as internal links.

- Size Text, Layout and About panels from their visible content. Android's Layout panel no longer leaves space for the omitted rotation controls. Text and Layout scroll when their controls do not fit on screen.

- Added an experimental Android debug APK for ARM64 devices running Android 9 or later. It opens DRM-free EPUBs and keeps books, settings and reading progress locally. Obtain testing APKs directly from this repository; they use a shared public signing key.

- Restore publisher spacing in Kobo books containing the original injected style hacks. Keep kepubify wrapper rules and custom styles.

## v1.0-beta.3

### What's changed

- Improved opening of EPUBs with imperfect markup and unsupported layout features, including books that use Adobe page templates.

- Added Hide non-linear fragments in Settings > Advanced. It skips optional EPUB sections during normal page turns while keeping them available through links, Chapters and page-number navigation. It is off by default.

- Increased the maximum text size from 20 pt to 48 pt and the maximum Thickness from +8 to +12.

- Fixed excessive view redrawing throughout Kobalt, which in practice means you see the screen flash less frequently.

- Opening the font and layout panels is now significantly faster.

- Renamed Bionic Reading to Bold Reading. Upgrades from Beta 2 keep its saved setting. The checkbox now applies immediately. Failed settings saves show a disk write error dialog and keep the previous setting.

- Save changed reading progress to Nickel every 30 seconds while reading and before sleep, so the sleep screen can show the current progress.

- Save reading progress before USB export, while preserving newer synced progress and allowing USB to continue if saving fails.

- Fixed native progress and sleep-cover updates for KEPUB pages without source element IDs or with normalized text. Nickel receives the start of the current chapter when the exact position cannot be mapped; Kobalt keeps the exact position locally.

- Fixed opening and bookmarking DRM-free books downloaded through compatible sync servers that use numeric or other non-UUID book IDs. Existing bookmark folders remain available.

- Stats now uses shared cached word counts on Kobo and Android, including plain EPUBs. Counts cover the whole book and chapter intervals, survive reflow and reuse the parsed-document cache. Chapters uses the EPUB table of contents without Nickel's word-count API.

- Added Automatic brightness for Kobo devices with an ambient light sensor. Manual brightness adjustments turn it off and stop a pending brightness animation from overwriting the chosen level. Cancelled gestures restore the original level and setting.

- Kobo devices with an orientation sensor now offer Portrait, Landscape and Both. Portrait lock is the default, and rejected orientations no longer trigger page layout changes. Devices without a sensor keep manual rotation.

- Added support for the Kobo Touch's physical Home button. Pressing it saves reading progress and returns to Nickel's Home screen.

- Page buttons now page Chapters, Bookmarks, search results and licenses instead of being ignored.

- Sideloaded fonts in folders up to two levels below `fonts/`, such as `fonts/Family/static/`, now appear in the font picker. Hidden files and Kobo's wrapped fonts are skipped.

- Publisher Fonts uses Libron for serif, Legible Next for sans-serif and IBM Plex Mono for monospace when the book has no embedded fonts. The default reading font remains a separate book override.

- Added hyphenation dictionaries for Estonian, Finnish, Georgian, Hungarian, Latvian, Lithuanian, Polish, Slovak, Slovenian and Ukrainian.

- Show formatting warnings only for confirmed unsupported layout features. Large books no longer show a warning when the formatting inspection reaches its limits.

- Show the book-loading screen at 0% in Kobalt's interface fonts, before publication inspection and full asset validation. This makes Kobalt feel faster to start and feels less like it's frozen.

- Long chapters in the Stats chart fill the chart height without a visible line through the bars when chapters are very long relative to other chapters.

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
