#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
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

struct Pages
{
    // The spine items in the order of their first page, and the start
    // position of each page of each spine item.
    QStringList items;
    QMap<QString, QStringList> starts;
};

Pages pages(const EngineDocument &model)
{
    Pages result;
    for (int page = 0; page < model.pages(); ++page) {
        const auto start = model.pageStart(page);
        Location location;
        require(model.locationOf(start, &location), "A page start has no location");
        if (!result.starts.contains(location.href))
            result.items.append(location.href);
        result.starts[location.href].append(start);
    }
    return result;
}
} // namespace

// A book that has the text of only some spine items must lay out those
// spine items into the same pages as the whole book does.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
#if __has_include(<koblaze.h>)
    EngineDocument model(test::readerFonts());
    open(model, test::engineBook(false, 8, 120));
    const auto whole = pages(model);
    const int wholePages = model.pages();
    require(whole.items.size() == 8, "The fixture needs eight spine items with pages");
    const auto chapter = whole.items[4];
    const auto position = whole.starts[chapter].value(2);
    require(!position.isEmpty() && model.goTo(position), "Cannot go into the fifth spine item");

    QString error;
    require(model.showSpineItems(3, 5, &error), qPrintable(error));
    const auto window = pages(model);
    require(window.items == whole.items.mid(3, 3), "Only the three spine items must have pages");
    for (const auto &item : window.items)
        require(window.starts[item] == whole.starts[item],
                "A spine item has other pages in the window than in the whole book");
    require(model.pages() < wholePages, "The window must have fewer pages than the book");
    require(model.position() == position || model.goTo(position),
            "A position in the window must resolve");
    require(model.pageStart(model.page()) == position, "The position must be on its own page");
    // Positions outside the window have no page.
    require(model.pagesOf({ whole.starts[whole.items[0]].first() }).value(0, -1) <= 0,
            "A spine item outside the window must have no pages of its own");

    // Another window, as when the reader goes to another chapter.
    require(model.showSpineItems(6, 7, &error), qPrintable(error));
    const auto last = pages(model);
    require(last.items == whole.items.mid(6, 2)
                    && last.starts[whole.items[7]] == whole.starts[whole.items[7]],
            "The last spine items must have the same pages as in the whole book");

    // A font change in a window, then the whole book again.
    require(model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", 36 } })),
            "Cannot change the font in a window");
    require(model.showSpineItems(-1, -1, &error), qPrintable(error));
    require(model.pages() > 0 && model.fontSize() == 36, "The whole book must keep the settings");
    require(model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", 28 } })),
            "Cannot change the font back");
    const auto again = pages(model);
    require(again.items == whole.items && again.starts == whole.starts,
            "The whole book must come back with the same pages");
    // A position at the start of a spine item outside the window, as the
    // contents list gives it, loads that spine item.
    require(model.showSpineItems(5, 6, &error), qPrintable(error));
    Location first;
    first.href = whole.items[1];
    require(model.goTo(model.positionOf(first)) && model.spineItemOf(model.position()) == 1
                    && model.pageStart(model.page()) == whole.starts[whole.items[1]].first(),
            "A jump to a spine item outside the window must load it");
    require(model.continuesAfter() && !model.continuesBefore(),
            "The window around the second spine item starts at the first");
    // Read the book from the first page to the last and back with a window
    // that moves at each chapter end. Every page must be a page of the
    // whole book, in the same order.
    QStringList all;
    for (const auto &item : whole.items)
        all += whole.starts[item];
    require(model.spineItems().size() == 8, "Every spine item must be known in a window");
    require(model.showSpineItems(0, 1, &error) && model.go(0), qPrintable(error));
    QStringList forward{ model.pageStart(model.page()) };
    while (model.turnInWindow(1))
        forward.append(model.pageStart(model.page()));
    require(forward == all, "Reading forward through moving windows gave other pages");
    QStringList backward{ model.pageStart(model.page()) };
    while (model.turnInWindow(-1))
        backward.prepend(model.pageStart(model.page()));
    require(backward == all, "Reading backward through moving windows gave other pages");
    std::printf("PASS: a window of spine items has the pages of the whole book\n");
#else
    std::printf("SKIP: stock CREngine loads every spine item\n");
#endif
    return 0;
}
