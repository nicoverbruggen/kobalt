#include "reader_inputs.h"
#include "engine/crengine/engine_document.h"
#include <QFile>
#include <QGuiApplication>
#include <QSet>
#include <cstdio>
#include "check.h"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 2, "Pass the synthetic dictionary EPUB");
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "Missing fixture");
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    QString error;
    require(model.open(file.readAll(), { 360, 700 }, {}, &error), qPrintable(error));
    const auto origin = model.position();
    auto search = [&](QString query) {
        QVector<kobalt::SearchHit> hits;
        QSet<QString> starts;
        for (int page = 0; page < model.pages(); ++page) {
            const auto batch = model.searchPage(query, page);
            for (const auto &hit : batch.hits) {
                require(!starts.contains(hit.start), "Duplicate boundary match");
                starts.insert(hit.start);
                require(!hit.start.isEmpty() && !hit.end.isEmpty() && hit.page == page,
                        "Invalid source anchor");
            }
            hits += batch.hits;
        }
        require(model.position() == origin, "Search changed the reading position");
        return hits;
    };
    for (const auto *word :
         { "rainbow", "cooperation", "extraordinary", "A𐐀word", "Reader's", "日本語", "中文" }) {
        const auto hits = search(QString::fromUtf8(word));
        if (hits.size() != 1) {
            fprintf(stderr, "%s: %d matches\n", word, int(hits.size()));
            return 1;
        }
    }
    require(search("CAFÉ").size() == 2, "Canonical or case-insensitive match failed");
    require(search("Hello").size() == 2, "Chapter search failed");
    require(search("More fixture text").size() == 80, "Page boundary match missing");
    require(search("zzzzmissing").isEmpty(), "False match");
    // Context keeps a space only where the book has one, so an excerpt can
    // join punctuation and inline markup to the match without a gap.
    const auto world = search("world");
    require(world.size() == 1, "Plain paragraph match missing");
    require(world.first().before.endsWith(", ") && world.first().after.startsWith("! "),
            "Search context lost its edge spacing");
    const auto part = search("bow");
    require(!part.isEmpty(), "Match inside a word missing");
    require(part.first().before.endsWith("rain") && part.first().after.startsWith(" and"),
            "Search context added a space inside a word");
    const auto hits = search("Chapter boundary");
    require(hits.size() == 1, "Last chapter missing");
    require(model.goTo(hits.first().start), "Search result cannot be opened");
    require(!model.rangeRects(hits.first().start, hits.first().end).isEmpty(),
            "Search match geometry missing");
    puts("PASS: Unicode, inline markup, soft hyphens, page and chapter boundaries, exact navigation and unchanged reading position");
}
