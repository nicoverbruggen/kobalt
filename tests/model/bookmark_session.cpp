#include "reader_inputs.h"
#include "annotation_session_checks.h"
#include "storage/profile_database.h"
#include "check.h"
#include "reading/bookmark_session.h"
#include "reading/reader_session.h"
#include "app/messages/opening_message.h"
#include "publication/native_resources.h"
#include "plain_archive.h"
#include "publication/resource_server.h"
#include "support/qt_compat.h"
#include "support/diagnostics.h"
#include <QEventLoop>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTimer>
#include <QMap>
#include <QSet>
#include <QFile>
#include <QDir>
#include <cstdio>

using namespace kobalt;

namespace {
void waitUntil(const std::function<bool()> &ready)
{
    if (ready())
        return;
    QEventLoop loop;
    QTimer poll, timeout;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop,
                     [] { require(false, "BookmarkSession check timed out"); });
    poll.start(1);
    timeout.start(60000);
    loop.exec();
}

void drain(ReaderSession &model)
{
    // The engine worker runs jobs in order. Once this reply arrives, every
    // earlier reply has been delivered.
    bool done = false;
    model.pageSummary(model.page(), [&](QString, QString) { done = true; });
    waitUntil([&] { return done; });
}

QByteArray read(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read bookmark fixture");
    return file.readAll();
}
} // namespace

