#include "book_fixtures.h"
#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include "reading/opening_job.h"
#include "reading/reader_session.h"
#include "reading/bookmark_session.h"
#include "reading/highlight_session.h"
#include "reading/reading_book.h"
#include "reading_bridge_fixture.h"
#include "publication/native_resources.h"
#include "publication/page_estimate.h"
#include "publication/resource_server.h"
#include "plain_archive.h"
#include <QDir>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <cstdio>
#include <functional>
#include <memory>

using namespace kobalt;

// Runs the same script of reader actions on the whole book (Accurate) and
// in chapter mode (Fast), and compares what each shows after every action.
// Accurate is the oracle. Prints one table and fails once at the end.
//
// KOBALT_PARITY_BOOK names another EPUB for the book script.
namespace {
const QSize viewport{ 600, 800 };
const QJsonObject settings{ { "font_pixels", 28 }, { "line_spacing_version", 1 } };

struct Book
{
    QString name;
    QByteArray bytes;
    test::BookFiles files;

    bool open(EngineDocument &model, const QJsonObject &saved, QString *error) const
    {
        if (!bytes.isEmpty())
            return model.open(bytes, viewport, saved, error);
        QMap<QString, qint64> sizes;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            sizes[it.key()] = it.value().size();
        const auto copy = files;
        auto source = std::make_shared<ResourceCache>(
                files.keys(),
                [copy](const QString &name, const ResourceCache::Sink &sink, QString *) {
                    const auto data = copy.value(name);
                    return sink(data.constData(), data.size());
                },
                std::make_shared<std::atomic_bool>(false));
        return model.openResources(source, sizes, viewport, saved, error);
    }
};

void nonLinearTurns(bool hide, int slicePages)
{
    Book book;
    book.files["cover.xhtml"] = test::publisherHtml(
            "<div><svg xmlns='http://www.w3.org/2000/svg'"
            " width='180' height='240'><rect width='180' height='240' fill='#555'/></svg></div>");
    QByteArray spine = "<itemref idref='cover.xhtml' linear='no'/>";
    for (const auto &name : { QByteArray("start"), QByteArray("notes"), QByteArray("other-notes"),
                              QByteArray("end"), QByteArray("appendix") }) {
        QByteArray body;
        for (int p = 0; p < 12; ++p)
            body += "<p style='page-break-before:always'>" + name + " " + QByteArray::number(p)
                    + ". " + QByteArray("The text continues. ").repeated(28) + "</p>";
        book.files[name + ".xhtml"] = test::publisherHtml(body);
        spine += "<itemref idref='" + name + ".xhtml'"
                + ((name == "start" || name == "end") ? "" : " linear='no'") + "/>";
    }
    book.files = test::publisherPackage("en", "Non-linear turns", book.files, spine);
    auto appearance = settings;
    appearance["hide_nonlinear_fragments"] = hide;
    QString error;
    EngineDocument whole(test::readerFonts()), chapters(test::readerFonts());
    require(book.open(whole, appearance, &error) && book.open(chapters, appearance, &error),
            qPrintable(error));
    chapters.setChapterSlices(slicePages);
    require(chapters.showSpineItems(0, 0) && chapters.go(0), "Cannot open the chapter fixture");
    const auto turn = [&](int direction) {
        const int from = whole.page(), target = whole.turnTarget(from, direction);
        const auto expected = whole.pageStart(target);
        const auto preview = chapters.previewTurn(direction, {});
        require((target == from) == preview.image.isNull()
                        && (target == from
                            || (preview.position == expected
                                && preview.image == whole.image(target))),
                qPrintable(QString("Preview from page %1 in direction %2 expected %3, got %4")
                                   .arg(from)
                                   .arg(direction)
                                   .arg(expected, preview.position)));
        require(chapters.turnInWindow(direction) == (target != from),
                "A non-linear boundary changed whether the turn succeeded");
        if (target != from)
            require(whole.go(target), "Cannot turn the reference reader");
        require(chapters.pageStart(chapters.page()) == expected
                        && chapters.image(chapters.page()) == whole.image(target),
                "A turn across a non-linear boundary differs from the whole book");
        if (target != from && target % 2 == 0)
            chapters.prepareAhead();
        return target != from;
    };
    require(whole.spineItemOf(whole.position()) == 0
                    && chapters.image(chapters.page()) == whole.image(whole.page()),
            "The non-linear cover must open as the first page");
    while (turn(1)) { }
    require(whole.spineItemOf(whole.position()) == (hide ? 4 : 5),
            "The final readable chapter must be reachable");
    while (turn(-1)) { }
    // Opening a hidden chapter explicitly allows reading all of its pages.
    Location notes;
    notes.href = "notes.xhtml";
    const auto firstNote = whole.positionOf(notes);
    require(whole.goTo(firstNote) && chapters.goTo(firstNote), "Cannot open the hidden chapter");
    while (whole.spineItemOf(whole.position()) != 4)
        require(turn(1), "Cannot leave a hidden chapter after reading it");
}

// A pair of short chapters shares a unit between two long, sliced chapters.
// Non-linear asides at its edges must be skipped when entering from either side.
void groupedNonLinearTurns()
{
    Book book;
    QByteArray spine;
    for (int i = 0; i < 4; ++i) {
        const auto name = QByteArray::number(i) + ".xhtml";
        QByteArray body;
        const auto aside = "<aside epub:type='footnote'><p>Hidden " + name + "</p></aside>";
        if (i == 1 || i == 3)
            body += aside;
        for (int p = 0; p < (i == 0 || i == 3 ? 22 : 2); ++p)
            body += "<p style='page-break-before:always'>" + name + " " + QByteArray::number(p)
                    + ". " + QByteArray("The chapter continues. ").repeated(18) + "</p>";
        if (i == 0 || i == 2)
            body += aside;
        book.files[name] = test::publisherHtml(body);
        spine += "<itemref idref='" + name + "'/>";
    }
    book.files = test::publisherPackage("en", "Grouped chapter edges", book.files, spine);
    auto options = settings;
    options["hide_nonlinear_fragments"] = true;
    EngineDocument whole(test::readerFonts()), chapters(test::readerFonts());
    QString error;
    require(book.open(whole, options, &error) && book.open(chapters, options, &error),
            qPrintable(error));
    chapters.setChapterSlices(2);
    require(chapters.showSpineItems(0, 0) && chapters.go(0),
            "Cannot open the sliced first chapter");
    for (const int direction : { 1, -1 }) {
        bool sawGroup = false;
        for (;;) {
            const int target = whole.turnTarget(whole.page(), direction);
            if (target == whole.page())
                break;
            const auto start = whole.pageStart(target);
            const auto preview = chapters.previewTurn(direction, {});
            require(chapters.turnInWindow(direction) && whole.go(target),
                    "Cannot cross the grouped chapter boundary");
            const auto actual = chapters.pageStart(chapters.page());
            require(actual == start && preview.position == start,
                    qPrintable(QString("Grouped turn %1 must show %2; turn showed %3, preview %4")
                                       .arg(direction)
                                       .arg(start, actual, preview.position)));
            require(chapters.image(chapters.page()) == whole.image(target)
                            && preview.image == whole.image(target),
                    "The grouped chapter turn or preview painted a different page");
            if (chapters.spineItemOf(actual) == 1) {
                // Out-of-window chapters have no pages. Find the last page
                // actually laid out in this window to prove the grouping.
                sawGroup |= chapters.spineItemOf(chapters.pageStart(chapters.pages() - 1)) == 2;
            }
        }
        require(sawGroup, "The two short chapters must share a unit in both directions");
    }
    // A long footnote in the middle and another at the end require several
    // slices. Reaching the end must leave the last linear page on screen.
    QByteArray middle;
    for (int p = 0; p < 36; ++p) {
        if (p == 9 || p == 30)
            middle += "<aside epub:type='footnote'>";
        if (p == 27)
            middle += "</aside>";
        middle += "<p style='page-break-before:always'>Slice " + QByteArray::number(p) + " "
                + QByteArray("The text continues. ").repeated(18) + "</p>";
    }
    middle += "</aside>";
    book.files = test::publisherPackage("en", "Sliced note",
                                        { { "middle.xhtml", test::publisherHtml(middle) } },
                                        "<itemref idref='middle.xhtml'/>");
    require(book.open(whole, options, &error) && book.open(chapters, options, &error),
            qPrintable(error));
    chapters.setChapterSlices(2);
    require(chapters.showSpineItems(0, 0) && chapters.go(0),
            "Cannot open the sliced aside fixture");
    for (const int step : { 1, -1 }) {
        for (;;) {
            const int from = whole.page(), target = whole.turnTarget(from, step);
            if (from == target) {
                const auto start = whole.pageStart(from);
                require(!chapters.turnInWindow(step) && chapters.pageStart(chapters.page()) == start
                                && chapters.image(chapters.page()) == whole.image(from),
                        "A failed turn through hidden slices must keep the visible page");
                break;
            }
            const auto start = whole.pageStart(target);
            const auto preview = chapters.previewTurn(step, {});
            require(chapters.turnInWindow(step) && whole.go(target)
                            && chapters.pageStart(chapters.page()) == start
                            && preview.position == start,
                    qPrintable(QString("Sliced aside at %1 in direction %2 expected %3, got %4")
                                       .arg(from)
                                       .arg(step)
                                       .arg(start, chapters.pageStart(chapters.page()))));
        }
    }
}

// One reader in one mode, with what the actions remember.
struct Reader
{
    const Book *book = nullptr;
    std::unique_ptr<EngineDocument> model;
    QStringList history;
    // Lines that explain a detail, printed for a difference.
    QStringList lines;
    QString bookmark, highlight;
    WordSelection selectedWord;
    QVector<QRect> selectedRects;
    QVector<SearchHit> hits;

