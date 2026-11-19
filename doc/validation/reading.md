# Startup and reading checks

The opening bar gives page formatting the largest share. It advances again when the engine finishes and when the window is built. It reaches 100% after presentation preparation. `desktop.openingProgressStages` checks this order. Presentation does not wait for text preset previews. Opening queues neighbour and estimate work after its completion callback. Preview requests can interrupt estimates without changing the session revision; the estimates resume after the preview. Deferred-image completion also resumes this idle work. The focused checks are `lifecycle.appearancePreviewInterruptsEstimate`, `lifecycle.pageImageDuringHold` and `session.reader-session`, including superseded whole-book-ready notifications.

`session.reader-session` parks a whole-book build before each of two font increases and a decrease in the same Hybrid session. It checks that a turn during reflow is refused, then compares the new page, the following turn and the final whole-book handoff with an Accurate session at the same size.

A numbered jump during handoff uses the displayed chapter. A second number can be used once the new pagination is on screen. A buffered reverse turn must return across the chapter boundary to the same pixels. Rotation while a whole-book count is ready keeps the chapter at the new viewport.

My Words resolves a saved passage while Hybrid switches to whole-book pagination. The returned page must match an Accurate session. `desktop.savedWords` also checks the page number on the action in the word detail view.

The engine checks compare Nickel positions in unloaded chapters without changing the reading page. The Nickel position codec keeps the parsed source of the last chapter file: `engine.reader` encodes every page of three fixtures in order and in an order that changes the file at almost every page, and requires the same positions. Imports preserve the exact source offset in sliced and unsliced chapter windows. Both layouts also resolve the chapter title and direction of a KOSync position outside the displayed chapter.

Native and synced position imports cancel outstanding page previews. An import interrupted by font edits must finish on the final frame, where its caller can save the position and turn the page. Replacing or revoking the source discards its old completion.

A failed jump to a passage in another chapter must preserve the current anchor and pixels and allow a subsequent valid jump to that chapter.

The finish prompt on exit uses whole-book progress even when later slices remain. Forward turns continue until the last readable page. Reopening a finished book at the start must show the original opening page in cold and retained chapter sessions. When a native position and its chapter are both missing, the retained session falls back to that same page.

A revoked source also discards queued illustration frames and neighbour-cache updates.

`lifecycle.preloadedBookOpensWithFreshHost` prepares a book on the engine worker, retains it and checks that its original source lease has ended. It opens the retained book through a fresh publication, checks for a reuse hit with unchanged settings and a new layout after dark mode changes. Both openings register the window’s device observers.

A session discards an unfinished frame when the host revokes the source or the reader cancels it. Font and viewport changes must keep the last displayed image and finish their progress indication.

Label `reading`. Build `mac-tests`, then run `ctest --preset mac -L reading` from `platforms/desktop/`. `ctest --preset mac -N -L reading` lists its tests.

These checks cover empty-library startup, opening books, rendered pages, page turns, saved positions, opening failures and cancellation, the feature tour, reader sessions, the opening cover, local resources and archives. [Test inventory](../validation.md#test-inventory) lists the desktop checks.

`model.resource-cache` checks compacted resource tails with different transfer chunk sizes, reads across block boundaries, empty resources, many small chapters, mixed-size eviction and failed-load accounting. Near the budget, it checks that compaction keeps the original block instead of exceeding the limit.

`ui.openingCoverLayout` checks cover placement, the centred group with a cover of any shape and with the logo, the text's distance from the cover's bottom edge, immediate branding, first-frame tile covers, native handoff and missing-cover fallback. `ui.openingHandoffLayout` checks that protected and remembered native routes keep the normal cover, bar and status positions, complete the bar, and show the destination and redirect icon in both themes, including missing covers and small screens. `ui.openingCoverUpgrade` checks sharper cover selection and asynchronous replacement of a displayed thumbnail. `ui.openingCoverDamage` checks that identical displayed pixels do not repaint, changed covers damage only their own surface, and progress leaves the cover untouched. `ui.openingMessages` checks both initial messages, the shared delayed message and the centered, fixed status area. `engine.local-reader` checks that reuse notifications agree with retained and parsed-book cache hits, including cache invalidation and cancelled opening. The loading gallery includes preparing, reopening, delayed status, missing cover and native handoff. These checks do not measure physical Kobo flashes or cover-loading latency.

`ui.openingLogoSplash` checks the centered logo, presentation without labels or progress controls, transition to live controls, and cancellation before first paint. The loading gallery includes the logo splash in each viewport and theme. Kobo diagnostics record `opening-first-frame` from Kobalt’s accepted opening to its first painted frame, with `candidate_ms`, `first_frame_ms` and `loading_frame_ms` in the opening timings. These measurements start after Nickel hands over the request and do not measure the physical panel refresh.

`desktop.formattingWarning` checks supported, unsupported and supported pages within one chapter, including the warning menu entry and notice. `engine.page-estimate` checks matched CSS, importance, inheritance, unused and hidden declarations, generated content, image-only pages, columns, footnotes and parsed-cache reopening. The `formattingWarning` gallery case captures the menu entry and notice at every viewport and variant. `session.reader-session` checks warning delivery and retained reopening.

A pending native progress comparison must report a stale result when its source ends. Pending selection, token and page-edge requests must return empty after a source ends. Queued chapter metadata must preserve the displayed table of contents when its source ends before publication. The session check covers reader cancellation and direct host revocation.

The Hybrid session test delivers an old pagination result after two font changes while the worker still holds an unpublished whole-book frame. The final frame must use the last font size and chapter pagination.

The chapter-parity reverse-group check records the source anchor and pixels of forward turns, then retraces them after neighboring chapters have been prepared and old documents evicted. It covers one and two columns.

`engine.chapter-parity` uses chapters with an even or odd number of text columns before the next chapter. It checks bookmarks, exclusive highlight ends, search results, contents and chapter lengths against the known column order. With three columns before an eight-column chapter, the second chapter starts on book page 2 and touches five whole-book spreads. Fast pairs its eight columns into four chapter spreads. Once Hybrid has prepared the whole book, its list labels use the whole-book pages while its marks still follow the chapter layout. The optional-chapter check reads the words painted in each column during forward and backward turns and after a direct jump, with the optional chapter shown and hidden.

`session.bookmark-session` reopens saved bookmarks and a highlight from SQLite. It checks their source anchors, local marks and book-page labels as the whole-book layout becomes ready. It also adds a bookmark on the last chapter spread and merges two bookmarks there whose anchors lie on different whole-book spreads.

Search scopes use the next page in the displayed layout, including the page beyond an exposed slice. Hybrid prepares the whole book on the first submitted query. Selecting a result keeps the displayed layout. A later page turn can adopt the prepared book. Fast scans the chapter windows in batches. A batch keeps its original spine range and returns each distinct source range once, including matches in ligatures and reordered combining marks.

The page estimate check enlarges the text after Hybrid finishes a background layout. It checks the displayed count before and after the new layout finishes against an independent opening at the larger text size.
