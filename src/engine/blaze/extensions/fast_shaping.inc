// SPDX-License-Identifier: GPL-2.0-or-later
// Included after the private LVFreeTypeFace definition in lvfntman.cpp.

inline void LVFreeTypeFace::koblazeClearKerning() {
    _koblaze_kern.clear();
    _koblaze_kern_systems.clear();
    _koblaze_features_nokern.clear();
    _koblaze_kern_failed = false;
    _koblaze_kern_runs = 0;
}

// Shapes the codepoints of _koblaze_text from `from` for `count` into
// `buffer`, with the given features.
inline void LVFreeTypeFace::koblazeShape( hb_buffer_t * buffer, unsigned int from, unsigned int count, hb_script_t script,
                                         hb_language_t language, int flags, const LVArray<hb_feature_t> & features )
{
    hb_buffer_clear_contents(buffer);
    for ( unsigned int k = 0; k < count; k++ )
        hb_buffer_add(buffer, _koblaze_text[from + k], k);
    hb_buffer_set_content_type(buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer, script);
    hb_buffer_set_language(buffer, language);
    hb_buffer_set_flags(buffer, (hb_buffer_flags_t)flags); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    hb_shape(_hb_font, buffer, features.ptr(), (unsigned int)features.length());
}

// koblaze: what the kern feature adds to the advance of `left` before
// `right`, asked of HarfBuzz once for each pair of glyphs. The two
// characters are shaped alone, with and without kerning. The pair is
// not usable when that gives other glyphs than the run has, or when
// kerning moves anything but the advance of the first glyph: the run
// is then shaped as a whole.
inline LVFreeTypeFace::KoblazeKern LVFreeTypeFace::koblazeKernPair( unsigned int left_first, unsigned int left_count, unsigned int right_first,
                                                                   unsigned int right_count, hb_codepoint_t left, hb_codepoint_t right,
                                                                   hb_script_t script, hb_language_t language )
{
    KoblazeKern result = { 0, false };
    hb_position_t without[2] = { 0, 0 };
    for ( int pass = 0; pass < 2; pass++ ) {
        // The characters of the two glyphs alone, without what is between them.
        hb_buffer_clear_contents(_koblaze_buffer);
        for ( unsigned int k = 0; k < left_count; k++ )
            hb_buffer_add(_koblaze_buffer, _koblaze_text[left_first + k], 0);
        for ( unsigned int k = 0; k < right_count; k++ )
            hb_buffer_add(_koblaze_buffer, _koblaze_text[right_first + k], 1);
        hb_buffer_set_content_type(_koblaze_buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_set_direction(_koblaze_buffer, HB_DIRECTION_LTR);
        hb_buffer_set_script(_koblaze_buffer, script);
        hb_buffer_set_language(_koblaze_buffer, language);
        const LVArray<hb_feature_t> & features = pass == 0 ? _koblaze_features_nokern : _hb_features;
        hb_shape(_hb_font, _koblaze_buffer, features.ptr(), (unsigned int)features.length());
        unsigned int glyphs = 0;
        hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_koblaze_buffer, &glyphs);
        hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_koblaze_buffer, NULL);
        if ( glyphs != 2 || info[0].codepoint != left || info[1].codepoint != right
                || pos[0].y_advance || pos[1].y_advance || pos[0].x_offset || pos[0].y_offset
                || pos[1].x_offset || pos[1].y_offset )
            return result;
        if ( pass == 0 ) {
            without[0] = pos[0].x_advance;
            without[1] = pos[1].x_advance;
        }
        else {
            if ( pos[1].x_advance != without[1] )
                return result;
            result.advance = pos[0].x_advance - without[0];
        }
    }
    result.usable = true;
    return result;
}

// The characters that HarfBuzz passes over when it matches a pair.
static bool koblazeIgnorable( hb_codepoint_t c )
{
    return c == 0x00AD || c == 0x034F || c == 0x061C || (c >= 0x200B && c <= 0x200F)
            || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF
            || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x180B && c <= 0x180E);
}