    bool open(const QJsonObject &saved)
    {
        model.reset(new EngineDocument(test::readerFonts()));
        QString error;
        return book->open(*model, saved, &error);
    }
};

struct Action
{
    QString name;
    // Does the action and returns a detail to compare, such as a note text.
    // A text that starts with "error:" is a failure.
    std::function<QString(Reader &)> run;
    // Not compared, with the reason. The action still runs.
    QString skipped = {};
};

// An action whose row is not compared, for example because another piece
// of work owns it.
Action skip(Action action, const QString &reason)
{
    action.skipped = reason;
    return action;
}

QString locationText(EngineDocument &model, const QString &position)
{
    Location location;
    if (!model.locationOf(position, &location))
        return "?" + position;
    QStringList path;
    for (int index : location.path)
        path.append(QString::number(index));
    return location.href + '/' + path.join('.') + ':' + QString::number(location.offset);
}

struct Shown
{
    QString location, checksum, words, position, detail;
    QStringList lines;

    bool operator==(const Shown &other) const
    {
        return location == other.location && checksum == other.checksum && detail == other.detail;
    }
};

Shown shown(Reader &reader, const QString &detail)
{
    auto &model = *reader.model;
    Shown result;
    const int page = model.page();
    result.position = model.pageStart(page);
    result.location = locationText(model, result.position);
    const QImage image = model.image(page);
    result.checksum = QString::fromLatin1(
            QCryptographicHash::hash(
                    QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()),
                                            int(image.sizeInBytes())),
                    QCryptographicHash::Md5)
                    .toHex()
                    .left(8));
    const auto end = page + 1 < model.pages() ? model.pageStart(page + 1) : QString();
    if (!end.isEmpty())
        result.words = model.textRange(result.position, end).text.simplified().left(40);
    result.detail = detail;
    result.lines = reader.lines;
    reader.lines.clear();
    return result;
}

// Actions, as the reader window asks them of the engine.
Action gotoChapter(int index)
{
    return { QString("gotoChapter(%1)").arg(index), [index](Reader &reader) -> QString {
                const auto chapters = reader.model->chapters();
                if (index >= chapters.size())
                    return "error: no chapter";
                return reader.model->goTo(chapters[index].position) ? QString()
                                                                    : "error: goTo failed";
            } };
}

Action turn(int delta)
{
    return { QString("turn(%1%2)").arg(delta > 0 ? "+" : "").arg(delta),
             [delta](Reader &reader) -> QString {
                 for (int step = 0; step < qAbs(delta); ++step) {
                     if (!reader.model->turnInWindow(delta > 0 ? 1 : -1))
                         return QString("error: stopped after %1").arg(step);
                     reader.model->prepareAhead();
                 }
                 return {};
             } };
}

Action followLink(const QString &label, bool unavailable = false)
{
    return { QString("followLink(%1)").arg(label), [label, unavailable](Reader &reader) -> QString {
                auto &model = *reader.model;
                for (const auto &link : model.pageLinks()) {
                    if (model.linkDetails(link.source).label != label)
                        continue;
                    // A tap asks for the details with a preview, as here.
                    const auto details = model.linkDetails(link.source);
                    if (unavailable)
                        return details.kind == LinkKind::Unavailable
                                ? "unavailable"
                                : "error: missing target resolved";
                    if (details.kind == LinkKind::Footnote) {
                        const auto note = details.html;
                        return "note: "
                                + QString(note)
                                          .remove(QRegularExpression("<[^>]*>"))
                                          .simplified()
                                          .left(40);
                    }
                    if (details.kind != LinkKind::Passage)
                        return QString("error: link kind %1").arg(int(details.kind));
                    reader.history.append(model.position());
                    return model.goTo(details.target) ? QString() : "error: goTo failed ";
                }
                return "error: no such link on the page";
            } };
}

// The kinds of the links on the page, as the page asks for them to mark
// notes.
// `guessed` names links whose kind chapter mode guesses until they are
// tapped. Their kind is shown as "?".
Action linkKinds(const QStringList &guessed = {})
{
    return { "linkKinds()", [guessed](Reader &reader) -> QString {
                QStringList kinds;
                for (const auto &link : reader.model->pageLinks())
                    kinds.append(guessed.contains(reader.model->linkDetails(link.source).label)
                                         ? QStringLiteral("?")
                                         : QString::number(int(link.kind)));
                return "kinds: " + kinds.join(' ');
            } };
}

Action back()
{
    return { "back()", [](Reader &reader) -> QString {
                if (reader.history.isEmpty())
                    return "error: no history";
                return reader.model->goTo(reader.history.takeLast()) ? QString()
                                                                     : "error: goTo failed";
            } };
}

// `abandoned` is a search that gets one batch and is then replaced, as when
// the reader types a new query while the first one runs.
Action search(const QString &query, const QString &abandoned = {})
{
    const auto name = abandoned.isEmpty() ? QString("search(%1)").arg(query)
                                          : QString("search(%1 after %2)").arg(query, abandoned);
    return { name, [query, abandoned](Reader &reader) -> QString {
                auto &model = *reader.model;
                // The reader measures the book in the background. With the
                // whole page map a hit in chapter mode has an estimated page.
                while (model.refinePageEstimate()) { }
                if (!abandoned.isEmpty())
                    model.searchText(abandoned, {}, 32);
                QVector<SearchHit> found;
                SearchBatch batch;
                for (int round = 0; round < 100000 && !batch.done; ++round) {
                    batch = model.searchText(query, batch.cursor, 512);
                    found += batch.hits;
                }
                reader.hits = model.searchDetails(found, SearchScope::Book, {});
                QStringList &hits = reader.lines;
                hits.clear();
                for (const auto &hit : reader.hits)
                    // The engine position: the text of a hit can be in a
                    // spine item that the document on screen has not loaded.
                    hits.append(hit.start + ' ' + hit.before.right(12) + '[' + hit.text + ']');
                return QString("%1 hits: %2")
                        .arg(reader.hits.size())
                        .arg(QString::fromLatin1(QCryptographicHash::hash(hits.join('\n').toUtf8(),
                                                                          QCryptographicHash::Md5)
                                                         .toHex()
                                                         .left(8)));
            } };
}

