# Acknowledgements

Kobalt uses code and assets from the projects below. Thank you to their authors and contributors, especially the KOReader community for its work on CREngine and the renderer extensions.

This list covers the Kobo, Android and macOS reader builds, adapted source, bundled assets and development tools. Some dependencies apply only to one platform. Full license texts and copyright notices are kept in [assets/notices](../assets/notices/). The reader also shows these notices in Settings → About → Acknowledgments.

## Reader dependencies

| Project | Authors and contributors | Use in Kobalt | Notices |
| --- | --- | --- | --- |
| [CREngine, KOReader fork](https://github.com/koreader/crengine) | Vadim Lopatin, original Cool Reader author, and the KOReader contributors. | EPUB parsing, layout, rendering, MathML and default book styles. | [CREngine](../assets/notices/CREngine-GPL-2) |
| [KOReader-base](https://github.com/koreader/koreader-base) | The KOReader contributors. | LunaSVG extensions for SVG text and images. | [AGPL](../assets/notices/AGPL-3) |
| [Qt](https://www.qt.io/) | The Qt Company and the Qt contributors. | Interface controls, graphics, networking, sensors and database access. Kobo builds use the device's Qt libraries. Android bundles Qt 6.11.3. | [LGPL](../assets/notices/Qt-LGPL-3), [GPL](../assets/notices/Qt-GPL-3), [Android third-party notices](../assets/notices/Qt-Android-Third-Party) |
| [LLVM C++ runtime](https://libcxx.llvm.org/) | The LLVM Project contributors. | Android NDK 27.2.12479018 supplies libc++, libc++abi and libunwind. | [Apache License 2.0 with LLVM exceptions and legacy notices](../assets/notices/Android-Cxx-Runtime) |
| [SQLite](https://www.sqlite.org/) | The SQLite authors and contributors. | Persistent reading data, bookmarks, annotations and the standalone library. Android's Qt plugin includes SQLite 3.54.0. Other targets use the device or SDK library. | [Public-domain dedication](../assets/notices/SQLite) |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | David Turner, Werner Lemberg, Behdad Esfahbod, Ebrahim Byagowi and contributors. Microsoft supplies the Universal Shaping Engine data. | Text shaping. | [HarfBuzz](../assets/notices/HarfBuzz), [Microsoft](../assets/notices/HarfBuzz-Microsoft) |
| [FreeType](https://freetype.org/) | David Turner, Robert Wilhelm, Werner Lemberg and the FreeType contributors. | Font rasterization and outline emboldening, using bundled FreeType 2.15.0. Portions of this software are copyright © 2026 The FreeType Project. All rights reserved. | [FreeType](../assets/notices/FreeType) |
| [FriBidi](https://github.com/fribidi/fribidi) | Dov Grobgeld, original author; Behdad Esfahbod, Roozbeh Pournader, Khaled Hosny and contributors. | Unicode bidirectional text, using bundled FriBidi 1.0.18. | [FriBidi](../assets/notices/FriBidi-LGPL-2.1) |
| [libunibreak](https://github.com/adah1972/libunibreak) | Wu Yongwei, Tom Hacohen, Petr Filipsky, Andreas Röver and contributors. | Unicode line and word breaking. | [libunibreak](../assets/notices/libunibreak) |
| [utf8proc](https://github.com/JuliaStrings/utf8proc) | Public Software Group; Steven G. Johnson, Jiahao Chen, Peter Colberg, Tony Kelman, Scott P. Jones, Jonas Fonseca and contributors. Unicode supplies the character data. | Unicode normalization for book search, including Unicode data. | [utf8proc](../assets/notices/utf8proc) |
| [LunaSVG](https://github.com/sammycage/lunasvg) and [PlutoVG](https://github.com/sammycage/plutovg) | Nwutobo Samuel Ugochukwu and contributors. | SVG rendering with KOReader extensions and local nesting limits. PlutoVG includes rasterization code from FreeType. | [LunaSVG](../assets/notices/LunaSVG), [FreeType rasterizer](../assets/notices/PlutoVG-FreeType) |
| [libjpeg-turbo](https://libjpeg-turbo.org/) | Miyasaka Masaru, D. R. Commander, Viktor Szathmáry and contributors; Thomas G. Lane, Guido Vollbeding and the Independent JPEG Group for the original JPEG code. | JPEG decoding. This software is based in part on the work of the Independent JPEG Group. | [libjpeg-turbo](../assets/notices/libjpeg-turbo), [Independent JPEG Group](../assets/notices/Independent-JPEG-Group) |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | Guy Eric Schalnat and Group 42; Andreas Dilger, Glenn Randers-Pehrson, Cosmin Truta and the PNG Reference Library contributors. | PNG decoding. Android bundles libpng 1.6.59; other targets use the device or SDK library. | [libpng](../assets/notices/libpng) |
| [zlib](https://zlib.net/) | Jean-loup Gailly and Mark Adler. | Compression and EPUB archive support. | [zlib](../assets/notices/zlib) |
| [xxHash](https://github.com/Cyan4973/xxHash) | Yann Collet and contributors. | Fast hashing. | [xxHash](../assets/notices/xxHash) |
| [OpenSSL](https://openssl-library.org/) | The OpenSSL Project authors and contributors. | HTTPS dictionary downloads in Android builds. | [OpenSSL](../assets/notices/OpenSSL) |

Application dependency revisions and checksums are recorded in [dependencies/manifest.json](../dependencies/manifest.json). Prepared Qt SDK source pins and patches are maintained in [kobuild](https://github.com/nicoverbruggen/kobuild). The release source bundle includes the exact Android Qtbase and QtSensors archives pinned in [dependencies/release-sources.json](../dependencies/release-sources.json). [dependencies/android-runtime.json](../dependencies/android-runtime.json) records the audited SDK, APK library inventory, Qt revisions, LLVM runtime source and build recipe, and notice checksums. CI rejects a changed SDK or native library inventory until these records are reviewed.

The Android Qt notices include the upstream copyrights and license texts from the matching SDK's SPDX dependency inventory. The module inventory also covers code conditional on other platforms; those credits do not mean every listed component runs on Android. Qt's source archive expands its Git revision marker and omits Git export metadata files. The compiled source files match the SDK's source inventory. The imported CREngine source and Qt source archives retain their upstream notices.

Kobalt's independent footnote matcher follows the popup detection rules developed by the [KOReader contributors](https://github.com/koreader/koreader/blob/892034c96ef6c4f8606809133d16bb604f8853c1/frontend/apps/reader/modules/readerlink.lua#L1411) and implemented in [KOReader-base](https://github.com/koreader/koreader-base/blob/51bc7096973bc8eab5f2fd6ca3ae9a0227c8905e/cre.cpp). Nico Verbruggen maintains the Kobalt implementation.

## Adapted code and Kobo integration

| Project | Authors and contributors | Use in Kobalt | Notices |
| --- | --- | --- | --- |
| [m-tky/crengine](https://github.com/m-tky/crengine/tree/6a56ffa0f1afc95e9e5463c5ca6584f57022d047) | m-tky, with the original CREngine and KOReader contributors. | Vertical writing changes integrated into Kobalt's CREngine fork, including the [rotated-run buffer safety fix](https://github.com/m-tky/crengine/commit/86d3ffb73b460b88374265a4524d65b20e605442). | [CREngine](../assets/notices/CREngine-GPL-2) |
| [Qt image scaling in CREngine](https://github.com/koreader/crengine) | Daniel M. Duley, Carsten Haitzler and the Qt, Imlib2 and CREngine contributors. | Image interpolation derived from Qt and Imlib2. | [Qt and Imlib2](../assets/notices/Qt-image-scaling) |
| [NickelHook](https://github.com/pgaskin/NickelHook) | Patrick Gaskin and contributors; Qt6 adaptation by Nico Verbruggen. | Mod loading, hooks and crash recovery. Kobalt includes an adapted Qt6 port. | [NickelHook](../assets/notices/NickelHook), [port license](../platforms/kobo/toolchains/nickelhook-qt6/LICENSE) |
| [NickelRotate](https://github.com/nicoverbruggen/NickelRotate) | Nico Verbruggen, with NickelHook by Patrick Gaskin. | Firmware 4 Clara touch orientation correction. | [NickelRotate](../assets/notices/NickelRotate-MIT) |
| [NickelDissolve](https://github.com/nicoverbruggen/NickelDissolve) | Nico Verbruggen, with underlying NickelMenu and NickelHook code by Patrick Gaskin. | MTK display submission and fallback behavior for directional page-turn bands. | [NickelDissolve](../assets/notices/NickelDissolve-MIT) |
| [Kobo-Reader source releases](https://github.com/kobolabs/Kobo-Reader) | Kobo and the upstream authors recorded in the Qt patch notices. | Firmware-specific Qt changes reconstructed in the Qt6 SDK patches. | [Qt patch notices](https://github.com/nicoverbruggen/kobuild/tree/main/qt6/source) |
| [OpenEmbedded meta-qt6](https://code.qt.io/yocto/meta-qt6.git) | Martin Jansa, Holger Freyther, Samuli Piippo and the meta-qt6 contributors. | Build and platform changes carried by Kobo's Qt source release. | [Patch provenance](https://github.com/nicoverbruggen/kobuild/tree/main/qt6/source) |
| [SQLite](https://www.sqlite.org/) and [SQLCipher](https://www.zetetic.net/sqlcipher/) | The SQLite authors and contributors; Stephen Lombardo and Zetetic LLC for SQLCipher. | Database code included in the Kobo Qt6 source patches. | [SQLite and SQLCipher notices](https://github.com/nicoverbruggen/kobuild/tree/main/qt6/source) |

## Fonts, icons and hyphenation

| Project | Authors and contributors | Use in Kobalt | Notices |
| --- | --- | --- | --- |
| [IBM Plex](https://github.com/IBM/plex) | IBM and the Plex contributors. | Plex Sans and Plex Mono for the interface and reading bar. | [IBM Plex](../assets/notices/IBM-Plex-OFL) |
| [Noto](https://github.com/notofonts) | The Noto Project Authors. | Noto Serif, Noto Sans Math, Noto Naskh Arabic and Noto Serif fallback fonts for Hebrew, Armenian, Georgian, Devanagari, Bengali, Tamil and Thai, including their available bold and italic faces. | [Noto](../assets/notices/Noto-OFL) |
| [Noto Serif CJK](https://github.com/notofonts/noto-cjk) | Adobe and the Noto Project Authors. | Chinese, Japanese and Korean fallback glyphs. | [Noto Serif CJK](../assets/notices/Noto-Serif-CJK-OFL) |
| [Instrument Serif](https://github.com/Instrument/instrument-serif) | The Instrument Serif Project Authors. | Loading-screen wordmark and caption. | [Instrument Serif](../assets/notices/Instrument-Serif-OFL) |
| [eBook Fonts](https://github.com/nicoverbruggen/ebook-fonts) | Nico Verbruggen, collection and font adaptations; original font authors listed below. | Reading fonts listed below. | [Font credits](../assets/notices/eBook-Fonts), [Atkinson Hyperlegible Next](../assets/notices/Atkinson-Hyperlegible-Next-OFL), [Domitian](../assets/notices/Domitian) |
| [Phosphor Icons](https://github.com/phosphor-icons/core) | Helena Zhang, Tobias Fried and the Phosphor contributors. | Reader interface, Navigate compass, Styling settings and annotation action icons, regular weight. | [MIT](../assets/notices/Phosphor-MIT) |
| [TeX hyphenation](https://github.com/hyphenation/tex-hyphen), [wortliste](https://repo.or.cz/wortliste.git), [LibreOffice dictionaries](https://github.com/LibreOffice/dictionaries) and [TeX Users Group exceptions](https://ctan.org/tex-archive/info/digests/tugboat/hyphenex) | The TeX hyphenation, wortliste, LibreOffice dictionary and TeX Users Group authors; conversions by the KOReader contributors. Individual pattern authors are listed below. | Bundled hyphenation patterns. | [Pattern credits and licenses](../assets/notices/Hyphenation-patterns) |

Nico Verbruggen maintains the eBook Fonts collection and its NV font adaptations. The original font designs remain credited to their authors below:

| Bundled family | Source and original authors |
| --- | --- |
| [Libron](https://github.com/nicoverbruggen/libron) | Libron and [Readerly](https://github.com/nicoverbruggen/readerly) are adaptations by Nico Verbruggen, based on [Newsreader](https://github.com/productiontype/Newsreader) by the Newsreader Project Authors. |
| [Sourcerer](https://github.com/nicoverbruggen/sourcerer) | Adaptation by Nico Verbruggen, based on Source Serif 4 by Adobe. |
| NV Ancizar Serif | [Ancizar Serif](https://github.com/UNAL-OMD/UNAL-Ancizar), by the Ancizar Serif Project Authors. |
| NV Bitter | [Bitter](https://github.com/solmatas/BitterPro), by the Bitter Project Authors. |
| NV Charis | Charis, by [SIL Global](https://www.sil.org/). |
| NV Garamond | [EB Garamond](https://github.com/octaviopardo/EBGaramond12), by the EB Garamond Project Authors. |
| NV Jost | [Jost](https://github.com/indestructible-type/Jost), by the Jost Project Authors. |
| NV Legible Next | Atkinson Hyperlegible Next, by the [Braille Institute](https://www.brailleinstitute.org/). |
| NV Literata | [Literata](https://github.com/googlefonts/literata), by the Literata Project Authors. |
| NV Palatium | Domitian, with original work by URW++ Design & Development and Daniel Benjamin Miller. See the [font source collection](https://github.com/nicoverbruggen/ebook-fonts) and [notices](../assets/notices/Domitian). |

The individual hyphenation pattern credits are preserved in the [pattern notices](../assets/notices/Hyphenation-patterns):

| Patterns | Original authors |
| --- | --- |
| English US | Gerard D. C. Kuiken; exception data by the TeX Users Group. |
| English GB | Dominik Wujastyk and Graham Toal. |
| German | The wortliste contributors. |
| Dutch | Piet Tutelaers. |
| French | Daniel Flipo, Bernard Gaulle, Arthur Reutenauer, Marc Lodewijck, Paul Pichaureau and Blaise Drayer. |
| Spanish | Javier Bezos and CervanTeX. |
| Italian | Claudio Beccari. |
| Portuguese | Pedro J. de Rezende and J. Joao Dias Almeida. |
| Brazilian Portuguese | Raimundo Santos Moura. |
| Estonian | Enn Saar. |
| Finnish | Kauko Saarinen. |
| Georgian | Levan Shoshiashvili. |
| Hungarian | Bence Nagy. |
| Lithuanian | Sigitas Tolušis and Yannis Haralambous; original patterns by Vytas Statulevičius. |
| Latvian | Janis Vilims. |
| Polish | Hanna Kołodziejska, Bogusław Jackowski and Marek Ryćko; conversion and corrections by Artur Polaczyński. |
| Slovak | Jana Chlebíková. |
| Slovenian | Matjaž Vrečko. |
| Ukrainian | Maksym Polyakov. |

The [font manifests](../assets/fonts/) and [icon manifest](../assets/icons/phosphor/manifest.json) record source revisions and checksums.

## Optional dictionaries

- [koreader-dicts](https://github.com/DanielGregorini/koreader-dicts), maintained by Daniel Gregorini and contributors, supplies the optional dictionary downloads used by standalone builds. The [dictionary catalogue](../assets/dictionaries.json) records each archive's source, license and checksum. These dictionaries are downloaded separately from the reader package.

## Build tools

[NickelTC](https://github.com/pgaskin/NickelTC), by Patrick Gaskin and contributors, supplies the firmware 4 Qt5 SDK used by Kobalt's toolchain. The Qt6 SDK uses the official Qt sources with the Kobo and OpenEmbedded changes credited above. The published [kobuild](https://github.com/nicoverbruggen/kobuild) images, maintained by Nico Verbruggen, provide the Kobo and Android SDKs.

The builds also use CMake, Ninja, GNU Make, GCC or Clang, GNU Binutils, pkg-config, Docker or Podman, QEMU user-mode emulation and the Android SDK and NDK. These are development tools, rather than libraries bundled into the Kobalt reader package.

The [in-app acknowledgments](../assets/acknowledgments.json), dependency manifests and license notices are the source records for this document.

Android BOOX support uses [AndroidHiddenApiBypass 4.3](https://github.com/LSPosed/AndroidHiddenApiBypass) by the LSPosed contributors, under the Apache License 2.0. Kobalt uses its own BOOX adapter and does not bundle the Onyx SDK.
