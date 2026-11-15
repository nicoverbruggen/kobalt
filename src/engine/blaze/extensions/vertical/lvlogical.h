#ifndef LVLOGICAL_H_INCLUDED
#define LVLOGICAL_H_INCLUDED

/**
 * CSS Logical Property index helpers for writing-mode-aware rendering.
 *
 * crengine stores CSS box properties using physical indices:
 *   padding/margin array: [0]=left  [1]=right  [2]=top    [3]=bottom
 *   measureBorder() side:  0=top    1=right     2=bottom   3=left    (TRBL)
 *
 * vertical-rl logical-to-physical mapping (CSS Writing Modes Level 3):
 *   Inline  direction = top → bottom  (physical Y)
 *   Block   direction = right → left  (physical X, for vertical-rl)
 *
 *   Logical          vertical-rl physical   horizontal-tb physical
 *   inline-start  =  top   (Y start)     =  left   (X start)
 *   inline-end    =  bottom(Y end)        =  right  (X end)
 *   block-start   =  right (X start)     =  top    (Y start)
 *   block-end     =  left  (X end)       =  bottom (Y end)
 *
 * Usage:
 *   CSSLogical L(style->writing_mode);
 *   int border_is = measureBorder(node, L.brdIS());   // inline-start border
 *   int pad_left  = lengthToPx(node, style->padding[L.padIS()], w) + border_is;
 *
 * Soft-fork note: this header is additive — no existing file is modified.
 * Changes in rendering code replace hard-coded indices (0,1,2,3) with
 * L.padIS() etc., minimising future upstream merge conflicts.
 */

#include "cssdef.h"

struct CSSLogical {
    css_writing_mode_t wm;

    explicit CSSLogical(css_writing_mode_t w) : wm(w) {}

    bool isVertical() const {
        return wm == css_wm_vertical_rl || wm == css_wm_vertical_lr;
    }

    // ── padding / margin array indices ──────────────────────────────────
    // Array layout: [0]=left  [1]=right  [2]=top  [3]=bottom

    /** inline-start: top (vert) or left (horiz) */
    int padIS() const { return isVertical() ? 2 : 0; }
    /** inline-end:   bottom (vert) or right (horiz) */
    int padIE() const { return isVertical() ? 3 : 1; }
    /** block-start:  right (vert-rl) or top (horiz) */
    int padBS() const { return isVertical() ? 1 : 2; }
    /** block-end:    left  (vert-rl) or bottom (horiz) */
    int padBE() const { return isVertical() ? 0 : 3; }

    // margin indices follow the same array order as padding
    int marIS() const { return padIS(); }
    int marIE() const { return padIE(); }
    int marBS() const { return padBS(); }
    int marBE() const { return padBE(); }

    // ── measureBorder() side indices ────────────────────────────────────
    // TRBL layout: 0=top  1=right  2=bottom  3=left

    /** inline-start border: top (vert) or left (horiz) */
    int brdIS() const { return isVertical() ? 0 : 3; }
    /** inline-end border:   bottom (vert) or right (horiz) */
    int brdIE() const { return isVertical() ? 2 : 1; }
    /** block-start border:  right (vert-rl) or top (horiz) */
    int brdBS() const { return isVertical() ? 1 : 0; }
    /** block-end border:    left  (vert-rl) or bottom (horiz) */
    int brdBE() const { return isVertical() ? 3 : 2; }
};

#endif // LVLOGICAL_H_INCLUDED