void searchUnopenedChapter()
{
    Book book;
    book.files = test::engineBook();
    book.files["OEBPS/c3.xhtml"] = test::publisherHtml(
            QStringLiteral("<p>Caf&#233;teria a−minus سـلام</p><p>One<!-- note > here -->Two</p>"
                           "<p>Pub<![CDATA[lication]]>Needle</p>"
                           "<p>rose<span style='display:none'>omitted</span>bud</p>")
                    .toUtf8());
    book.files["OEBPS/c2.xhtml"] =
            test::publisherHtml("<p>Andr&eacute;nav</p>"
                                + QByteArray("<p>Intervening chapter text.</p>").repeated(800));
    QString error;
    EngineDocument whole(test::readerFonts()), chapters(test::readerFonts());
    require(book.open(whole, settings, &error) && book.open(chapters, settings, &error),
            qPrintable(error));
    chapters.setChapterSlices(2);
    require(chapters.showSpineItems(0, 0), "Cannot open the first chapter");
    const auto hits = [](EngineDocument &model, const QString &query) {
        QStringList positions;
        SearchBatch batch;
        do {
            batch = model.searchText(query, batch.cursor, 16);
            for (const auto &hit : batch.hits)
                positions.append(hit.start);
        } while (!batch.done);
        return positions;
    };
    for (const auto &query :
         { QStringLiteral("cafe"), QStringLiteral("a-minus"), QStringLiteral("سلام"),
           QStringLiteral("OneTwo"), QStringLiteral("PublicationNeedle"),
           QStringLiteral("Andrénav"), QStringLiteral("rosebud") }) {
        const auto expected = hits(whole, query);
        require(expected.size() == 1 && hits(chapters, query) == expected,
                qPrintable("Missing search result in an unopened chapter: " + query));
    }
}

// A negative index counts from the end.
Action openResult(int index)
{
    return { QString("openResult(%1)").arg(index), [index](Reader &reader) -> QString {
                const int at = index < 0 ? reader.hits.size() + index : index;
                if (at < 0 || at >= reader.hits.size())
                    return "error: no such result";
                reader.history.append(reader.model->position());
                return reader.model->goTo(reader.hits[at].start) ? QString() : "error: goTo failed";
            } };
}

// A position in another slice has no page in the current layout. Returning
// a nearby page would let a bookmark mark unrelated text.
Action pagesOfOtherSlice()
{
    return { "pagesOf(other slice)", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                const bool fast = model.bookFraction() >= 0;
                if (fast)
                    model.setChapterSlices(2);
                const auto chapters = model.chapters();
                if (chapters.size() <= 4 || !model.goTo(chapters[4].position))
                    return "error: no chapter for slice lookup";
                const QString first = model.pageStart(model.page());
                const QString chapter = model.chapterTitle(first);
                for (int i = 0; i < 8; ++i)
                    if (!model.turnInWindow(1))
                        return "error: no later slice";
                const QString later = model.pageStart(model.page());
                if (model.chapterTitle(later) != chapter)
                    return "error: later position left the chapter";
                if (!model.goTo(first))
                    return "error: cannot return to first slice";
                const auto fromFirst = model.pagesOf({ first, later });
                const bool laterKnown = fromFirst.value(1, -1) >= 0;
                if (fromFirst.value(0, -1) < 0 || laterKnown == fast)
                    return "error: later slice aliases a current page";
                if (!model.goTo(later))
                    return "error: cannot return to later slice";
                const auto fromLater = model.pagesOf({ first, later });
                const bool firstKnown = fromLater.value(0, -1) >= 0;
                if (fromLater.value(1, -1) < 0 || firstKnown == fast)
                    return "error: earlier slice aliases a current page";
                return "only positions with a layout have pages";
            } };
}

// A query skips early chapters, then background preparation replaces its side document.
Action searchAfterAhead()
{
    return { "searchAfterAhead()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                if (model.bookFraction() >= 0) {
                    model.setChapterSlices(1000);
                    QString error;
                    require(model.showSpineItems(0, 0, &error) && model.go(0), qPrintable(error));
                }
                SearchBatch batch;
                QStringList hits;
                for (int round = 0; round < 1000 && !batch.done; ++round) {
                    batch = model.searchText("wine", batch.cursor, 1, SearchScope::Book, {}, false);
                    for (const auto &hit : batch.hits)
                        hits.append(hit.start);
                    // The shown item ends before the query's first chapter.
                    // Its next prepared unit includes that chapter too.
                    model.prepareAhead();
                }
                require(batch.done, "Search did not finish after replacing the side document");
                reader.lines = hits;
                return QString("%1 hits: %2")
                        .arg(hits.size())
                        .arg(QString::fromLatin1(QCryptographicHash::hash(hits.join('\n').toUtf8(),
                                                                          QCryptographicHash::Md5)
                                                         .toHex()
                                                         .left(8)));
            } };
}

// A cancelled query stops before reporting hits, including while opening a side document.
Action searchCancellation()
{
    return { "searchCancellation()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                const auto stopped = model.searchText("the", {}, 1, SearchScope::Book, {}, false,
                                                      [] { return false; });
                require(stopped.hits.isEmpty() && !stopped.done,
                        "A cancelled search reported hits or completion");
                if (model.bookFraction() >= 0) {
                    SearchCursor cursor;
                    cursor.item = 4;
                    int checks = 0;
                    const auto interrupted =
                            model.searchText("the", cursor, 1, SearchScope::Book, {}, false,
                                             [&checks] { return ++checks < 32; });
                    require(checks >= 32 && interrupted.hits.isEmpty()
                                    && interrupted.cursor.item == cursor.item && !interrupted.done,
                            "Opening a chapter for search ignored cancellation");
                    SearchBatch retry;
                    retry.cursor = cursor;
                    for (int round = 0; round < 1000 && retry.hits.isEmpty() && !retry.done;
                         ++round)
                        retry = model.searchText("the", retry.cursor, 1, SearchScope::Book, {},
                                                 false);
                    require(!retry.hits.isEmpty(), "Cancelled chapter search cannot resume");
                }
                return "cancelled; retry succeeds";
            } };
}

// The contents use book pages, including chapters outside the loaded slice.
Action contentsList()
{
    return { "contentsList()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                while (model.refinePageEstimate()) { }
                ReaderFrame frame;
                captureChapters(frame, model);
                QStringList positions;
                for (const auto &link : frame.chapters)
                    positions.append(link.position);
                const auto pages = model.bookPages(positions);
                QStringList rows;
                for (int i = 0; i < frame.chapters.size(); ++i) {
                    const auto &link = frame.chapters[i];
                    rows.append(link.title
                                + (link.page >= 0 && link.page + 1 == pages.value(i)
                                           ? " [book page]"
                                           : " [missing book page]"));
                }
                return rows.join(';');
            } };
}

