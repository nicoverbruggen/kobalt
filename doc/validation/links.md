# Links and search checks

Label `links`. Build `mac-tests`, then run `ctest --preset mac -L links` from `platforms/desktop/`. `ctest --preset mac -N -L links` lists its tests.

These checks cover link classification, footnote panels, direct link taps, link reveal, links in the bookmark corner, engine search and search results in the reader. The `engine` checks run on CREngine directly, and the `desktop` and `lifecycle` checks drive the production reader. Word selection in the reader is in [Input](input.md).

The native suite launches no external browser. Physical touch accuracy still needs device validation.
