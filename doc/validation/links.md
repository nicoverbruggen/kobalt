# Links and search checks

Label `links`. Build `mac-tests`, then run `ctest --preset mac -L links` from `platforms/desktop/`. `ctest --preset mac -N -L links` lists its tests.

These checks cover link classification, footnote panels, direct link taps, link reveal, links in the bookmark corner, engine search and search results in the reader. The `engine` checks run on CREngine directly, and the `desktop` and `lifecycle` checks drive the production reader. Word selection in the reader is in [Input](input.md).

`desktop.tappableLinks` checks every direct-tap mode in both themes with Highlight links shown and hidden. It also checks that actual Settings hides the discovery controls. `desktop.settingsFeatures` toggles the feature through the checklist and checks the action after saving and reopening.

The native suite launches no external browser. Physical touch accuracy still needs device validation.

`desktop.links` opens short, long and legacy footnotes in the book, then returns to the reference. It checks the exact source position and rendered page pixels. Run it with `KOBALT_CHAPTER_MODE=0`, `1` or `2` to cover Accurate, Fast or Hybrid rendering.

`engine.links` also checks explicit EPUB page-note markup, token lists, individual InDesign note bodies, labeled Gutenberg paragraphs and continuations, exclusion of groups, backlinks, hidden bodies, unlinked notes and ordinary numeric destinations. It compares rendered pages to check that Kobalt overrides publisher note boxes and spacing while preserving inline markup. It checks note continuation fragments and page bounds, popup recognition, note selection rectangles and handles, saved-highlight redraw, source anchors, viewport reflow, restored pixels when disabled and parsed-cache reuse across both choices and themes. `desktop.inpageFootnotes` drives the checkbox, deferred saving, popup access, long-note page turns, reopening and restoring defaults in both themes. The page-note gallery shows the setting, ordinary text, notes, popup and continuation pages at the existing gallery sizes. These cases do not establish compatibility with every publisher’s note markup.

The chapter link checks follow a passage beyond the current slice and a passage in another chapter. They compare the destinations and rendered pages with Accurate and check that hidden and missing passages stay unavailable.

Search checks compare the prepared-text path with page search for Unicode normalization, inline markup, source anchors and excerpts. They verify that scanning leaves excerpts and page numbers unresolved, then compare on-demand details with page search, including scope clipping. They exercise bounded batches, uncapped dense matches beyond 500, chapter and page-boundary scopes, query replacement and reflow during both unfinished and completed searches. The reader check also covers the scope radio selection, reopening partial results, two-second progress updates, immediate completion and valid or invalid result-page input. It measures the scope caption ink to check vertical centering and compares the toolbar icon with the selected radio-row icon for every scope. Before/after scopes include/exclude the displayed page respectively. Prepared text is an in-memory layout cache; it does not change parsed-book or saved-data formats. The CREngine search patch exposes the existing normalization pipeline without changing parsing or rendering, so cache versions remain unchanged.

`engine.chapter-parity` searches unopened chapters for accented text, punctuation variants, Arabic words with tatweel, and words split by comments or CDATA. It compares the source position of each result with whole-book search.

The scoped search check compares Current chapter, Up to this page and After this page with Accurate on the Type Tester. It compares the source ranges and clipped excerpts across chapter and slice boundaries, including after following a result and on the last page.

Worker budgets are checked between logical text blocks. A single unusually large paragraph can exceed the preparation budget; physical Kobo typing latency and memory use still need device validation.

The reader checks verify zero, one and grouped match counts in every scope, localized number grouping, exact result-page totals at completion and partial totals after cancellation. They also check that the single-page counter stays visible and the zero-match counter stays hidden.

Search uses the existing Settings status strip. The reader checks cover its clock and battery, and the Zen menu check verifies that Search uses the full-width strip.
