// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLAZE_PAGE_SPACING_H
#define BLAZE_PAGE_SPACING_H

#include "lvpagesplitter.h"
#include "lvtextfm.h"

namespace blaze {

// Classify the block once. The splitter consumes the flags of each line.
class BlockSpacing {
public:
    BlockSpacing(ldomNode *enode, bool vertical);
    bool avoidsFloats() const { return text_flags && !(text_flags & RN_LINE_ANCHORED_SPACING); }
    int lineFlags(const formatted_line_t &line, int index, int count, bool beside_float) const;
private:
    int text_flags = 0;
    int group_flags = 0;
    bool allow_images = false;
};

}

#endif
