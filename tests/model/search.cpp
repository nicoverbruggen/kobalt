#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
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
    auto search = [&](QString query) {
        const auto origin = model.position();
        QVector<kobalt::SearchHit> hits;
        QSet<QString> starts;
        bool truncated = false;
        for (int page = 0; page < model.pages(); ++page) {
            const auto batch = model.searchPage(query, page);
            truncated |= batch.limited;
            for (const auto &hit : batch.hits) {
                require(!starts.contains(hit.start), "Duplicate boundary match");
                starts.insert(hit.start);
                require(!hit.start.isEmpty() && !hit.end.isEmpty() && hit.page == page,
                        "Invalid source anchor");
            }
            hits += batch.hits;
        }
        QVector<kobalt::SearchHit> prepared;
        kobalt::SearchCursor cursor;
        for (int turns = 0;; ++turns) {
            require(turns < 10000, "Prepared search did not finish");
            const auto batch =
                    model.searchText(query, cursor, 7, kobalt::SearchScope::Book, {}, false);
            cursor = batch.cursor;
            for (const auto &hit : batch.hits)
                require(hit.page == -1 && hit.before.isEmpty() && hit.text.isEmpty()
                                && hit.after.isEmpty(),
                        "Scan eagerly resolved result details");
            prepared += model.searchDetails(batch.hits, kobalt::SearchScope::Book, {});
            if (batch.done)
                break;
        }
        require(truncated ? prepared.size() >= hits.size() : prepared.size() == hits.size(),
                "Prepared search changed the match count");
        for (int i = 0; !truncated && i < hits.size(); ++i) {
            require(prepared[i].start == hits[i].start && prepared[i].end == hits[i].end
                            && prepared[i].page == hits[i].page,
                    "Prepared search changed source anchors");
            require(prepared[i].before == hits[i].before && prepared[i].after == hits[i].after,
                    "Prepared search changed excerpts");
        }
        require(model.position() == origin, "Search changed the reading position");
        return prepared;
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
    require(search("Densemarker").size() == 600, "Dense search lost matches beyond 500");
    require(search("ffi").size() == 1, "Ligature expansion failed");
    require(search(QString::fromUtf8("STRAßE")).size() == 1, "Case-insensitive matching failed");
    require(search("e-mail").size() == 1, "Hyphen folding failed");
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
    // Scope boundaries partition the book and keep excerpt context on the allowed side.
    const QString middle = search("More fixture text")[40].start;
    QString scopedQuery = "More fixture text", scopedOrigin = middle;
    const auto scoped = [&](kobalt::SearchScope scope) {
        QVector<kobalt::SearchHit> result;
        kobalt::SearchCursor cursor;
        for (int turns = 0;; ++turns) {
            require(turns < 10000, "Scoped search did not finish");
            auto batch = model.searchText(scopedQuery, cursor, 3, scope, scopedOrigin, false);
            result += model.searchDetails(batch.hits, scope, scopedOrigin);
            cursor = batch.cursor;
            if (batch.done)
                return result;
        }
    };
    const auto before = scoped(kobalt::SearchScope::Before);
    const auto after = scoped(kobalt::SearchScope::After);
    // A phrase straddling the page boundary belongs to neither scope: showing
    // the complete phrase in Before would reveal unread text.
    const auto verifyPartition = [&](const QVector<kobalt::SearchHit> &all,
                                     const QVector<kobalt::SearchHit> &before,
                                     const QVector<kobalt::SearchHit> &after,
                                     const QString &origin) {
        require(model.goTo(origin) && model.go(model.page() + 1), "Cannot open scope boundary");
        QVector<QString> expectedBefore, expectedAfter, actualBefore, actualAfter;
        for (const auto &hit : all) {
            if (model.comparePosition(hit.end, nullptr) <= 0)
                expectedBefore.append(hit.start);
            if (model.comparePosition(hit.start, nullptr) >= 0)
                expectedAfter.append(hit.start);
        }
        for (const auto &hit : before)
            actualBefore.append(hit.start);
        for (const auto &hit : after)
            actualAfter.append(hit.start);
        require(actualBefore == expectedBefore && actualAfter == expectedAfter,
                "Scoped search crossed or lost a source boundary");
    };
    verifyPartition(search(scopedQuery), before, after, scopedOrigin);
    require(!before.isEmpty() && !after.isEmpty() && before.last().page < after.first().page,
            "After scope included the displayed page");
    require(!scoped(kobalt::SearchScope::Chapter).isEmpty(), "Current chapter was empty");
    scopedQuery = "Densemarker";
    scopedOrigin = search(scopedQuery)[300].start;
    const auto denseBefore = scoped(kobalt::SearchScope::Before);
    const auto denseAfter = scoped(kobalt::SearchScope::After);
    verifyPartition(search(scopedQuery), denseBefore, denseAfter, scopedOrigin);
    require(model.go(denseBefore.last().page + 1), "Cannot open scope boundary");
    const auto boundary = model.position();
    const auto allowedAfter = model.textRange(denseBefore.last().end, boundary).text.simplified();
    const auto allowedBefore =
            model.textRange(boundary, denseAfter.first().start).text.simplified();
    require(allowedAfter.startsWith(denseBefore.last().after.trimmed()),
            "Before-scope excerpt exposed the next page");
    require(allowedBefore.endsWith(denseAfter.first().before.trimmed()),
            "After-scope excerpt crossed its boundary");
    model.resize({ 420, 700 });
    require(search("ffi").size() == 1 && search("Densemarker").size() == 600,
            "Reflow reused stale prepared text positions");
    puts("PASS: Unicode, inline markup, soft hyphens, page and chapter boundaries, exact navigation and unchanged reading position");
}