// Highlight endpoints name the first excluded character, including across chapters.
Action exclusiveEndPages()
{
    return { "exclusiveEndPages()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                while (model.refinePageEstimate()) { }
                const auto chapters = model.chapters();
                if (chapters.size() <= 8 || !model.goTo(chapters[0].position)
                    || !model.goTo(chapters[4].position))
                    return "error: no exclusive-end chapter fixture";
                const int page = model.page();
                if (page + 1 >= model.pages())
                    return "error: no next-page endpoint fixture";
                const QString start = model.pageStart(page), end = model.pageStart(page + 1);
                const auto loaded = captureBookmarkLocations(model, { start, end }, true, false);
                if (loaded.bookPages.size() != 2 || loaded.bookPages[0] != loaded.bookPages[1])
                    return "error: loaded exclusive end includes the next page";
                if (!model.goTo(chapters[8].position))
                    return "error: cannot leave exclusive-end chapter";
                const auto offscreen = captureBookmarkLocations(
                        model, { chapters[0].position, chapters[1].position }, true, false);
                const int next = model.bookPages({ chapters[1].position }).value(0, -1) - 1;
                if (offscreen.bookPages.size() != 2 || offscreen.bookPages[1] < 0
                    || offscreen.bookPages[1] >= next)
                    return QString("error: offscreen exclusive end %1 includes next chapter %2")
                            .arg(offscreen.bookPages.value(1, -1))
                            .arg(next);
                return "exclusive ends stay on the preceding page";
            } };
}

// Real session caches retain labels and titles after leaving a saved passage.
Action sessionLists()
{
    return {
        "sessionLists()",
        [](Reader &reader) -> QString {
            const auto wait = [](const char *stage, const std::function<bool()> &ready) {
                if (ready())
                    return;
                QEventLoop loop;
                QTimer poll, timeout;
                QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                    if (ready())
                        loop.quit();
                });
                QObject::connect(&timeout, &QTimer::timeout, &loop, [stage] {
                    require(false,
                            qPrintable(QString("Session list parity timed out: %1").arg(stage)));
                });
                poll.start(1);
                timeout.setSingleShot(true);
                timeout.start(10000);
                loop.exec();
            };
            const auto previousBackend = qgetenv("KOBALT_ANNOTATIONS_SQLITE");
            const auto previousPages = qgetenv("KOBALT_CHAPTER_PAGES");
            qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
            qputenv("KOBALT_CHAPTER_PAGES", "2");
            QString labels;
            {
                const QString path =
                        QStringLiteral(KOBALT_TEST_FIXTURES) + "/trevelyans-type-tester.kepub.epub";
                native::ResourceServer server;
                require(server.open(path, [path] { return native::openPlainArchive(path); }),
                        "Cannot serve the session list fixture");
                QTemporaryDir scratch(
                        QDir(QStringLiteral(KOBALT_TEST_FIXTURES))
                                .absoluteFilePath("../../build/parity-session-XXXXXX"));
                require(scratch.isValid(), "Cannot create session list profile");
                auto owned = std::make_unique<ReaderSession>(test::readerFonts(),
                                                             test::readerPlaceholder());
                auto &session = *owned;
                auto lease = std::make_shared<SourceLease>();
                OpenRequest request{ "file:///parity-session.epub",
                                     path,
                                     viewport,
                                     {},
                                     {},
                                     {},
                                     lease,
                                     nativeResourceFactory(path, server.port(), lease) };
                request.appearanceDefaults = settings;
                bool opened = false;
                session.open(std::move(request), [&](const OpeningResult &result) {
                    require(result.ready(), "Session list fixture did not open");
                    opened = true;
                });
                wait("open", [&] { return opened && !session.busy(); });
                const auto chapters = session.chapters();
                require(chapters.size() > 8, "Missing session list chapters");
                const auto go = [&](const QString &position) {
                    require(session.goTo(position), "Cannot queue session list jump");
                    wait("jump and neighbours",
                         [&] { return !session.busy() && !session.preparingNeighbours(); });
                    require(session.error().isEmpty(), "Session list jump failed");
                };
                go(chapters[4].position);
                qint64 readingClock = 0;
                const auto progressCalls = std::make_shared<test::HostProgress>();
                ReadingBook readingBook(std::move(owned), scratch.path() + "/book-state.json",
                                        storage::Paths{ scratch.path() }, "session-lists",
                                        session.sourceHash(),
                                        std::make_unique<test::HostReadingBridge>(progressCalls),
                                        {}, {}, {}, {}, [&] { return readingClock; });
                readingBook.discardPendingSaves();
                readingBook.observeReading();
                const auto layout = [&] {
                    return bookmarkLayoutKey(session.pages(), session.state(), viewport);
                };
                BookmarkSession bookmarks(session, layout, scratch.path() + "/annotations",
                                          "session-lists", session.sourceHash(), session.title());
                HighlightSession highlights(session, layout, scratch.path() + "/annotations",
                                            "session-lists", session.sourceHash(), session.title());
                require(bookmarks.sync(), "Empty bookmark list is not ready");
                const auto addingState = session.state();
                require(bookmarks.toggle(session.page()) == BookmarkSession::Toggle::Adding,
                        "Cannot add session bookmark");
                bool summarySettled = false;
                session.pageSummary(session.page(),
                                    [&](QString, QString) { summarySettled = true; });
                wait("bookmark save", [&] {
                    if (summarySettled && bookmarks.bookmarks().isEmpty() && !bookmarks.saving()) {
                        const auto state = session.state();
                        std::fprintf(
                                stderr,
                                "bookmark rejected: fast=%d pages %d->%d rough %d->%d slice same=%d\n",
                                state.contains("book_fraction"), addingState.value("pages").toInt(),
                                state.value("pages").toInt(),
                                addingState.value("rough_pages").toInt(),
                                state.value("rough_pages").toInt(),
                                addingState.value("slice_start") == state.value("slice_start"));
                        require(false, "Session bookmark add was discarded before saving");
                    }
                    return bookmarks.bookmarks().size() == 1 && !bookmarks.saving();
                });
                wait("initial bookmark labels", [&] {
                    bookmarks.sync();
                    return bookmarks.resolved();
                });
                const auto bookmark = bookmarks.bookmarks().first();
                require(!bookmark.chapter.isEmpty(), "Session bookmark lost chapter title");
                wait("book page estimate", [&] {
                    return !session.state().contains("book_fraction")
                            || session.state().value("rough_page").toInt() > 0;
                });
                wait("bookmark labels after estimate", [&] {
                    bookmarks.sync();
                    return bookmarks.resolved();
                });
                const auto bookmarkSlice = layout();
                const int bookmarkBookPage = bookmarks.bookPage(0);
                require(bookmarkBookPage >= 0, "New bookmark has no book page");
                require(session.turn(1), "Cannot turn within the bookmark slice");
                wait("same-slice turn",
                     [&] { return !session.busy() && !session.preparingNeighbours(); });
                require(layout() == bookmarkSlice, "Bookmark fixture left its initial slice");
                bookmarks.sync();
                wait("same-slice bookmark resolution", [&] {
                    bookmarks.sync();
                    return bookmarks.resolved();
                });
                require(bookmarks.bookPage(0) == bookmarkBookPage,
                        "New bookmark lost its book page after a same-slice turn");
                // Reload the chapter so the later slice-cache check starts
                // with the same fresh slice as the original fixture.
                go(chapters[8].position);
                go(chapters[4].position);
                auto &model = *reader.model;
                require(model.goTo(model.chapters()[4].position),
                        "Cannot prepare highlight fixture");
                const auto hits = model.searchPage("the", model.page()).hits;
                require(!hits.isEmpty(), "No session highlight fixture");
                bool saved = false;
                highlights.save(hits.first().start, hits.first().end, {}, [&](bool ok) {
                    require(ok, "Cannot save session highlight");
                    saved = true;
                });
                wait("highlight save", [&] { return saved && !highlights.saving(); });
                Highlight highlight;
                require(highlights.detail(highlights.highlights().first().id, &highlight),
                        "Cannot read session highlight");
                require(highlight.chapter == bookmark.chapter,
                        "Session highlight has another chapter title");
                const auto sliceKey = layout();
                const auto slicePages = session.pages();
                const auto sliceState = session.state();
                const bool fastSession = sliceState.contains("book_fraction");
                ReadingPace positionPace;
                positionPace.observePosition(session.state().value("position").toString(), false);
                int paceTurns = 0;
                for (int step = 0; step < 3
                     || (fastSession
                         && session.state().value("slice_start")
                                 == sliceState.value("slice_start"));
                     ++step) {
                    require(step < 20, "Session fixture did not reach another slice");
                    require(session.turn(1), "Cannot turn into a later session slice");
                    wait("later-slice turn",
                         [&] { return !session.busy() && !session.preparingNeighbours(); });
                    const auto position = session.state().value("position").toString();
                    positionPace.observePosition(position, true);
                    positionPace.observePosition(position, true);
                    ++paceTurns;
                }
                require(positionPace.forwardTurns() == paceTurns,
                        "Source pace lost a slice-boundary turn or counted a repeated frame");
                require(session.chapterTitle() == bookmark.chapter,
                        "Later slice lost its chapter title");
                if (session.state().contains("book_fraction")) {
                    require(bookmarkLayoutKey(slicePages, session.state(), viewport) != sliceKey,
                            "Same-size Fast slice retained the old list cache key");
                }
                const auto oldKey = layout();
                const auto progress = session.progressFraction();
                require(progress > 0 && progress < 1, "Midbook sync fraction is invalid");
                go(chapters[8].position);
                require(session.progressFraction() >= progress && session.progressFraction() <= 1,
                        "Forward chapter jump reset the sync fraction");
                readingBook.observeReading();
                require(progressCalls->observedActivity.page == session.bookPage()
                                && progressCalls->observedActivity.pages == session.bookPages(),
                        "Reading activity still uses slice-local pages");
                readingBook.pace().observe(0, false);
                for (int page = 1; page <= 3; ++page)
                    readingBook.pace().observe(page, true);
                readingClock = ReadingPace::minimumMilliseconds;
                const auto estimates = readingBook.estimates();
                require(estimates.bookMilliseconds
                                == readingClock * (session.bookPages() - session.bookPage()) / 3,
                        "Fallback time left still uses slice-local pages");
                if (session.state().contains("book_fraction"))
                    require(layout() != oldKey,
                            "Fast chapter jump retained the old list cache key");
                bookmarks.sync();
                highlights.sync();
                wait("offscreen lists resolution", [&] {
                    bookmarks.sync();
                    highlights.sync();
                    return bookmarks.resolved() && highlights.resolved();
                });
                require(!bookmarks.marked(session.page()),
                        "Bookmark in another chapter marked the local page");
                require(bookmarks.bookPage(0) >= 0, "Bookmark list lost its book page");
                QVector<int> displayedPages;
                bool pageLabelReady = false;
                session.bookmarkPages({ bookmark.position }, [&](QVector<int> pages) {
                    displayedPages = std::move(pages);
                    pageLabelReady = true;
                });
                wait("saved-word label", [&] { return pageLabelReady; });
                require(displayedPages.value(0, -1) == bookmarks.bookPage(0),
                        "Saved-word page label differs from the bookmark book page");
                require(highlights.locations().bookPages.value(0, -1) >= 0,
                        "Highlight list lost its book page");
                labels = bookmark.chapter + ";" + highlight.chapter;
                go(bookmark.position);
                bookmarks.sync();
                wait("reopened bookmark resolution", [&] {
                    bookmarks.sync();
                    return bookmarks.resolved();
                });
                require(bookmarks.marked(session.page()), "Reopened bookmark is not marked");
                go(highlight.start);
                require(session.chapterTitle() == highlight.chapter,
                        "Reopened highlight has another chapter title");
            }
            bool drained = false;
            ReaderSession::drain([&] { drained = true; });
            wait("engine drain", [&] { return drained; });
            if (previousBackend.isNull())
                qunsetenv("KOBALT_ANNOTATIONS_SQLITE");
            else
                qputenv("KOBALT_ANNOTATIONS_SQLITE", previousBackend);
            if (previousPages.isNull())
                qunsetenv("KOBALT_CHAPTER_PAGES");
            else
                qputenv("KOBALT_CHAPTER_PAGES", previousPages);
            return labels;
        }
    };
}