// koblaze: _hb_buffer holds a left-to-right horizontal run that was
// shaped without the kern feature, and _koblaze_text holds its
// characters. Add the kerning of each pair of neighbors from memory.
// A book has a few thousand different pairs, so nearly every pair is
// found there, and the kerning lookups of the font are not walked for
// every glyph. Returns false when the run is not plain enough for this:
// a cluster of several glyphs, a glyph that is moved, or a pair that is
// not usable. The caller then shapes the run as a whole.
inline bool LVFreeTypeFace::koblazeKernRun( hb_script_t script, hb_language_t language )
{
    unsigned int glyphs = 0;
    hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_hb_buffer, &glyphs);
    hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_hb_buffer, NULL);
    const unsigned int len = (unsigned int)_koblaze_text.size();
    for ( unsigned int g = 0; g < glyphs; g++ ) {
        // One glyph for each cluster, in the order of the text.
        if ( info[g].cluster >= len || (g > 0 && info[g].cluster <= info[g - 1].cluster) || (g == 0 && info[g].cluster != 0) )
            return false;
        if ( info[g].codepoint == 0 || pos[g].y_advance || pos[g].x_offset || pos[g].y_offset )
            return false;
    }
    // Each script and language of the face has its own pairs.
    unsigned int system = 0;
    for ( ; system < _koblaze_kern_systems.size(); system++ ) {
        if ( _koblaze_kern_systems[system].first == script && _koblaze_kern_systems[system].second == language )
            break;
    }
    if ( system == _koblaze_kern_systems.size() ) {
        if ( system >= 255 )
            return false;
        _koblaze_kern_systems.push_back(std::make_pair(script, language));
    }
    int left = -1;
    for ( unsigned int g = 0; g < glyphs; g++ ) {
        const unsigned int first = info[g].cluster, end = g + 1 < glyphs ? info[g + 1].cluster : len;
        // A cluster of only passed-over characters does not take part.
        bool ignorable = true;
        for ( unsigned int c = first; c < end && ignorable; c++ )
            ignorable = koblazeIgnorable(_koblaze_text[c]);
        if ( ignorable )
            continue;
        if ( left >= 0 ) {
            const unsigned long long key = ((unsigned long long)(info[left].codepoint & 0xFFFFFFF) << 36)
                    | ((unsigned long long)(info[g].codepoint & 0xFFFFFFF) << 8) | system;
            std::unordered_map<unsigned long long, KoblazeKern>::const_iterator found = _koblaze_kern.find(key);
            KoblazeKern kern;
            if ( found != _koblaze_kern.end() ) {
                kern = found->second;
            }
            else {
                const unsigned int left_first = info[left].cluster;
                const unsigned int left_end = (unsigned int)left + 1 < glyphs ? info[left + 1].cluster : len;
                kern = koblazeKernPair(left_first, left_end - left_first, first, end - first,
                                       info[left].codepoint, info[g].codepoint, script, language);
                if ( _koblaze_kern.size() > 60000 )
                    _koblaze_kern.clear();
                _koblaze_kern[key] = kern;
            }
            if ( !kern.usable )
                return false;
            pos[left].x_advance += kern.advance;
        }
        left = (int)g;
    }
    return true;
}

