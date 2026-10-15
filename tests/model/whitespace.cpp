#include "check.h"
#include "engine/crengine/font_weight.h"

#include <lvdrawbuf.h>
#include <lvfntman.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>
#include <vector>

namespace {
void checkFixture() {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    require(!FT_Init_FreeType(&library), "Cannot initialize whitespace fixture inspection");
    require(!FT_New_Face(library, KOBALT_FONT_DIRECTORY "/InstrumentSerif-Regular.ttf", 0, &face),
            "Cannot inspect whitespace fixture");
    for (lChar32 code : {U'\u200b', U'\u2060', U'\ufeff', U'\u2003', U'\u2009', U'\u200a', U'\u202f', U'\u205f'}) {
        require(FT_Get_Char_Index(face, code) == 0, "Whitespace fixture unexpectedly gained glyph coverage");
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
}

struct Rendered {
    int advance;
    std::vector<lUInt32> pixels;
};

Rendered draw(LVFontRef font, const lString32 &text) {
    LVColorDrawBuf buffer(512, 192, 32);
    buffer.Clear(0xffffff);
    buffer.SetTextColor(0);
    Rendered result{font->DrawTextString(&buffer, 0, 0, text.c_str(), text.length(), U'?'), {}};
    for (int y = 0; y < buffer.GetHeight(); ++y) {
        for (int x = 0; x < buffer.GetWidth(); ++x) {
            result.pixels.push_back(buffer.GetPixel(x, y));
        }
    }
    return result;
}

struct Fallback {
    const char *file;
    const char *family;
};

void check(kerning_mode_t mode, Fallback fallback) {
    require(InitFontManager(lString8()), "Cannot initialize font manager");
    require(fontMan->RegisterFont(lString8(KOBALT_FONT_DIRECTORY "/InstrumentSerif-Regular.ttf")),
            "Cannot register whitespace test font");
    if (fallback.family) {
        require(fontMan->RegisterFont(lString8(KOBALT_FONT_DIRECTORY "/") + fallback.file),
                "Cannot register whitespace fallback font");
        require(fontMan->SetFallbackFontFaces(lString8(fallback.family)), "Cannot select whitespace fallback font");
    }
    fontMan->SetKerningMode(mode);
    fontMan->SetHintingMode(HINTING_MODE_DISABLED);
    auto font = fontMan->GetFont(128, 400, false, css_ff_serif, lString8("Instrument Serif"));
    require(!font.isNull() && font->getTypeFace() == lString8("Instrument Serif"), "Wrong whitespace test font");
    const auto reference = draw(font, lString32(U"HH"));
    for (lChar32 code : {U'\u200b', U'\u2060', U'\ufeff'}) {
        lString32 text(U"H");
        text.append(1, code);
        text.append(U"H");
        const auto actual = draw(font, text);

        std::printf("mode=%d fallback=%s U+%04X advance=%d reference=%d\n", mode,
                    fallback.family ? fallback.family : "none", unsigned(code), actual.advance, reference.advance);
        require(actual.advance == reference.advance && actual.pixels == reference.pixels,
                "Missing zero-width space or word joiner produced visible ink or width");
        require(font->getTextWidth(&code, 1) == 0, "Missing zero-width character measured with nonzero width");
    }
    const auto blank = draw(font, lString32(U""));
    const auto unknown = draw(font, lString32(U"\u0378"));
    require(unknown.advance > 0 && unknown.pixels != blank.pixels,
            "An ordinary missing character lost its visible replacement");
    for (lChar32 code : {U'\u2003', U'\u2009', U'\u200a', U'\u202f', U'\u205f'}) {
        lString32 text;
        text.append(1, code);
        const auto actual = draw(font, text);
        require(actual.advance > 0 && actual.pixels == blank.pixels,
                "Missing whitespace produced visible ink or lost its spacing");
        require(font->getTextWidth(&code, 1) == unsigned(actual.advance),
                "Missing whitespace measurement differs from drawing");
    }
    if (mode == KERNING_MODE_HARFBUZZ && !fallback.family) {
        const int hair = font->getTextWidth(U"\u200a", 1);
        const int thin = font->getTextWidth(U"\u2009", 1);
        const int em = font->getTextWidth(U"\u2003", 1);
        require(hair > 0 && hair < thin && thin < em && em == 128,
                "HarfBuzz lost the relative widths of missing Unicode spaces");
    }
    font.Clear();
    ShutdownFontManager();
}
}

int main() {
    checkFixture();
    kobalt::setExtraFontWeight(0);
    for (auto fallback : {Fallback{nullptr, nullptr}, Fallback{"NotoSerif-Regular.ttf", "Noto Serif"},
                          Fallback{"NotoSansMath-Regular.ttf", "Noto Sans Math"}}) {
        for (auto mode :
             {KERNING_MODE_DISABLED, KERNING_MODE_FREETYPE, KERNING_MODE_HARFBUZZ_LIGHT, KERNING_MODE_HARFBUZZ}) {
            check(mode, fallback);
        }
    }
    std::puts("PASS: missing Unicode whitespace stays blank with and without fallback fonts");
}