// A saved position in another chapter must retain a whole-book page label.
Action savedList(const QString &kind)
{
    return { kind + "List()", [kind](Reader &reader) -> QString {
                auto &model = *reader.model;
                while (model.refinePageEstimate()) { }
                const auto position =
                        kind == QLatin1String("bookmark") ? reader.bookmark : reader.highlight;
                const auto locations = captureBookmarkLocations(model, { position }, false, false);
                const int expected = model.bookPages({ position }).value(0, 0);
                return expected > 0 && locations.bookPages.value(0, -1) + 1 == expected
                        ? "book page available"
                        : "missing book page";
            } };
}

Action addBookmark()
{
    return { "addBookmark()", [](Reader &reader) -> QString {
                reader.bookmark = reader.model->position();
                return {};
            } };
}

Action openBookmark()
{
    return { "openBookmark()", [](Reader &reader) -> QString {
                return reader.model->goTo(reader.bookmark) ? QString() : "error: goTo failed";
            } };
}

// Highlights the first match of a word on the page.
Action addHighlight(const QString &word)
{
    return { QString("addHighlight(%1)").arg(word), [word](Reader &reader) -> QString {
                const auto hits = reader.model->searchPage(word, reader.model->page()).hits;
                if (hits.isEmpty())
                    return "error: word not on the page";
                reader.highlight = hits.first().start;
                return {};
            } };
}

Action openHighlight()
{
    return { "openHighlight()", [](Reader &reader) -> QString {
                return reader.model->goTo(reader.highlight) ? QString() : "error: goTo failed";
            } };
}

Action setFontSize(int delta)
{
    return { QString("setFontSize(%1%2)").arg(delta > 0 ? "+" : "").arg(delta),
             [delta](Reader &reader) -> QString {
                 const int pixels = reader.model->fontSize() + delta;
                 return reader.model->applyAppearance(
                                appearance::Patch(QJsonObject{ { "font_pixels", pixels } }))
                         ? QString()
                         : "error: font change failed";
             } };
}

Action saveAndReopen()
{
    return { "saveAndReopen()", [](Reader &reader) -> QString {
                const auto saved = reader.model->state();
                return reader.open(saved) ? QString() : "error: reopen failed";
            } };
}

Action backwardChapterBoundary()
{
    return { "backAcrossChapter()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                if (model.bookFraction() >= 0)
                    model.setChapterSlices(2);
                const auto chapters = model.chapters();
                return chapters.size() > 4 && model.goTo(chapters[4].position)
                                && model.turnInWindow(-1)
                        ? QString()
                        : QStringLiteral("error: backward chapter turn failed");
            } };
}

Action jumpToBoundaryChapter()
{
    return { "jumpToBoundaryChapter(9)", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                if (model.bookFraction() >= 0)
                    model.setChapterSlices(2);
                const auto chapters = model.chapters();
                return chapters.size() > 9 && model.goTo(chapters[9].position)
                        ? QString()
                        : QStringLiteral("error: boundary chapter jump failed");
            } };
}

