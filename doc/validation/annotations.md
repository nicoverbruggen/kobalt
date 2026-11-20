# Bookmarks and annotations checks

The bookmark-session checks also prepare neighboring slices in Fast and Hybrid modes, add and remove a bookmark on the displayed passage, and reopen the saved store to verify that a later bookmark survives. They run against the existing JSON and SQLite storage variants.

The bookmark-session checks compare cached layout keys with the serialized frame after text reflow, viewport changes, a change of slice and revised page counts. With two columns, bookmarks and highlights also update their page labels when Hybrid finishes the whole-book layout, even if the page total stays the same. The bookmark-session check dismisses a search that prepared the whole book and checks the Contents and saved labels before another turn. Chapter parity checks that a turn within the same slice reuses the key shared by bookmarks and highlights.

Label `annotations`. Build `mac-tests`, then run `ctest --preset mac -L annotations` from `platforms/desktop/`. `ctest --preset mac -N -L annotations` lists its tests.

These checks cover bookmarks, pinned bookmarks, bookmark returns, highlights, notes, annotation mode, the annotation list, the annotation database and annotation export. The `model` checks hold the stores and the database, the `session` checks run each editing case in separate JSON and SQLite processes, and the `desktop`, `lifecycle` and `ui` checks drive the production reader.

`desktop.fastSavedLists` creates saved marks through the reader and visits enough chapters to exceed the retained-document limit. It checks bookmark returns and annotation jumps after reopening in Fast and Hybrid chapter mode, and after Hybrid adopts the whole document. The return action must show the origin's whole-book page number and restore its exact source position. The returned annotation must still open its editor without changing its saved data.

`lifecycle.turnSkipsHiddenControls` assigns a different excerpt font to the hidden Bookmarks list. A page turn and opening the reading menu leave that font in place. Opening Bookmarks must apply the book’s selected reading face.

SQLite is the default backend, with an explicit legacy JSON override as documented in [Storage compatibility](../storage.md). A constrained SQLite fixture checks rollback after `SQLITE_FULL`. It does not simulate a full FAT volume or physical power loss. To run only the annotation order cases, use `kobalt-annotation-database-tests --order-regressions [TEMP_PARENT]`. An explicit parent keeps the temporary databases inside a controlled writable test directory.

Right-click annotation entry is a host capability. Desktop enables it; other hosts leave it disabled. Pointer checks cover a normal click, mixed-button gestures, revealed links and a disabled capability.

`desktop.annotationNotes` drives the note flow through the production reader: Add note on a draft, Cancel, an injected SQLite failure and retry with the draft intact, Add note on a saved highlight, the compact menu of a noted highlight, Color with Discard and Apply changes, Edit note and removal by saving an empty note. `model.highlight-session` checks that compact records report which highlights have notes, that excerpts carry notes, that a note written on a compact record keeps its text and chapter, and that whitespace-only and oversized notes are handled. The gallery tour captures the note editor, a noted highlight and its Color step. These checks use the widget keyboard. Nickel's text-box keyboard receiver is checked only against pewter; typing into it, and what its return key does, still need device validation.

`desktop.quickAnnotations` chooses Quick annotation mode in Settings, checks that the regular toolbar button and the Zen tab are hidden while the Zen saved list remains available, and drives a hold and drag across words through the production reader. It checks the release menu, saving, Clear selection and Look up, leaving annotation mode after each, and holding a saved highlight. The `quickAnnotation` gallery case captures the setting's choices and the menu over an outlined selection.

`AnnotationDraft` owns selection endpoints, punctuation policies, the resolved range, viewport identity and draft geometry. The reader owns gestures, popovers and persistence decisions. `model.annotation-draft` checks that overlap rejection preserves the previous selection, reversed endpoints retain resolved policies, old asynchronous responses cannot replace a newer selection, and image decoding keeps current geometry when its markup is older. Save preparation uses a separate revision: sleep invalidates interactive queries but does not abandon a pending save. Abandoned saves cannot clear a newer draft. Existing desktop and lifecycle checks cover dragging across pages, editing, save failures and annotation frames.

