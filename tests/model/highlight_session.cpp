#include "check.h"
#include "annotation_session_checks.h"
#include "storage/profile_database.h"
#include "app/messages/opening_message.h"
#include "plain_archive.h"
#include "publication/native_resources.h"
#include "publication/resource_server.h"
#include "reader_inputs.h"
#include "reading/bookmark_session.h"
#include "reading/highlight_session.h"
#include "reading/reader_session.h"
#include "support/qt_compat.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>

using namespace kobalt;

namespace {
Highlight detail(HighlightSession &session, const QString &id)
{
    Highlight result;
    require(session.detail(id, &result), "Cannot load selected highlight details");
    return result;
}

bool samePassage(const Highlight &a, const Highlight &b)
{
    // Both stores persist UTC ISO dates at second precision. Completing a
    // queued save can replace in-memory details with that stored timestamp.
    return a.id == b.id
            && a.created.toUTC().toString(Qt::ISODate) == b.created.toUTC().toString(Qt::ISODate)
            && a.start == b.start && a.end == b.end && a.text == b.text && a.chapter == b.chapter
            && a.note == b.note && a.chosenStart == b.chosenStart && a.chosenEnd == b.chosenEnd
            && a.sentenceStartPolicy == b.sentenceStartPolicy
            && a.sentenceEndPolicy == b.sentenceEndPolicy;
}

QVector<Highlight> passages(HighlightSession &session)
{
    QVector<Highlight> result;
    for (const auto &row : session.highlights())
        result.append(detail(session, row.id));
    return result;
}

void waitUntil(const std::function<bool()> &ready)
{
    if (ready()) {
        return;
    }
    QEventLoop loop;
    QTimer poll, timeout;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready()) {
            loop.quit();
        }
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop,
                     [] { require(false, "HighlightSession check timed out"); });
    poll.start(1);
    timeout.start(60000);
    loop.exec();
}

void drain()
{
    bool done = false;
    ReaderSession::drain([&] { done = true; });
    waitUntil([&] { return done; });
}

} // namespace