Action prepareChapterBoundary(bool cancel = false)
{
    return { cancel ? "cancelBoundaryPreparation()" : "prepareBoundary()",
             [cancel](Reader &reader) -> QString {
                 auto &model = *reader.model;
                 const auto position = model.position();
                 const auto image = model.image(model.page());
                 int checks = 0;
                 model.prepareAhead(cancel ? std::function<bool()>([&] { return ++checks < 8; })
                                           : std::function<bool()>());
                 if (model.position() != position || model.image(model.page()) != image)
                     return "error: background boundary preparation changed the reading page";
                 if (cancel && model.bookFraction() >= 0 && checks < 8)
                     return "error: boundary cancellation was not exercised";
                 return {};
             } };
}

Action cachedBoundaryTurn(int direction, bool prepare = true, bool cached = true)
{
    return { QString("%1BoundaryTurn(%2)")
                     .arg(prepare          ? "prepared"
                                  : cached ? "retained"
                                           : "cold")
                     .arg(direction),
             [direction, prepare, cached](Reader &reader) -> QString {
                 auto &model = *reader.model;
                 const auto position = model.position();
                 const auto image = model.image(model.page());
                 if (prepare)
                     model.prepareAhead();
                 if (model.position() != position || model.image(model.page()) != image)
                     return "error: boundary preparation changed the reading page";
                 if (!model.turnInWindow(direction))
                     return "error: prepared boundary turn failed";
                 if (cached && model.bookFraction() >= 0 && model.chapterLoadMilliseconds() != 0)
                     return QString("error: prepared boundary reloaded the chapter (%1 ms)")
                             .arg(model.chapterLoadMilliseconds());
                 return {};
             } };
}

Action groupedChapterBoundaries()
{
    return { "groupedChapterBoundaries()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                const bool fast = model.bookFraction() >= 0;
                if (fast)
                    model.setChapterSlices(25);
                const auto chapters = model.chapters();
                if (chapters.size() <= 2 || !model.goTo(chapters[2].position))
                    return "error: cannot open grouped chapter";
                QStringList frames;
                int crossings = 0;
                for (int step = 0; step < 40; ++step) {
                    model.prepareAhead();
                    const bool crossing =
                            fast && model.page() + 1 == model.pages() && model.continuesAfter();
                    for (int direction : { 1, -1, 1 }) {
                        if (!model.turnInWindow(direction))
                            return "error: grouped chapter turn stopped";
                        if (crossing && model.chapterLoadMilliseconds() != 0)
                            return "error: adjacent grouped chapter was reloaded";
                        const auto frame = shown(reader, {});
                        frames.append(frame.location + QLatin1Char(' ') + frame.checksum);
                    }
                    crossings += crossing;
                }
                if (fast && crossings == 0)
                    return "error: no grouped chapter boundary exercised";
                reader.lines = frames;
                return QString::fromLatin1(
                        QCryptographicHash::hash(frames.join(QLatin1Char('\n')).toUtf8(),
                                                 QCryptographicHash::Md5)
                                .toHex());
            } };
}

// Adjacent previews preserve the shown reading position at chapter and slice boundaries.
Action adjacentPagePreviews()
{
    return { "adjacentPagePreviews()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                if (model.bookFraction() >= 0)
                    model.setChapterSlices(2);
                const auto chapters = model.chapters();
                if (chapters.size() <= 5 || !model.goTo(chapters[4].position))
                    return "error: cannot open preview chapter";
                if (model.bookFraction() >= 0) {
                    const auto anchor = model.position();
                    int checks = 0;
                    const auto cancelled = model.previewTurn(-1, {}, [&] { return ++checks < 8; });
                    if (!cancelled.image.isNull() || checks < 8 || model.position() != anchor)
                        return "error: boundary preview ignored cancellation or moved the reader";
                }
                QStringList rows;
                bool complete = true;
                const auto record = [&](int step) {
                    const auto position = model.position();
                    const int page = model.page(), count = model.pages();
                    const auto shownImage = model.image(page);
                    const auto hits = model.searchPage("the", page).hits;
                    QVector<QPair<QString, QString>> ranges;
                    if (!hits.isEmpty())
                        ranges.append(qMakePair(hits.first().start, hits.first().end));
                    for (int direction : { -1, 0, 1 }) {
                        const auto preview = model.previewTurn(direction, ranges);
                        complete =
                                complete && !preview.image.isNull() && !preview.position.isEmpty();
                        if (!preview.position.isEmpty()) {
                            const auto marked =
                                    model.previewTurn(direction, ranges, {}, { preview.position });
                            complete = complete && marked.bookmarked;
                            if (direction) {
                                const auto unrelated =
                                        model.previewTurn(direction, ranges, {}, { position });
                                complete = complete && !unrelated.bookmarked;
                            }
                        }
                        if (model.position() != position || model.page() != page
                            || model.pages() != count || model.image(page) != shownImage)
                            complete = false;
                        if (!direction && !ranges.isEmpty()
                            && preview.ranges.value(0)
                                    != model.rangeRects(ranges[0].first, ranges[0].second))
                            complete = false;
                        const auto checksum =
                                QCryptographicHash::hash(
                                        QByteArray::fromRawData(reinterpret_cast<const char *>(
                                                                        preview.image.constBits()),
                                                                int(preview.image.sizeInBytes())),
                                        QCryptographicHash::Md5)
                                        .toHex();
                        rows.append(QString("%1/%2 %3 %4")
                                            .arg(step)
                                            .arg(direction)
                                            .arg(preview.position, QString::fromLatin1(checksum)));
                    }
                };
                record(0);
                for (int step = 1; step <= 8; ++step) {
                    if (!model.turnInWindow(1))
                        return "error: cannot reach another preview slice";
                    record(step);
                }
                if (!model.goTo(chapters[5].position) || !model.turnInWindow(-1))
                    return "error: cannot reach chapter-end preview";
                record(9);
                reader.lines = rows;
                return complete
                        ? QString::fromLatin1(
                                  QCryptographicHash::hash(rows.join(QChar(10)).toUtf8(),
                                                           QCryptographicHash::Md5)
                                          .toHex())
                        : QStringLiteral("error: missing preview or reading document changed");
            } };
}

// Exercise selection after several small slices, rather than only at a chapter start.
Action laterSlice()
{
    return { "laterSlice()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                if (model.bookFraction() >= 0)
                    model.setChapterSlices(2);
                const auto chapters = model.chapters();
                if (chapters.size() <= 4 || !model.goTo(chapters[4].position))
                    return "error: Cannot open the later-slice chapter";
                for (int i = 0; i < 8; ++i)
                    if (!model.turnInWindow(1))
                        return "error: Cannot reach the later slice";
                return {};
            } };
}

QString rectanglesText(const QVector<QRect> &rectangles)
{
    QStringList result;
    for (const auto &rectangle : rectangles)
        result.append(QString("%1,%2,%3,%4")
                              .arg(rectangle.x())
                              .arg(rectangle.y())
                              .arg(rectangle.width())
                              .arg(rectangle.height()));
    return result.join(';');
}

// A dictionary hold and an annotation both start from the word under a point.
Action selectWord()
{
    return { "wordAt(later slice)", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                WordSelection word;
                for (int y = 16; y < viewport.height() && word.text.isEmpty(); y += 8)
                    for (int x = 16; x < viewport.width() && word.text.isEmpty(); x += 8)
                        word = model.wordAt({ x, y });
                if (word.text.isEmpty() || word.start.isEmpty() || word.end.isEmpty()
                    || word.rectangles.isEmpty())
                    return "error: The later-slice word has no text, position or rectangles";
                if (model.textRange(word.start, word.end).text != word.text)
                    return "error: The dictionary word differs from its selected source text";
                const auto held = model.wordAt(word.rectangles.first().center());
                if (held.text != word.text || held.start != word.start || held.end != word.end)
                    return "error: A dictionary hold lost the selected word";
                reader.selectedWord = word;
                reader.selectedRects = model.rangeRects(word.start, word.end);
                if (reader.selectedRects.isEmpty())
                    return "error: The selected highlight has no geometry";
                return word.text + ' ' + locationText(model, word.start) + ' '
                        + locationText(model, word.end) + ' '
                        + rectanglesText(reader.selectedRects);
            } };
}