The first-paint lifecycle check reopens a book with 256 off-page highlights. It repeats presentation and switches the display to grayscale, checking the saved color and the number of display capability queries. The reader-session check moves a draft without copying its saved ranges and opens an appearance preview with the reopened book's saved highlights and draft in its first frame.

Physical-device touch and e-ink refresh still need device validation.

Custom-pin discovery requires 64-bit inode and file-offset types, including in 32-bit firmware builds. A compile-time check enforces this contract. The model check reports valid-file discovery separately from unsafe-file rejection, so an empty catalog cannot be mistaken for an accepted unsafe file.

`model.custom-pins` checks the restricted SVG shapes, proportional sizing, transparency, malformed and unsupported inputs, XML entities, reference rejection, geometry and directory limits, symbolic links, special files, replacement and removal. The prepared Kobo Qt5 and Qt6 checks also build and run it against their firmware Qt libraries under emulation. `desktop.customPinnedBookmarks` checks custom-first ordering in the combined picker, selection across pages, opening on the selected icon's page, saved names and positions, and the missing-file fallback after reopening in light and dark themes. The gallery uses the same custom editor, help, failed-file list, shortcut, missing-icon and question-mark placeholder scenes. The custom shortcut uses the checked-in Lucide cat SVG in `tests/fixtures/custom-pins/`, with its source and ISC license in the file. The same SVG is validated unchanged on native Qt and both firmware Qt versions. Harmless export classes and accessibility labels must not change its mask. Failed files retain an alphabetically sorted, bounded list of five filenames and a total count. The warning modal uses the shared list rows with left-aligned filenames and dividers. On short screens, the check opens each page at the standard card width and reads the visible filenames in order. Opening and closing help or the warning preserves the draft name and selected icon. The UI checks require one button at the right of the popup header: a warning when files fail to load, or help otherwise. Help remains available from the warning modal. They also require one shared divider between cells. Desktop help shows the supplied profile path rather than Kobo instructions. The pin database regression retains a custom ID through legacy JSON, SQLite import, edits, rollback and stale-writer rejection. These checks do not establish physical storage latency or arbitrary SVG compatibility.

The pinned-bookmark checks require an opaque theme background inside each shortcut capsule, including after switching themes. Rounded corners and gaps between columns remain transparent. The gallery includes a contrasting backdrop so a transparent interior cannot hide over a blank page margin. Stats checks cover bounded shortcuts without overlap with the chart, figures or navigation, reopening with Stats as the default, switching views and rotation. The same jump and return handlers serve both shortcut layouts.

## Export

`model.highlight-store` builds the export archives, and `desktop.annotations` exports through the reader with both the Kobo default folder and the save picker. Headless Qt uses its widget file picker for these checks. Native Mac save sheets, Android document-provider writes and Nickel's import scan still need platform validation.

The host export operation owns confirmation, overwrite checks, destination selection and writing. Shared code supplies the archive and lifecycle eligibility. `lifecycle.annotationExportHandoff` holds a destination write while turning a page, then verifies that close, USB, shutdown and revocation wait for completion. Cancellation drops the observer but keeps a started write in the storage barrier. `lifecycle.annotationExportOwnerDestruction` checks that a profile write completes while the destination is stalled. It also verifies that destroying the observer suppresses callbacks while the operation drains, and that destroying the operation itself finishes its write before returning. Failed writes report failure. The existing desktop checks retain replacement consent, including a destination created while the first prompt is open.

### Export HTML interaction check

The optional Node check runs the production HTML script in a small DOM harness. It does not render HTML or establish browser behavior for local files. It adds no runtime dependency to the app.

After building `mac-fast-tests`, run from the repository root:

```sh
mkdir -p build/annotation-export-preview/search-check
cd platforms/desktop
KOBALT_TEST_EXPORT_ARCHIVE="$PWD/../../build/annotation-export-preview/search-check/fixture.zip" ctest --preset mac -R '^model.highlight-store$'
cd ../..
unzip -p build/annotation-export-preview/search-check/fixture.zip annotations.html > build/annotation-export-preview/search-check/fixture.html
node tests/support/annotation_export_html.cjs build/annotation-export-preview/search-check/fixture.html
```
