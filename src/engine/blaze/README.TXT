Blaze
=====

Blaze is an experimental fork of CREngine for use with Kobalt. It is the
rendering engine of the Kobalt reader. It is not a general-purpose library
and it is not meant to be used outside this repository.

CREngine is the e-book rendering library of CoolReader 3. Blaze starts from
the CREngine that KOReader maintains (https://github.com/koreader/crengine,
commit 4ce1aed17e9d7852e047e0b9b0bf6a637486fd97) and adds the changes that
Kobalt needs. Most of them serve one goal: open books and turn pages fast on
slow e-ink devices, by laying out one chapter at a time. See doc/engines.md
and doc/engines/koblaze.md in this repository.

Folders
-------

crengine   The library: the CREngine sources and headers, with the Blaze
           changes. Changes that belong to Blaze are marked with KOBLAZE
           in the code; crengine/include/koblaze.h lists them.
adapter    The Kobalt side: the code that drives the library for the
           reader (opening a book, pages, links, search, positions).
data       The default style sheets of the engine, and the list of names
           of the hyphenation patterns that the engine knows. Kobalt ships
           its own pattern files from assets/hyph.

CMakeLists.txt builds the library. Kobalt calls it from
dependencies/local-engine.cmake. Do not build it on its own.

License
-------

CoolReader 3 - free e-book reader
(c) Vadim Lopatin, 1998-2009

LICENSE: All source codes (except thirdparty directory)
are provided under the terms of GNU GPL license, version 2

The full license text is in the file COPYING in this folder. The image
scaler in crengine/qimagescale has its own license in
crengine/qimagescale/QIMAGETRANSFORM_LICENSE.txt. Keep these notices and the
copyright headers of the source files.