Action revisitHighlight()
{
    return { "highlightAfterReturn()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                const auto &word = reader.selectedWord;
                const auto chapters = model.chapters();
                if (chapters.size() <= 8 || !model.goTo(chapters[8].position))
                    return "error: Cannot leave the highlighted chapter";
                if (!model.rangeRects(word.start, word.end).isEmpty())
                    return "error: A highlight in another chapter is drawn on the current page";
                if (!model.goTo(word.start))
                    return "error: Cannot return to the saved highlight";
                const auto rectangles = model.rangeRects(word.start, word.end);
                if (rectangles != reader.selectedRects || rectangles.isEmpty())
                    return "error: The highlight geometry changed after leaving its chapter";
                return word.text + ' ' + rectanglesText(rectangles);
            } };
}

Action changeLayout(const QString &name, const QJsonObject &patch)
{
    return { name, [patch](Reader &reader) -> QString {
                auto &model = *reader.model;
                const auto before = model.image(model.page());
                if (!model.applyAppearance(appearance::Patch(patch)))
                    return "error: The layout setting could not be applied";
                const auto state = model.state();
                for (auto value = patch.constBegin(); value != patch.constEnd(); ++value)
                    if (state.value(value.key()) != value.value())
                        return "error: The layout setting was not retained";
                if (model.image(model.page()) == before)
                    return "error: The layout setting did not change the page";
                return {};
            } };
}

// Counts and chapter weights must cover text beyond the loaded Fast window.
Action wholeBookWords()
{
    return { "wholeBookWords()", [](Reader &reader) -> QString {
                const QVector<QPair<QByteArray, qint64>> fragments{
                    { "one<img/>two", 1 },
                    { "one<svg><text>hidden</text></svg>two", 1 },
                    { "one<br/>two", 2 },
                    { "one<hr/>two", 2 },
                    { "one<em>two</em>", 1 },
                    { "one<rt>hidden</rt>two", 1 },
                    { "one<script>hidden</script>two", 1 },
                    { "one <img/> two", 2 }
                };
                for (const auto &fragment : fragments) {
                    const auto xhtml = QByteArray("<html><body><p>") + fragment.first
                            + "</p><p>next</p></body></html>";
                    const auto item = measurePageEstimate(xhtml);
                    qint64 prefix = -1;
                    pageEstimateTextFraction(xhtml, QStringLiteral("/body/p[2]"), false, &prefix);
                    if (!item.parsed || item.words != fragment.second + 1
                        || prefix != fragment.second)
                        return "error: source item and chapter prefix disagree at "
                                + fragment.first;
                }
                auto &model = *reader.model;
                while (model.refinePageEstimate()) { }
                model.wordCounts();
                const auto graph = model.chapterGraph();
                if (graph.size() < 9 || model.totalWords() <= 0)
                    return "error: whole-book word counts unavailable";
                for (int i = 2; i < graph.size(); ++i)
                    if (graph[i].words <= 0)
                        return "error: future chapter has no words";
                const auto total = model.totalWords();
                auto contents = model.chapters();
                contents.first().title = "Retained words chapter";
                model.setChapters(contents, graph);
                model.prepareAhead();
                if (!model.turnInWindow(1) || model.totalWords() != total
                    || model.chapters().first().title != contents.first().title)
                    return "error: prepared-unit adoption lost source word metadata";
                if (!model.goTo(contents[8].position) || model.totalWords() != total
                    || model.chapters().first().title != contents.first().title)
                    return "error: chapter jump lost source word metadata";
                QStringList weights;
                for (const auto &chapter : graph)
                    weights.append(QString::number(chapter.words));
                return QString("whole-book chapter weights retained: %1 words; %2")
                        .arg(total)
                        .arg(weights.join(','));
            } };
}

// Changing engines keeps the page and source metadata, even after cancellation.
Action switchModes()
{
    return { "switchModes()", [](Reader &reader) -> QString {
                {
                    const auto files = test::linkBook();
                    auto loads = std::make_shared<QMap<QString, int>>();
                    QMap<QString, qint64> sizes;
                    for (auto it = files.cbegin(); it != files.cend(); ++it)
                        sizes[it.key()] = it.value().size();
                    auto resources = std::make_shared<ResourceCache>(
                            files.keys(),
                            [files, loads](const QString &name, const ResourceCache::Sink &sink,
                                           QString *) {
                                ++(*loads)[name];
                                const auto data = files.value(name);
                                return sink(data.constData(), data.size());
                            },
                            std::make_shared<std::atomic_bool>(false));
                    EngineDocument reload(test::readerFonts());
                    QString error;
                    if (!reload.openResources(resources, sizes, viewport, settings, &error))
                        return "error: Cannot open the reload fixture";
                    const auto position = reload.position();
                    const auto image = reload.image(reload.page());
                    const auto cacheLoads = reload.resourceStats().value("loads");
                    const bool fast = reload.bookFraction() >= 0;
                    const auto patch = appearance::Patch(reload.state());
                    if (reload.setChapterMode(!fast, patch, viewport, {}, [] { return false; })
                        || reload.position() != position || reload.image(reload.page()) != image
                        || reload.resourceStats().value("loads") != cacheLoads)
                        return "error: Cancelled reload changed the displayed source";
                    const int readsBeforeCancel = loads->value("mimetype");
                    if (reload.setChapterMode(!fast, patch, viewport, {},
                                              [loads, readsBeforeCancel] {
                                                  return loads->value("mimetype")
                                                          == readsBeforeCancel;
                                              })
                        || reload.position() != position || reload.image(reload.page()) != image
                        || reload.resourceStats().value("loads") != cacheLoads)
                        return "error: Interrupted resource reload changed the displayed source";
                    for (bool next : { !fast, fast }) {
                        const int before = loads->value("mimetype");
                        if (!reload.setChapterMode(next, patch, viewport))
                            return "error: Resource reload failed";
                        if (loads->value("mimetype") <= before || reload.openedFromCache())
                            return "error: Mode switch reused publication resources";
                        if (reload.position() != position || reload.image(reload.page()) != image)
                            return "error: Resource reload changed the displayed passage";
                    }
                }
                auto &model = *reader.model;
                const bool fast = model.bookFraction() >= 0;
                const auto position = model.position();
                const auto before = shown(reader, {});
                const auto patch = appearance::Patch(model.state());
                while (model.refinePageEstimate()) { }
                model.wordCounts();
                const auto words = model.totalWords();
                auto contents = model.chapters();
                auto graph = model.chapterGraph();
                if (contents.isEmpty() || graph.isEmpty() || words < 0)
                    return "error: Missing source metadata before switching";
                const auto publicationTitle = contents.first().title;
                contents.first().title = "Retained chapter title";
                graph.first().title = "Native graph sentinel";
                graph.first().words = 42;
                for (bool withContents : { true, false }) {
                    model.setChapters(withContents ? contents : QVector<ChapterLink>(), graph);
                    if (model.setChapterMode(!fast, patch, viewport, {}, [] { return false; }))
                        return "error: A cancelled mode change succeeded";
                    if (model.position() != position || !(shown(reader, {}) == before))
                        return "error: A cancelled mode change altered the displayed page";
                    for (bool next : { !fast, fast }) {
                        if (!model.setChapterMode(next, patch, viewport))
                            return "error: Mode switch failed";
                        if ((model.bookFraction() >= 0) != next || model.position() != position)
                            return "error: Mode switch changed the reading position";
                        if (!(shown(reader, {}) == before))
                            return "error: Mode switch changed the displayed page";
                        if (model.totalWords() != words || model.chapters().isEmpty()
                            || model.chapters().first().title
                                    != (withContents ? contents.first().title : publicationTitle)
                            || model.chapterGraph().size() != graph.size()
                            || model.chapterGraph().first().title != graph.first().title
                            || model.chapterGraph().first().words != graph.first().words)
                            return "error: Mode switch lost source metadata";
                        if (!reader.selectedWord.start.isEmpty()
                            && model.rangeRects(reader.selectedWord.start, reader.selectedWord.end)
                                    != reader.selectedRects)
                            return "error: Mode switch changed highlight geometry";
                    }
                }
                return {};
            } };
}

