# Disposable caches

Caches hold no reading data. Kobalt can always rebuild them, and rebuilding never touches settings, bookmarks, annotations or reading history. Never put saved data in a cache directory.

## Blaze parsed books

- On Kobo, Blaze keeps parsed books in `.cache-blaze/`, in Hybrid and Accurate modes.
- Beta 8 uses the sibling `.cache/`. Blaze never reads, migrates or deletes it, so going back to beta 8 keeps its parsed books. Each engine has its own budget.
- On Android, the cache is `blaze-documents/` in the app's cache folder. Android can clear it and leaves it out of backups. A book then parses again.
- Desktop builds keep no parsed books.
- The budget is 128 MiB. The format is engine-specific binary. It is not an interchange or backup format.
- The engine deletes files it does not recognize in the folder.
- Once the reader shows the whole book, Blaze writes the parsed book in idle steps of at most 20 ms. Closing writes the rest.
- An interrupted save resumes after the completed writes. A change to the document restarts it.
- The cache index keeps access order, also for entries of the same size.

## Compatibility keys

Two keys decide whether a cache is reused.

The parser format is the `formatVersion` argument of `documentCacheChecksum` in `src/engine/blaze/adapter/document_cache.h`.

- The checksum also holds the beta 7 policy token from `platforms/cache-policy.cmake` and the requested DOM version.
- Dependency revisions, build recipes and patch checksums do not change it.
- Review upstream and local patches before calling an update compatible.
- Bump the format when parser or preprocessing changes make a cached DOM unsafe to reuse. The book is then parsed again on its next opening.
- CREngine also rejects incompatible engine formats. Density, user CSS and bold-reading variants keep their own checks.

The rendering policy is `rendering_version` in `platforms/cache-policy.cmake`, plus the names and contents of the bundled XML and packed hyphenation patterns.

- Bump `rendering_version` for shaping or layout changes not covered by CREngine's formatting version. The comments in that file list what each version changed.
- A cache with an older or missing policy keeps its parsed DOM and Kobalt markers, then rebuilds styles and pagination once. Saving records the current policy.
- The optional `kobalt.shaping.text.250000` property stores the bounded text count used to choose fast shaping. A cache without it computes the count once.
- The render context also includes the side allowance each paragraph can use for hanging punctuation and glyph overflow. Two columns with hanging punctuation have a larger allowance. A layout with a different allowance is laid out again.

Parser formats that settings depend on:

| Format | Added |
| --- | --- |
| 6 | Source line boxes and widow and orphan gap markers, for `vertical_justification` |
| 7 | Paragraph spacing and indent markers, for `paragraph_spacing` and `paragraph_indent` |
| 10 | Paragraph markers for containers with zero-width borders |
| 11 | Embedded font size limit |
| 12 | Writing-mode state in computed styles |
| 15 | Flex declarations and computed styles |

Older parsed caches add the optional page-note markers for labeled Gutenberg note paragraphs on opening. Source text, IDs and positions do not change.

## Word counts and chapters

- Chapter indices are rebuilt from the parsed TOC for each layout. The 10,000-entry chapter limit changes no format.
- Cached word counts are JSON of at most 512 KiB, measured by encoded size. Larger counts stay in memory only.
- Stored counts with a different number or order of chapter positions are rejected and counted again.
- Decorative page-spacing records keep prose-gap boundaries and the opening prefix for cache reopening and layout snapshots. A change to them needs a review of both the parser format and the rendering policy.

## Layout snapshots in memory

- An open book can keep one layout for another viewport in memory: node geometry, styles and page boundaries. It shares the DOM and resources. It holds no second engine and no page images.
- Each snapshot has a 4 MiB budget. A swap can hold two for a moment. Larger layouts and DOM changes use normal pagination.
- A snapshot is reused only when the viewport size, column count, column spacing and side allowance match. Typography, layout, user CSS and density changes discard it.
- Closing the book or releasing an idle book discards it. Snapshots never cross document lifetimes. The font manager refuses a different font set while documents are open.
- Snapshots add no disk variants. The disk cache still stores the latest layout.
- Parked chapter layouts release their page images.

## Sleep-screen backdrops