// koblaze: shapes the run in _hb_buffer. With pair kerning from memory
// it shapes without the kern feature and adds the kerning; it checks the
// result against HarfBuzz for the first runs of a face and for one run
// in fifty after that, and stops for the face when they differ.
inline void LVFreeTypeFace::koblazeShapeRun( bool usable )
{
    if ( !usable || _koblaze_kern_failed || !koblazePairKerning() ) {
        hb_shape(_hb_font, _hb_buffer, _hb_features.ptr(), (unsigned int)_hb_features.length());
        return;
    }
    unsigned int len = 0;
    hb_glyph_info_t * added = hb_buffer_get_glyph_infos(_hb_buffer, &len);
    const hb_script_t script = hb_buffer_get_script(_hb_buffer);
    const hb_language_t language = hb_buffer_get_language(_hb_buffer);
    const int flags = (int)hb_buffer_get_flags(_hb_buffer);
    _koblaze_text.resize(len);
    for ( unsigned int k = 0; k < len; k++ )
        _koblaze_text[k] = added[k].codepoint;
    if ( !_koblaze_buffer )
        _koblaze_buffer = hb_buffer_create();
    if ( _koblaze_features_nokern.length() == 0 ) {
        for ( int f = 0; f < _hb_features.length(); f++ )
            _koblaze_features_nokern.add(_hb_features[f]);
        hb_feature_t off;
        if ( hb_feature_from_string("-kern", -1, &off) )
            _koblaze_features_nokern.add(off);
    }
    hb_shape(_hb_font, _hb_buffer, _koblaze_features_nokern.ptr(), (unsigned int)_koblaze_features_nokern.length());
    const bool kerned = len >= 2 ? koblazeKernRun(script, language) : true;
    const bool check = kerned && ( _koblaze_kern_runs < 200 || _koblaze_kern_runs % 50 == 0 );
    _koblaze_kern_runs++;
    if ( kerned && !check )
        return;
    if ( kerned ) {
        // The same run as a whole, for comparison.
        koblazeShape(_koblaze_buffer, 0, len, script, language, flags, _hb_features);
        unsigned int glyphs = 0, expected = 0;
        hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_hb_buffer, &glyphs);
        hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_hb_buffer, NULL);
        hb_glyph_info_t * whole_info = hb_buffer_get_glyph_infos(_koblaze_buffer, &expected);
        hb_glyph_position_t * whole_pos = hb_buffer_get_glyph_positions(_koblaze_buffer, NULL);
        bool same = glyphs == expected;
        for ( unsigned int g = 0; same && g < glyphs; g++ )
            same = info[g].codepoint == whole_info[g].codepoint && info[g].cluster == whole_info[g].cluster
                    && pos[g].x_advance == whole_pos[g].x_advance && pos[g].x_offset == whole_pos[g].x_offset
                    && pos[g].y_offset == whole_pos[g].y_offset && pos[g].y_advance == whole_pos[g].y_advance;
        if ( same )
            return;
        _koblaze_kern_failed = true;
        CRLog::warn("koblaze: kerning from memory differs from HarfBuzz for %s, run %u: off for this face",
                    _faceName.c_str(), _koblaze_kern_runs);
    }
    // Shape the run as a whole.
    koblazeShape(_hb_buffer, 0, len, script, language, flags, _hb_features);
}

inline void LVFreeTypeFace::koblazeClearWords() {
    _word_cache.clear();
    _koblaze_words_failed = false;
    _koblaze_word_runs = 0;
    _koblaze_build_failed = false;
    _koblaze_built_words = 0;
}