Action realBookEnd()
{
    return { "realBookEnd()", [](Reader &reader) -> QString {
                auto &model = *reader.model;
                const auto chapters = model.chapters();
                if (chapters.isEmpty() || !model.goTo(chapters.last().position))
                    return "error: Cannot open the last chapter";
                int turns = 0;
                while (model.turnInWindow(1))
                    if (++turns >= 200)
                        return "error: The last chapter did not reach the book end";
                while (model.refinePageEstimate()) { }
                if (model.page() != model.pages() - 1 || model.continuesAfter())
                    return "error: The engine stopped before the real last page";
                if (model.turnInWindow(1))
                    return "error: The book end still accepts a forward page turn";
                if (model.bookFraction() >= 0) {
                    const auto pages = model.roughPages();
                    if (model.bookFraction() != 1 || pages.page != pages.pages)
                        return "error: The last page does not report complete book progress";
                }
                return "last page; no next page; complete book progress";
            } };
}

// The scrubber of the reader window asks for a page of the engine's layout.
Action scrubTo(double fraction)
{
    return { QString("scrubTo(%1)").arg(fraction), [fraction](Reader &reader) -> QString {
                const int page =
                        qBound(0, int(reader.model->pages() * fraction), reader.model->pages() - 1);
                return reader.model->go(page) ? QString() : "error: go failed";
            } };
}

QVector<Shown> play(const Book &book, const QVector<Action> &script, bool fast, QString *openError)
{
    setChapterModeOverride(fast ? 1 : 0);
    Reader reader;
    reader.book = &book;
    QVector<Shown> rows;
    if (!reader.open(settings)) {
        *openError = "cannot open";
        return rows;
    }
    rows.append(shown(reader, {}));
    for (const auto &action : script)
        rows.append(shown(reader, action.run(reader)));
    setChapterModeOverride(-1);
    return rows;
}

int compare(const Book &book, const QVector<Action> &script)
{
    QString error;
    const auto accurate = play(book, script, false, &error);
    const auto fast = play(book, script, true, &error);
    require(error.isEmpty(), qPrintable(book.name + ": " + error));
    int differences = 0;
    std::printf("\n%s\n", qPrintable(book.name));
    for (int row = 0; row < accurate.size(); ++row) {
        const auto name = row ? script[row - 1].name : QString("open");
        const auto skipped = row ? script[row - 1].skipped : QString();
        if (!skipped.isEmpty()) {
            std::printf("  %-28s SKIPPED (%s)\n", qPrintable(name), qPrintable(skipped));
            continue;
        }
        const bool same = accurate[row] == fast[row] && !accurate[row].detail.startsWith("error:")
                && !fast[row].detail.startsWith("error:");
        differences += !same;
        std::printf("  %-28s %s\n", qPrintable(name), same ? "SAME" : "DIFFERENT");
        if (same)
            continue;
        for (const auto *side : { &accurate[row], &fast[row] }) {
            std::printf("    %-8s %s %s \"%s\"%s\n", side == &accurate[row] ? "accurate" : "fast",
                        qPrintable(side->location), qPrintable(side->checksum),
                        qPrintable(side->words),
                        side->detail.isEmpty() ? "" : qPrintable(" | " + side->detail));
        }
        // The first line of a detail that differs.
        const auto &one = accurate[row].lines, &two = fast[row].lines;
        for (int line = 0; line < qMax(one.size(), two.size()); ++line)
            if (one.value(line) != two.value(line)) {
                std::printf("    line %d:\n      accurate %s\n      fast     %s\n", line + 1,
                            qPrintable(one.value(line)), qPrintable(two.value(line)));
                break;
            }
    }
    return differences;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), qPrintable("Cannot read " + path));
    return file.readAll();
}

struct Scenario
{
    QString name;
    // "links" for test::linkBook(), "trevelyan" for the fixture, or a path.
    QString book;
    QVector<Action> actions;
};
} // namespace

#include "chapter_parity_scenarios.h"

namespace {
Book loadBook(const QString &name)
{
    if (name == QLatin1String("links"))
        return { name, {}, test::linkBook() };
    if (name == QLatin1String("bordered vertical text")
        || name == QLatin1String("shaded paragraphs") || name == QLatin1String("padded paragraphs")) {
        const bool vertical = name == QLatin1String("bordered vertical text");
        QByteArray body;
        for (int group = 0; group < 8; ++group) {
            body += vertical ? "<div style='border:1px solid #333;padding:12px 8px'>"
                             : name == QLatin1String("padded paragraphs") ? "<div style='padding:12px 8px'>"
                                                                          : "<div style='background:#eee'>";
            for (int paragraph = 0; paragraph < 3; ++paragraph)
                body += "<p>"
                        + QByteArray("吾輩は猫である。名前はまだ無い。")
                                  .repeated((vertical ? 18 : 12) + (group + paragraph) % 3)
                        + "</p>";
            body += "</div>";
        }
        auto chapter = test::publisherHtml(body);
        chapter.replace("</head>",
                        QByteArray("<style>body{font-family:Noto Serif CJK JP;writing-mode:")
                                + (vertical ? "vertical-rl" : "horizontal-tb")
                                + "}</style></head>");
        return { name,
                 {},
                 test::publisherPackage("ja", name.toUtf8(), { { "chapter.xhtml", chapter } },
                                        "<itemref idref='chapter.xhtml'/>") };
    }
    if (name == QLatin1String("trevelyan"))
        return { name,
                 readFile(QStringLiteral(KOBALT_TEST_FIXTURES)
                          + "/trevelyans-type-tester.kepub.epub"),
                 {} };
    return { name, readFile(name), {} };
}
} // namespace

// An argument runs only the scenarios whose name contains it.
// KOBALT_PARITY_BOOK replaces the fixture in the scenarios that use it.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
#if __has_include(<koblaze.h>)
    const QString only = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (only.isEmpty() || only == QLatin1String("non-linear")) {
        groupedNonLinearTurns();
        nonLinearTurns(true, 2);
        nonLinearTurns(true, 18);
        nonLinearTurns(false, 2);
    }
    if (only.isEmpty() || only == QLatin1String("unopened search"))
        searchUnopenedChapter();
    const auto other = qEnvironmentVariable("KOBALT_PARITY_BOOK");
    int differences = 0;
    for (const auto &scenario : scenarios()) {
        if (!only.isEmpty() && !scenario.name.contains(only))
            continue;
        auto book = loadBook(scenario.book == QLatin1String("trevelyan") && !other.isEmpty()
                                     ? other
                                     : scenario.book);
        book.name = scenario.name + " (" + book.name + ")";
        differences += compare(book, scenario.actions);
    }
    std::printf("\n%d actions differ\n", differences);
    require(differences == 0, "Chapter mode shows other pages than the whole book");
    std::printf("PASS: chapter mode matches the whole book\n");
#else
    std::printf("SKIP: stock CREngine lays out the whole book\n");
#endif
    QTimer::singleShot(0, &app, &QCoreApplication::quit);
    return app.exec();
}
