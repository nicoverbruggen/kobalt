# Library checks

Label `library`. Build `mac-tests`, then run `ctest --preset mac -L library` from `platforms/desktop/`. `ctest --preset mac -N -L library` lists its tests.

`model.library` holds the profile writer busy when the Library save timer fires. It checks that the next event runs while storage is busy, the settings reach storage while the Library is open, and closing saves the most recent edit. It also checks that progress saves and edits to other folders leave unchanged folder rows alone. Folder order and labels survive reopening. Removing an imported book whose key starts with U+FEFF preserves the book with the otherwise identical key.

These checks cover Library onboarding, folder scanning, covers, list and grid layouts, the scan caption, action buttons, Library page buttons, text inheritance for new books and reopening finished books. `model.library` checks the catalogue model. The `desktop` checks drive the standalone Library and reader, and the `ui` checks measure Library widgets in pixels. `session.reading-book`, in [Storage and history](storage.md), checks reopening finished books in the session.

The theming gallery returns from a book to the Library and checks the saved preset’s typeface, palette and toolbar icons. The standalone Library also applies these settings at startup.

The About checks open the screen from an empty Library without requesting a book. They cover shared version fields, project-row painting through the standalone canvas, project paging, nested Back navigation, both themes, portrait and landscape sizes, and interface text at 100% and 150%. The gallery includes the Library entry and all About pages.

Android Open With and the Library use the same native delivery callback. A Library provider copy carries its request ID so a cancelled or older copy cannot reuse a passage. The portable Library model test checks request matching, cancellation and supersession. Android picker delivery and rendering still need device validation. These checks do not validate Nickel database actions or reopening on a physical Kobo or Android device.

On Android, check book-to-Library return with both the system navigation bar and gesture navigation. On BOOX, select Settings > Gestures > Bottom > Gestures, open a book, then tap Library. The Library must replace the reader and accept input with the navigation bar still hidden. Repeat with Navigation Bar selected and restore the original setting after testing. A visible status bar is sufficient for the Library handoff; gesture navigation can leave the navigation inset at zero.

`desktop.resourceFolders` drives the optional Android Library entry, the three resource choices, changing and clearing them, and modal paging. The gallery includes the surrounding menu and empty and selected folder states. The host picker is injected for these checks; Android's system picker and persisted grants still need device validation. Run `cmake -P platforms/android/check-user-folders.cmake` with a JDK to check the real Java mirror worker against controlled document sources. It covers unchanged copies, replacement, removal, unavailable providers, failed copies, cancellation, unknown dates, nested fonts, invalid names, duplicate names, limits and clearing one choice without affecting another. The Android build runs these checks with the prepared SDK's JDK before compiling the APK.
