# Bookmarks and annotations checks

Label `annotations`. Build `mac-tests`, then run `ctest --preset mac -L annotations` from `platforms/desktop/`. `ctest --preset mac -N -L annotations` lists its tests.

These checks cover bookmarks, pinned bookmarks, bookmark returns, highlights, notes, annotation mode, the annotation list, the annotation database and annotation export. The `model` checks hold the stores and the database, the `session` checks run each editing case in separate JSON and SQLite processes, and the `desktop`, `lifecycle` and `ui` checks drive the production reader.

SQLite is the default backend, with an explicit legacy JSON override as documented in [Storage compatibility](../storage.md). A constrained SQLite fixture checks rollback after `SQLITE_FULL`. It does not simulate a full FAT volume or physical power loss. To run only the annotation order cases, use `kobalt-annotation-database-tests --order-regressions [TEMP_PARENT]`. An explicit parent keeps the temporary databases inside a controlled writable test directory.

Right-click annotation entry is a host capability. Desktop enables it; other hosts leave it disabled. Pointer checks cover a normal click, mixed-button gestures, revealed links and a disabled capability.

`AnnotationDraft` owns selection endpoints, punctuation policies, the resolved range, viewport identity and draft geometry. The reader owns gestures, popovers and persistence decisions. `model.annotation-draft` checks that overlap rejection preserves the previous selection, reversed endpoints retain resolved policies, old asynchronous responses cannot replace a newer selection, and image decoding keeps current geometry when its markup is older. Save preparation uses a separate revision: sleep invalidates interactive queries but does not abandon a pending save. Abandoned saves cannot clear a newer draft. Existing desktop and lifecycle checks cover dragging across pages, editing, save failures and annotation frames.

Physical-device touch and e-ink refresh still need device validation.

The pinned-bookmark checks require an opaque theme background inside each shortcut capsule, including after switching themes. Rounded corners and gaps between columns remain transparent. The gallery includes a contrasting backdrop so a transparent interior cannot hide over a blank page margin.

## Export

`model.highlight-store` builds the export archives, and `desktop.annotations` exports through the reader with both the Kobo default folder and the save picker. Headless Qt uses its widget file picker for these checks. Native Mac save sheets, Android document-provider writes and Nickel's import scan still need platform validation.

The host export operation owns confirmation, overwrite checks, destination selection and writing. Shared code supplies the archive and lifecycle eligibility. `lifecycle.annotationExportHandoff` holds a destination write while turning a page, then verifies that close, USB, shutdown and revocation wait for completion. Cancellation drops the observer but keeps a started write in the storage barrier. `lifecycle.annotationExportOwnerDestruction` verifies that destroying the observer suppresses callbacks while the operation drains, and that destroying the operation itself finishes its write before returning. Failed writes report failure. The existing desktop checks retain replacement consent, including a destination created while the first prompt is open.

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
