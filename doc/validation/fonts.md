# Font checks

Label `fonts`. Build `mac-tests`, then run `ctest --preset mac -L fonts` from `platforms/desktop/`. `ctest --preset mac -N -L fonts` lists its tests.

These checks cover font sources, system and embedded fonts, the font pickers, Publisher Fonts as the default, optical size, KF variants and the font memory limits. `resource.resource-provider` and `engine.reader` check embedded publisher fonts. `component.ui`, in [Settings and controls](settings.md), checks font previews, their cache and fallback faces.

`platform.font-sources` uses the pinned Amstelvar font for source precedence, because its family is absent from the bundled reader catalogue. Bundled Bitter would test duplicate-family precedence instead. `platform.system-fonts` uses synthetic plain, compressed and encrypted wrappers in temporary directories.

The KF variant fixtures keep the original glyphs and metrics. They test catalogue names, not Kobo font transformations. KF variants need no saved-data or rendering-cache format change. Physical-device behavior remains separate validation.

Font pages reuse their cells. A cell that shows another font repaints completely, because Qt 5.2 does not repaint the area of a caption that it hides, and the previous caption stays on screen. `desktop.fontPagingClearsCaptions` checks the full repaint. Desktop Qt 6 repaints that area by itself, so the check cannot show the Qt 5.2 failure. Device validation on firmware 4 covers it.

## Font memory and crash context

Font pickers render each visible font-name preview when its label first paints. The renderer opens a regular face from its file, renders the label, then closes the face. It uses a separate FreeType library and does not register preview fonts with Qt or keep their bytes in the private text renderer. Characters that the face lacks use the bundled fallback faces, which are also opened from their files. A page therefore shows all its samples in the same screen update. The Publisher Fonts row uses a Libron sample. A font that cannot render its name keeps the interface-font label. Nothing generates the whole catalogue at startup.

A cache miss renders on the GUI thread while the page paints. Hardware validation must confirm one display update for cold and cached openings and for paging on the Clara BW. A bounded disk cache for previews, with generation timing and invalidation, is deferred. Broader memory-pressure, performance and energy measurements remain separate work.

The shared preview cache holds at most 4 MiB of 8-bit coverage bitmaps and cache-entry costs. A bitmap is limited to 256 Ki pixels. File paths, sizes and modification times, label text, dimensions, density-scaled sample sizes, weight, Bold Reading, alignment and wrapping identify a cached result. A failed render is cached too, so its label does not reopen the files on every paint. Theme and selection colors reuse the coverage bitmap. FreeType working memory and temporary rendering images are separate from the bitmap cache limit.

The reader discovers selected reading faces from metadata and reads them when text needs them. The private interface renderer keeps at most 100 MiB of font file bytes, enough for one book face, the bundled CJK collection and the interface faces together. Before another read, it releases the least recently used faces and their derived caches. Faces used by the current layout stay pinned. Collections share their source bytes. This limit excludes FreeType and HarfBuzz allocations, glyph and layout caches, Qt fonts and the reading engine. It is not a process memory limit. Bundled script fallbacks load when a glyph needs them, and native text fields register only the missing scripts present in their text.

Normal and verbose diagnostics take a resource sample on the diagnostic worker, at most once per second while they process event batches. Only detailed logs write its fields. Normal mode keeps it for the crash report and the `memory-pressure` check, which warns when available memory falls below 16 MiB, at most once per minute. Samples include process RSS and peak RSS, retained private font bytes, native fallback bytes, preview bitmap cache bytes and descriptor limits. Linux adds available and free system memory, virtual and data memory, thread count, descriptor count and the highest descriptor. Missing kernel fields stay absent. Samples are not taken at the input event; `resource_sample_monotonic_ms` gives their actual age.

`font-preview-bitmap` reports generation time, bitmap bytes, cache bytes and success. Font reads, eviction, picker opening, preview requests and default changes leave operation breadcrumbs. Crash reports copy the latest resource sample and a fixed ring of recent breadcrumbs without the diagnostic queue. Reports cannot capture `SIGKILL`, and a stale sample does not show memory use at the crash. Hardware validation must establish the memory margin on each device.
