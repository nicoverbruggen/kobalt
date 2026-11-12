#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include <QGuiApplication>
#include <cstdio>

using namespace kobalt;

namespace {
void open(EngineDocument &model, const test::BookFiles &files)
{
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto source = std::make_shared<ResourceCache>(
            files.keys(),
            [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    QString error;
    require(model.openResources(source, sizes, { 600, 800 },
                                { { "font_pixels", 28 }, { "line_spacing_version", 1 } }, &error),
            qPrintable(error));
}
} // namespace

// A long spine item that is laid out in slices must never lay out more
// than about one slice, and must still give every part of its text, in
// order, with every position reachable.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
#if __has_include(<koblaze.h>)
    const int slicePages = 6;
    EngineDocument model(test::readerFonts());
    open(model, test::engineBook(false, 3, 500));
    QStringList whole;
    QVector<int> wholeItems;
    for (int page = 0; page < model.pages(); ++page) {
        whole.append(model.pageStart(page));
        wholeItems.append(model.spineItemOf(whole.last()));
    }
    const int longest = int(std::count(wholeItems.cbegin(), wholeItems.cend(), 1));
    require(longest > 6 * slicePages, "The fixture needs a spine item of many slices");

    model.setChapterSlices(slicePages);
    QString error;
    require(model.showSpineItems(0, 0, &error), qPrintable(error));
    require(model.pages() <= 2 * slicePages, "The first slice has too many pages");
    require(model.continuesAfter() && !model.continuesBefore(),
            "The book continues after the first slice");

    // Read forward. Positions must never go back, every layout stays small,
    // and the whole text is passed.
    require(model.go(0), "Cannot go to the first page");
    QStringList forward{ model.pageStart(model.page()) };
    int largest = model.pages();
    while (model.turnInWindow(1)) {
        forward.append(model.pageStart(model.page()));
        largest = qMax(largest, model.pages());
    }
    require(largest <= 2 * slicePages, "A slice has too many pages");
    require(forward.first() == whole.first(), "Reading must start at the start of the book");
    require(forward.size() >= whole.size(), "Slices cannot need fewer pages than the whole book");
    require(forward.size() <= whole.size() + whole.size() / slicePages + 6,
            "Slices added more than one short page each");
    require(model.spineItemOf(model.position()) == 2 && !model.continuesAfter(),
            "Reading forward must end in the last spine item");

    // Read backward to the first page.
    QStringList backward{ model.pageStart(model.page()) };
    while (model.turnInWindow(-1))
        backward.prepend(model.pageStart(model.page()));
    require(backward == forward, "Reading backward gave other pages than reading forward");

    // Every page start of the whole book is reachable and is on a page of
    // its own slice.
    for (int page = 0; page < whole.size(); page += 7) {
        require(model.goTo(whole[page]), "A position in the book is not reachable");
        require(model.spineItemOf(model.position()) == wholeItems[page],
                "A jump landed in another spine item");
        require(model.pages() <= 2 * slicePages, "A jump laid out more than a slice");
        QString compareError;
        require(model.comparePosition(model.pageStart(model.page()), &compareError) <= 0,
                "The page of a position starts after the position");
    }

    // A font change keeps the position and stays in slices.
    const auto position = whole[whole.size() / 2];
    require(model.goTo(position), "Cannot go to the middle of the book");
    require(model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", 36 } })),
            "Cannot change the font size in a slice");
    require(model.position() == position && model.pages() <= 2 * slicePages,
            "A font change in a slice lost the position or laid out too much");
    std::printf(
            "PASS: slices of %d pages; %d pages whole, %d pages in slices, largest layout %d pages\n",
            slicePages, int(whole.size()), int(forward.size()), largest);
#else
    std::printf("SKIP: stock CREngine lays out a whole spine item\n");
#endif
    return 0;
}
