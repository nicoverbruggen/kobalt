// SPDX-License-Identifier: GPL-2.0-or-later
#include "embedded_font_cache.h"
#include "lvfntman.h"
#include FT_TRUETYPE_TABLES_H
#include <cstring>

namespace blaze {

LVByteArrayRef EmbeddedFontCache::read(FT_Library library, const lString8 & name, const LVStreamRef & stream, lvsize_t size)
{
    const std::string key = std::string(name.c_str()) + ':' + std::to_string((unsigned long long)size);
    const auto known = _entries.find(key);
    if ( known != _entries.end() )
        return known->second;
    LVByteArrayRef data = stream->GetData();
    if ( !data.isNull() ) {
        // FreeType reconstructs a WOFF2 on each face open. Share the decoded
        // SFNT across font sizes and chapter documents, like a plain TTF.
        if (data->length() >= 48 && !std::memcmp(data->get(), "wOF2", 4)
                && std::memcmp(data->get() + 4, "ttcf", 4)) {
            FT_Face face = nullptr;
            if (FT_New_Memory_Face(library, data->get(), data->length(), 0, &face))
                return {};
            // FreeType has opened the face. Bound the SFNT copy kept by Blaze.
            FT_ULong length = 0;
            LVByteArrayRef decoded;
            if (!FT_Load_Sfnt_Table(face, 0, 0, nullptr, &length) && length
                    && length <= _maxReconstructedBytes) {
                decoded = LVByteArrayRef(new LVByteArray(length, 0));
                if (FT_Load_Sfnt_Table(face, 0, 0, decoded->ptr(), &length))
                    decoded = LVByteArrayRef();
            }
            FT_Done_Face(face);
            if (!decoded)
                return {};
            data = decoded;
        }
        size = data->length();
        if ( _bytes + size > 32u * 1024 * 1024 )
            clear();
        _entries[key] = data;
        _bytes += size;
    }
    return data;
}

void EmbeddedFontCache::clear()
{
    _entries.clear();
    _bytes = 0;
}

}

#if BUILD_LITE!=1
#include "lvtinydom.h"

// koblaze: a host that turns the fonts of the document off gives every
// element its own font. A rule that it does not reach, such as one for
// ::first-letter, can still name a font of the book. That font is read when
// a style first asks for it, so the text looks as with the fonts on.
void ldomDocument::koblazeReadFontsFor( const lString8 & families )
{
    if ( _koblazeUnreadFonts.empty() || families.empty() )
        return;
    lString8 wanted = families;
    wanted.lowercase();
    int i = 0;
    while ( i < _koblazeUnreadFonts.length() ) {
        LVEmbeddedFontDef * def = _koblazeUnreadFonts.get(i);
        lString8 face = def->getFace();
        face.lowercase();
        // font_name holds each name in quotes.
        if ( wanted.pos(cs8("\"") + face + cs8("\"")) < 0 ) {
            i++;
            continue;
        }
        const int parsing = _parsingDocFragmentIdx;
        _parsingDocFragmentIdx = def->getDocFragmentIdx();
        _koblazeReadingFont = true;
        registerFontFace(def->getUrl(), def->getFace(), def->getWeight(), def->getWidth(),
                         def->getItalic(), def->getIsLocal());
        _koblazeReadingFont = false;
        _parsingDocFragmentIdx = parsing;
        _koblazeUnreadFonts.erase(i, 1);
    }
}
#endif