// What a character can be in a word that is measured from memory:
// 0 nothing, 1 inside a word only, 2 also first or last, 3 a Latin
// letter. Marks and characters that HarfBuzz passes over can join the
// glyph of a neighbor, so they are not allowed at the edge of a word.
static int koblazeWordChar( lChar32 c )
{
    if ( c < 0x250 ) {
        if ( c < 0x21 || (c >= 0x7F && c < 0xA0) )
            return 0;
        if ( c == 0xAD )
            return 1;
        if ( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') )
            return 3;
        if ( c < 0xC0 )
            return c == 0xAA || c == 0xBA ? 3 : 2;
        return c == 0xD7 || c == 0xF7 ? 2 : 3;
    }
    if ( c >= 0x2010 && c <= 0x2027 )
        return 2;
    if ( c >= 0x2030 && c <= 0x203A )
        return 2;
    if ( c >= 0x1E00 && c <= 0x1EFF )
        return 3;
    if ( c < 0x370 ) // phonetic letters, modifier letters and marks
        return 1;
    if ( c >= 0x2000 && c <= 0x206F ) // spaces and joiners, but no direction marks
        return c == 0x200E || c == 0x200F || (c >= 0x2028 && c <= 0x202E) || c >= 0x2066 ? 0 : 1;
    if ( (c >= 0x20A0 && c <= 0x20CF) || (c >= 0x2100 && c <= 0x214F) || (c >= 0xFB00 && c <= 0xFB06) || c == 0xFEFF )
        return 1;
    return 0;
}

// Shapes `count` characters as Latin text into _koblaze_buffer.
inline void LVFreeTypeFace::koblazeShapeWord( const lChar32 * text, int count, const lChar32 * more, int more_count )
{
    hb_buffer_clear_contents(_koblaze_buffer);
    for ( int k = 0; k < count; k++ )
        hb_buffer_add(_koblaze_buffer, (hb_codepoint_t)text[k], k);
    for ( int k = 0; k < more_count; k++ )
        hb_buffer_add(_koblaze_buffer, (hb_codepoint_t)more[k], count + k);
    hb_buffer_set_content_type(_koblaze_buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_direction(_koblaze_buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(_koblaze_buffer, HB_SCRIPT_LATIN);
    hb_buffer_set_language(_koblaze_buffer, _word_cache.language());
    hb_shape(_hb_font, _koblaze_buffer, _hb_features.ptr(), (unsigned int)_hb_features.length());
}

// koblaze: the record of a word, shaped at its first use. Returns its
// offset in the word cache, which stays valid until the memory is
// cleared; pointers into records do not.
inline unsigned int LVFreeTypeFace::koblazeWord( const lChar32 * text, int len, unsigned int hash )
{
    using namespace blaze::word;
    const auto entry = _word_cache.findOrAdd(text, len, hash);
    const unsigned int offset = entry.offset;
    if ( !entry.inserted )
        return offset;
    int * word = _word_cache.record(offset);
    if ( _koblaze_build_failed || !koblazeWordPairs() || !koblazeBuildWord(text, len, word) ) {
        koblazeFillWord(text, len, word);
        return offset;
    }
    // Compare the first words that are built, and one in sixteen after that.
    _koblaze_built_words++;
    if ( _koblaze_built_words > 500 && _koblaze_built_words % 16 != 0 )
        return offset;
    const int size = KW_TEXT + 4 * len;
    _koblaze_built_check.assign(word, word + size);
    for ( int k = KW_USABLE; k < size; k++ ) {
        if ( k < KW_TEXT ? (k != KW_KERN_AFTER && k != KW_KERN_BEFORE) : ((k - KW_TEXT) & 3) != 0 )
            word[k] = 0;
    }
    koblazeFillWord(text, len, word);
    for ( int k = 0; k < size; k++ ) {
        if ( word[k] != _koblaze_built_check[k] ) {
            _koblaze_build_failed = true;
            CRLog::warn("koblaze: a word built from pairs differs from HarfBuzz for %s, word %u: off for this face",
                        _faceName.c_str(), _koblaze_built_words);
            break;
        }
    }
    return offset;
}

// koblaze: fills the record of a word from its characters and pairs
// of characters, with no shaping of the word. HarfBuzz is asked once
// for each character alone and once for each pair. Returns false when
// the word holds a character or a pair that is not plain: more or
// fewer glyphs than characters, other glyphs in the pair than alone,
// or a glyph that moves. HarfBuzz then shapes the word.
inline bool LVFreeTypeFace::koblazeBuildWord( const lChar32 * text, int len, int * word )
{
    using namespace blaze::word;
    auto & cached = _word_cache.glyphs();
    if ( cached.char_glyph.empty() ) {
        cached.char_glyph.assign(KW_CHARS, KW_UNKNOWN);
        cached.char_advance.assign(KW_CHARS, 0);
    }
    int index = 0, glyph = 0, advance = 0;
    for ( int t = len - 1; t >= 0; t-- ) {
        // Only characters that can be at the edge of a word: no marks,
        // and nothing that HarfBuzz passes over.
        const lChar32 c = text[t];
        if ( koblazeWordChar(c) < 2 )
            return false;
        const int next_index = index, next_glyph = glyph, next_advance = advance;
        if ( c < 0x250 )
            index = (int)c;
        else if ( c >= 0x1E00 && c <= 0x1EFF )
            index = KW_EXTENDED + (int)c - 0x1E00;
        else if ( c >= 0x2010 && c <= 0x203A )
            index = KW_PUNCT + (int)c - 0x2010;
        else
            return false;
        if ( cached.char_glyph[index] == KW_UNKNOWN ) {
            koblazeShapeWord(&c, 1);
            unsigned int glyphs = 0;
            const hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_koblaze_buffer, &glyphs);
            const hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_koblaze_buffer, NULL);
            const bool plain = glyphs == 1 && info[0].codepoint != 0 && !pos[0].y_advance && !pos[0].x_offset && !pos[0].y_offset;
            cached.char_glyph[index] = plain ? (int)info[0].codepoint : KW_UNUSABLE;
            cached.char_advance[index] = plain ? pos[0].x_advance : 0;
        }
        glyph = cached.char_glyph[index];
        advance = cached.char_advance[index];
        if ( glyph == KW_UNUSABLE )
            return false;
        int raw = advance;
        if ( t + 1 < len ) {
            const unsigned int key = (unsigned int)index * KW_CHARS + (unsigned int)next_index;
            std::unordered_map<unsigned int, int>::const_iterator found = cached.pair_kern.find(key);
            int kern;
            if ( found != cached.pair_kern.end() ) {
                kern = found->second;
            }
            else {
                koblazeShapeWord(text + t, 2);
                unsigned int glyphs = 0;
                const hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_koblaze_buffer, &glyphs);
                const hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_koblaze_buffer, NULL);
                const bool plain = glyphs == 2 && (int)info[0].codepoint == glyph && (int)info[1].codepoint == next_glyph
                        && info[0].cluster == 0 && info[1].cluster == 1 && pos[1].x_advance == next_advance
                        && !pos[0].y_advance && !pos[1].y_advance && !pos[0].x_offset && !pos[0].y_offset
                        && !pos[1].x_offset && !pos[1].y_offset;
                kern = plain ? pos[0].x_advance - advance : KW_UNUSABLE;
                cached.pair_kern[key] = kern;
            }
            if ( kern == KW_UNUSABLE )
                return false;
            raw += kern;
        }
        word[KW_TEXT + 4 * t + 1] = raw ? raw + _synth_weight_strength : 0;
        word[KW_TEXT + 4 * t + 2] = raw ? FONT_METRIC_TO_PX(raw + _synth_weight_strength) : 0;
        word[KW_TEXT + 4 * t + 3] = raw ? KW_BIT_GLYPH | KW_BIT_ADVANCE : KW_BIT_GLYPH;
        if ( t == len - 1 ) {
            word[KW_LAST_GLYPH] = glyph;
            word[KW_LAST_CLUSTER] = t;
            word[KW_LAST_ADVANCE] = advance;
        }
    }
    word[KW_FIRST_GLYPH] = glyph;
    word[KW_FIRST_END] = 1;
    word[KW_USABLE] = 1;
    return true;
}

