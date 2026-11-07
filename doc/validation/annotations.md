# Bookmarks and annotations checks

Label `annotations`. Build `mac-tests`, then run `ctest --preset mac -L annotations` from `platforms/desktop/`. `ctest --preset mac -N -L annotations` lists its tests.

These checks cover bookmarks, pinned bookmarks, bookmark returns, highlights, notes, annotation mode, the annotation list, the annotation database and annotation export. The `model` checks hold the stores and the database, the `session` checks run each editing case in separate JSON and SQLite processes, and the `desktop`, `lifecycle` and `ui` checks drive the production reader.

SQLite is the default backend, with an explicit legacy JSON override as documented in [Storage compatibility](../storage.md). A constrained SQLite fixture checks rollback after `SQLITE_FULL`. It does not simulate a full FAT volume or physical power loss. To run only the annotation order cases, use `kobalt-annotation-database-tests --order-regressions [TEMP_PARENT]`. An explicit parent keeps the temporary databases inside a controlled writable test directory.

Physical-device touch and e-ink refresh still need device validation.

The pinned-bookmark checks require an opaque theme background inside each shortcut capsule, including after switching themes. Rounded corners and gaps between columns remain transparent. The gallery includes a contrasting backdrop so a transparent interior cannot hide over a blank page margin.

## Export

`model.highlight-store` builds the export archives, and `desktop.annotations` exports through the reader with both the Kobo default folder and the save picker. Headless Qt uses its widget file picker for these checks. Native Mac save sheets, Android document-provider writes and Nickel's import scan still need platform validation.

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
