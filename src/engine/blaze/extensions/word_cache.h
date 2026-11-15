// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLAZE_WORD_CACHE_H
#define BLAZE_WORD_CACHE_H

#include "lvtypes.h"
#include <hb.h>
#include <climits>
#include <unordered_map>
#include <vector>

namespace blaze {

namespace word {
// Layout of a shaped word. After KW_TEXT each character takes four ints:
// code point, 26.6 advance, pixel advance and flags.
enum {
    KW_HASH, KW_LEN, KW_USABLE,
    KW_FIRST_GLYPH, KW_FIRST_END,   // the first glyph and the end of its cluster
    KW_LAST_GLYPH, KW_LAST_CLUSTER, // the last glyph and the character holding its advance
    KW_LAST_ADVANCE,
    KW_KERN_AFTER, KW_KERN_BEFORE,  // kerning with a following or preceding space
    KW_TEXT
};
enum { KW_UNKNOWN = INT_MIN, KW_UNUSABLE = INT_MIN + 1 };

} // namespace word

class WordCache {
public:
    struct Glyphs {
        std::vector<int> space_kern[2];
        std::vector<int> char_glyph;
        std::vector<int> char_advance;
        std::unordered_map<unsigned int, int> pair_kern;
    };

    void selectLanguage(hb_language_t language);
    void clear();
    hb_language_t language() const { return active.language; }
    Glyphs & glyphs() { return active.glyphs; }

    // Offsets survive insertions in this language. Pointers into records do not.
    struct Lookup { unsigned int offset; bool inserted; };
    Lookup findOrAdd(const lChar32 * text, int len, unsigned int hash);
    int * record(unsigned int offset) { return active.words.data() + offset; }

private:
    struct Language {
        hb_language_t language = HB_LANGUAGE_INVALID;
        std::vector<int> words;
        std::vector<unsigned int> table; // record offset plus one, zero for an empty slot
        unsigned int count = 0;
        Glyphs glyphs;
        void clearRecords();
    };
    Language active;
    std::vector<Language> others;
};

} // namespace blaze
#endif
