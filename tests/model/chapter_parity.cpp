#include "book_fixtures.h"
#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
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

// One reader in one mode, with what the actions remember.
struct Reader
{
    const Book *book = nullptr;
    std::unique_ptr<EngineDocument> model;
    QStringList history;
    // Lines that explain a detail, printed for a difference.
    QStringList lines;
    QString bookmark, highlight;
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

Action followLink(const QString &label)
{
    return { QString("followLink(%1)").arg(label), [label](Reader &reader) -> QString {
                auto &model = *reader.model;
                for (const auto &link : model.pageLinks()) {
                    if (model.linkDetails(link.source).label != label)
                        continue;
                    // A tap asks for the details with a preview, as here.
                    const auto details = model.linkDetails(link.source);
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
        const bool same = accurate[row] == fast[row];
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
    return 0;
}
