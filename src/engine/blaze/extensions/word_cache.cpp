// SPDX-License-Identifier: GPL-2.0-or-later
#include "crsetup.h"
#if USE_FREETYPE == 1 && USE_HARFBUZZ == 1
#include "word_cache.h"
#include <utility>

namespace blaze {

void WordCache::Language::clearRecords()
{
    words.clear();
    table.clear();
    count = 0;
    glyphs.space_kern[0].clear();
    glyphs.space_kern[1].clear();
    glyphs.char_glyph.clear();
    glyphs.char_advance.clear();
    glyphs.pair_kern.clear();
}

void WordCache::clear()
{
    others.clear();
    active.language = HB_LANGUAGE_INVALID;
    active.clearRecords();
}

void WordCache::selectLanguage(hb_language_t language)
{
    constexpr size_t max_inactive_languages = 8;
    constexpr unsigned int max_words = 50000;
    if ( active.language != language ) {
        size_t slot = 0;
        while ( slot < others.size() && others[slot].language != language )
            slot++;
        if ( slot == others.size() ) {
            if ( others.size() >= max_inactive_languages )
                others.erase(others.begin());
            others.emplace_back();
            slot = others.size() - 1;
            others[slot].language = language;
        }
        std::swap(active, others[slot]);
    }
    if ( active.count > max_words )
        active.clearRecords();
}

WordCache::Lookup WordCache::findOrAdd(const lChar32 * text, int len, unsigned int hash)
{
    using namespace word;
    if ( active.table.empty() )
        active.table.assign(4096, 0);
    unsigned int mask = (unsigned int)active.table.size() - 1;
    unsigned int slot = hash & mask;
    for ( ; active.table[slot]; slot = (slot + 1) & mask ) {
        const int * word = &active.words[active.table[slot] - 1];
        if ( word[KW_HASH] != (int)hash || word[KW_LEN] != len )
            continue;
        int k = 0;
        while ( k < len && word[KW_TEXT + 4 * k] == (int)text[k] )
            k++;
        if ( k == len )
            return { active.table[slot] - 1, false };
    }
    // A new word.
    const unsigned int offset = (unsigned int)active.words.size();
    active.words.resize(offset + KW_TEXT + 4 * len, 0);
    active.table[slot] = offset + 1;
    active.count++;
    if ( active.count * 2 > active.table.size() ) {
        std::vector<unsigned int> table(active.table.size() * 2, 0);
        mask = (unsigned int)table.size() - 1;
        for ( size_t old = 0; old < active.table.size(); old++ ) {
            if ( !active.table[old] )
                continue;
            unsigned int to = (unsigned int)active.words[active.table[old] - 1 + KW_HASH] & mask;
            while ( table[to] )
                to = (to + 1) & mask;
            table[to] = active.table[old];
        }
        active.table.swap(table);
    }
    int * word = &active.words[offset];
    word[KW_HASH] = (int)hash;
    word[KW_LEN] = len;
    word[KW_KERN_AFTER] = KW_UNKNOWN;
    word[KW_KERN_BEFORE] = KW_UNKNOWN;
    for ( int k = 0; k < len; k++ )
        word[KW_TEXT + 4 * k] = (int)text[k];
    return { offset, true };
}

} // namespace blaze
#endif
