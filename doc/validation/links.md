# Links and search checks

Label `links`. Build `mac-tests`, then run `ctest --preset mac -L links` from `platforms/desktop/`. `ctest --preset mac -N -L links` lists its tests.

These checks cover link classification, footnote panels, direct link taps, link reveal, links in the bookmark corner, engine search and search results in the reader. The `engine` checks run on CREngine directly, and the `desktop` and `lifecycle` checks drive the production reader. Word selection in the reader is in [Input](input.md).

The native suite launches no external browser. Physical touch accuracy still needs device validation.

`engine.links` also checks explicit EPUB page-note markup, token lists, individual InDesign note bodies, labeled Gutenberg paragraphs and continuations, exclusion of groups, backlinks, hidden bodies, unlinked notes and ordinary numeric destinations. It compares rendered pages to check that Kobalt overrides publisher note boxes and spacing while preserving inline markup. It checks note continuation fragments and page bounds, popup recognition, note selection rectangles and handles, saved-highlight redraw, source anchors, viewport reflow, restored pixels when disabled and parsed-cache reuse across both choices and themes. `desktop.inpageFootnotes` drives the checkbox, deferred saving, popup access, long-note page turns, reopening and restoring defaults in both themes. The page-note gallery shows the setting, ordinary text, notes, popup and continuation pages at the existing gallery sizes. These cases do not establish compatibility with every publisher’s note markup.
