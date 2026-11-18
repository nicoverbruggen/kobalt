#include "check.h"
#include "reader_inputs.h"
#include "engine/engine.h"
#include "reading/style_sample.h"
#include "reading/text_presets.h"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <cstdio>

using namespace kobalt;

namespace {
TextSampleRequest request(const QString &face)
{
    TextSampleRequest sample;
    sample.face = face;
    sample.pixels = 46;
    sample.lineSpacing = 150;
    sample.alignment = QStringLiteral("justify");
    sample.canvas = QSize(680, 480);
    sample.margin = 32;
    setTextSampleText(sample);
    require(!sample.heading.isEmpty() && !sample.paragraphs.isEmpty(),
            "The Styling sample must give the text");
    return sample;
}

bool hasInk(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 128)
                return true;
    return false;
}

// Text preset tiles are drawn with the engine's fonts and no sample document.
// Each example's face draws, the gallery canvas breaks words with the
// hyphenation patterns, and the time of the first draw of a face, which
// loads it, is reported apart from a draw with the face loaded.
void examplesDraw()
{
    const auto engine = createEngine(test::readerFonts());
    for (const auto &preset : textpresets::examples()) {
        const auto face = preset.fontFace;
        auto sample = request(face);
        QElapsedTimer timer;
        timer.start();
        const auto first = engine->drawTextSample(sample);
        const double loading = timer.nsecsElapsed() / 1e6;
        timer.restart();
        const auto again = engine->drawTextSample(sample);
        const double drawing = timer.nsecsElapsed() / 1e6;
        require(!first.isNull() && first.size() == sample.canvas, "A preset sample must draw");
        require(hasInk(first), "A preset sample must show text");
        require(again == first, "A preset sample must draw the same pixels again");
        std::printf("%s: first draw %.1f ms, loaded %.1f ms\n", qPrintable(face), loading, drawing);
        require(engine->pages() == 0, "A sample must not open a document");
    }
    // The gallery can crop the lower lines on short screens. Check that the
    // first few body lines differ at the size of the bundled Kobalt preset.
    auto sample = request(QStringLiteral("Libron"));
    sample.alignment = QStringLiteral("left");
    const auto plain = engine->drawTextSample(sample);
    sample.hyphenate = true;
    const auto hyphenated = engine->drawTextSample(sample);
    require(!plain.isNull() && !hyphenated.isNull(), "Gallery samples must draw");
    const QRect visible(0, 0, sample.canvas.width(), 240);
    require(plain.copy(visible) != hyphenated.copy(visible),
            "Hyphenation must change line breaks near the top of a gallery sample");
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    examplesDraw();
    std::printf("PASS: text preset samples without a document\n");
    return 0;
}
