#include "check.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include <QFile>
#include <QGuiApplication>
#include <cmath>
#include <cstdio>

using namespace kobalt;

namespace {
const QSize viewport{ 1264, 1680 };

QJsonObject settings(int font = 44)
{
    return { { "font_pixels", font },  { "margin_pixels", 80 },  { "top_margin_pixels", 80 },
             { "bottom_margin_pixels", 80 }, { "line_spacing", 150 }, { "line_spacing_version", 1 } };
}

QByteArray typeTester()
{
    QFile file(QStringLiteral(KOBALT_TEST_FIXTURES) + "/trevelyans-type-tester.kepub.epub");
    require(file.open(QIODevice::ReadOnly), "Cannot read the book fixture");
    return file.readAll();
}

// Accurate: the whole book has a layout. It is the answer key for where a
// position lands.
void openAccurate(EngineDocument &model, const QByteArray &bytes, int font = 44)
{
    QString error;
    require(model.open(bytes, viewport, settings(font), &error), qPrintable(error));
}

// Fast: slices of two pages of one spine item, so that most chapters have
// several, with the whole book measured, as after the idle work.
void openFast(EngineDocument &model, const QByteArray &bytes, int font = 44)
{
    openAccurate(model, bytes, font);
    model.setChapterSlices(2);
    QString error;
    require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
    while (model.refinePageEstimate()) {
    }
}

// Every page of the whole book, opened from a position in another chapter
// or another slice, shows the same first words as in Accurate.
void jumpsLandOnTheSamePage(EngineDocument &accurate, EngineDocument &fast)
{
    const int total = accurate.pages();
    // An order that alternates between the end and the start of the book.
    for (int i = 0; i < total; ++i) {
        const int page = i % 2 ? total - 1 - i / 2 : i / 2;
        const auto position = accurate.pageStart(page);
        require(fast.goTo(position), "Fast cannot go to the position");
        if (fast.pageStart(fast.page()) != position)
            std::printf("page %d: fast shows %s\n", page + 1,
                        qPrintable(fast.pageStart(fast.page())));
        require(fast.pageStart(fast.page()) == position,
                "A jump in Fast shows another page than Accurate");
    }
}
// The page number of a position anywhere in the book: before a jump to it,
// from its XHTML, and after the jump, from the layout, it is the number
// that the header shows there.
void bookPagesMatchTheHeader(EngineDocument &accurate, EngineDocument &fast)
{
    const int total = accurate.pages();
    QStringList positions;
    for (int page = 0; page < total; ++page)
        positions.append(accurate.pageStart(page));
    int previous = 0, worst = 0;
    for (int i = 0; i < total; ++i) {
        // From the other end of the book, so that the position has no layout.
        require(fast.goTo(positions[total - 1 - i]), "Fast cannot go to the position");
        const int before = fast.bookPages({ positions[i] }).value(0, -2);
        require(fast.goTo(positions[i]), "Fast cannot go to the position");
        const int shown = fast.roughPages().page;
        const int after = fast.bookPages({ positions[i] }).value(0, -2);
        require(after == shown, "The page of the position on screen is not the header's page");
        worst = qMax(worst, qAbs(before - shown));
        if (qAbs(before - shown) > 1)
            std::printf("page %d: %d before the jump, %d after\n", i + 1, before, shown);
        require(before >= previous, "Page numbers must follow the order of the book");
        previous = before;
    }
    std::printf("book pages: at most %d off before a jump\n", worst);
    require(worst <= 1, "A page number before a jump is far from the one after it");
}
} // namespace

// Fast mode against Accurate: whole-book page numbers, jumps and the
// surfaces that show them.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
#if __has_include(<koblaze.h>)
    const auto bytes = typeTester();
    EngineDocument accurate(test::readerFonts());
    openAccurate(accurate, bytes);
    EngineDocument fast(test::readerFonts());
    openFast(fast, bytes);
    jumpsLandOnTheSamePage(accurate, fast);
    bookPagesMatchTheHeader(accurate, fast);
    std::printf("PASS: fast surfaces\n");
#endif
    return 0;
}
