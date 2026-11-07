# Startup and reading checks

Label `reading`. Build `mac-tests`, then run `ctest --preset mac -L reading` from `platforms/desktop/`. `ctest --preset mac -N -L reading` lists its tests.

These checks cover empty-library startup, opening books, rendered pages, page turns, saved positions, opening failures and cancellation, the feature tour, reader sessions, the opening cover, local resources and archives. [Test inventory](../validation.md#test-inventory) lists the desktop checks.

`ui.openingCoverLayout` checks immediate branding, a seeded cover on the first paint, cover placement, cached covers delivered during a Nickel handoff and preservation of a cover when later extraction fails. `ui.openingCoverUpgrade` checks that the disk worker can replace a seeded thumbnail and that a late lower-resolution image cannot replace a sharper cover. The loading gallery includes the cover, delayed hint, missing cover and Nickel handoff. Physical Kobo cover-loading latency still requires a device check.