// koblaze: fills the record of a word from the glyphs that HarfBuzz
// gives for it. The record stays unusable when the word is not plain
// enough to measure from memory.
inline void LVFreeTypeFace::koblazeFillWord( const lChar32 * text, int len, int * word )
{
    using namespace blaze::word;
    koblazeShapeWord(text, len);
    unsigned int glyphs = 0;
    const hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_koblaze_buffer, &glyphs);
    const hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_koblaze_buffer, NULL);
    // Every glyph is in this font, and the clusters follow the text.
    if ( glyphs == 0 || info[0].cluster != 0 )
        return;
    for ( unsigned int g = 0; g < glyphs; g++ ) {
        if ( info[g].codepoint == 0 || (int)info[g].cluster >= len || (g > 0 && info[g].cluster < info[g - 1].cluster) )
            return;
    }
    // The same sums as measureText() makes for each character.
    unsigned int g = 0;
    for ( int t = 0; t < len; t++ ) {
        int advance_64 = 0, advance = 0, bits = 0;
        for ( ; g < glyphs && (int)info[g].cluster <= t; g++ ) {
            bits |= KW_BIT_GLYPH;
            if ( pos[g].x_advance ) {
                advance_64 += pos[g].x_advance + _synth_weight_strength;
                advance += FONT_METRIC_TO_PX(pos[g].x_advance + _synth_weight_strength);
                bits |= KW_BIT_ADVANCE;
            }
        }
        word[KW_TEXT + 4 * t + 1] = advance_64;
        word[KW_TEXT + 4 * t + 2] = advance;
        word[KW_TEXT + 4 * t + 3] = bits;
    }
    word[KW_FIRST_GLYPH] = (int)info[0].codepoint;
    word[KW_FIRST_END] = len;
    for ( unsigned int n = 1; n < glyphs; n++ ) {
        if ( info[n].cluster != 0 ) {
            word[KW_FIRST_END] = (int)info[n].cluster;
            break;
        }
    }
    word[KW_LAST_GLYPH] = (int)info[glyphs - 1].codepoint;
    word[KW_LAST_CLUSTER] = (int)info[glyphs - 1].cluster;
    word[KW_LAST_ADVANCE] = pos[glyphs - 1].x_advance;
    word[KW_USABLE] = 1;
}

