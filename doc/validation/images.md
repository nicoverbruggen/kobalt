# Image checks

Label `images`. Build `mac-tests`, then run `ctest --preset mac -L images` from `platforms/desktop/`. `ctest --preset mac -N -L images` lists its tests.

These checks cover the image viewer, image proportions, SVG limits and the image policy for resources. `desktop.imageViewer` opens the checked-in Type Tester cover in the production reader. `session.reading-book`, in [Storage and history](storage.md), checks the saved image views.

`engine.image-proportions` includes both figures from [GitHub #87](https://github.com/nicoverbruggen/kobalt/discussions/87). Set `KOBALT_TEST_IMAGE_PROPORTIONS_CAPTURE_DIR` to keep its rendered fixture pages, and run it alone with `ctest --preset mac -R '^engine.image-proportions$'`.

SVG images are rendered at their source dimensions in this prototype. These checks do not establish physical touch-driver behavior, pan refresh quality or memory use on Kobo and Android. Physical Kobo rendering and the original reported EPUB need separate validation.
