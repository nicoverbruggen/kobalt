#include "font_weight.h"
#include <lvfntman.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

namespace {
int extraWeight = 0;

FT_Error embolden(FT_GlyphSlot slot)
{
    if (!slot || !extraWeight || slot->format != FT_GLYPH_FORMAT_OUTLINE)
        return 0;
    // Each step adds 1/256 em. Centre the added outline thickness without
    // changing the selected face, glyph metrics or shaping advances.
    const FT_Pos strength = FT_MulFix(slot->face->units_per_EM, slot->face->size->metrics.y_scale)
            * extraWeight / 256;
    const auto error = FT_Outline_Embolden(&slot->outline, strength);
    if (!error)
        FT_Outline_Translate(&slot->outline, -strength / 2, -strength / 2);
    return error;
}
} // namespace

void kobalt::setExtraFontWeight(int steps)
{
    if (steps == extraWeight)
        return;
    extraWeight = steps;
    if (fontMan)
        fontMan->clearGlyphCache();
}

// Only the private CREngine archive references these names. Nickel and Qt
// continue to call FreeType directly.
extern "C" FT_Error kobalt_font_render_glyph(FT_GlyphSlot slot, FT_Render_Mode mode)
{
    const auto error = embolden(slot);
    return error ? error : FT_Render_Glyph(slot, mode);
}

extern "C" FT_Error kobalt_font_load_glyph(FT_Face face, FT_UInt index, FT_Int32 flags)
{
    if (!extraWeight || !(flags & FT_LOAD_RENDER))
        return FT_Load_Glyph(face, index, flags);
    const auto error = FT_Load_Glyph(face, index, flags & ~FT_LOAD_RENDER);
    if (error || face->glyph->format == FT_GLYPH_FORMAT_BITMAP)
        return error;
    auto mode = FT_LOAD_TARGET_MODE(flags);
    if (mode == FT_RENDER_MODE_NORMAL && (flags & FT_LOAD_MONOCHROME))
        mode = FT_RENDER_MODE_MONO;
    return kobalt_font_render_glyph(face->glyph, mode);
}
