// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLAZE_VERTICAL_BOXES_H
#define BLAZE_VERTICAL_BOXES_H

#include "lvtextfm.h"
#include "lvtinydom.h"

namespace blaze {

int resolveEffectiveWritingMode(ldomNode *enode);
int resolveEffectiveDrawWritingMode(ldomNode *enode, LVDrawBuf &drawbuf);
bool shouldDebugForkVerticalBoxNode(ldomNode *enode);
bool shouldStartForkVerticalImageLineOnFreshPage(bool is_vertical, LFormattedTextRef txform,
                                                 const formatted_line_t *line);
bool isForkVerticalSingleImageLine(bool is_vertical, LFormattedTextRef txform,
                                   const formatted_line_t *line);
void DrawBorderVertical(ldomNode *enode, LVDrawBuf & drawbuf,
                        int x0, int y0, int doc_x, int doc_y, RenderRectAccessor fmt);
void DrawBackgroundColorVertical(LVDrawBuf & drawbuf, ldomNode *enode,
                                 int x0, int y0, int doc_x, int doc_y, RenderRectAccessor fmt, lUInt32 bg_color, int debug_padding);
lvRect verticalBackgroundImageRect(ldomNode *enode, LVDrawBuf &drawbuf, int x0, int y0,
                                   int doc_x, int doc_y, int block_size);
void DrawBackgroundImageVertical(ldomNode *enode, LVDrawBuf & drawbuf, int x0, int y0,
                                 int doc_x, int doc_y, int width, int height);

} // namespace blaze

#endif