// koblaze: what HarfBuzz adds to the advance of the glyph `left` when
// the glyph `right` follows it. One of the two is the space, and the
// answer is kept for the other glyph, so HarfBuzz is asked once for
// each glyph and side. Each side is the text of one cluster.
// KW_UNUSABLE when the pair gives other glyphs than each side alone,
// or when anything else moves.
inline int LVFreeTypeFace::koblazeWordKern( const lChar32 * left_text, int left_len, int left, const lChar32 * right_text,
                                           int right_len, int right, bool space_is_left )
{
    using namespace blaze::word;
    std::vector<int> & known = _word_cache.glyphs().space_kern[space_is_left ? 1 : 0];
    const size_t glyph = (size_t)(space_is_left ? right : left);
    if ( glyph >= known.size() )
        known.resize(glyph + 256, KW_UNKNOWN);
    if ( known[glyph] == KW_UNKNOWN )
        known[glyph] = koblazeShapePair(left_text, left_len, left, right_text, right_len, right);
    return known[glyph];
}

inline int LVFreeTypeFace::koblazeShapePair( const lChar32 * left_text, int left_len, int left, const lChar32 * right_text,
                                            int right_len, int right )
{
    using namespace blaze::word;
    hb_position_t alone[2];
    for ( int side = 0; side < 2; side++ ) {
        koblazeShapeWord(side ? right_text : left_text, side ? right_len : left_len);
        unsigned int glyphs = 0;
        const hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_koblaze_buffer, &glyphs);
        const hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_koblaze_buffer, NULL);
        if ( glyphs != 1 || (int)info[0].codepoint != (side ? right : left) )
            return KW_UNUSABLE;
        alone[side] = pos[0].x_advance;
    }
    koblazeShapeWord(left_text, left_len, right_text, right_len);
    unsigned int glyphs = 0;
    const hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_koblaze_buffer, &glyphs);
    const hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_koblaze_buffer, NULL);
    if ( glyphs != 2 || (int)info[0].codepoint != left || (int)info[1].codepoint != right
            || info[0].cluster != 0 || (int)info[1].cluster != left_len || pos[1].x_advance != alone[1]
            || pos[0].y_advance || pos[1].y_advance || pos[0].x_offset || pos[0].y_offset
            || pos[1].x_offset || pos[1].y_offset )
        return KW_UNUSABLE;
    return pos[0].x_advance - alone[0];
}

