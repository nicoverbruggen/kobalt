#include "check.h"
#include "reader_inputs.h"
#include "engine/crengine/reader_model.h"

#include <QFileInfo>
#include <QGuiApplication>

#include <lvfntman.h>
#include <lvdrawbuf.h>
#include <vector>

#include <cstring>

namespace {
std::vector<lUInt32> shapedPixels(LVFontRef font, const lString32 &text) {
    LVColorDrawBuf buffer(512, 160, 32);
    buffer.Clear(0xffffff);
    buffer.SetTextColor(0);
    font->DrawTextString(&buffer, 0, 100 - font->getBaseline(), text.c_str(), text.length(), U'?');
    std::vector<lUInt32> pixels;
    for (int y = 0; y < buffer.GetHeight(); ++y)
        for (int x = 0; x < buffer.GetWidth(); ++x)
            pixels.push_back(buffer.GetPixel(x, y));
    return pixels;
}
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    kobalt::ReaderModel model(kobalt::test::readerFonts());
    require(model.initializeFonts(), "Cannot initialize fallback styles");
    require(fontMan->RegisterFont(
                lString8(kobalt::test::readerAssets().file("fonts/InstrumentSerif-Regular.ttf").toUtf8().constData())),
            "Cannot register the primary font for fallback checks");
    fontMan->SetFallbackFontSizesAdjusted(false);
    struct Sample {
        const char *family;
        const char *stem;
        lChar32 glyph;
        bool italics;
    };
    for (const auto &sample :
         {Sample{"Noto Serif", "NotoSerif", U'Ж', true}, Sample{"Noto Serif", "NotoSerif", U'Ω', true},
          Sample{"Noto Naskh Arabic", "NotoNaskhArabic", U'ع', false},
          Sample{"Noto Serif Hebrew", "NotoSerifHebrew", U'ש', false},
          Sample{"Noto Serif Armenian", "NotoSerifArmenian", U'Հ', false},
          Sample{"Noto Serif Georgian", "NotoSerifGeorgian", U'ქ', false},
          Sample{"Noto Serif Devanagari", "NotoSerifDevanagari", U'ह', false},
          Sample{"Noto Serif Bengali", "NotoSerifBengali", U'ব', false},
          Sample{"Noto Serif Tamil", "NotoSerifTamil", U'த', true},
          Sample{"Noto Serif Thai", "NotoSerifThai", U'ภ', false}}) {
        require(fontMan->SetFallbackFontFaces(lString8(kobalt::fallbackFontFaces)),
                "Cannot select production fallback chain");
        for (const bool italic : {false, true}) {
            if (italic && !sample.italics) {
                continue;
            }
            for (const bool bold : {false, true}) {
                const auto style = bold ? (italic ? "BoldItalic" : "Bold") : (italic ? "Italic" : "Regular");
                const auto expectedFile = QString("%1-%2.ttf").arg(sample.stem, style);
                lString8 filename;
                int index = -1, family = -1;
                bool math = false, emojis = false;
                require(fontMan->getFontFileNameAndFaceIndex(Utf8ToUnicode(sample.family), bold, italic, filename,
                                                             index, family, math, emojis) &&
                            QFileInfo(QString::fromUtf8(filename.c_str())).fileName() == expectedFile,
                        qPrintable("Renderer did not select " + expectedFile));
                bool inCatalog = false;
                for (const auto &entry : model.fontCatalog()) {
                    if (entry.family == sample.family) {
                        for (const auto &file : entry.files) {
                            inCatalog |= QFileInfo(file).fileName() == expectedFile;
                        }
                    }
                }
                require(inCatalog, qPrintable("Font picker omitted " + expectedFile));
                const auto direct =
                    fontMan->GetFont(64, bold ? 700 : 400, italic, css_ff_serif, lString8(sample.family));
                const auto primary =
                    fontMan->GetFont(64, bold ? 700 : 400, italic, css_ff_serif, lString8("Instrument Serif"));
                require(!direct.isNull() && !primary.isNull(), "Cannot create styled fonts");
                const auto expected = direct->getGlyph(sample.glyph);
                const auto actual = primary->getGlyph(sample.glyph);
                require(expected && actual && actual->bmp_width == expected->bmp_width &&
                            actual->bmp_height == expected->bmp_height && actual->bmp_pitch == expected->bmp_pitch &&
                            actual->advance == expected->advance && actual->origin_x == expected->origin_x &&
                            actual->origin_y == expected->origin_y &&
                            std::memcmp(actual->bmp, expected->bmp, expected->bmp_pitch * expected->bmp_height) == 0,
                        qPrintable("Missing-glyph fallback lost the requested style: " + expectedFile));
            }
        }
    }
    require(fontMan->SetFallbackFontFaces(lString8(kobalt::fallbackFontFaces)),
            "Cannot restore production fallback order");
    for (const bool bold : {false, true}) {
        const auto primary = fontMan->GetFont(64, bold ? 700 : 400, false, css_ff_serif, lString8("Instrument Serif"));
        const auto arabic = fontMan->GetFont(64, bold ? 700 : 400, false, css_ff_serif, lString8("Noto Naskh Arabic"));
        const lString32 word(U"العربية");
        const auto joined = shapedPixels(arabic, word);
        require(joined != shapedPixels(arabic, lString32(U"ا\u200cل\u200cع\u200cر\u200cب\u200cي\u200cة")),
                "Arabic shaping reference did not distinguish joined letters");
        require(shapedPixels(primary, word) == joined,
                "Production fallback order lost Arabic joining or the requested weight");
    }
    const auto primary = fontMan->GetFont(64, 400, false, css_ff_serif, lString8("Instrument Serif"));
    const auto math = fontMan->GetFont(64, 400, false, css_ff_serif, lString8("Noto Sans Math"));
    const auto cjk = fontMan->GetFont(64, 400, false, css_ff_serif, lString8("Noto Serif CJK JP"));
    require(shapedPixels(primary, lString32(U"∀∂∇")) == shapedPixels(math, lString32(U"∀∂∇")),
            "Mathematical symbols fell through to a text or CJK font");
    require(shapedPixels(primary, lString32(U"漢")) == shapedPixels(cjk, lString32(U"漢")),
            "CJK text lost its covering fallback font");
    puts("PASS: production fallback styles, Arabic joining, mathematical symbols and CJK text");
}