// Checks highlight editing, overlap, notes, reflow, reopening and export order on the JSON and SQLite backends.
int main(int argc, char **argv)
{
    const bool sqlite = argc == 3 && QByteArray(argv[2]) == "--sqlite";
    qputenv("KOBALT_ANNOTATIONS_SQLITE", sqlite ? "1" : "0");
    QGuiApplication app(argc, argv);
    require(argc == 2 || sqlite, "Expected a synthetic EPUB and optional SQLite variant");
    const auto path = QString::fromLocal8Bit(argv[1]);
    native::ResourceServer server;
    require(server.open(path, [path] { return native::openPlainArchive(path); }),
            "Cannot open synthetic publication");
    QTemporaryDir annotations;
    require(annotations.isValid(), "Cannot create annotation directory");
    // Run inside the event loop, so quitting stops the engine worker as the
    // application does.
    qt::later(0, &app, [&] {
        auto owned =
                std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        auto &model = *owned;
        auto lease = std::make_shared<SourceLease>();
        bool ready = false;
        model.open({ "file:///highlight-session.epub",
                     path,
                     QSize(758, 900),
                     {},
                     {},
                     {},
                     lease,
                     nativeResourceFactory(path, server.port(), lease) },
                   [&](const OpeningResult &result) {
                       require(result.ready(), qPrintable(openingMessage(result)));
                       ready = true;
                   });
        waitUntil([&] { return ready && !model.busy(); });
        auto layout = [&] {
            return bookmarkLayoutKey(model.pages(), model.state(), QSize(758, 900));
        };
        HighlightSession session(model, layout, annotations.path(),
                                 "file:///highlight-session.epub", model.sourceHash(), "Book");
        require(session.writable(), "New highlight session is not writable");
        bool searched = false;
        SearchHit first, last;
        model.search("the", [&](SearchBatch batch) {
            if (!batch.hits.isEmpty()) {
                if (first.start.isEmpty()) {
                    first = batch.hits.first();
                }
                last = batch.hits.last();
            }
            searched = batch.done;
        });
        waitUntil([&] { return searched; });
        require(!first.start.isEmpty(), "No source text fixture for highlight");
        model.cancelSearch();
        if (sqlite) {
            QTemporaryDir damagedRoot;
            const QString identity = "sqlite-damaged-highlight";
            HighlightStore seed(damagedRoot.path(), identity, model.sourceHash(), "Book");
            require(seed.load(), "Cannot initialize malformed SQLite highlight fixture");
            Highlight original{ "damaged",  first.start, first.end,
                                first.text, "Chapter",   QDateTime::currentDateTimeUtc() };
            original.note = QString::fromUtf8("kept \xc3\xa9") + QChar(0) + " note";
            seed.setHighlights({ original });
            require(seed.save(), "Cannot seed malformed SQLite highlight fixture");
            require(storage::profileTransaction(
                            damagedRoot.path(),
                            [](QSqlDatabase &db) {
                                QSqlQuery query(db);
                                return query.exec("UPDATE entries SET start='' WHERE kind=1");
                            }),
                    "Cannot damage SQLite highlight fixture");
            {
                HighlightSession damaged(model, layout, damagedRoot.path(), identity,
                                         model.sourceHash(), "Book");
                require(!damaged.writable() && !damaged.error().isEmpty()
                                && !damaged.remove(original.id),
                        "Malformed SQLite highlight accepted an edit");
                require(storage::profileTransaction(
                                damagedRoot.path(),
                                [](QSqlDatabase &db) {
                                    QSqlQuery query(db);
                                    return query.exec("SELECT start FROM entries WHERE kind=1")
                                            && query.next() && query.value(0).toString().isEmpty();
                                }),
                        "Failed load removed the damaged SQLite highlight");
            }
            require(storage::profileTransaction(
                            damagedRoot.path(),
                            [&](QSqlDatabase &db) {
                                QSqlQuery query(db);
                                query.prepare("UPDATE entries SET start=? WHERE kind=1");
                                query.addBindValue(original.start);
                                return query.exec();
                            }),
                    "Cannot repair SQLite highlight fixture");
            HighlightSession repaired(model, layout, damagedRoot.path(), identity,
                                      model.sourceHash(), "Book");
            require(repaired.writable() && repaired.highlights().size() == 1
                            && detail(repaired, original.id).text == original.text
                            && detail(repaired, original.id).note == original.note,
                    "Reopened SQLite session did not recover the repaired highlight");
        }
        for (bool appearanceFailure : { false, true }) {
            const auto revision = model.layoutRevision();
            const auto page = model.page();
            if (appearanceFailure) {
                auto invalid = model.state();
                invalid["font_pixels"] = -1;
                model.apply(invalid);
            } else {
                require(model.goTo("/body[999999]/text().0"), "Cannot queue missing passage jump");
            }
            waitUntil([&] { return !model.busy(); });
            require(!model.error().isEmpty() && !model.image().isNull() && model.pages() > 0
                            && model.page() == page,
                    "Failed command did not retain the usable page fixture");
            require(appearanceFailure ? model.layoutRevision() != revision
                                      : model.layoutRevision() == revision,
                    "Failed command changed the wrong layout generation");
            for (int attempt = 0; attempt < 2; ++attempt) {
                bool located = false;
                model.bookmarkLocations(
                        { first.start, first.end },
                        [&](BookmarkLocations result) {
                            require(result.pages.size() == 2 && result.pages.first() >= 0,
                                    "Failed reading command disabled later endpoint queries on a valid frame");
                            located = true;
                        },
                        true);
                waitUntil([&] { return located; });
            }
            QTemporaryDir recoveredRoot;
            require(recoveredRoot.isValid(), "Cannot create failed-command highlight fixture");
            HighlightStore seeded(recoveredRoot.path(), "failed-command", model.sourceHash(),
                                  "Book");
            require(seeded.load(), "Cannot load failed-command highlight fixture");
            seeded.setHighlights({ { "recoverable",
                                     first.start,
                                     first.end,
                                     first.text,
                                     {},
                                     QDateTime::currentDateTimeUtc() } });
            require(seeded.save(), "Cannot seed failed-command highlight fixture");
            HighlightSession recovered(model, layout, recoveredRoot.path(), "failed-command",
                                       model.sourceHash(), "Book");
            int replies = 0;
            recovered.changed = [&] {
                ++replies;
                recovered.sync();
            };
            recovered.sync();
            drain();
            require(recovered.resolved() && replies == 1,
                    "Failed command left highlight resolution retrying an unusable readiness flag");
        }
        {
            bool replied = false;
            const auto revision = model.layoutRevision();
            const auto cancelled = std::make_shared<std::atomic_bool>(false);
            model.bookmarkLocations(
                    { first.start, first.end },
                    [&](BookmarkLocations result) {
                        require(result.pages.isEmpty() && result.order.isEmpty(),
                                "Explicitly cancelled endpoint lookup returned locations");
                        replied = true;
                    },
                    true, true, cancelled);
            cancelled->store(true);
            waitUntil([&] { return replied; });
            require(model.layoutRevision() == revision,
                    "Request cancellation invalidated the source layout");
        }
        {
            bool located = false, summarized = false;
            const auto revision = model.layoutRevision();
            const int summaryPage = model.page();
            model.bookmarkLocations(
                    { first.start, first.end },
                    [&](BookmarkLocations result) {
                        require(result.pages.size() == 2 && result.order.size() == 2
                                        && result.pages[0] >= 0,
                                "A page turn discarded a valid endpoint lookup");
                        located = true;
                    },
                    true);
            model.pageSummary(summaryPage, [&](QString start, QString excerpt) {
                require(!start.isEmpty() && !excerpt.isEmpty(),
                        "A page turn discarded a valid page summary");
                summarized = true;
            });
            require(model.go(model.page() == 0 ? 1 : 0), "Cannot turn during endpoint lookup");
            require(model.layoutRevision() == revision,
                    "Navigation invalidated the source layout generation");
            waitUntil([&] { return located && summarized && !model.busy(); });
        }
        require(model.goTo(first.start), "Cannot show grouped geometry fixture");
        waitUntil([&] { return !model.busy(); });
        bool geometryReady = false;
        model.rangeRectGroups(
                { { first.start, first.end }, { "bad", "bad" }, { first.start, first.end } },
                [&](QVector<QVector<QRect>> groups) {
                    require(groups.size() == 3 && !groups[0].isEmpty() && groups[1].isEmpty()
                                    && groups[0] == groups[2],
                            "Grouped geometry lost range order or clipping");
                    geometryReady = true;
                });
        waitUntil([&] { return geometryReady; });
        {
            const auto identity = QStringLiteral("future-color-book");
            HighlightStore futureStore(annotations.path(), identity, model.sourceHash(), "Future");
            require(futureStore.load(), "Cannot initialize future-color fixture");
            Highlight future{ "future",   first.start, first.end,
                              first.text, {},          QDateTime::currentDateTimeUtc() };
            future.color = "orange";
            future.style = "future";
            future.note = QString::fromUtf8("A future note\nwith café and 日本語");
            futureStore.setHighlights({ future });
            require(futureStore.save(), "Cannot store future color");
            HighlightSession futureSession(model, layout, annotations.path(), identity,
                                           model.sourceHash(), "Future");
            bool futureDone = false;
            futureSession.save(first.start, last.end, future.id, [&](bool ok) {
                require(ok, "Cannot edit future-color passage");
                futureDone = true;
            });
            waitUntil([&] { return futureDone && !futureSession.saving(); });
            require(futureSession.highlights().first().color == "orange"
                            && highlightDisplayColor(futureSession.highlights().first().color)
                                    == "gray"
                            && futureStore.load()
                            && futureStore.highlights().first().color == "orange"
                            && futureSession.highlights().first().style == "future",
                    "Omitted edit color destroyed unknown stored key");
            require(detail(futureSession, future.id).note == future.note
                            && futureStore.highlights().first().note == future.note,
                    "Editing a passage lost its attached note");
            require(futureSession.recolor(future.id, "gray")
                            && futureSession.highlights().first().color == "gray",
                    "Displayed gray future color could not explicitly become gray");
            waitUntil([&] { return !futureSession.saving(); });
            require(futureStore.load() && futureStore.highlights().first().color == "gray",
                    "An explicit gray color was not saved");
            require(futureSession.restyle(future.id, "dotted"),
                    "Cannot restyle future-color passage");
            waitUntil([&] { return !futureSession.saving(); });
            require(futureStore.load() && futureStore.highlights().first().note == future.note,
                    "Changing annotation appearance lost its note");
        }
        bool completed = false, saved = false;
        session.save(first.start, last.end, {}, [&](bool ok) {
            saved = ok;
            completed = true;
        });
        waitUntil([&] { return completed; });
        require(saved && session.highlights().size() == 1, "Passage save failed");
        completed = false;
        session.save(
                first.start, last.end, session.highlights().first().id,
                [&](bool ok) {
                    saved = ok;
                    completed = true;
                },
                last.end, true, first.start, true);
        waitUntil([&] { return completed; });
        require(saved, "Cannot save automatic endpoint provenance");
        const auto beforeColor = detail(session, session.highlights().first().id);
        require(beforeColor.color == "gray", "New omitted color was not gray");
        require(session.recolor(beforeColor.id, "blue")
                        && session.highlights().first().color == "blue"
                        && samePassage(beforeColor, detail(session, beforeColor.id)),
                "Recolor changed passage metadata or failed");
        require(!session.recolor(beforeColor.id, "orange")
                        && session.highlights().first().color == "blue",
                "Unsupported explicit color was accepted");
        require(session.restyle(beforeColor.id, "underline")
                        && session.highlights().first().style == "underline"
                        && samePassage(beforeColor, detail(session, beforeColor.id))
                        && session.highlights().first().color == "blue",
                "Restyle changed passage metadata or color");
        require(!session.restyle(beforeColor.id, "future")
                        && session.highlights().first().style == "underline",
                "Unsupported explicit style was accepted");
        const auto passage = session.highlights().first();
        completed = false;
        session.save(first.start, last.end, {}, [&](bool ok) {
            saved = ok;
            completed = true;
        });
        waitUntil([&] { return completed; });
        require(!saved && session.highlights().size() == 1 && session.error() == "overlap",
                "Overlapping create changed saved highlights");
        completed = false;
        session.save(first.start, first.end, passage.id, [&](bool ok) {
            saved = ok;
            completed = true;
        });
        waitUntil([&] { return completed; });
        require(saved, "Cannot shorten first highlight");
        completed = false;
        session.save(last.start, last.end, {}, [&](bool ok) {
            saved = ok;
            completed = true;
        });
        waitUntil([&] { return completed; });
        require(saved && session.highlights().size() == 2, "Disjoint highlights were rejected");
        completed = false;
        session.save(first.end, last.start, {}, [&](bool ok) {
            saved = ok;
            completed = true;
        });
        waitUntil([&] { return completed; });
        require(saved && session.highlights().size() == 3,
                "Touching endpoints were treated as an overlap");
        require(session.remove(session.highlights().last().id),
                "Cannot remove adjacent-range fixture");
        const auto beforeOverlap = passages(session);
        for (const bool finishWrites : { false, true }) {
            if (finishWrites) {
                // Exercise a later UI interaction after the queued write and
                // its detail-cache update have completed, without a sleep.
                session.flush();
                QCoreApplication::processEvents();
            }
            completed = false;
            session.save(first.start, last.end, passage.id, [&](bool ok) {
                saved = ok;
                completed = true;
            });
            waitUntil([&] { return completed; });
            require(!saved && samePassage(beforeOverlap[0], detail(session, beforeOverlap[0].id))
                            && samePassage(beforeOverlap[1], detail(session, beforeOverlap[1].id)),
                    "Overlapping edit changed a saved passage");
        }
        bool reflowDiscarded = false;
        const auto beforeReflow = model.layoutRevision();
        model.bookmarkLocations(
                { first.start, first.end },
                [&](BookmarkLocations result) {
                    require(result.pages.isEmpty() && result.order.isEmpty(),
                            "A reflow accepted old endpoint locations");
                    reflowDiscarded = true;
                },
                true);
        require(model.style(model.fontSize() == 64 ? 63 : 64, 120),
                "Cannot reflow highlight fixture");
        require(model.layoutRevision() != beforeReflow, "Reflow kept the prior layout generation");
        bool pendingLayoutDiscarded = false;
        model.bookmarkLocations({ first.start }, [&](BookmarkLocations result) {
            require(result.pages.isEmpty(),
                    "A lookup started during pending reflow accepted old layout pages");
            pendingLayoutDiscarded = true;
        });
        waitUntil([&] { return !model.busy() && reflowDiscarded && pendingLayoutDiscarded; });
        waitUntil([&] {
            session.sync();
            return session.resolved();
        });
        require(session.page(0) >= 0 && session.endPage(0) >= session.page(0)
                        && session.highlights().size() == 2,
                "Reflow lost or merged highlights");
        const auto beforeBeautifier = passages(session);
        for (bool enabled : { true, false }) {
            require(model.setBookBeautifier(enabled),
                    "Cannot toggle beautifier with saved highlights");
            waitUntil([&] { return !model.busy(); });
            waitUntil([&] {
                session.sync();
                return session.resolved();
            });
            const auto after = passages(session);
            require(after.size() == beforeBeautifier.size(), "Beautifier lost highlights");
            for (int i = 0; i < after.size(); ++i)
                require(samePassage(after[i], beforeBeautifier[i]) && session.page(i) >= 0,
                        "Beautifier changed a highlight or lost its source location");
        }
        const auto cachedLocations = session.locations();
        require(session.recolor(passage.id, "pink") && session.resolved()
                        && session.locations().pages == cachedLocations.pages
                        && session.locations().order == cachedLocations.order,
                "Recolor discarded unchanged endpoint locations");
        require(session.restyle(passage.id, "dotted") && session.resolved()
                        && session.locations().pages == cachedLocations.pages
                        && session.locations().order == cachedLocations.order,
                "Restyle discarded unchanged endpoint locations");
        require(session.updateAppearance(passage.id, passage.color, passage.style),
                "Cannot restore passage appearance");
        int recolorChanges = 0;
        session.changed = [&] { ++recolorChanges; };
        require(session.recolor(passage.id, passage.color) && recolorChanges == 0
                        && session.resolved(),
                "Same-color recolor invalidated geometry or notified a change");
        session.changed = {};
        waitUntil([&] { return !session.saving(); });
        {
            QTemporaryDir pendingRoot;
            require(pendingRoot.isValid(), "Cannot create pending highlight fixture");
            HighlightStore seeded(pendingRoot.path(), "file:///highlight-session.epub",
                                  model.sourceHash(), "Book");
            require(seeded.load(), "Cannot initialize pending highlight fixture");
            seeded.setHighlights(passages(session));
            require(seeded.save(), "Cannot seed pending highlight fixture");
            HighlightSession pending(model, layout, pendingRoot.path(),
                                     "file:///highlight-session.epub", model.sourceHash(), "Book");
            int replies = 0;
            pending.changed = [&] { ++replies; };
            require(!pending.sync() && !pending.sync(),
                    "Pending endpoints unexpectedly resolved synchronously");
            pending.cancelLookup();
            drain();
            require(!pending.resolved() && replies == 0,
                    "Abandoned lookup installed locations or retried itself");
            require(!pending.sync(), "Cancelled endpoint lookup could not restart");
            require(pending.recolor(passage.id, "pink"),
                    "Cannot recolor during an endpoint lookup");
            const int edits = replies;
            require(model.go(model.page() == 0 ? 1 : 0), "Cannot turn during highlight resolution");
            waitUntil([&] { return !model.busy(); });
            drain();
            require(pending.resolved() && replies == edits + 1,
                    "Navigation or recolor discarded an endpoint batch or queued a duplicate");
            const auto beforeRemoval = pending.locations();
            pending.cancelLookup();
            require(pending.locations().pages == beforeRemoval.pages
                            && pending.locations().order == beforeRemoval.order,
                    "Cancelling idle lookup discarded valid cached locations");
            require(pending.remove(pending.highlights().first().id) && pending.resolved(),
                    "Removing a passage discarded remaining endpoint locations");
            const auto afterRemoval = pending.locations();
            require(afterRemoval.pages == beforeRemoval.pages.mid(2)
                            && afterRemoval.order.size() == 2 && afterRemoval.order[0] == 0
                            && afterRemoval.order[1] == 1,
                    "Removing a passage lost page pairing or left stale relative ranks");
        }
        waitUntil([&] { return !session.saving(); });
        {
            if (sqlite)
                storage::closeProfile();
            HighlightSession reopened(model, layout, annotations.path(),
                                      "file:///highlight-session.epub", model.sourceHash(), "Book");
            require(reopened.highlights().first().color == "blue"
                            && reopened.highlights().first().style == "underline"
                            && !reopened.highlights().first().sentenceEndPolicy
                            && reopened.highlights().first().chosenEnd == QString()
                            && !reopened.highlights().first().sentenceStartPolicy
                            && reopened.highlights().first().chosenStart == QString(),
                    "Reopened session lost raw endpoint policy");
            require(reopened.highlights().size() == 2
                            && detail(reopened, reopened.highlights()[0].id).text == first.text,
                    "Reopened session lost full passage");
        }
        completed = false;
        session.save(first.start, first.end, passage.id, [&](bool ok) {
            saved = ok;
            completed = true;
        });
        waitUntil([&] { return completed; });
        require(saved && session.highlights().size() == 2
                        && session.highlights()[0].id == passage.id
                        && detail(session, session.highlights()[0].id).text == first.text
                        && session.highlights()[0].color == "blue"
                        && session.highlights()[0].style == "underline",
                "Editing changed passage identity or kept old text");
        require(session.remove(passage.id) && session.highlights().size() == 1, "Delete failed");
        completed = false;
        session.save("bad", first.end, {}, [&](bool ok) {
            saved = ok;
            completed = true;
        });
        waitUntil([&] { return completed; });
        require(!saved && session.highlights().size() == 1, "Invalid range changed saved passages");
        // A failed atomic replacement must preserve the saved passage and
        // remain retryable through the ordinary save API. Earlier edits must
        // be on storage first, or their saves would fail against the block too.
        waitUntil([&] { return !session.saving(); });
        const auto previous = detail(session, session.highlights().first().id);
        const auto savedPath = annotations.path() + '/'
                + BookmarkStore::bookKey("file:///highlight-session.epub", model.sourceHash())
                + "/highlights.json";
        const auto backup = savedPath + ".save-backup";
        auto readSaved = [&] {
            if (sqlite) {
                HighlightStore persisted(annotations.path(), "file:///highlight-session.epub",
                                         model.sourceHash(), "Book");
                require(persisted.load(), "Cannot reload persisted SQLite highlights");
                QJsonArray rows;
                for (const auto &row : persisted.highlights())
                    rows.append(highlightJson(row));
                return QJsonDocument(rows).toJson(QJsonDocument::Compact);
            }
            QFile file(savedPath);
            require(file.open(QIODevice::ReadOnly), "Cannot read saved highlight fixture");
            return file.readAll();
        };
        const auto savedBytes = readSaved();
        std::unique_ptr<SqliteSessionWriteFailure> blocked;
        if (sqlite)
            blocked = std::make_unique<SqliteSessionWriteFailure>(annotations.path(), 1);
        else
            require(QFile::rename(savedPath, backup) && QDir().mkdir(savedPath),
                    "Cannot block atomic highlight replacement");
        // Edits show at once. A failed save returns to the saved passage.
        int saveFailures = 0;
        session.saveFailed = [&] { ++saveFailures; };
        require(session.recolor(previous.id, "pink")
                        && session.highlights().first().color == "pink",
                "A recolor did not show at once");
        waitUntil([&] { return saveFailures == 1; });
        const auto failedRecolorError = session.error();
        const auto restoredRecolor = detail(session, previous.id);
        require(samePassage(previous, restoredRecolor)
                        && session.highlights().first().color == previous.color
                        && !failedRecolorError.isEmpty(),
                "Failed recolor kept its change in memory");
        require(session.updateAppearance(previous.id, "blue", "dotted"), "Cannot start a restyle");
        waitUntil([&] { return saveFailures == 2; });
        require(session.highlights().first().style == previous.style
                        && samePassage(previous, detail(session, previous.id)),
                "Failed restyle kept its change in memory");
        completed = false;
        session.save(
                first.start, first.end, previous.id,
                [&](bool ok) {
                    saved = ok;
                    completed = true;
                },
                first.end, true, first.start, true);
        waitUntil([&] { return completed && saveFailures == 3; });
        require(saved && session.highlights().size() == 1
                        && session.highlights().first().id == previous.id
                        && samePassage(previous, detail(session, previous.id)),
                "Failed edit changed live saved data");
        session.saveFailed = {};
        if (sqlite)
            blocked.reset();
        else
            require(QDir().rmdir(savedPath) && QFile::rename(backup, savedPath),
                    "Cannot restore atomic highlight fixture");
        require(readSaved() == savedBytes, "Failed edit replaced prior saved bytes");
        completed = false;
        session.save(
                first.start, first.end, previous.id,
                [&](bool ok) {
                    saved = ok;
                    completed = true;
                },
                first.end, true, first.start, true);
        waitUntil([&] { return completed; });
        require(saved && session.highlights().size() == 1
                        && session.highlights().first().id == previous.id
                        && session.highlights().first().created == previous.created
                        && detail(session, session.highlights().first().id).text == first.text
                        && session.highlights().first().sentenceStartPolicy
                        && session.highlights().first().sentenceEndPolicy,
                "Retry failed or changed identity, creation date or punctuation provenance");
        completed = false;
        session.save(
                last.start, last.end, {},
                [&](bool ok) {
                    saved = ok;
                    completed = true;
                },
                {}, false, {}, false, "yellow");
        waitUntil([&] { return completed; });
        require(saved && session.highlights().last().color == "yellow",
                "Explicit create color was lost");
        require(session.recolor(previous.id, "pink")
                        && session.highlights().first().color == "pink",
                "Failed recolor was not retryable");
        require(session.restyle(previous.id, "underline")
                        && session.updateAppearance(previous.id, "green", "shaded")
                        && session.highlights().first().style == "shaded"
                        && session.highlights().first().color == "green",
                "Choosing a color did not atomically replace the underline with shading");
        {
            const auto previousBackend = qgetenv("KOBALT_ANNOTATIONS_SQLITE");
            struct RestoreBackend
            {
                QByteArray value;
                ~RestoreBackend()
                {
                    if (value.isNull())
                        qunsetenv("KOBALT_ANNOTATIONS_SQLITE");
                    else
                        qputenv("KOBALT_ANNOTATIONS_SQLITE", value);
                }
            } restoreBackend{ previousBackend };
            qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
            QTemporaryDir compactRoot;
            require(compactRoot.isValid(), "Cannot create compact session fixture");
            Highlight a{ "compact-a", first.start, first.end,
                         first.text,  "First",     QDateTime::currentDateTimeUtc() };
            Highlight b{ "compact-b", last.start, last.end, last.text, "Last", a.created };
            a.note = QString(4096, QChar('n'));
            HighlightStore seed(compactRoot.path(), "compact-session", model.sourceHash(), "Book");
            require(seed.load(), "Cannot initialize compact session storage");
            seed.setHighlights({ b, a });
            require(seed.save(), "Cannot seed compact session storage");
            int layoutReads = 0;
            HighlightSession compact(
                    model,
                    [&] {
                        ++layoutReads;
                        return layout();
                    },
                    compactRoot.path(), "compact-session", model.sourceHash(), "Book");
            auto isCompact = [&] {
                for (const auto &h : compact.highlights())
                    require(!h.detailsLoaded && h.text.isEmpty() && h.chapter.isEmpty()
                                    && h.note.isEmpty(),
                            "Session retained a highlight payload");
            };
            isCompact();
            QVector<HighlightExcerpt> excerpts;
            require(compact.excerpts({ b.id, a.id }, &excerpts) && excerpts.size() == 2
                            && excerpts[0].text == b.text && excerpts[1].text == a.text,
                    "Compact session did not fetch requested excerpts in order");
            require(detail(compact, a.id).note == a.note, "Compact session lost an unloaded note");
            isCompact();
            require(!compact.sync(), "Compact session unexpectedly resolved without a batch");
            waitUntil([&] { return compact.resolved(); });
            layoutReads = 0;
            const auto compactLocations = compact.locations();
            require(layoutReads == 1 && compactLocations.pages.size() == 4,
                    "A location snapshot repeated the layout validity calculation");
            QVector<Highlight> exportedRows;
            layoutReads = 0;
            require(compact.exportDetails(&exportedRows) && exportedRows.size() == 2
                            && exportedRows[0].id == a.id && exportedRows[1].id == b.id
                            && exportedRows[0].note == a.note,
                    "Export did not hydrate and sort compact records into source order");
            require(layoutReads == 1,
                    "Export repeated the layout validity calculation for each annotation");
            isCompact();
            require(compact.recolor(a.id, "blue") && detail(compact, a.id).note == a.note,
                    "Compact recolor erased an unloaded note");
            waitUntil([&] { return !compact.saving(); });
            isCompact();
            bool adjusted = false;
            compact.save(first.start, first.end, a.id, [&](bool ok) {
                require(ok, "Cannot adjust compact saved highlight");
                adjusted = true;
            });
            waitUntil([&] { return adjusted && !compact.saving(); });
            require(detail(compact, a.id).note == a.note, "Compact endpoint edit erased its note");
            isCompact();
            require(compact.remove(b.id) && compact.highlights().size() == 1
                            && detail(compact, a.id).note == a.note,
                    "Compact deletion changed the remaining note");
            waitUntil([&] { return !compact.saving(); });
            isCompact();
        }
        if (sqlite) {
            QTemporaryDir closingRoot;
            HighlightStore seed(closingRoot.path(), "sqlite-close", model.sourceHash(), "Book");
            require(seed.load(), "Cannot initialize SQLite close fixture");
            Highlight original{ "closing",  first.start, first.end,
                                first.text, "Chapter",   QDateTime::currentDateTimeUtc() };
            original.note = QString::fromUtf8("kept \xc3\xa9") + QChar(0) + " note";
            seed.setHighlights({ original });
            require(seed.save(), "Cannot seed SQLite close fixture");
            auto closing = std::make_unique<HighlightSession>(
                    model, layout, closingRoot.path(), "sqlite-close", model.sourceHash(), "Book");
            require(closing->updateAppearance(original.id, "pink", "dotted"),
                    "Cannot edit before closing SQLite session");
            closing.reset(); // Destruction must finish the save, even before its GUI reply.
            storage::closeProfile();
            HighlightStore verified(closingRoot.path(), "sqlite-close", model.sourceHash(), "Book");
            original.color = "pink";
            original.style = "dotted";
            require(verified.load() && verified.highlights().size() == 1
                            && highlightJson(verified.highlights().first())
                                    == highlightJson(original),
                    "Closing SQLite lost the last highlight edit or its payload");
        }
        {
            bool located = false;
            model.bookmarkLocations({ first.start }, [&](BookmarkLocations result) {
                require(result.pages.isEmpty() && result.order.isEmpty(),
                        "A revoked source accepted endpoint results");
                located = true;
            });
            lease->revoke();
            waitUntil([&] { return located; });
        }
        owned.reset();
        ReaderSession::drain([&] { app.quit(); });
    });
    const int result = app.exec();
    if (result || sqlite)
        return result;
    return sqliteSessionVariant(path);
}
