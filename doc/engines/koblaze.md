# Koblaze source

Kobalt owns the engine source in `src/engine/blaze`. It is an ordinary source directory, not a submodule or a link to another checkout. Desktop, Kobo and Android builds use this directory. Kobalt’s adapter lives in `src/engine/blaze/adapter`; the imported library retains its upstream directory structure beside it. Engine objects stay in each platform build directory; the shared dependency cache supplies the external libraries.

The initial import is the library portion of the local koblaze fork at `6d3946edf08ef69afe19a3c457b88af126ae64aa`, based on `koreader/crengine` commit `4ce1aed17e9d7852e047e0b9b0bf6a637486fd97`. These are the same source bytes used by the preceding Mac and Qt5 test builds. The original checkout and its Git history remain separate and unchanged. The import copies source files, not Git history.

The import holds the library sources and headers, the image scaler, the default style sheets and the license notices. It leaves out the CoolReader applications and their packaging, the bundled third-party libraries that Kobalt does not build, and the upstream lint setup, test scripts and development notes. Kobalt supplies its enabled third-party libraries through `dependencies/`. `src/engine/blaze/CMakeLists.txt` is the upstream build file without its lint targets; Kobalt configures it with `AUTO=OFF`.

The engine knows a fixed list of hyphenation pattern names. `src/engine/blaze/data/hyphenation-patterns.txt` holds that list. The pattern files themselves are not part of the engine source: Kobalt ships its own selection from `assets/hyph`.

Preserve the GPLv2 notice in `src/engine/blaze/README.TXT`, the license text in `src/engine/blaze/COPYING`, the image-scaler license, and the copyright headers. Make engine changes in the imported source. The files in `dependencies/crengine-patches.txt` describe the former patched-source build and are no longer applied.

Moving the same source into this repository does not change parsed-book or rendering cache formats. Later parser, preprocessing, shaping or layout changes must follow [Storage compatibility](../storage.md).