// Checks bookmark editing, merging, reflow and reopening on the JSON and SQLite backends, including failed writes.
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
        model.open({ "file:///bookmark-session.epub",
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
        // Use the largest supported size so the later reflow can merge bookmarks.
        require(model.style(maximumFontPixels(), 120),
                "Cannot set the initial bookmark fixture size");
        waitUntil([&] { return !model.busy(); });
        require(model.pages() > 4, "The synthetic book is too short");
        const QSize pageArea(758, 900);
        const auto originalState = model.state();
        const auto originalKey = bookmarkLayoutKey(model.pages(), originalState, pageArea);
        {
            QTemporaryDir emptyRoot;
            require(emptyRoot.isValid(), "Cannot create immediate-bookmark fixture");
            BookmarkSession empty(
                    model,
                    [&] { return bookmarkLayoutKey(model.pages(), model.state(), pageArea); },
                    emptyRoot.path(), "immediate-bookmark", model.sourceHash(), model.title());
            require(empty.available() && empty.writable(),
                    "Immediate bookmark storage is unavailable");
            require(empty.sync() && empty.resolved(),
                    "An empty bookmark list deferred its ready layout");
            bool added = false;
            empty.added = [&] { added = true; };
            require(empty.toggle(model.page()) == BookmarkSession::Toggle::Adding,
                    "The first bookmark after resolving an empty layout was refused");
            waitUntil([&] { return added; });
        }
        for (const auto *field :
             { "top_margin_pixels", "bottom_margin_pixels", "weight_steps", "bold_reading" }) {
            auto changed = originalState;
            changed[field] = QByteArray(field) == "bold_reading"
                    ? QJsonValue(QStringLiteral("bold"))
                    : QJsonValue(40);
            require(bookmarkLayoutKey(model.pages(), changed, pageArea) != originalKey,
                    "A pagination setting reused the bookmark layout key");
        }
        require(bookmarkLayoutKey(model.pages(), originalState, QSize(758, 899)) != originalKey,
                "A viewport height change reused the bookmark layout key");
        QTemporaryDir diagnostics;
        require(diagnostics.isValid(), "Cannot create bookmark diagnostics folder");
        initializeDiagnostics(diagnostics.path());
        if (sqlite) {
            QTemporaryDir damagedRoot;
            const QString identity = "sqlite-damaged-book";
            const auto position = model.state().value("position").toString();
            BookmarkStore seed(damagedRoot.path(), identity, model.sourceHash(), model.title());
            seed.setBookmarks(
                    { { position, "Chapter", "Saved passage", QDateTime::currentDateTimeUtc() } });
            require(seed.save(), "Cannot seed malformed SQLite bookmark");
            require(storage::profileTransaction(
                            damagedRoot.path(),
                            [](QSqlDatabase &db) {
                                QSqlQuery query(db);
                                return query.exec("UPDATE entries SET start='' WHERE kind=0");
                            }),
                    "Cannot damage SQLite bookmark");
            {
                BookmarkSession damaged(
                        model, [&] { return originalKey; }, damagedRoot.path(), identity,
                        model.sourceHash(), model.title());
                require(damaged.available() && !damaged.writable() && !damaged.error().isEmpty(),
                        "Malformed SQLite bookmark remained writable");
                int failures = 0;
                damaged.failed = [&] { ++failures; };
                require(!damaged.sync()
                                && damaged.toggle(model.page()) == BookmarkSession::Toggle::Refused
                                && failures == 1,
                        "Malformed SQLite bookmark accepted an edit");
                require(storage::profileTransaction(
                                damagedRoot.path(),
                                [](QSqlDatabase &db) {
                                    QSqlQuery query(db);
                                    return query.exec("SELECT start FROM entries WHERE kind=0")
                                            && query.next() && query.value(0).toString().isEmpty();
                                }),
                        "Failed load removed the damaged SQLite row");
            }
            require(storage::profileTransaction(
                            damagedRoot.path(),
                            [&](QSqlDatabase &db) {
                                QSqlQuery query(db);
                                query.prepare("UPDATE entries SET start=? WHERE kind=0");
                                query.addBindValue(position);
                                return query.exec();
                            }),
                    "Cannot repair SQLite bookmark fixture");
            BookmarkSession repaired(
                    model, [&] { return originalKey; }, damagedRoot.path(), identity,
                    model.sourceHash(), model.title());
            require(repaired.writable() && repaired.bookmarks().size() == 1
                            && repaired.bookmarks().first().position == position,
                    "Reopened SQLite session did not recover after repair");
        }
        for (const auto &original : { QByteArray("damaged but recoverable"),
                                      QByteArray(BookmarkStore::maxFileBytes + 1, 'x') }) {
            if (sqlite)
                continue; // These are damaged legacy JSON files.
            QTemporaryDir failedRoot;
            BookmarkStore store(failedRoot.path(), "file:///private-book.epub", model.sourceHash(),
                                model.title());
            require(QDir().mkpath(store.folder()), "Cannot create failed-load folder");
            const auto path = store.folder() + "/bookmarks.json";
            QFile file(path);
            require(file.open(QIODevice::WriteOnly) && file.write(original) == original.size(),
                    "Cannot write failed-load fixture");
            file.close();
            BookmarkSession failedLoad(
                    model, [] { return QStringLiteral("failed-load"); }, failedRoot.path(),
                    "file:///private-book.epub", model.sourceHash(), model.title());
            int failed = 0, added = 0;
            failedLoad.failed = [&] { ++failed; };
            failedLoad.added = [&] { ++added; };
            require(failedLoad.available() && !failedLoad.writable()
                            && !failedLoad.error().isEmpty(),
                    "Failed load has no usable error state");
            require(!failedLoad.sync()
                            && failedLoad.toggle(model.page()) == BookmarkSession::Toggle::Refused,
                    "A failed load accepted an edit");
            drain(model);
            require(failed == 1 && added == 0 && failedLoad.bookmarks().isEmpty()
                            && read(path) == original,
                    "Editing after a failed load changed the saved bytes or announced success");
            flushDiagnostics();
            const auto events = read(diagnostics.path() + "/events.jsonl");
            require(!events.contains(failedRoot.path().toUtf8())
                            && !events.contains("private-book.epub")
                            && !events.contains("/bookmarks.json"),
                    "Bookmark load diagnostics disclosed a private path");
        }
        // Obstruct each output after loading. Adds, removals and automatic
        // merges must leave both the current list and saved list unchanged.
        const QStringList outputs =
                sqlite ? QStringList{ "database" } : QStringList{ "bookmarks.json", "book.json" };
        for (const auto &blockedName : outputs) {
            QTemporaryDir failedRoot;
            const auto position = model.state().value("position").toString();
            BookmarkStore store(failedRoot.path(), "file:///private-book.epub", model.sourceHash(),
                                model.title());
            const QVector<Bookmark> original{
                { position, "Chapter", "First words",
                  QDateTime::fromString("2026-09-20T10:00:00Z", Qt::ISODate).toUTC() }
            };
            store.setBookmarks(original);
            require(store.save(), "Cannot save failure fixture");
            const auto marksPath = store.folder() + "/bookmarks.json";
            const auto originalBytes = sqlite ? QByteArray() : read(marksPath);
            const auto blockedPath = store.folder() + '/' + blockedName;
            BookmarkSession failedSave(
                    model, [] { return QStringLiteral("failed-save"); }, failedRoot.path(),
                    "file:///private-book.epub", model.sourceHash(), model.title());
            waitUntil([&] { return failedSave.sync(); });
            const auto originalPages = failedSave.pages();
            std::unique_ptr<SqliteSessionWriteFailure> blocked;
            if (sqlite)
                blocked = std::make_unique<SqliteSessionWriteFailure>(failedRoot.path(), 0);
            else
                require(QFile::rename(blockedPath, blockedPath + ".saved")
                                && QDir().mkdir(blockedPath),
                        "Cannot obstruct bookmark save");
            int failed = 0, added = 0;
            failedSave.failed = [&] { ++failed; };
            failedSave.added = [&] { ++added; };
            // Edits show at once. A failed save returns to the saved list.
            require(failedSave.remove(position) && failedSave.bookmarks().isEmpty(),
                    "A removal did not show at once");
            waitUntil([&] { return failed == 1; });
            require(failedSave.bookmarks() == original && failedSave.pages() == originalPages,
                    "A failed removal kept its change in memory");
            require(failedSave.toggle(model.page()) == BookmarkSession::Toggle::Removed,
                    "Cannot start toggle removal");
            waitUntil([&] { return failed == 2; });
            require(failedSave.bookmarks() == original && failedSave.pages() == originalPages,
                    "A failed toggle removal kept its change in memory");
            require(model.go(1), "Cannot turn for failed-add fixture");
            waitUntil([&] { return !model.busy(); });
            require(failedSave.toggle(model.page()) == BookmarkSession::Toggle::Adding,
                    "Cannot start failed add");
            waitUntil([&] { return failed == 3; });
            require(added == 1 && failedSave.bookmarks() == original
                            && failedSave.pages() == originalPages && !failedSave.error().isEmpty(),
                    "A failed add kept its change in memory or hid the error");
            if (sqlite)
                blocked.reset();
            else {
                require(QDir().rmdir(blockedPath)
                                && QFile::rename(blockedPath + ".saved", blockedPath),
                        "Cannot restore bookmark output");
                require(read(marksPath) == originalBytes,
                        "A failed edit changed persisted bookmarks");
            }
            BookmarkStore persisted(failedRoot.path(), "file:///private-book.epub",
                                    model.sourceHash(), model.title());
            require(persisted.load() && persisted.bookmarks() == original,
                    "Failed edits changed persisted bookmark records");
            require(failedSave.toggle(model.page()) == BookmarkSession::Toggle::Adding,
                    "Cannot retry failed add");
            waitUntil([&] { return added == 2 && !failedSave.saving(); });
            require(failedSave.error().isEmpty() && failedSave.bookmarks().size() == 2,
                    "A successful retry kept the save error");
            flushDiagnostics();
            const auto events = read(diagnostics.path() + "/events.jsonl");
            require(!events.contains(failedRoot.path().toUtf8())
                            && !events.contains("/bookmarks.json")
                            && !events.contains("private-book.epub"),
                    "Bookmark save diagnostics disclosed a private path");
            require(model.go(0), "Cannot return after failed-save checks");
            waitUntil([&] { return !model.busy(); });
        }
        {
            QTemporaryDir failedRoot;
            const auto position = model.state().value("position").toString();
            BookmarkStore store(failedRoot.path(), "file:///merge.epub", model.sourceHash(),
                                model.title());
            const QVector<Bookmark> original{
                { position, "Chapter", "First",
                  QDateTime::fromString("2026-09-20T10:00:00Z", Qt::ISODate).toUTC() },
                { position, "Chapter", "Second",
                  QDateTime::fromString("2026-09-20T10:00:00Z", Qt::ISODate).toUTC() }
            };
            store.setBookmarks(original);
            require(store.save(), "Cannot save failed-merge fixture");
            const auto path = store.folder() + "/bookmarks.json";
            const auto originalBytes = sqlite ? QByteArray() : read(path);
            BookmarkSession failedMerge(
                    model, [] { return QStringLiteral("failed-merge"); }, failedRoot.path(),
                    "file:///merge.epub", model.sourceHash(), model.title());
            std::unique_ptr<SqliteSessionWriteFailure> blocked;
            if (sqlite)
                blocked = std::make_unique<SqliteSessionWriteFailure>(failedRoot.path(), 0);
            else
                require(QFile::rename(path, path + ".saved") && QDir().mkdir(path),
                        "Cannot obstruct merged bookmarks");
            int failed = 0;
            failedMerge.failed = [&] { ++failed; };
            require(!failedMerge.sync(), "A merge reused unresolved pages");
            waitUntil([&] { return failed == 1; });
            require(failedMerge.sync() && failedMerge.bookmarks() == original
                            && failedMerge.pages() == QVector<int>({ 0, 0 }),
                    "A failed merge changed the list or lost its page pairing");
            drain(model);
            require(failed == 1, "A failed merge kept retrying without another edit");
            if (sqlite)
                blocked.reset();
            else
                require(QDir().rmdir(path) && QFile::rename(path + ".saved", path)
                                && read(path) == originalBytes,
                        "A failed merge changed persisted bookmarks");
            BookmarkStore persisted(failedRoot.path(), "file:///merge.epub", model.sourceHash(),
                                    model.title());
            require(persisted.load() && persisted.bookmarks() == original,
                    "Failed merge changed persisted bookmark records");
        }

        // The window's layout key. Changing it asks the session to look pages up
        // again; the engine layout itself stays the same.
        QString layout = originalKey;
        auto session = std::make_unique<BookmarkSession>(
                model, [&] { return layout; }, annotations.path(), "file:///bookmark-session.epub",
                model.sourceHash(), model.title());
        require(session->available(), "A sideloaded book has no bookmark folder");
        int changed = 0, added = 0;
        session->changed = [&] { ++changed; };
        session->added = [&] { ++added; };
        const auto resolve = [&] { waitUntil([&] { return session->sync(); }); };
        resolve();
        for (int page : { 1, 3 }) {
            require(model.go(page), "Cannot turn to a bookmark page");
            waitUntil([&] { return !model.busy(); });
            const int before = added;
            require(session->toggle(page) == BookmarkSession::Toggle::Adding,
                    "Adding a bookmark was refused");
            waitUntil([&] { return added > before; });
        }
        require(session->bookmarks().size() == 2 && session->marked(1) && session->marked(3),
                "Two bookmarks were not added");
        layout = "navigation-during-lookup";
        const int beforeLookup = changed;
        require(!session->sync() && !session->sync(),
                "A new bookmark layout resolved synchronously");
        require(model.go(2), "Cannot turn during bookmark lookup");
        waitUntil([&] { return !model.busy(); });
        drain(model);
        require(session->resolved() && session->resolvedPages() == QVector<int>({ 1, 3 })
                        && changed == beforeLookup + 1,
                "A page turn discarded or repeated an otherwise valid bookmark lookup");

        // Two taps on one page before the engine answers add one bookmark.
        require(model.go(2), "Cannot turn to page 2");
        waitUntil([&] { return !model.busy(); });
        const int beforeDouble = added;
        require(session->toggle(2) == BookmarkSession::Toggle::Adding
                        && session->toggle(2) == BookmarkSession::Toggle::Adding,
                "A second tap during an add was refused or removed");
        drain(model);
        require(added == beforeDouble + 1 && session->bookmarks().size() == 3
                        && session->pages().count(2) == 1,
                "Two taps added two bookmarks on one page");
        require(session->toggle(2) == BookmarkSession::Toggle::Removed
                        && session->bookmarks().size() == 2,
                "Cannot remove the page 2 bookmark");

        // A bookmark removed while pages are looked up for a new layout must not
        // be matched to the old reply. Both happen in one GUI turn.
        auto changedState = originalState;
        changedState["top_margin_pixels"] = model.topMargin() + 1;
        layout = bookmarkLayoutKey(model.pages(), changedState, pageArea);
        const auto removed = session->bookmarks().first().position;
        require(!session->sync(), "A new layout reused old pages");
        require(session->remove(removed), "The bookmark was not removed");
        drain(model);
        resolve();
        require(session->bookmarks().size() == 1 && session->pages().size() == 1,
                "Pages and bookmarks lost their pairing");
        require(!session->marked(1) && session->marked(3),
                "An old reply marked the removed bookmark's page");
        require(session->pageOf(removed) == -1 && session->page(0) == 3,
                "The remaining bookmark lost its page");

        // Bookmarks that come to share a page merge. Removing one of them while
        // the merge waits for the engine must not stop the others merging.
        require(session->remove(session->bookmarks().first().position),
                "Cannot clear the earlier bookmark");
        const int first = model.pages() / 2;
        for (int page = first; page < first + 5; ++page) {
            require(model.go(page), "Cannot turn to a bookmark page");
            waitUntil([&] { return !model.busy(); });
            const int before = added;
            require(session->toggle(page) == BookmarkSession::Toggle::Adding,
                    "Adding a bookmark was refused");
            waitUntil([&] { return added > before; });
        }
        require(session->bookmarks().size() == 5, "Five bookmarks were not added");
        // The smallest text fits several of those pages on one. Five pages then
        // span at most two, so one page holds at least three bookmarks.
        // Page 0 has no bookmark. The merge below adds one there too.
        require(model.go(0), "Cannot return to the first page");
        waitUntil([&] { return !model.busy(); });
        const auto beforeBeautifier = session->bookmarks();
        for (bool enabled : { true, false }) {
            require(model.setBookBeautifier(enabled), "Cannot toggle beautifier with bookmarks");
            waitUntil([&] { return !model.busy(); });
            waitUntil([&] {
                session->sync();
                return session->resolved();
            });
            require(session->bookmarks().size() == beforeBeautifier.size(),
                    "Beautifier lost bookmarks");
            for (int i = 0; i < beforeBeautifier.size(); ++i)
                require(session->bookmarks()[i].position == beforeBeautifier[i].position
                                && session->page(i) >= 0,
                        "Beautifier changed a bookmark anchor");
        }
        const int pagesBefore = model.pages();
        require(model.style(minimumFontPixels(), 100), "Cannot change the text size");
        waitUntil([&] { return !model.busy(); });
        require(model.pages() < pagesBefore / 3,
                "The smallest size does not merge enough bookmark pages");
        {
            QTemporaryDir cancelledAnnotations;
            require(cancelledAnnotations.isValid(), "Cannot create cancelled merge annotations");
            BookmarkStore store(cancelledAnnotations.path(), "file:///bookmark-session.epub",
                                model.sourceHash(), model.title());
            store.setBookmarks(session->bookmarks());
            require(store.save(), "Cannot save bookmarks for the cancelled merge");
            BookmarkSession cancelled(
                    model, [&] { return QStringLiteral("smallest"); }, cancelledAnnotations.path(),
                    "file:///bookmark-session.epub", model.sourceHash(), model.title());
            bool turnedDuringMerge = false;
            cancelled.changed = [&] {
                if (turnedDuringMerge || !cancelled.resolved())
                    return;
                QSet<int> pages;
                for (int page : cancelled.pages())
                    pages.insert(page);
                require(pages.size() < cancelled.pages().size(),
                        "The cancelled merge needs bookmarks sharing a page");
                // Merge summaries are queued before this callback. A page turn
                // must keep them valid because their source layout is unchanged.
                turnedDuringMerge = true;
                require(model.go(1), "Cannot turn during the bookmark merge");
            };
            require(!cancelled.sync(), "The cancelled merge reused known pages");
            waitUntil([&] { return turnedDuringMerge && !model.busy(); });
            drain(model);
            require(cancelled.resolved(),
                    "A page turn forced another bookmark lookup after merging");
            QSet<int> seen;
            for (int page : cancelled.pages()) {
                require(page >= 0 && !seen.contains(page), "A page turn left bookmarks unmerged");
                seen.insert(page);
            }
            require(cancelled.bookmarks().size() == cancelled.pages().size(),
                    "Merge lost bookmark page pairing");
        }
        require(model.go(0), "Cannot return after the cancelled bookmark merge");
        waitUntil([&] { return !model.busy(); });
        layout = "smallest";
        bool removedDuringMerge = false, addingDuringMerge = false;
        // Known pages must pair with the list whenever an add finishes.
        session->added = [&] {
            ++added;
            require(session->pages().isEmpty()
                            || session->bookmarks().size() == session->pages().size(),
                    "An add finished with pages that do not pair with the list");
            require(!session->resolved() || !session->pages().isEmpty(), "Known pages are missing");
        };
        session->changed = [&] {
            ++changed;
            if (removedDuringMerge || !session->resolved())
                return;
            // The lookup has finished and the merge is waiting for the engine.
            // Remove one of the shared bookmarks, and start an add that the
            // engine answers after the merge.
            QMap<int, int> counts;
            for (int page : session->pages())
                ++counts[page];
            for (int i = 0; i < session->pages().size(); ++i) {
                if (counts.value(session->pages()[i]) < 3)
                    continue;
                require(session->remove(session->bookmarks()[i].position),
                        "Cannot remove a bookmark during the merge");
                removedDuringMerge = true;
                break;
            }
            require(removedDuringMerge, "No page holds three bookmarks at the smallest size");
            require(model.page() == 0 && !session->marked(0),
                    "The first page is not free for an add");
            addingDuringMerge = session->toggle(0) == BookmarkSession::Toggle::Adding;
        };
        resolve();
        require(removedDuringMerge && addingDuringMerge,
                "The lookup did not finish before the merge");
        drain(model);
        resolve();
        drain(model);
        resolve();
        QSet<int> seen;
        for (int page : session->pages()) {
            require(page >= 0 && !seen.contains(page), "Bookmarks on one page were left unmerged");
            seen.insert(page);
        }
        require(session->bookmarks().size() == session->pages().size()
                        && session->bookmarks().size() <= 3,
                "The merge lost or kept bookmarks");
        require(session->marked(0), "The add during the merge was lost");
        session->changed = [&] { ++changed; };
        session->added = [&] { ++added; };

        // A session destroyed while its lookup is pending drops the reply.
        session->flush();
        const auto beforeClose = session->bookmarks();
        layout = "third";
        require(!session->sync(), "A new layout reused old pages");
        session.reset();
        drain(model);
        if (sqlite) {
            storage::closeProfile();
            auto reopened = std::make_unique<BookmarkSession>(
                    model, [&] { return layout; }, annotations.path(),
                    "file:///bookmark-session.epub", model.sourceHash(), model.title());
            require(reopened->bookmarks().size() == beforeClose.size(),
                    "Closing SQLite lost bookmarks");
            for (int i = 0; i < beforeClose.size(); ++i)
                require(bookmarkJson(reopened->bookmarks()[i]) == bookmarkJson(beforeClose[i]),
                        "Reopening SQLite changed a bookmark");
            waitUntil([&] { return reopened->sync(); });
            require(!reopened->bookmarks().isEmpty()
                            && reopened->remove(reopened->bookmarks().first().position),
                    "Cannot edit before closing SQLite bookmark session");
            const auto expected = reopened->bookmarks();
            reopened.reset(); // Do not wait for the async save's GUI reply.
            storage::closeProfile();
            BookmarkStore verified(annotations.path(), "file:///bookmark-session.epub",
                                   model.sourceHash(), model.title());
            require(verified.load() && verified.bookmarks() == expected,
                    "Closing SQLite dropped the last bookmark edit");
        }

        lease->revoke();
        owned.reset();
        ReaderSession::drain([&] { app.quit(); });
    });
    const int result = app.exec();
    std::puts(
            "PASS: bookmark pages follow list changes, merges survive removals, and pending replies outlive no session");
    if (result || sqlite)
        return result;
    return sqliteSessionVariant(path);
}
