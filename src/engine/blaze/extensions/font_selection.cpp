// Font selection from CoolReader Engine lvrend.cpp and the Blaze fork.
// (c) Vadim Lopatin, 2000-2006. Distributed under the GNU General Public License.
#include "font_selection.h"
#include "lvtinydom.h"
#include "lvrend.h"
#include "fb2def.h"
#include "lvbionic.h"

static float rend_font_optical_size = 0;
void LVRendSetFontOpticalSize(float points)
{
    rend_font_optical_size = points > 0 ? points : 0;
}
float LVRendGetFontOpticalSize()
{
    return rend_font_optical_size;
}

namespace blaze {

// opsz when gRenderDPI isn't a usable screen DPI: typical body text size
#define OPSZ_FALLBACK_PT 12.0f

LVFontVariations fontVariations(const css_style_rec_t *style, int size)
{
    LVFontVariations variations;
    // opsz in points. A user-set gRenderDPI approximates the screen DPI;
    // the default (96) or "off" (0) doesn't, so use a fixed fallback.
    if (rend_font_optical_size > 0)
        variations.set(LVFONT_TAG_OPSZ, rend_font_optical_size);
    else if (style->font_optical_sizing != css_fos_none) {
        float opsz = gRenderDPI >= 100 ? size * 72.0f / (float)gRenderDPI : OPSZ_FALLBACK_PT;
        variations.set(LVFONT_TAG_OPSZ, opsz);
    }
    // font-width: selects among static width faces, and sets the wdth axis of variable ones.
    if (style->font_width != css_fwd_normal)
        variations.set(LVFONT_TAG_WDTH, style->font_width / 10.0f);
    return variations;
}

// Rendering and intrinsic box sizing must use the same prefix fonts and runs.
LVFontRef bionicFont(ldomNode *node, LVFontRef font, int length, int offset, bool legacy) {
    if (!node->isEffectiveText()) return LVFontRef();
    int mode = node->getDocument()->getProps()->getIntDef("kobalt.bionic.mode", 0);
    auto *parent = node->getParentNode();
    auto style = parent->getStyle();
    if (!mode || legacy || offset || length >= 65536 || font->getWeight() >= 700
        || style->font_family == css_ff_monospace || style->white_space >= css_ws_pre)
        return LVFontRef();
    for (auto *ancestor = parent; ancestor; ancestor = ancestor->getParentNode()) {
        auto ancestorStyle = ancestor->getStyle();
        int writing = ancestorStyle.isNull() ? css_wm_horizontal_tb : ancestorStyle->writing_mode;
        if (ancestor->getNodeId() == el_math || ancestor->getNodeId() == el_ruby || ancestor->getNodeId() == el_code
            || writing == css_wm_vertical_rl || writing == css_wm_vertical_lr)
            return LVFontRef();
    }
    LVFontVariations variations = fontVariations(style.get(), font->getSize());
    return fontMan->GetFont(font->getSize(), 700,
        style->font_style >= css_fs_italic, font->getFontFamily(), font->getTypeFace(), font->getFeatures(),
        node->getDocument()->getFontContextDocIndex(), false,
        variations.empty() ? NULL : &variations, node->getDocFragmentIdx());
}
bool bionicContinuation(ldomNode *node) {
    for (auto *before = node; before && before->getParentNode(); before = before->getParentNode()) {
        auto *container = before->getParentNode();
        if (before->getNodeIndex() > 0) {
            auto *previous = container->getChildNode(before->getNodeIndex() - 1);
            while (previous && previous->hasChildren()) previous = previous->getLastChild();
            if (previous && previous->isEffectiveText()) {
                auto text = previous->getEffectiveText();
                return !text.empty() && lvBionicWord(text[text.length() - 1]);
            }
            return false;
        }
        if (container->getRendMethod() != erm_inline) break;
    }
    return false;
}

}
