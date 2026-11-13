#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <cstdio>

using namespace kobalt;

namespace {
const QJsonObject settings{ { "font_pixels", 28 }, { "line_spacing_version", 1 } };

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
    require(model.openResources(source, sizes, { 600, 800 }, settings, &error), qPrintable(error));
}

// Each page as its start and a checksum of its image.
QString pageRecord(EngineDocument &model)
{
    const QImage image = model.image(model.page());
    return model.pageStart(model.page()) + ' '
            + QString::fromLatin1(
                    QCryptographicHash::hash(QByteArray::fromRawData(reinterpret_cast<const char *>(
                                                                             image.constBits()),
                                                                     int(image.sizeInBytes())),
                                             QCryptographicHash::Md5)
                            .toHex());
}

// Chapter mode must give the pages of the whole book, pixel for pixel.
void requireWholeBookPages(const test::BookFiles &files, int slicePages, const char *what)
{
    EngineDocument model(test::readerFonts());
    open(model, files);
    QStringList whole;
    for (int page = 0; page < model.pages(); ++page) {
        require(model.go(page), "Cannot go to a page of the whole book");
        whole.append(pageRecord(model));
    }
    // Chapter mode opens at the position of the whole book.
    require(model.go(0), "Cannot go back to the first page");
    model.setChapterSlices(slicePages);
    QString error;
    require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
    QStringList chapters{ pageRecord(model) };
    while (model.turnInWindow(1) && chapters.size() <= 2 * whole.size())
        chapters.append(pageRecord(model));
    int differs = 0;
    while (differs < qMin(whole.size(), chapters.size()) && whole[differs] == chapters[differs])
        ++differs;
    if (differs < whole.size() || chapters.size() != whole.size())
        std::printf("%s: page %d of %d differs (%d in chapter mode)\n", what, differs + 1,
                    int(whole.size()), int(chapters.size()));
    require(chapters == whole, what);
}

test::BookFiles styledBook(const QByteArray &css, const QByteArray &body,
                           test::BookFiles files = {}, const QByteArray &spine = {})
{
    auto text = test::publisherHtml(body);
    text.replace("</head>", "<link rel=\"stylesheet\" href=\"style.css\"/></head>");
    files["text.xhtml"] = text;
    files["style.css"] = css;
    return test::publisherPackage("en", "Slices", files, "<itemref idref=\"text.xhtml\"/>" + spine);
}