`cache/sleep-screen.png` is the Kobo power-screen backdrop.

- It is saved when a Kobalt book closes or finishes its last USB save. The status tile is drawn later, at sleep or power-off, in the style chosen then.
- The file is replaced atomically. It is at most 20 MiB and 4,194,304 pixels. PNG compression is lossless, zlib level 1.
- PNG metadata version 1 holds the last-read token, mode, orientation, progress, percentage, remaining-time text, theme and a dimming flag.
- Optional metadata: title, author, chapter, reading font, greeting, cover bounds, design and `private_book` (default false). A private snapshot holds only an anonymous backdrop and statistics.
- Images without a percentage still load. Styles that show it then leave it out.

`cache/sleep-screen.json` replaces the PNG when Kobo's native `N3_FULL` cover supplied the book cover.

- It has version 1, kind `native-cover`, and is at most 16 KiB.
- It holds the same display metadata and token, the viewport size, and the cover's path, size and modification time.
- Saving checks the cover again without decoding it. If the cover is missing at save time, the PNG is saved instead.
- Loading checks the cover again, limits it to 20 MiB and 4,194,304 pixels, fits it to the saved viewport and flattens transparency on white.
- A present reference wins over a PNG. A missing, changed or damaged reference shows Kobo's built-in screen, never an older PNG.
- Saving either form deletes the other.

Both forms:

- The last-read token comes from a separate read-only database connection, so it sees committed data.
- Encoding and deletion run on their own worker. For one path, the newest request replaces queued work.
- Standby, USB export and power-off wait for pending snapshot work.
- Home sleep checks the last-read token, and rejects ties, before it uses a backdrop. Native references are checked again before reuse from memory.
- Opening a book or losing authorization clears the backdrop in memory and deletes both files.

## Other caches

`cache/last-book.json` names the last book shown, by `book` identity and `path`.

- Kobalt writes it in the background when a book shows. It reads it after Nickel starts, to prepare that book. See [Opening books](../reader/opening.md).
- A missing, unreadable or outdated file only means no book is prepared.

`cache/previews/` holds the preview tiles of the All presets grid and Text for new books.

- Each tile is PNG data in a `.preview` file. Kobo does not import that extension.
- The file name is a SHA-256 of the store version `kobalt-preview-1` and everything that decides the pixels: the text sample request and the font inputs, which include the rendering policy.
- A tile is drawn once at 600 by 480 pixels at 300 PPI, without a book. The grid scales and colors it.
- A saved preset's tile is drawn when it is saved. An example's tile is drawn the first time a grid needs it.
- Grey tiles are 8-bit palette PNG. Other tiles are RGBA PNG. Both use zlib level 1.
- Files are replaced atomically. Each is at most 1 MiB and 2,097,152 pixels. The folder is at most 4 MiB. The oldest files go first, never the file just written.
- A damaged, oversized or unreadable file counts as a miss and is drawn again.
- Reads and writes run in order on one worker, never on the GUI thread.
- Bump `kobalt-preview-1` when the file contents change incompatibly. Old files then never match, and the size limit removes them.
- Eviction only deletes regular files with a 64-character lowercase hex name and the `.preview` extension.

Kobo cleanup of older `.png` previews:

- At Kobo startup, Kobalt deletes regular `.png` files directly in `cache/previews/`, including hidden ones. Other files, folders and symbolic links stay.
- Cleanup opens each folder without following symbolic links and works relative to that open folder. A later rename or link swap cannot redirect it.
- A `png-migration` marker records pending deletion, pending library scan or done. The pending state is saved before deleting.
- After deletion, Kobalt asks Nickel for its normal library scan, once Nickel has finished starting. Nickel removes the missing books. Its `doneProcessing` signal marks the migration done.
- A failed deletion, a missing scan API or an interrupted scan leaves the migration pending for the next startup.
- An unreadable or malformed marker stops the migration without deleting files.
- Profiles without old PNGs finish without a scan. Other hosts finish after the file cleanup.

`cache/reopen-layout.json` is retired. It was a version-1 cache of whole-book inspection results. Kobalt no longer reads or writes it. Existing files hold no reading data.