// koblaze: measures a run of plain left-to-right Latin text from the
// memory of its words, as measureText() does with HarfBuzz. The run is
// split at each space; a space is a word of its own. The glyphs of a
// word do not depend on its neighbors, except that the font can kern
// the space with the glyph next to it, which is added here. Returns
// false when the run does not qualify; measureText() then shapes it.
inline bool LVFreeTypeFace::koblazeMeasureWords( const lChar32 * text, int len, lUInt16 * widths, lUInt8 * flags, int max_width,
                                                TextLangCfg * lang_cfg, int letter_spacing, lUInt32 hints, lUInt16 & last_fit )
{
    using namespace blaze::word;
    if ( _koblaze_words_failed || !koblazeWordWidths() )
        return false;
    if ( hints & (LFNT_HINT_IS_VERTICAL | LFNT_HINT_DIRECTION_IS_RTL | LFNT_HINT_DIRECTION_IS_TTB | LFNT_HINT_IS_FALLBACK_FONT) )
        return false;
    // Without a fallback font, measureText() replaces missing characters first.
    if ( getFallbackFont(false).isNull() )
        return false;
    if ( _hb_features_is_vertical != 0 )
        setupHBFeatures(false);
    const hb_language_t language = lang_cfg ? lang_cfg->getHBLanguage() : hb_language_get_default();
    _word_cache.selectLanguage(language);
    if ( !_koblaze_buffer )
        _koblaze_buffer = hb_buffer_create();
    static const lChar32 space = ' ';
    const bool fractional = getFractionalGlyphPositioningGranularity() > 1;
    bool has_latin = false;
    int cur_width = 0, cur_width_64 = 0;
    last_fit = 0;
    // Finds the word that starts at `from`: its end and its record.
    // Returns false when the run does not qualify.
    auto find = [&]( int from, int & end, unsigned int & offset ) -> bool {
        unsigned int hash = 2166136261u;
        if ( text[from] == space ) {
            end = from + 1;
            hash = (hash ^ (unsigned int)space) * 16777619u;
        }
        else {
            int kind = koblazeWordChar(text[from]);
            if ( kind < 2 )
                return false;
            for ( end = from; end < len && text[end] != space; end++ ) {
                kind = koblazeWordChar(text[end]);
                if ( kind == 0 )
                    return false;
                has_latin = has_latin || kind == 3;
                hash = (hash ^ (unsigned int)text[end]) * 16777619u;
            }
            if ( kind < 2 || end - from > KW_MAX_LEN )
                return false;
        }
        offset = koblazeWord(text + from, end - from, hash);
        return _word_cache.record(offset)[KW_USABLE] != 0;
    };
    int start = 0, end = 0, next_end = 0;
    unsigned int offset = 0, next_offset = 0;
    if ( !find(0, end, offset) )
        return false;
    for ( ;; ) {
        const bool more = end < len;
        if ( more && !find(end, next_end, next_offset) )
            return false;
        // The kerning between this word and the next one.
        int kern = 0;
        if ( more ) {
            const int * word = _word_cache.record(offset);
            const int * next = _word_cache.record(next_offset);
            if ( text[end] == space ) {
                if ( word[KW_KERN_AFTER] == KW_UNKNOWN ) {
                    const int kerned = koblazeWordKern(text + start + word[KW_LAST_CLUSTER], end - start - word[KW_LAST_CLUSTER],
                                                       word[KW_LAST_GLYPH], &space, 1, next[KW_FIRST_GLYPH], false);
                    _word_cache.record(offset)[KW_KERN_AFTER] = kerned;
                }
                kern = _word_cache.record(offset)[KW_KERN_AFTER];
            }
            else {
                if ( next[KW_KERN_BEFORE] == KW_UNKNOWN ) {
                    const int kerned = koblazeWordKern(&space, 1, word[KW_LAST_GLYPH], text + end, next[KW_FIRST_END],
                                                       next[KW_FIRST_GLYPH], true);
                    _word_cache.record(next_offset)[KW_KERN_BEFORE] = kerned;
                }
                kern = _word_cache.record(next_offset)[KW_KERN_BEFORE];
            }
            if ( kern == KW_UNUSABLE )
                return false;
        }
        const int * word = _word_cache.record(offset);
        if ( kern && ( !word[KW_LAST_ADVANCE] || !(word[KW_LAST_ADVANCE] + kern) ) )
            return false;
        const int * record = word + KW_TEXT;
        for ( int t = start; t < end; t++, record += 4 ) {
            int advance_64 = record[1], advance = record[2];
            if ( kern && t - start == word[KW_LAST_CLUSTER] ) {
                advance_64 += kern;
                advance += FONT_METRIC_TO_PX(word[KW_LAST_ADVANCE] + _synth_weight_strength + kern)
                           - FONT_METRIC_TO_PX(word[KW_LAST_ADVANCE] + _synth_weight_strength);
            }
            bool has_advance;
            if ( fractional ) {
                cur_width_64 += advance_64;
                cur_width = FONT_METRIC_TO_PX(cur_width_64);
                has_advance = (record[3] & KW_BIT_ADVANCE) != 0;
            }
            else {
                cur_width += advance;
                has_advance = advance != 0;
            }
            if ( !(record[3] & KW_BIT_GLYPH) ) {
                flags[t] = LCHAR_IS_CLUSTER_TAIL;
            }
            else {
                flags[t] = GET_CHAR_FLAGS(text[t]);
                if ( !has_advance ) {
                    flags[t] |= LCHAR_IS_CLUSTER_TAIL;
                }
                else if ( fractional ) {
                    cur_width_64 += letter_spacing * 64;
                    cur_width = FONT_METRIC_TO_PX(cur_width_64);
                }
                else {
                    cur_width += letter_spacing;
                }
            }
            widths[t] = (lUInt16)cur_width;
            if ( cur_width > max_width )
                return has_latin; // measureText() stops here too
            last_fit = (lUInt16)(t + 1);
        }
        if ( !more )
            break;
        start = end;
        end = next_end;
        offset = next_offset;
    }
    // HarfBuzz shapes a run without a letter with other rules.
    return has_latin;
}
