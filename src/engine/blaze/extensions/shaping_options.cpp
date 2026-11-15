// SPDX-License-Identifier: GPL-2.0-or-later
#include "crsetup.h"
#include "koblaze.h"
#include <stdlib.h>

#if USE_FREETYPE == 1
static int koblaze_pair_kerning = -1;

void koblazeSetPairKerning(bool enabled)
{
    koblaze_pair_kerning = enabled ? 1 : 0;
}

bool koblazePairKerning()
{
    // Until a host chooses, KOBLAZE_PAIR_KERNING=1 in the environment turns it on.
    if ( koblaze_pair_kerning < 0 ) {
        const char * value = getenv("KOBLAZE_PAIR_KERNING");
        koblaze_pair_kerning = value && value[0] == '1' ? 1 : 0;
    }
    return koblaze_pair_kerning == 1;
}

static int koblaze_word_widths = -1;

void koblazeSetWordWidths(bool enabled)
{
    koblaze_word_widths = enabled ? 1 : 0;
}

bool koblazeWordWidths()
{
    // Until a host chooses, KOBLAZE_WORD_WIDTHS=1 in the environment turns it on.
    if ( koblaze_word_widths < 0 ) {
        const char * value = getenv("KOBLAZE_WORD_WIDTHS");
        koblaze_word_widths = value && value[0] == '1' ? 1 : 0;
    }
    return koblaze_word_widths == 1;
}

static int koblaze_word_pairs = -1;

void koblazeSetWordPairs(bool enabled)
{
    koblaze_word_pairs = enabled ? 1 : 0;
}

bool koblazeWordPairs()
{
    // Until a host chooses, KOBLAZE_WORD_PAIRS=1 in the environment turns it on.
    if ( koblaze_word_pairs < 0 ) {
        const char * value = getenv("KOBLAZE_WORD_PAIRS");
        koblaze_word_pairs = value && value[0] == '1' ? 1 : 0;
    }
    return koblaze_word_pairs == 1;
}
#endif

static int koblaze_plain_joiner = -1;

void koblazeSetPlainJoiner(bool enabled)
{
    koblaze_plain_joiner = enabled ? 1 : 0;
}

bool koblazePlainJoiner()
{
    // Until a host chooses, KOBLAZE_PLAIN_JOINER=1 in the environment turns it on.
    if ( koblaze_plain_joiner < 0 ) {
        const char * value = getenv("KOBLAZE_PLAIN_JOINER");
        koblaze_plain_joiner = value && value[0] == '1' ? 1 : 0;
    }
    return koblaze_plain_joiner == 1;
}
