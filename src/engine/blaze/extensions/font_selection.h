// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLAZE_FONT_SELECTION_H
#define BLAZE_FONT_SELECTION_H

#include "lvfntman.h"
#include "lvstyles.h"

struct ldomNode;

namespace blaze {

LVFontVariations fontVariations(const css_style_rec_t *style, int size);
LVFontRef bionicFont(ldomNode *node, LVFontRef font, int length, int offset, bool legacy);
bool bionicContinuation(ldomNode *node);

}

#endif
