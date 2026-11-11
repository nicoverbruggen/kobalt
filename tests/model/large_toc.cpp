#include "engine/crengine/engine_document.h"
#include "platform/api/position_codec.h"
#include "book_fixtures.h"
#include "check.h"
#include "reader_inputs.h"
#include "support/qt_compat.h"
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QTemporaryDir>

using namespace kobalt;

// Checks total TOC boundaries, hierarchy, exact titles and layout/cache refresh.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    for (int count : { 1025, 1023, 1024, 9999, 10000, 10001 }) {
        QTemporaryDir directory;
        require(directory.isValid(), "Cannot create songbook directory");
        require(test::writeBook(directory.path(), "songs.epub", test::songbook(count)),
                "Cannot write songbook");
        QFile file(directory.filePath("songs.epub"));
        require(file.open(QIODevice::ReadOnly), "Cannot read songbook");
        const auto bytes = file.readAll();
        const auto key = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        EngineDocument model(test::readerFonts());
        model.setDocumentCache(directory.filePath("cache"), key, "large-toc", 128 * 1024 * 1024);
        QString error;
        require(model.open(bytes, QSize(600, 800), {}, &error), qPrintable(error));
        const auto links = model.chapters();
        const int expected = qMin(count, 10000);
        require(links.size() == expected,
                qPrintable(QString("TOC count %1: got %2, expected %3")
                                   .arg(count)
                                   .arg(links.size())
                                   .arg(expected)));
        for (int i = 0; i < expected; ++i) {
            require(links[i].title == QString("Song %1").arg(i + 1)
                            && links[i].depth == (i % 10 == 0 ? 0 : 1) && links[i].page >= 0
                            && !links[i].position.isEmpty(),
                    "TOC lost a title, destination, hierarchy or stable order");
        }
        for (int i : { 0, 1022, qMin(1024, expected - 1), expected / 2, expected - 1 }) {
            require(model.goTo(links[i].position), "Cannot jump to song");
            require(model.chapterTitle(links[i].position) == links[i].title,
                    "Wrong title at an exact song destination");
            require(model.chapterSelection(model.chapters()).current == i, "Wrong selected song");
            require(model.chapterProgress().title == links[i].title,
                    "Wrong current title, including same-page song starts");
        }
        const auto shared = model.chapters();
        require(shared.constData() == model.chapters().constData(),
                "Repeated chapter reads rebuilt the index");
        require(model.goTo(links[qMin(1024, expected - 1)].position), "Cannot select later song");
        const int page = model.page();
        require(model.go(model.turnTarget(page, -1)) && model.go(model.turnTarget(model.page(), 1)),
                "Forward/backward page navigation failed");
        require(model.chapterProgress().title == model.chapterTitle(model.position()),
                "Page navigation disagrees with source title lookup");
        require(model.goTo(links.last().position), "Cannot return to last indexed song");
        if (count > expected) {
            QString overflow;
            for (const auto &link : model.pageLinks()) {
                const auto details = model.linkDetails(link.source, false);
                if (!details.target.isEmpty() && model.comparePosition(details.target, &error) > 0)
                    overflow = details.target;
            }
            require(!overflow.isEmpty() && model.goTo(overflow),
                    "The unindexed song is no longer reachable by a page link");
            require(model.chapterTitle(overflow) == links.last().title,
                    "Truncated content should retain the last indexed title");
            require(model.goTo(links.last().position), "Cannot return from unindexed content");
        }
        bool followed = false;
        for (const auto &link : model.pageLinks()) {
            const auto details = model.linkDetails(link.source, false);
            if (details.kind != LinkKind::Passage || details.target.isEmpty())
                continue;
            require(model.goTo(details.target), "Cannot follow song link");
            require(model.chapterProgress().title == model.chapterTitle(model.position()),
                    "Page link disagrees with source title lookup");
            followed = true;
            break;
        }
        require(followed, "No reachable song link on the last indexed page");
        auto native = links;
        native[expected - 1].title = "Native last song";
        model.setChapters(native, native);
        require(model.chapterTitle(links.last().position) == "Native last song",
                "Native chapter titles stopped at the former limit");
        native.append({ "Overflow", links.last().position });
        model.setChapters(native, native);
        require(model.chapters().size() == qMin(expected + 1, 10000),
                "Native TOC did not apply the total bound");
        QVector<ChapterTarget> targets;
        for (const auto &link : native)
            targets.append({ link.title, link.position, link.depth, link.words });
        const PositionCodec identity{
            [](const EngineDocument &, const QString &position, QString *) { return position; },
            [](const EngineDocument &document, QString *) { return document.position(); },
            [](const QString &position) { return position; }
        };
        const auto resolved = resolveChapterTargets(model, identity, targets, targets);
        require(resolved.contents.size() == qMin(expected + 1, 10000)
                        && resolved.graph.size() == resolved.contents.size(),
                "Native target resolution did not apply the total bound");
        model.setChapters({ { "Unresolved", {} } }, {});
        require(model.chapters().size() == expected
                        && model.chapterTitle(links.last().position) == links.last().title,
                "Unresolved native TOC did not restore the engine fallback");
        QVector<ChapterLink> samePage(10000, { "Shared", {}, 0 });
        const auto lengths = chapterPageLengths(samePage, 10);
        require(lengths.size() == 10000 && lengths.first().words == 10
                        && lengths.last().words == 10,
                "Large shared-page graph changed its page lengths");
        const auto unsorted = chapterPageLengths(
                { { "a", {}, 8 }, { "b", {}, 2 }, { "c", {}, 5 }, { "d", {}, 10 } }, 12);
        require(unsorted[0].words == 2 && unsorted[1].words == 3 && unsorted[2].words == 5,
                "Unsorted TOC graph lost the nearest later page");
        const auto counts = model.wordCounts();
        require(counts.total > 0 && counts.chapters.size() == expected,
                "Word counts lost indexed songs");
        require(model.chapterGraph().size() == expected, "Graph lost songs");
        require(model.wordCounts().cached, "Large TOC word counts did not fit their cache");
        require(model.applyAppearance(appearance::Patch(qt::object({ { "font_pixels", 32 } })),
                                      QSize(700, 900)),
                "Cannot reflow songbook");
        require(model.chapters().size() == expected
                        && model.chapterTitle(links.last().position) == links.last().title,
                "Reflow lost the last indexed title");
        require(model.wordCounts().cached, "Reflow discarded source word counts");
        require(model.saveDocumentCache(), "Cannot cache songbook");
        EngineDocument reopened(test::readerFonts());
        reopened.setDocumentCache(directory.filePath("cache"), key, "large-toc", 128 * 1024 * 1024);
        require(reopened.open(bytes, QSize(700, 900), model.state(), &error), qPrintable(error));
        require(reopened.openedFromCache() && reopened.chapters().size() == expected
                        && reopened.chapterTitle(links.last().position) == links.last().title,
                "Cached reopening lost the last indexed title");
        require(reopened.wordCounts().cached, "Cached reopening recounted the songbook");
        qInfo("PASS: %d song destinations, %d indexed entries", count, expected);
    }
}
