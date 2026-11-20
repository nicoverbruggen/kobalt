#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "reader_source_index_probe.h"
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
    EngineDocument model(test::readerFonts());
    open(model, test::engineBook(false, 8, 120));
    const auto whole = pages(model);
    const int wholePages = model.pages();
    require(whole.items.size() == 8, "The fixture needs eight spine items with pages");
    const auto fullInfo = model.pageInfo();
    require(fullInfo.bookPages.exact() && fullInfo.bookPages.value() == wholePages,
            "A whole-book layout must report the exact book total");
    // Compare the exact text and glyphs with the whole-book layout, including
    // an entire intervening chapter and an exclusive end in a third one.
    const auto start = whole.starts[whole.items[1]].at(1);
    const auto end = whole.starts[whole.items[3]].at(1);
    auto passage = model.textRange(start, end, true);
    require(!passage.text.isEmpty(), "Cross-chapter fixture has no selectable text");
    QString rangeError;
    const auto malformed = end.section('.', 0, -2) + ".9999999";
    const auto hitIn = [&](int item) {
        for (const auto &position : whole.starts[whole.items[item]]) {
            require(model.goTo(position), "Cannot open the selection fixture page");
            const auto hits = model.searchPage("reader", model.page()).hits;
            if (!hits.isEmpty())
                return hits.first();
        }
        return SearchHit();
    };
    const auto left = hitIn(1), right = hitIn(3);
    require(!left.start.isEmpty() && !right.end.isEmpty(), "Missing cross-chapter tokens");
    const auto selected =
            model.annotationRange(left.start, left.end, right.start, right.end, true, true);
    const auto reversed = model.annotationRange(right.start, left.end);
    const auto hasLookup = [&] {
        for (int item = 0; item < whole.items.size(); ++item)
            if (ReaderSourceIndexProbe::lookupIncludes(model, item))
                return true;
        return false;
    };
    require(model.showSpineItems(0, 0, &rangeError), qPrintable(rangeError));
    require(model.visibleRanges(QVector<QPair<QString, QString>>(2000, { start, end })).isEmpty()
                    && !hasLookup(),
            "Offscreen marks opened a remote chapter");
    require(model.showSpineItems(2, 2, &rangeError), qPrintable(rangeError));
    const auto edited =
            model.annotationRange(left.start, left.end, right.start, right.end, true, true);
    require(!edited.text.isEmpty() && edited.text == selected.text && edited.start == selected.start
                    && edited.end == selected.end && edited.chosenStart == selected.chosenStart
                    && edited.chosenEnd == selected.chosenEnd,
            "Cross-chapter draft lost its selected words");
    require(model.annotationRange(right.start, left.end).text == reversed.text,
            "Reversed selection lost a token outside the displayed chapter");
    require(model.intersectingRanges(left.start, right.end,
                                     { { right.start, right.end },
                                       { right.end, right.start },
                                       { left.start, malformed } })
                    == QVector<int>{ 0 },
            "A cross-chapter edit missed an overlap or accepted an invalid endpoint");
    require(model.visibleRanges(QVector<QPair<QString, QString>>(2000, { start, end })).size()
                            == 2000
                    && ReaderSourceIndexProbe::lookupIncludes(model, 3),
            "Crossing marks failed to check their remote endpoints");
    require(model.showSpineItems(1, 1, &rangeError) && model.goTo(start), qPrintable(rangeError));
    ReaderSourceIndexProbe::discardLookup(model);
    require(model.rangeVisible(start, end) && !hasLookup(),
            "The next chapter reopened a verified endpoint");
    for (int item = 0; item < 5; ++item) {
        const auto place = whole.starts[whole.items[item]].at(item == 3 ? 0 : 1);
        require(model.showSpineItems(-1, -1, &rangeError) && model.goTo(place),
                qPrintable(rangeError));
        const auto expected = model.annotationGeometry(start, end);
        require(model.showSpineItems(item, item, &rangeError) && model.goTo(place),
                qPrintable(rangeError));
        const auto original = model.position();
        const auto range = model.textRange(end, start, true);
        require(range.text == passage.text && range.context == passage.context
                        && range.start == passage.start && range.end == passage.end,
                "A chapter window changed the source text or surrounding sentence");
        const auto actual = model.annotationGeometry(start, end);
        require(actual.rectangles == expected.rectangles
                        && actual.start.visible == expected.start.visible
                        && actual.end.visible == expected.end.visible,
                "Chapter window changed selection geometry");
        require(model.rangeVisible(start, end) == !expected.rectangles.isEmpty()
                        && model.visibleRanges({ { start, end } }).size()
                                == int(!expected.rectangles.isEmpty()),
                "Draft and saved cross-chapter ranges disagree");
        require(model.rangeRects(start, malformed).isEmpty()
                        && model.annotationGeometry(start, malformed).rectangles.isEmpty()
                        && !model.rangeVisible(start, malformed),
                "Invalid endpoint painted a range");
        require(model.position() == original, "Range query moved the current chapter");
    }
    require(model.showSpineItems(-1, -1, &rangeError), qPrintable(rangeError));
    const auto chapter = whole.items[4];
    const auto position = whole.starts[chapter].value(2);
    require(!position.isEmpty() && model.goTo(position), "Cannot go into the fifth spine item");

    QString error;
    require(model.showSpineItems(3, 5, &error), qPrintable(error));
    const auto face = model.fontFace();
    require(!face.isEmpty(), "The chapter must open with a reading font");
    require(!model.applyAppearance(
                    appearance::Patch({ { "font_face", "" }, { "margin_pixels", -1 } })),
            "A negative margin must reject the appearance change");
    require(model.fontFace() == face, "A rejected margin switched the chapter to publisher fonts");
    const auto window = pages(model);
    require(window.items == whole.items.mid(3, 3), "Only the three spine items must have pages");
    for (const auto &item : window.items)
        require(window.starts[item] == whole.starts[item],
                "A spine item has other pages in the window than in the whole book");
    require(model.pages() < wholePages, "The window must have fewer pages than the book");
    require(model.position() == position || model.goTo(position),
            "A position in the window must resolve");
    require(model.pageStart(model.page()) == position, "The position must be on its own page");
    const auto partialInfo = model.pageInfo();
    require(!partialInfo.bookPages.exact() && !partialInfo.bookPage.exact()
                    && !partialInfo.bookProgress.exact(),
            "A chapter window must not report its local pages as exact book progress");
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
    const auto restored = model.pageInfo();
    require(restored.bookPages.exact() && restored.bookPages.value() == wholePages
                    && restored.bookPage.exact() && restored.bookPage.value() == model.page(),
            "Restoring whole-book layout must restore exact book pages");
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
    return 0;
}
