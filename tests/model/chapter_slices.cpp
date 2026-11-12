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
    require(model.pages() <= 3 * slicePages, "The first slice has too many pages");
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
    require(largest <= 3 * slicePages, "A slice has too many pages");
    require(forward.first() == whole.first(), "Reading must start at the start of the book");
    require(forward.size() >= whole.size(), "Slices cannot need fewer pages than the whole book");
    require(forward.size() <= whole.size() + whole.size() / slicePages + 6,
            "Slices added more than one short page each");
    require(model.spineItemOf(model.position()) == 2 && !model.continuesAfter(),
            "Reading forward must end in the last spine item");

    // The reader prepares the next slice after it shows a page. The pages
    // must be the same with that step, whenever it runs.
    require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
    QStringList prepared{ model.pageStart(model.page()) };
    for (int turn = 0;; ++turn) {
        if (turn % 3)
            model.prepareAhead();
        if (!model.turnInWindow(1))
            break;
        prepared.append(model.pageStart(model.page()));
    }
    require(prepared == forward, "Preparing the next slice ahead changed the pages");

    // In the last slice of a spine item the reader also opens the next
    // spine item ahead. The pages must be the same when it does so after
    // every page.
    require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
    QStringList ahead{ model.pageStart(model.page()) };
    for (;;) {
        model.prepareAhead();
        if (!model.turnInWindow(1))
            break;
        ahead.append(model.pageStart(model.page()));
    }
    require(ahead == forward, "Opening the next spine item ahead changed the pages");

    // The work ahead stops when other work waits. Stop it after every
    // number of checks: the document on screen must stay intact, and the
    // pages after it must be the same as without any work ahead.
    int stopped = 0;
    const auto fromEndOfFirst = [&](bool stops) {
        require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
        while (model.spineItemOf(model.position()) == 0)
            require(model.turnInWindow(1), "The first spine item has no end");
        require(model.turnInWindow(-1) && model.spineItemOf(model.position()) == 0,
                "Cannot go back to the end of the first spine item");
        const auto lastPage = model.pageStart(model.page());
        bool finished = false;
        for (int limit = 1; stops && limit < 200000 && !finished; limit += 1 + limit / 3) {
            int checks = 0;
            bool asked = false;
            model.prepareAhead([&] {
                asked = asked || ++checks > limit;
                return !asked;
            });
            asked ? ++stopped : finished = true;
            require(model.pageStart(model.page()) == lastPage
                            && model.spineItemOf(model.position()) == 0,
                    "Stopped work ahead changed the page on screen");
        }
        require(!stops || finished, "The work ahead never finished");
        QStringList pages{ lastPage };
        while (model.turnInWindow(1))
            pages.append(model.pageStart(model.page()));
        return pages;
    };
    const auto withoutStops = fromEndOfFirst(false);
    require(fromEndOfFirst(true) == withoutStops, "Stopped work ahead changed the pages after it");
    require(stopped > 5, "The work ahead did not stop");
    std::printf("work ahead: stopped %d times before it finished\n", stopped);

    // A spine item that was opened ahead has the settings of that moment.
    // After a change of settings it must not be shown.
    const auto afterFontChange = [&](bool prepare) {
        require(model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", 28 } })),
                "Cannot set the font size");
        require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
        while (model.spineItemOf(model.position()) == 0)
            require(model.turnInWindow(1), "The first spine item has no end");
        require(model.turnInWindow(-1) && model.spineItemOf(model.position()) == 0,
                "Cannot go back to the end of the first spine item");
        if (prepare)
            model.prepareAhead();
        require(model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", 36 } })),
                "Cannot change the font size");
        QStringList pages;
        for (int turn = 0; turn < 8 && model.turnInWindow(1); ++turn)
            pages.append(model.pageStart(model.page()));
        return pages;
    };
    const auto plainPages = afterFontChange(false);
    require(plainPages.size() == 8 && afterFontChange(true) == plainPages,
            "A spine item opened ahead was shown with old settings");
    require(model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", 28 } })),
            "Cannot set the font size back");
    require(model.showSpineItems(2, 2, &error), qPrintable(error));
    while (model.turnInWindow(1)) { }

    // Read backward to the first page. A spine item that is entered from
    // its end has no recorded slices, so its pages can differ from the
    // forward ones, but the whole text is passed again.
    QStringList backward{ model.pageStart(model.page()) };
    while (model.turnInWindow(-1)) {
        backward.prepend(model.pageStart(model.page()));
        require(model.pages() <= 3 * slicePages, "A slice has too many pages going backward");
    }
    require(backward.first() == whole.first(),
            "Reading backward must end at the start of the book");
    require(backward.size() >= whole.size()
                    && backward.size() <= whole.size() + whole.size() / slicePages + 6,
            "Reading backward passed too few or too many pages");

    // Going back over slices that were just read shows the same pages.
    require(model.go(0), "Cannot go to the first page");
    QStringList there{ model.pageStart(model.page()) };
    for (int turn = 0; turn < 4 * slicePages && model.turnInWindow(1); ++turn)
        there.append(model.pageStart(model.page()));
    QStringList back{ model.pageStart(model.page()) };
    for (int turn = 1; turn < there.size() && model.turnInWindow(-1); ++turn)
        back.prepend(model.pageStart(model.page()));
    require(back == there, "Going back over slices that were just read gave other pages");

    // A slice ends where a page starts a block, so the join adds no short
    // page: the pages of the first spine item are those of the whole book.
    QStringList first;
    for (const auto &start : forward)
        if (model.goTo(start) && model.spineItemOf(model.position()) == 0)
            first.append(start);
    QStringList wholeFirst;
    for (int page = 0; page < whole.size(); ++page)
        if (wholeItems[page] == 0)
            wholeFirst.append(whole[page]);
    std::printf("first spine item: %d pages whole, %d pages in slices\n", int(wholeFirst.size()),
                int(first.size()));
    require(first.size() <= wholeFirst.size() + 2, "The joins between slices added short pages");

    // Every page start of the whole book is reachable and is on a page of
    // its own slice.
    for (int page = 0; page < whole.size(); page += 7) {
        require(model.goTo(whole[page]), "A position in the book is not reachable");
        require(model.spineItemOf(model.position()) == wholeItems[page],
                "A jump landed in another spine item");
        require(model.pages() <= 3 * slicePages, "A jump laid out more than a slice");
        QString compareError;
        require(model.comparePosition(model.pageStart(model.page()), &compareError) <= 0,
                "The page of a position starts after the position");
    }

    // A font change keeps the position and stays in slices.
    const auto position = whole[whole.size() / 2];
    require(model.goTo(position), "Cannot go to the middle of the book");
    require(model.applyAppearance(appearance::Patch(QJsonObject{ { "font_pixels", 36 } })),
            "Cannot change the font size in a slice");
    require(model.position() == position && model.pages() <= 3 * slicePages,
            "A font change in a slice lost the position or laid out too much");
    std::printf(
            "PASS: slices of %d pages; %d pages whole, %d pages in slices, largest layout %d pages\n",
            slicePages, int(whole.size()), int(forward.size()), largest);
#else
    std::printf("SKIP: stock CREngine lays out a whole spine item\n");
#endif
    return 0;
}