// Sentences of varied length, so that page breaks fall in many places.
QByteArray prose(int seed, int sentences)
{
    static const QByteArray words[] = { "reader",   "chapter", "keeps", "the",  "page",
                                        "turning",  "quietly", "while", "a",    "lantern",
                                        "flickers", "beside",  "old",   "town", "walls" };
    QByteArray text;
    for (int sentence = 0; sentence < sentences; ++sentence) {
        const int length = 4 + (seed * 7 + sentence * 5) % 13;
        for (int word = 0; word < length; ++word)
            text += words[(seed * 3 + sentence * 11 + word * 7) % 15]
                    + QByteArray(word + 1 < length ? " " : ". ");
    }
    return text;
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
        // Many stops early in the load: a stop there must not use up one
        // of the few document slots of the engine each time.
        for (int early = 0; stops && early < 60; ++early) {
            int checks = 0;
            model.prepareAhead([&] { return ++checks <= early % 12; });
            ++stopped;
            require(model.pageStart(model.page()) == lastPage,
                    "Stopped work ahead changed the page on screen");
        }
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

    // The layout of the next slice ahead stops too, and the slice on screen
    // comes back. Stop it after a different number of checks at every page:
    // the page on screen stays, and all pages are the same as without stops.
    require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
    QStringList stopping{ model.pageStart(model.page()) };
    int sliceStops = 0;
    for (int turn = 0;; ++turn) {
        const auto shown = model.pageStart(model.page());
        const int limit = (turn * 7) % 45;
        int checks = 0;
        bool asked = false;
        model.prepareAhead([&] {
            asked = asked || ++checks > limit;
            return !asked;
        });
        sliceStops += asked;
        require(model.pageStart(model.page()) == shown,
                "Stopped work ahead changed the page on screen");
        if (!model.turnInWindow(1))
            break;
        stopping.append(model.pageStart(model.page()));
    }
    require(stopping == forward, "Stopped layouts of the next slice changed the pages");
    require(sliceStops > 20, "The work ahead was not stopped");
    std::printf("work ahead: %d stops while reading the book\n", sliceStops);

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

    // A rule of the book that the reading font does not reach, here one for
    // ::first-letter, can name a font of the book. Chapter mode does not
    // read those fonts first, and must still use that one.
    {
        QFile font(test::readerAssets().file("fonts/ebook-core/NV_Bitter-Regular.ttf"));
        require(font.open(QIODevice::ReadOnly), "Cannot read the embedded font fixture");
        QByteArray body;
        for (int part = 0; part < 6; ++part)
            body += "<h2>Part " + QByteArray::number(part + 1) + "</h2><p class=\"first\">"
                    + prose(part, 6) + "</p><p>" + prose(part + 9, 9) + "</p>";
        requireWholeBookPages(
                styledBook("@font-face{font-family:Letter;src:url(letter.ttf)}"
                           "p.first::first-letter{font-family:Letter;font-size:3em;line-height:0}",
                           body, { { "letter.ttf", font.readAll() } }),
                3, "A book font of ::first-letter changed the pages of chapter mode");
    }
    // A block without text before the first block of a slice, here a rule
    // drawn by ::before, belongs to the slice before. A slice must not
    // start on a page that shows it.
    {
        QByteArray body;
        for (int entry = 0; entry < 40; ++entry) {
            body += "<blockquote class=\"entry\">";
            for (int paragraph = 0; paragraph < 1 + entry % 3; ++paragraph)
                body += "<p>" + prose(entry * 3 + paragraph, 3 + (entry + paragraph) % 5) + "</p>";
            body += "</blockquote>";
        }
        requireWholeBookPages(
                styledBook("blockquote{margin:0}p{margin:0;text-indent:1em}"
                           ".entry+.entry::before{border-top:1px solid;content:\"\";display:block;"
                           "height:0;margin:1.5em auto;width:25%}",
                           body),
                2, "A rule before a slice changed the pages of chapter mode");
    }
    // A table adds its lines to the pages without the flow: the page break
    // after it depends on the block before the table. A slice that starts
    // after a table must get the same break.
    {
        QByteArray body;
        for (int scene = 0; scene < 8; ++scene) {
            body += "<section><h3>Scene " + QByteArray::number(scene + 1)
                    + "</h3><p class=\"direction\">" + prose(scene, 2) + "</p><table>";
            for (int row = 0; row < 6 + scene % 4; ++row)
                body += "<tr><td>SPEAKER</td><td><p>" + prose(scene * 10 + row, 2 + row % 3)
                        + "</p></td></tr>";
            body += "</table></section>";
        }
        requireWholeBookPages(
                styledBook("section{margin-top:2em}h3{margin-top:3em;page-break-after:avoid}"
                           "p{margin:0}p.direction{margin:1em auto;width:75%}"
                           "table{border-collapse:collapse;margin:1em auto;width:100%}"
                           "td{padding:.5em;vertical-align:top}",
                           body),
                2, "A slice after a table changed the pages of chapter mode");
    }
    // A page can start a little below the top of the last line of the
    // paragraph before. Its start then names the next paragraph, but the
    // page shows that line: a slice must not start there.
    {
        QByteArray body;
        for (int paragraph = 0; paragraph < 120; ++paragraph)
            body += "<p>" + prose(paragraph, 1 + paragraph % 6) + "</p>";
        requireWholeBookPages(
                styledBook("p{display:block;text-indent:0.2em;padding:0.1em;margin:0}", body), 2,
                "A line above the page start changed the pages of chapter mode");
    }
    // A last spine item without content adds no page to the whole book. As
    // the last unit of chapter mode it must not add one, nor show the pages
    // before it again.
    requireWholeBookPages(styledBook("p{margin:0}", "<p>" + prose(1, 30) + "</p>",
                                     { { "empty.xhtml", test::publisherHtml({}) } },
                                     "<itemref idref=\"empty.xhtml\"/>"),
                          2, "An empty last spine item changed the pages of chapter mode");
    // A hidden navigation document with white space around it gives, alone,
    // a window with one page without height. As the last unit it adds no
    // page either.
    {
        auto files =
                styledBook("p{margin:0}", "<p>" + prose(2, 30) + "</p>",
                           { { "nav.xhtml",
                               test::publisherHtml(
                                       "\n<nav epub:type=\"toc\" hidden=\"\"><ol>"
                                       "<li><a href=\"text.xhtml\">Text</a></li></ol></nav>\n") } },
                           "<itemref idref=\"nav.xhtml\"/>");
        files["book.opf"].replace("href=\"nav.xhtml\"", "href=\"nav.xhtml\" properties=\"nav\"");
        requireWholeBookPages(files, 2, "A hidden last spine item added a page in chapter mode");
    }
    // A contents entry can point to an id in a spine item that chapter mode
    // has not loaded. The entry and the entries below it must stay in the
    // list, and a jump to it must find the place of the whole book.
    {
        const auto chapter = [](const QByteArray &id, int seed) {
            return test::publisherHtml("<p>" + prose(seed, 20) + "</p><h1 id=\"" + id + "\">"
                                       + id + "</h1><p>" + prose(seed + 1, 20) + "</p>");
        };
        auto files = styledBook(
                "p{margin:0}", "<p>" + prose(3, 30) + "</p>",
                { { "two.xhtml", chapter("two", 4) },
                  { "three.xhtml", chapter("three", 6) },
                  { "nav.xhtml",
                    test::publisherHtml("<nav epub:type=\"toc\"><ol>"
                                        "<li><a href=\"text.xhtml\">Text</a></li>"
                                        "<li><a href=\"two.xhtml#two\">Two</a><ol>"
                                        "<li><a href=\"three.xhtml#three\">Three</a></li>"
                                        "</ol></li></ol></nav>") } },
                "<itemref idref=\"two.xhtml\"/><itemref idref=\"three.xhtml\"/>");
        files["book.opf"].replace("href=\"nav.xhtml\"", "href=\"nav.xhtml\" properties=\"nav\"");
        EngineDocument model(test::readerFonts());
        open(model, files);
        const auto whole = model.chapters();
        require(whole.size() == 3 && whole[2].depth == 1, "The fixture needs a nested contents list");
        model.setChapterSlices(2);
        QString error;
        require(model.showSpineItems(0, 0, &error), qPrintable(error));
        const auto listed = model.chapters();
        require(listed.size() == whole.size(), "Chapter mode lost contents entries");
        for (int i = 0; i < whole.size(); ++i)
            require(listed[i].title == whole[i].title && listed[i].depth == whole[i].depth
                            && !listed[i].position.isEmpty(),
                    "Chapter mode changed a contents entry");
        // The id is in the middle of its spine item: the jump must land on
        // the page of the id, as in the whole book, not on the first page.
        require(listed[2].position != whole[2].position,
                "The fixture needs an entry without its text");
        require(model.goToChapter(2, listed[2].position),
                "Cannot go to a contents entry of chapter mode");
        require(model.chapters()[2].position == whole[2].position,
                "A loaded contents entry must point to its id");
        const auto landed = model.pageStart(model.page());
        EngineDocument reference(test::readerFonts());
        open(reference, files);
        require(reference.goTo(whole[2].position)
                        && reference.pageStart(reference.page()) == landed,
                "A contents entry of chapter mode must open the page of its id");
        require(model.goTo(listed[2].position) && model.pageStart(model.page()) != landed,
                "The fixture needs an id after the first page of its spine item");
    }

    std::printf(
            "PASS: slices of %d pages; %d pages whole, %d pages in slices, largest layout %d pages\n",
            slicePages, int(whole.size()), int(forward.size()), largest);
#else
    std::printf("SKIP: stock CREngine lays out a whole spine item\n");
#endif
    return 0;
}
