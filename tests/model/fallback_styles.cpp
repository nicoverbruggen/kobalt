#include "check.h"
#include "reader_inputs.h"
#include "engine/crengine/reader_model.h"

#include <QFileInfo>
#include <QGuiApplication>

#include <lvfntman.h>

#include <cstring>

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
        require(fontMan->SetFallbackFontFaces(lString8(sample.family)), "Cannot select style fallback");
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
    puts("PASS: real fallback styles, missing-glyph rendering and complete font-picker families");
}
