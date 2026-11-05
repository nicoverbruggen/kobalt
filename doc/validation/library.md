# Library checks

Label `library`. Build `mac-tests`, then run `ctest --preset mac -L library` from `platforms/desktop/`. `ctest --preset mac -N -L library` lists its tests.

These checks cover Library onboarding, folder scanning, covers, list and grid layouts, the scan caption, action buttons, Library page buttons, text inheritance for new books and reopening finished books. `model.library` checks the catalogue model. The `desktop` checks drive the standalone Library and reader, and the `ui` checks measure Library widgets in pixels. `session.reading-book`, in [Storage and history](storage.md), checks reopening finished books in the session.

Android Open With uses the same import callback as the Library, but its delivery and rendering need Android device validation. These checks do not validate Nickel database actions or reopening on a physical Kobo or Android device.
