#include "reader_inputs.h"
#include "check.h"
#include "reading/reading_book.h"
#include "reading/appearance_settings.h"
#include "reading/session_timeout.h"
#include "app/messages/opening_message.h"
#include "reading/reader_session.h"
#include "storage/reader_storage.h"
#include "storage/bookmark_store.h"
#include "storage/highlight_store.h"
#include "storage/record_writer.h"
#include "storage/profile_database.h"
#include "support/diagnostics.h"
#include <atomic>
#include <QSqlQuery>
#include "publication/native_resources.h"
#include "support/qt_compat.h"
#include "publication/resource_server.h"
#include "plain_archive.h"
#include <QEventLoop>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <utility>

using namespace kobalt;

namespace {
QJsonObject persistedBook(QJsonObject state)
{
    for (const auto *key : appearance::layoutKeys)
        state.remove(key);
    for (const auto *key : appearance::marginKeys)
        state.remove(key);
    for (const auto *key : { "chapter_title", "chapter_page", "chapter_pages", "page", "pages",
                             "next_page", "text_font_face", "embedded_font_count", "cached_pages" })
        state.remove(key);
    return state;
}

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
                     [] { require(false, "ReadingBook check timed out"); });
    poll.start(1);
    timeout.start(60000);
    loop.exec();
}

void tick(int milliseconds = 10)
{
    QEventLoop loop;
    qt::later(milliseconds, &loop, [&] { loop.quit(); });
    loop.exec();
}

struct Calls
{
    int reads = 0, writes = 0, observed = 0, stopped = 0, destroyed = 0;
    bool writable = true, transition = false, changed = true, failWrite = false, signalled = false;
    bool finished = false;
    QString location;
    ReadingProgress current{ "store-id", "chapter.xhtml#one", 1, 1, true };
    ReadingEstimates estimate;
    ReadingActivity writtenActivity, observedActivity, stoppedActivity;
    bool providesEstimates = false;
};

class Bridge final : public ReadingBridge
{
public:
    explicit Bridge(Calls &calls) : calls(calls) { }

    ~Bridge() override { ++calls.destroyed; }

    bool deviceTransition() const override { return calls.transition; }

    bool writable() const override { return calls.writable && !calls.transition; }

    ReadingProgress read(QString *) const override
    {
        ++calls.reads;
        return calls.current;
    }

    bool takeChange() override { return std::exchange(calls.changed, false); }

    bool signalsChanges() const override { return calls.signalled; }

    bool write(const ReadingProgress &expected, const QString &location, double, bool finished,
               const ReadingActivity &activity, ReadingProgress *verified, QString *error) override
    {
        ++calls.writes;
        calls.writtenActivity = activity;
        calls.finished = finished;
        calls.location = location;
        require(expected == calls.current, "Bridge received a stale expected baseline");
        if (calls.failWrite) {
            *error = "synthetic write failure";
            return false;
        }
        calls.current.location = location;
        ++calls.current.modified;
        calls.current.status = finished ? 2 : 1;
        *verified = calls.current;
        return true;
    }

    void observeReading(const ReadingActivity &activity) override
    {
        calls.observedActivity = activity;
        ++calls.observed;
    }

    bool estimates(const ReadingActivity &, ReadingEstimates *result) override
    {
        *result = calls.estimate;
        return calls.providesEstimates;
    }

    void stopReading(const ReadingActivity &activity) override
    {
        calls.stoppedActivity = activity;
        stopReading();
    }

    void stopReading() override { ++calls.stopped; }

private:
    Calls &calls;
};
} // namespace

// Checks the reading-book session: saved fields, background saves, image views, finish state, reading time and history.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    {
        // Replay the Clara BW report: 307 seconds of reading included 223
        // seconds of kernel standby that CLOCK_MONOTONIC did not count.
        qint64 boot = 44661;
        ReadingTimer timer([&] { return boot; });
        require(!timer.isValid() && timer.elapsed() == 0, "Reading timer started before reading");
        timer.start();
        boot = 352003;
        require(timer.includesStandby() && timer.elapsed() == 307342,
                "Reading timer lost standby time from the Clara BW report");
        const auto first = timer.elapsed();
        timer.invalidate();
        boot += 600000;
        require(!timer.isValid() && timer.elapsed() == 0, "Paused reading timer kept counting");
        timer.start();
        boot += 60000;
        require(first + timer.elapsed() == 367342, "Resuming counted deliberate sleep");
        boot = -1;
        require(timer.elapsed() == 60000, "A failed clock read changed the accumulated duration");
        boot = 352003 + 600000 + 90000;
        require(timer.elapsed() == 90000,
                "Reading clock did not recover in its original time domain");
        boot -= 1000;
        require(timer.elapsed() == 90000, "A backward clock sample reduced reading time");
        timer.start();
        boot += 1000;
        require(timer.elapsed() == 1000, "A new segment retained the previous duration");
        boot = -1;
        timer.start();
        require(!timer.includesStandby() && timer.elapsed() >= 0,
                "An unavailable boot clock did not select the segment fallback");
        boot = 1000000000;
        require(!timer.includesStandby() && timer.elapsed() < 60000,
                "A recovering clock changed origins halfway through a fallback segment");
    }
    require(argc == 2, "Expected a synthetic EPUB");
    const auto path = QString::fromLocal8Bit(argv[1]);
    native::ResourceServer server;
    require(server.open(path, [path] { return native::openPlainArchive(path); }),
            "Cannot open synthetic publication");
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create state directory");
    storage::Paths paths{ temporary.path() };
    const auto open = [&](QSize size = QSize(758, 900), const QJsonObject &state = QJsonObject(),
                          const QString &identity = QStringLiteral("test-book"),
                          PositionCodec positions = {}, bool restart = false,
                          std::unique_ptr<ReaderSession> session = {}) {
        if (!session)
            session =
                    std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        OpenRequest request{ identity, path,
                             size,     state,
                             {},       {},
                             lease,    nativeResourceFactory(path, server.port(), lease) };
        request.positions = positions;
        request.restartAtStart = restart;
        if (restart)
            request.platformLocation = "invalid-finished-native-location";
        session->open(std::move(request), [&](const OpeningResult &result) {
            require(result.ready(), qPrintable(openingMessage(result)));
            ready = true;
        });
        waitUntil([&] { return ready; });
        return session;
    };
    {
        Calls calls;
        const PositionCodec positions{
            [](const EngineDocument &, const QString &location, QString *) { return location; },
            [](const EngineDocument &model, QString *) { return model.position(); },
            [](const QString &location) { return location; }
        };
        auto session = open(QSize(758, 900), {}, "session-event", positions);
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("session-event"), paths,
                         "session-event", hash, std::make_unique<Bridge>(calls), calls.current,
                         calls.current);
        book.observeReading();
        const auto opening = calls.observedActivity;
        require(!opening.nativePosition.isEmpty(),
                "Session activity lost the displayed frame's native mapping");
        require(book.session()->turn(1), "Cannot turn before stopping the native interval");
        waitUntil([&] { return !book.session()->busy(); });
        // Sleep/close can stop before observeReading sees this completed frame.
        book.stopReading();
        QString mappingError;
        const auto finalPosition = book.session()->savedNativePosition(&mappingError);
        require(mappingError.isEmpty() && calls.stoppedActivity.page != opening.page
                        && calls.stoppedActivity.nativePosition != opening.nativePosition
                        && calls.stoppedActivity.nativePosition == finalPosition
                        && calls.stoppedActivity.percent
                                == qMin(99,
                                        pageProgressPercent(book.session()->page(),
                                                            book.session()->pages())),
                "The closing native interval received the preceding page's progress or mapping");
        require(calls.writes == 0, "Stopping the native interval persisted book progress");
    }
    // Optional book state must survive both supported backends without
    // changing reading data or whether a book is finished.
    for (const auto *backend : { "0", "1" }) {
        qputenv("KOBALT_ANNOTATIONS_SQLITE", backend);
        QTemporaryDir profile;
        require(profile.isValid(), "Finish profile unavailable");
        storage::Paths finishPaths{ profile.path() };
        {
            const QString identity = "image-views", a(64, 'a'), b(64, 'b');
            const QJsonObject first{
                { "id", a }, { "zoom", 2.0 }, { "x", .25 }, { "y", .75 }, { "future", "keep" }
            };
            const QJsonObject second{ { "id", b }, { "zoom", 4.0 }, { "x", .6 }, { "y", .4 } };
            const QJsonObject original{ { "schema", 1 },
                                        { "book", identity },
                                        { "extra", "keep" },
                                        { "image_views",
                                          QJsonArray{ first, second,
                                                      QJsonObject{ { "id", QString(64, 'c') },
                                                                   { "zoom", 99 },
                                                                   { "x", .5 },
                                                                   { "y", .5 } } } } };
            require(storage::writeBookState(finishPaths, identity, original),
                    "Cannot seed image views");
            {
                auto session = open(QSize(758, 900), {}, identity);
                const auto hash = session->sourceHash();
                ReadingBook book(std::move(session), finishPaths.bookState(identity), finishPaths,
                                 identity, hash, nullptr, {}, {});
                require(book.imageView(a) == first && book.imageView(b) == second
                                && book.imageView(QString(64, 'c')).isEmpty(),
                        "Image views lost identity or accepted invalid zoom");
                require(!book.rememberImageView("invalid", first)
                                && !book.rememberImageView(
                                        a, { { "zoom", 2 }, { "x", -1 }, { "y", .5 } })
                                && !book.rememberImageView(
                                        a, { { "zoom", true }, { "x", .5 }, { "y", .5 } }),
                        "Malformed image views were accepted");
                require(book.rememberImageView(a, { { "zoom", 3 }, { "x", .2 }, { "y", .7 } }),
                        "Cannot update image view");
                require(book.imageView(a).value("future") == "keep" && book.imageView(b) == second,
                        "Updating an image changed another image or removed optional fields");
                require(!book.rememberImageView(a, book.imageView(a)),
                        "An unchanged image view caused another save");
                QJsonObject snapshot;
                require(book.checkpoint(&snapshot, true), "Cannot queue image-view checkpoint");
                waitUntil([&] { return !book.savesPending(); });
                require(storage::readBookState(finishPaths, identity)
                                        .object.value("image_views")
                                        .toArray()
                                        .size()
                                == 2,
                        "Background checkpoint lost image views");
            }
            auto saved = storage::readBookState(finishPaths, identity).object;
            auto session = open(QSize(900, 758), saved, identity);
            const auto hash = session->sourceHash();
            ReadingBook reopened(std::move(session), finishPaths.bookState(identity), finishPaths,
                                 identity, hash, nullptr, {}, {});
            require(reopened.imageView(a).value("zoom").toDouble() == 3
                            && reopened.imageView(b) == second,
                    "Reopening the book lost its image views");
            require(reopened.rememberImageView(a, { { "zoom", 1 }, { "x", .5 }, { "y", .5 } })
                            && reopened.imageView(a).isEmpty(),
                    "Fit did not clear the remembered image view");
            require(reopened.rememberImageView(b, { { "zoom", 1 }, { "x", .5 }, { "y", .5 } }),
                    "Cannot reset second image");
            QJsonObject snapshot;
            require(reopened.checkpoint(&snapshot)
                            && !storage::readBookState(finishPaths, identity)
                                        .object.contains("image_views"),
                    "Fit restored stale views from the opening record");
            for (int i = 0; i < 70; ++i)
                require(reopened.rememberImageView(QString::number(i, 16).rightJustified(64, '0'),
                                                   { { "zoom", 2 }, { "x", .25 }, { "y", .75 } }),
                        "Cannot remember bounded image views");
            require(reopened.checkpointState().value("image_views").toArray().size() == 64
                            && reopened.imageView(QString(64, '0')).isEmpty(),
                    "Image-view history was not bounded");
            require(reopened.checkpoint(&snapshot), "Cannot save bounded image views");
            saved = storage::readBookState(finishPaths, identity).object;
            saved["large_optional"] = QString(10000, 'x');
            saved["book"] = "image-view-budget";
            saved.remove("image_views");
            require(storage::writeBookState(finishPaths, "image-view-budget", saved),
                    "Cannot seed image-view byte budget");
            auto budgetSession = open();
            const auto budgetHash = budgetSession->sourceHash();
            ReadingBook budget(std::move(budgetSession), finishPaths.bookState("image-view-budget"),
                               finishPaths, "image-view-budget", budgetHash, nullptr, {}, {});
            for (int i = 0; i < 64; ++i)
                budget.rememberImageView(QString::number(i, 16).rightJustified(64, '0'),
                                         { { "zoom", 2 }, { "x", .25 }, { "y", .75 } });
            require(budget.checkpoint(&snapshot),
                    "Image views prevented a reading-position checkpoint");
            const auto bounded = storage::readBookState(finishPaths, "image-view-budget").object;
            require(bounded.value("large_optional").toString().size() == 10000
                            && bounded.value("image_views").toArray().size() < 64
                            && QJsonDocument(bounded).toJson(QJsonDocument::Compact).size()
                                    <= storage::maxJsonBytes,
                    "Image views did not respect the book byte budget or changed unrelated fields");
        }
        for (const auto &flag : { QJsonValue(), QJsonValue("invalid"), QJsonValue(true) }) {
            const QString identity = "finish-book";
            QJsonObject original{ { "schema", 1 },
                                  { "book", identity },
                                  { "extra", QJsonObject{ { "keep", "value" } } },
                                  { "suppress_finish_prompt", flag },
                                  { "dictionary_id", "dictionary" } };
            if (flag.isNull())
                original.remove("suppress_finish_prompt");
            require(storage::writeBookState(finishPaths, identity, original,
                                            finishPaths.bookState(identity)),
                    "Cannot prepare finish state");
            auto session = open();
            require(session->style(32, 150), "Cannot set finish-book appearance");
            waitUntil([&] { return !session->busy(); });
            require(session->go(session->pages() - 1), "Cannot reach finish page");
            waitUntil([&] { return !session->busy(); });
            const auto hash = session->sourceHash();
            ReadingBook book(std::move(session), finishPaths.bookState(identity), finishPaths,
                             identity, hash, nullptr, {}, {});
            require(book.offerFinish() == !flag.toBool(false),
                    "Missing or invalid finish flag changed default");
            const auto position = book.checkpointState().value("position");
            BookmarkStore bookmarks(finishPaths.annotations(), identity, hash, "Finish test",
                                    finishPaths.database());
            HighlightStore highlights(finishPaths.annotations(), identity, hash, "Finish test",
                                      finishPaths.database());
            require(bookmarks.load() && highlights.load(), "Cannot load finish annotations");
            const QVector<Bookmark> savedBookmarks{ { position.toString(), "chapter", "excerpt",
                                                      QDateTime::fromString("2026-11-01T09:00:00Z",
                                                                            Qt::ISODate) } };
            auto annotation = Highlight{};
            annotation.id = "finish-annotation";
            annotation.start = position.toString();
            annotation.end = position.toString() + ".1";
            annotation.text = "Saved passage";
            annotation.created = QDateTime::currentDateTimeUtc();
            annotation.note = "Keep this note";
            bookmarks.setBookmarks(savedBookmarks);
            highlights.setHighlights({ annotation });
            require(bookmarks.save() && highlights.save(), "Cannot prepare finish annotations");
            book.suppressFinishPrompt();
            require(!book.offerFinish(), "Suppression still offers finish");
            QJsonObject snapshot;
            require(book.checkpoint(&snapshot), "Suppression checkpoint failed");
            const auto stored =
                    storage::readBookState(finishPaths, identity, finishPaths.bookState(identity))
                            .object;
            require(stored.value("suppress_finish_prompt").toBool()
                            && stored.value("extra") == original.value("extra")
                            && stored.value("dictionary_id") == original.value("dictionary_id")
                            && stored.value("position") == position
                            && stored.value("finished_position").toString().isEmpty(),
                    "Suppression changed saved position, finish state or supported extra fields");
            require(bookmarks.load() && bookmarks.bookmarks() == savedBookmarks && highlights.load()
                            && highlights.highlights().size() == 1
                            && highlights.highlights().first().note == annotation.note,
                    "Suppression changed bookmarks or annotations");
            book.markFinished();
            require(!book.checkpointState().value("finished_position").toString().isEmpty(),
                    "Suppressing the prompt disabled explicit finishing");
            require(book.checkpoint(&snapshot), "Cannot save finished checkpoint");
            const auto finished = storage::readBookState(finishPaths, identity).object;
            // Reopening must reset progress without resetting book preferences or annotations.
            auto reopened = open(QSize(758, 900), finished, identity);
            require(reopened->page() == 0 && reopened->fontSize() == book.session()->fontSize(),
                    "Finished book did not restart with its saved appearance");
            auto retained = book.session()->retained();
            require(bool(retained), "Cannot retain finished document");
            retained = open(QSize(758, 900), finished, identity, {}, false, std::move(retained));
            require(retained->page() == 0, "Retained finished book did not restart");
            ReadingBook restarted(std::move(reopened), finishPaths.bookState(identity), finishPaths,
                                  identity, hash, nullptr, {}, {});
            require(restarted.checkpoint(&snapshot), "Cannot save restarted checkpoint");
            const auto started = storage::readBookState(finishPaths, identity).object;
            require(started.value("finished_position").toString().isEmpty()
                            && started.value("dictionary_id") == original.value("dictionary_id")
                            && started.value("extra") == original.value("extra")
                            && started.value("suppress_finish_prompt").toBool(),
                    "Restart changed book preferences or retained the finished marker");
            require(bookmarks.load() && bookmarks.bookmarks() == savedBookmarks && highlights.load()
                            && highlights.highlights().first().note == annotation.note,
                    "Restart changed annotations");
            auto reading = finished;
            reading.remove("finished_position");
            auto continued = open(QSize(758, 900), reading, identity);
            require(continued->page() == continued->pages() - 1,
                    "Unfinished book lost its saved position");
            auto hostFinished = open(QSize(758, 900), reading, identity, {}, true);
            require(hostFinished->page() == 0, "Host-finished book did not restart");
            auto hostFirstOpen = open(QSize(758, 900), {}, identity, {}, true);
            require(hostFirstOpen->page() == 0,
                    "First host-finished opening restored its native final location");
            auto hostRetained = continued->retained();
            require(bool(hostRetained), "Cannot retain host-finished document");
            hostRetained =
                    open(QSize(758, 900), reading, identity, {}, true, std::move(hostRetained));
            require(hostRetained->page() == 0, "Retained host-finished book did not restart");
        }
        storage::closeProfile();
    }
    qunsetenv("KOBALT_ANNOTATIONS_SQLITE");
    const auto historyValue = [&](const QString &statement) {
        QVariant value;
        require(storage::profileTransaction(paths.directory,
                                            [&](QSqlDatabase &db) {
                                                QSqlQuery query(db);
                                                if (!query.exec(statement) || !query.next())
                                                    return false;
                                                value = query.value(0);
                                                return true;
                                            }),
                "Cannot inspect reading history");
        return value;
    };

    for (const auto &identity : { QStringLiteral("file:///mnt/onboard/book.epub"),
                                  QStringLiteral("file:///mnt/onboard/book.kepub.epub") }) {
        Calls calls;
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook sideload(std::move(session), paths.bookState(identity), paths, identity, hash,
                             std::make_unique<Bridge>(calls), {}, {});
        require(!sideload.usesNativeCloudSync() && sideload.canWriteNative(),
                "Sideloaded EPUB and KEPUB keep native local progress without excluding KOsync");
    }
    for (const auto &original : { qt::object({ { "line_spacing", 122 } }), QJsonObject(),
                                  qt::object({ { "font_pixels", 31 },
                                               { "font_face", "Libron" },
                                               { "line_spacing", QJsonValue(QJsonValue::Null) },
                                               { "dark_mode", true } }) }) {
        auto migratedOriginal = original;
        appearance::migrateLineSpacing(migratedOriginal);
        const QString identity = "file:///mnt/onboard/kobalt-benchmark.kepub.epub";
        auto session =
                std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        OpenRequest request{
            identity, path, QSize(758, 900), original,
            {},       {},   lease,           nativeResourceFactory(path, server.port(), lease)
        };
        request.temporaryAppearance = appearance::benchmarkTypography(identity);
        request.originalAppearance = original;
        session->open(std::move(request), [&](const OpeningResult &result) {
            require(result.ready(), qPrintable(openingMessage(result)));
            ready = true;
        });
        waitUntil([&] { return ready; });
        require(session->temporaryTypography() && session->fontFace().isEmpty()
                        && session->fontSize() == defaultFontPixels()
                        && session->margin() == readingPixels(60)
                        && session->topMargin() == readingPixels(60)
                        && !session->state().value("dark_mode").toBool(),
                "Temporary appearance was lost during cold opening");
        require(!session->style(30, 100, "Libron", 0, "left") && !session->setWeight(2)
                        && !session->setHyphenation("on") && !session->setVerticalMargins(0),
                "Temporary appearance accepted a typography edit");
        session->setDarkMode(true);
        session->editAppearance(kobalt::appearance::Patch(original));
        waitUntil([&] { return !session->busy(); });
        const auto hash = session->sourceHash();
        const auto statePath = paths.bookState(identity);
        std::unique_ptr<ReaderSession> retained;
        {
            ReadingBook benchmark(std::move(session), statePath, paths, identity, hash, nullptr, {},
                                  {});
            QJsonObject snapshot;
            require(benchmark.checkpoint(&snapshot), "Benchmark checkpoint failed");
            require(snapshot.value("font_pixels").toInt() == defaultFontPixels(),
                    "Live snapshot lost benchmark appearance");
            const auto persisted = storage::readBookState(paths, identity, statePath).object;
            require(appearance::same(persisted, migratedOriginal),
                    "Checkpoint persisted temporary appearance or added absent keys");
            require(benchmark.session()->turn(1), "Benchmark cannot turn");
            waitUntil([&] { return !benchmark.session()->busy(); });
            require(benchmark.checkpoint(&snapshot), "Benchmark turn checkpoint failed");
            require(appearance::same(storage::readBookState(paths, identity, statePath).object,
                                     migratedOriginal),
                    "Turn checkpoint changed original appearance");
            require(!storage::readJson(paths.preferences()).contains(appearance::lastTextLayoutKey),
                    "Temporary benchmark typography became the next book's style");
            retained = benchmark.session()->retained();
            require(bool(retained), "Benchmark could not retain its parsed document");
        }
        require(appearance::same(storage::readBookState(paths, identity, statePath).object,
                                 migratedOriginal),
                "Closing changed original appearance");
        ready = false;
        lease = std::make_shared<SourceLease>();
        retained->open({ identity,
                         path,
                         QSize(758, 900),
                         storage::readBookState(paths, identity, statePath).object,
                         {},
                         {},
                         lease,
                         nativeResourceFactory(path, server.port(), lease) },
                       [&](const OpeningResult &result) {
                           require(result.ready(), qPrintable(openingMessage(result)));
                           ready = true;
                       });
        waitUntil([&] { return ready; });
        require(!retained->temporaryTypography()
                        && retained->fontSize()
                                == original.value("font_pixels").toInt(defaultFontPixels())
                        && retained->spacing()
                                == migratedOriginal.value("line_spacing").toInt(defaultLineSpacing),
                "Normal reopening retained temporary typography");
        auto ordinary = open();
        require(!ordinary->temporaryTypography(),
                "Unmatched opening inherited benchmark typography");
    }
    {
        const QString identity = "checkpoint-fields";
        const auto statePath = paths.bookState(identity);
        const auto optional = qt::array(
                { false, QJsonValue(),
                  qt::object({ { "note", QString::fromUtf8("\xc3\xa9") + QChar(0) + "tail" } }) });
        const auto original = qt::object({ { "schema", 1 },
                                           { "book", identity },
                                           { "future_optional", optional },
                                           { "future_null", QJsonValue() },
                                           { "dictionary_id", "old-dictionary" },
                                           { "chapter_title", "Old display title" },
                                           { "cached_pages", 123 } });
        require(storage::writeJson(statePath, original), "Cannot create legacy checkpoint fields");
        const auto imported = storage::readBookState(paths, identity, statePath);
        require(imported.status == storage::ReadStatus::Ok && imported.object == original,
                "Legacy checkpoint fields changed during import");
        {
            const PositionCodec positions{
                [](const EngineDocument &, const QString &location, QString *) { return location; },
                [](const EngineDocument &model, QString *) { return model.position(); },
                [](const QString &location) { return location; }
            };
            auto session = open(QSize(758, 900), imported.object, identity, positions);
            const auto hash = session->sourceHash();
            int failures = 0;
            ReadingBook book(std::move(session), statePath, paths, identity, hash, nullptr, {}, {});
            book.onCheckpointFailed([&] { ++failures; });
            require(!book.session()->state().value("beautifier_initials").toBool(),
                    "Beautifier must default off");
            require(book.session()->setBookBeautifier(true), "Cannot enable beautifier");
            waitUntil([&] { return !book.session()->busy(); });
            require(book.saveDictionaryChoice({}), "Cannot clear an imported dictionary choice");
            QJsonObject snapshot;
            require(book.checkpoint(&snapshot), "Cannot save imported checkpoint fields");
            auto saved = storage::readBookState(paths, identity).object;
            require(saved.value("future_optional") == optional && saved.contains("future_null")
                            && saved.value("future_null").isNull(),
                    "Opening checkpoint lost optional fields");
            require(!saved.contains("dictionary_id"),
                    "Checkpoint restored a cleared dictionary choice");
            require(saved.value("beautifier_initials").toBool(),
                    "Checkpoint lost beautifier rendering state");
            {
                auto reopened = open(QSize(758, 900), saved, identity, positions);
                require(reopened->state().value("beautifier_initials").toBool(),
                        "Reopen lost beautifier rendering state");
            }
            require(snapshot.contains("chapter_title") && snapshot.contains("cached_pages")
                            && snapshot.contains("page"),
                    "Inspection snapshot lost display fields");
            // A display title can exceed the saved-record budget without
            // changing the reading position or any persistent preference.
            const QString title(int(storage::maxJsonBytes) + 1, 'T');
            book.session()->setChapters({ { title, snapshot.value("position").toString() } }, {});
            waitUntil([&] { return book.session()->chapterTitle() == title; });
            require(book.checkpoint(&snapshot) && snapshot.value("chapter_title") == title
                            && storage::readBookState(paths, identity).object == saved,
                    "Display-only chapter change affected the saved checkpoint");
            for (const auto *key :
                 { "chapter_title", "chapter_page", "chapter_pages", "page", "pages", "next_page",
                   "text_font_face", "embedded_font_count", "cached_pages" })
                require(!saved.contains(key), "Checkpoint persisted a display-only field");
            require(book.session()->turn(1), "Cannot turn before background checkpoint");
            waitUntil([&] { return !book.session()->busy(); });
            require(book.checkpoint(&snapshot, true), "Cannot queue preserved checkpoint fields");
            waitUntil([&] { return !book.savesPending(); });
            saved = storage::readBookState(paths, identity).object;
            require(saved.value("future_optional") == optional
                            && saved.value("position") == snapshot.value("position"),
                    "Background checkpoint lost optional fields or reading position");
            require(storage::profileTransaction(
                            paths.directory,
                            [](QSqlDatabase &db) {
                                QSqlQuery query(db);
                                return query.exec(
                                        "CREATE TRIGGER refuse_fields BEFORE UPDATE ON book_state "
                                        "BEGIN SELECT RAISE(ABORT,'test'); END");
                            }),
                    "Cannot obstruct preserved checkpoint");
            require(book.session()->turn(1), "Cannot turn before failed checkpoint");
            waitUntil([&] { return !book.session()->busy(); });
            require(book.checkpoint(&snapshot, true), "Cannot queue obstructed checkpoint");
            waitUntil([&] { return failures == 1; });
            require(storage::readBookState(paths, identity).object == saved,
                    "Failed checkpoint changed saved fields");
            require(storage::profileTransaction(paths.directory,
                                                [](QSqlDatabase &db) {
                                                    QSqlQuery query(db);
                                                    return query.exec("DROP TRIGGER refuse_fields");
                                                })
                            && book.checkpoint(&snapshot),
                    "Cannot retry preserved checkpoint");
            require(storage::readBookState(paths, identity).object.value("future_optional")
                            == optional,
                    "Retried checkpoint lost optional fields");
        }
        require(storage::writeBookState(paths, identity, qt::object({ { "book", identity } })),
                "Cannot reset book state");
        {
            auto session = open();
            const auto hash = session->sourceHash();
            ReadingBook book(std::move(session), statePath, paths, identity, hash, nullptr, {}, {});
            QJsonObject snapshot;
            require(book.checkpoint(&snapshot), "Cannot checkpoint after explicit reset");
            const auto saved = storage::readBookState(paths, identity).object;
            require(!saved.contains("future_optional") && !saved.contains("future_null")
                            && !saved.contains("dictionary_id"),
                    "Reopening restored explicitly reset fields");
        }
    }
    for (const bool newlyUnread : { true, false }) {
        const QString identity = newlyUnread ? "native-unread-reset" : "native-unread-acknowledged";
        Calls calls;
        calls.current = { identity, {}, 0, 0, true };
        auto previous = calls.current;
        if (newlyUnread) {
            previous.status = 1;
            previous.location = "old-position";
        }
        const auto original = qt::object({ { "schema", 1 },
                                           { "book", identity },
                                           { "future_optional", "keep" },
                                           { "dictionary_id", "old-dictionary" },
                                           { "nickel_progress", previous.json() } });
        require(storage::writeBookState(paths, identity, original),
                "Cannot seed native unread checkpoint");
        // Nickel's coordinator resets the opening snapshot without replacing
        // the stored record first. An acknowledged unread state is not a reset.
        auto opening = original;
        require(resetNativeUnread(opening, calls.current) == newlyUnread,
                "Unexpected native unread reset");
        auto session = open(QSize(758, 900), opening, identity);
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState(identity), paths, identity, hash,
                         std::make_unique<Bridge>(calls), calls.current, calls.current);
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot), "Cannot checkpoint native unread state");
        const auto saved = storage::readBookState(paths, identity).object;
        require(saved.contains("future_optional") == !newlyUnread
                        && saved.contains("dictionary_id") == !newlyUnread,
                "Checkpoint ignored or repeated Nickel's explicit unread reset");
    }
    {
        auto session = open();
        const auto hash = session->sourceHash();
        require(storage::writeJson(paths.preferences(),
                                   qt::object({ { "show_clock", false },
                                                { "header_text_size", "large" },
                                                { "orientation", 2 } })),
                "Cannot save global layout");
        // History work may cross a clock tick after the stop boundary.
        bool delayHistoryClock = false;
        ReadingBook local(std::move(session), paths.bookState("local-id"), paths, "local-id", hash,
                          nullptr, {}, {}, {}, [&] {
                              if (delayHistoryClock)
                                  QThread::msleep(10);
                              return QDateTime::currentMSecsSinceEpoch();
                          });
        require(local.saveDictionaryChoice("test-dictionary"), "Cannot save dictionary choice");
        QJsonObject layoutSnapshot;
        require(local.checkpoint(&layoutSnapshot), "Cannot checkpoint book layout");
        const auto savedLayout = storage::readBookState(paths, "local-id").object;
        require(!savedLayout.contains("show_clock") && !savedLayout.contains("header_text_size")
                        && !savedLayout.contains("orientation")
                        && !savedLayout.contains("margin_pixels"),
                "Checkpoint persisted global layout with a book");
        require(savedLayout["dictionary_id"].toString() == "test-dictionary",
                "Checkpoint lost dictionary choice");
        require(storage::readJson(paths.preferences()).value("orientation").toInt() == 2
                        && !layoutSnapshot.value("show_clock").toBool(),
                "Checkpoint lost shared rotation or layout inspection");
        require(local.session()->setVerticalMargins(readingPixels(36)),
                "Cannot change shared vertical margins");
        waitUntil([&] { return !local.session()->busy(); });
        require(local.checkpoint(&layoutSnapshot), "Cannot checkpoint changed global margins");
        require(storage::readJson(paths.preferences())
                                .value(appearance::pageMarginsKey)
                                .toObject()
                                .value("top_margin_pixels")
                                .toInt()
                        == 36,
                "A margins-only change was skipped by checkpoint caching");
        require(!local.usesNativeCloudSync(),
                "Standalone opaque identities remain eligible for KOsync");
        require(!local.canWriteNative() && !local.deviceTransition(),
                "Local book acquired platform services");
        require(local.estimates().learningPace && local.estimates().bookMilliseconds > 0,
                "Standalone book did not provide a rough estimate at opening");
        local.pollProgress();
        local.observeProgress();
        require(!local.readProgress().valid && !local.pendingProgress(),
                "Local book invented external progress");
        local.observeReading();
        tick();
        delayHistoryClock = true;
        local.stopReading();
        delayHistoryClock = false;
        auto active = local.readingMilliseconds();
        require(active > 0, "Local reading time did not run without a bridge");
        require(local.tracksReadingHistory()
                        && historyValue(
                                   "SELECT SUM(active_ms) FROM reading_sessions WHERE identity='local-id'")
                                        .toLongLong()
                                == active
                        && historyValue(
                                   "SELECT COUNT(*) FROM reading_sessions WHERE identity='local-id' AND status='completed'")
                                        .toInt()
                                == 1,
                "Local stop did not persist reading history");
        local.observeReading();
        tick();
        require(local.checkpointReadingHistory(), "Active interval checkpoint failed");
        QJsonObject unchangedCheckpoint;
        require(local.checkpoint(&unchangedCheckpoint), "Initial unchanged-checkpoint fixture");
        require(storage::profileTransaction(
                        paths.directory,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            return query.exec("CREATE TABLE history_write_audit(value INTEGER)")
                                    && query.exec(
                                            "CREATE TRIGGER audit_history AFTER UPDATE ON reading_sessions BEGIN INSERT INTO history_write_audit VALUES(1); END");
                        }),
                "History write audit fixture");
        tick();
        require(local.checkpoint(&unchangedCheckpoint)
                        && historyValue("SELECT COUNT(*) FROM history_write_audit").toInt() == 1,
                "Lifecycle checkpoint must save time even without a position change");
        require(local.session()->turn(1), "Cannot turn before routine checkpoint");
        waitUntil([&] { return !local.session()->busy(); });
        require(local.checkpoint(&unchangedCheckpoint, true),
                "Cannot queue routine position checkpoint");
        storage::drainWriterThread();
        require(historyValue("SELECT COUNT(*) FROM history_write_audit").toInt() == 1,
                "Routine position checkpoint added a synced history write");
        require(local.checkpointReadingHistory()
                        && historyValue("SELECT COUNT(*) FROM history_write_audit").toInt() == 2,
                "Periodic checkpoint skipped time without a page turn");
        require(storage::profileTransaction(paths.directory,
                                            [](QSqlDatabase &db) {
                                                QSqlQuery query(db);
                                                return query.exec("DROP TRIGGER audit_history")
                                                        && query.exec(
                                                                "DROP TABLE history_write_audit");
                                            }),
                "Remove history write audit");

        require(storage::profileTransaction(
                        paths.directory,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            return query.exec(
                                    "CREATE TRIGGER fail_history BEFORE UPDATE ON reading_sessions BEGIN SELECT RAISE(ABORT,'test'); END");
                        }),
                "History failure fixture");
        require(!local.checkpointReadingHistory(), "Blocked history update succeeded");
        QJsonObject independentCheckpoint;
        require(!local.checkpoint(&independentCheckpoint),
                "A lifecycle checkpoint must report history failure");
        require(storage::profileTransaction(paths.directory,
                                            [](QSqlDatabase &db) {
                                                QSqlQuery query(db);
                                                return query.exec("DROP TRIGGER fail_history");
                                            })
                        && local.checkpointReadingHistory(),
                "History update retry failed");

        local.stopReading();
        require(historyValue("SELECT COUNT(*) FROM reading_sessions WHERE identity='local-id'")
                                .toInt()
                        == 2,
                "Resume did not start a separate interval");
        active = local.readingMilliseconds();

        tick();
        require(local.readingMilliseconds() == active, "Stopped local reading time kept running");
        require(local.session()->go(local.session()->pages() - 1), "Cannot reach the final page");
        waitUntil([&] { return !local.session()->busy(); });
        require(local.offerFinish(), "Local book cannot be marked finished without a bridge");
        local.markFinished();
        require(!local.offerFinish(), "Local book repeatedly offered the same finish position");
        require(local.writeNative({}, "no native mapping"),
                "Local-only save required a platform position");
        QJsonObject snapshot;
        require(local.checkpoint(&snapshot), "Local checkpoint failed");
        require(persistedBook(snapshot) == storage::readBookState(paths, "local-id").object,
                "Local checkpoint fields changed on disk");
        require(snapshot.value("book") == QStringLiteral("local-id")
                        && snapshot.value("schema") == 1
                        && snapshot.value("fingerprint").toString() == QString::fromLatin1(hash)
                        && !snapshot.value("finished_position").toString().isEmpty()
                        && snapshot.value("native_writeback") == QStringLiteral("local-only")
                        && snapshot.value("native_writeback_error").toString().isEmpty(),
                "Local checkpoint lost identity, finish or writeback state");
        local.releaseNative();
        local.observeReading();
        tick();
        require(local.readingMilliseconds() == active && !local.offerFinish(),
                "Released local book became active again");
    }
    {
        auto session = open(QSize(400, 600));
        const auto hash = session->sourceHash();
        ReadingBook first(std::move(session), paths.bookState("pace-first"), paths, "pace-first",
                          hash, nullptr, {}, {});
        auto *model = first.session();
        const ReadingActivity activity{ model->state().value("position").toString(),
                                        0,
                                        model->page(),
                                        model->pages(),
                                        0,
                                        model->totalWords(),
                                        model->chapterGraph(),
                                        model->graphSelection() };
        const int words = wordEstimates::pageWords(activity);
        require(words >= 2 && words < 1000, "Pace persistence fixture has no usable page sample");
        first.observeReading();
        // The default single-shot timer can wake early. Pace samples need at
        // least two seconds on the book's clock, not a requested timer delay.
        const auto sampleMilliseconds = qMax(2100, words * 60);
        waitUntil([&] { return first.readingMilliseconds() >= sampleMilliseconds; });
        require(model->turn(1), "Pace persistence fixture cannot turn");
        waitUntil([&] { return !model->busy(); });
        first.observeReading();
        first.stopReading();
        const auto saved =
                storage::readRecord(paths.directory, "reading-speed", paths.readingSpeed()).object;
        require(saved.value("samples").toInt() == 1 && saved.value("words").toDouble() == words,
                qPrintable(
                        QString("Stopping did not save accepted reading history: words=%1, elapsed=%2, page=%3, direction=%4, saved=%5")
                                .arg(words)
                                .arg(first.readingMilliseconds())
                                .arg(model->page())
                                .arg(model->turnDirection())
                                .arg(QString::fromUtf8(
                                        QJsonDocument(saved).toJson(QJsonDocument::Compact)))));
        ReadingSpeed expected;
        expected.restore(saved);
        auto next = open(QSize(400, 600));
        ReadingBook second(std::move(next), paths.bookState("pace-second"), paths, "pace-second",
                           hash, nullptr, {}, {});
        require(second.estimates().bookMilliseconds == expected.estimates(activity).bookMilliseconds
                        && second.estimates().learningPace,
                "Another book did not reuse saved partial history");
        second.observeReading();
        second.observeReading(false);
        second.stopReading();
        require(storage::readRecord(paths.directory, "reading-speed", paths.readingSpeed()).object
                        == saved,
                "Unsampled reading overwrote saved history");
    }
    {
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "0");
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook legacy(std::move(session), paths.bookState("legacy-history"), paths,
                           "legacy-history", hash, nullptr, {}, {});
        require(!legacy.tracksReadingHistory(), "Legacy diagnostic backend enabled SQLite history");
        legacy.observeReading();
        tick();
        legacy.stopReading();
        require(historyValue(
                        "SELECT COUNT(*) FROM reading_sessions WHERE identity='legacy-history'")
                                .toInt()
                        == 0,
                "Legacy diagnostic backend wrote session history");
        qputenv("KOBALT_ANNOTATIONS_SQLITE", "1");
    }
    for (const auto minutes : sessionTimeout::choices) {
        auto session = open(QSize(400, 600));
        const auto hash = session->sourceHash();
        const auto identity = QString("timeout-%1").arg(minutes);
        qint64 utc = 100000;
        ReadingBook book(std::move(session), paths.bookState(identity), paths, identity, hash,
                         nullptr, {}, {}, {}, [&] { return utc; });
        const auto count = [&] {
            return historyValue("SELECT COUNT(*) FROM reading_sessions WHERE identity='" + identity
                                + "'")
                    .toInt();
        };
        book.observeReading();
        tick();
        const QJsonObject preferences{ { sessionTimeout::preferenceKey, minutes } };
        book.sleepReading(preferences);
        bool drained = false;
        book.drainReadingSave([&](bool saved) {
            require(saved, "Sleep native drain");
            drained = true;
        });
        waitUntil([&] { return drained; });
        const auto first = book.sessionReadingMilliseconds();
        require(first > 0, "Session timer fixture");
        utc -= 60000;
        book.observeReading();
        require(book.sessionReadingMilliseconds() >= first,
                "Backward wall clock expired the session");
        book.sleepReading(preferences);
        utc += minutes ? qint64(minutes) * 60000 - 1 : qint64(30) * 86400000;
        book.sleepReading(preferences);
        book.stopReading();
        book.observeReading();
        require(book.sessionReadingMilliseconds() >= first,
                "Short sleep or Never reset the session timer");
        book.checkpointReadingHistory();
        require(count() == 1, "Short sleep or Never split session history");
        book.sleepReading(preferences);
        drained = false;
        book.drainReadingSave([&](bool saved) {
            require(saved, "Expired sleep native drain");
            drained = true;
        });
        waitUntil([&] { return drained; });
        const auto total = book.readingMilliseconds();
        utc += minutes ? qint64(minutes) * 60000 : qint64(30) * 86400000;
        book.observeReading();
        require(book.readingMilliseconds() >= total, "Rollover reset the cumulative reading clock");
        require(minutes ? book.sessionReadingMilliseconds() < first
                        : book.sessionReadingMilliseconds() >= first,
                "Expired sleep did not reset session time or Never did reset it");
        book.checkpointReadingHistory();
        require(count() == (minutes ? 2 : 1),
                "Exact timeout boundary split the wrong session count");
        book.stopReading();
    }
    {
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("sleep-history"), paths,
                         "sleep-history", hash, nullptr, {}, {}, {}, [] {
                             QThread::msleep(10);
                             return QDateTime::currentMSecsSinceEpoch();
                         });
        book.observeReading();
        tick();
        book.sleepReading({});
        const auto paused = book.readingMilliseconds();
        require(historyValue(
                        "SELECT SUM(active_ms) FROM reading_sessions WHERE identity='sleep-history'")
                                .toLongLong()
                        == paused,
                "Sleep history and reading time used different stop boundaries");
        book.stopReading();
        book.checkpointReadingHistory();
        tick();
        require(!book.readingActive() && book.readingMilliseconds() == paused,
                "Sleeping book kept counting reading time");
        require(historyValue(
                        "SELECT COUNT(*) FROM reading_sessions WHERE identity='sleep-history' AND status='active'")
                                .toInt()
                        == 1,
                "Sleep or hide completed a short session");
        book.observeReading();
        tick();
        book.sleepReading({});
        bool closed = false;
        book.checkpointForClose([&](bool saved) {
            require(saved, "Sleeping close save");
            closed = true;
        });
        waitUntil([&] { return closed; });
        require(historyValue(
                        "SELECT COUNT(*) FROM reading_sessions WHERE identity='sleep-history' AND status='completed'")
                                .toInt()
                        == 1,
                "Closing while sleeping did not finish the retained session");
    }
    for (const bool closing : { false, true }) {
        for (const auto *outcome :
             { "success", "failure", "cancel", "destroy", "close-destroys" }) {
            const bool closeDestroys = QString(outcome) == "close-destroys";
            if (closeDestroys && !closing)
                continue;
            const QString identity =
                    QString("activity-continuation-%1-%2").arg(closing).arg(outcome);
            auto session = open();
            const auto hash = session->sourceHash();
            auto book = std::make_unique<ReadingBook>(std::move(session), paths.bookState(identity),
                                                      paths, identity, hash, nullptr,
                                                      ReadingProgress(), ReadingProgress());
            book->observeReading();
            QJsonObject snapshot;
            require(book->checkpoint(&snapshot), "Seed continuation history");
            const bool failure = QString(outcome) == "failure";
            if (failure)
                require(storage::profileTransaction(
                                paths.directory,
                                [](QSqlDatabase &db) {
                                    QSqlQuery query(db);
                                    return query.exec(
                                            "CREATE TRIGGER refuse_activity_history BEFORE UPDATE ON reading_sessions "
                                            "BEGIN SELECT RAISE(ABORT,'test'); END");
                                }),
                        "Refuse activity history");
            std::atomic_bool entered{ false }, released{ false };
            storage::RecordWriter blocker;

            struct Release
            {
                std::atomic_bool &released;

                ~Release() { released = true; }
            } release{ released };

            blocker.post(
                    [&] {
                        entered = true;
                        while (!released)
                            QThread::msleep(1);
                        return true;
                    },
                    {});
            waitUntil([&] { return entered.load(); });
            book->saveReadingActivity();
            require(book->session()->turn(1), "Turn while activity is blocked");
            waitUntil([&] { return !book->session()->busy(); });
            const auto newest = book->checkpointState().value("position");
            int completed = 0;
            bool saved = false;
            if (closing) {
                book->stopReading(true);
                book->checkpointForClose([&](bool ok) {
                    ++completed;
                    saved = ok;
                    if (closeDestroys) {
                        book->discardPendingSaves();
                        book.reset();
                    }
                });
                book->checkpointForClose([&](bool ok) {
                    ++completed;
                    saved = ok;
                });
            } else {
                require(book->checkpoint(&snapshot, true), "Queue checkpoint behind activity");
            }
            tick(30);
            require(completed == 0 && book->savesPending(),
                    "Continuation ran before activity completed");
            released = true;
            if (QString(outcome) == "cancel")
                require(book->checkpoint(&snapshot), "Synchronous checkpoint cancels activity");
            if (QString(outcome) == "destroy") {
                book->discardPendingSaves();
                book.reset();
                tick();
                require(completed == 0, "Destroyed owner received an activity continuation");
            } else if (closeDestroys) {
                waitUntil([&] { return !book; });
                tick();
                require(completed == 1 && saved,
                        "Close continued callbacks after owner destruction");
            } else {
                waitUntil([&] { return !book->savesPending() && (!closing || completed == 2); });
                if (closing)
                    require(completed == 2 && saved == !failure,
                            "Waiting close lost its completion or save outcome");
                if (!closing || !failure)
                    require(storage::readBookState(paths, identity).object.value("position")
                                    == newest,
                            "Activity completion lost the newer checkpoint");
            }
            if (failure)
                require(storage::profileTransaction(
                                paths.directory,
                                [](QSqlDatabase &db) {
                                    QSqlQuery query(db);
                                    return query.exec("DROP TRIGGER refuse_activity_history");
                                }),
                        "Release activity history");
        }
    }
    {
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("appearance-retry"), paths,
                         "appearance-retry", hash, nullptr, {}, {});
        require(book.session()->setVerticalMargins(readingPixels(40)),
                "Set appearance retry margin");
        waitUntil([&] { return !book.session()->busy(); });
        const auto preferences = paths.preferences(), backup = preferences + ".retry-test";
        require(QFile::rename(preferences, backup) && QDir().mkpath(preferences),
                "Obstruct appearance write");
        book.rememberAppearance();
        storage::drainWriterThread();
        tick(); // Deliver the failure, then repair storage before the next attempt.
        require(QDir().rmdir(preferences) && QFile::rename(backup, preferences),
                "Restore appearance file");
        require(book.session()->setVerticalMargins(readingPixels(48)),
                "Change model after failed appearance snapshot");
        waitUntil([&] { return !book.session()->busy(); });
        waitUntil([&] { return !book.savesPending(); });
        require(storage::readJson(preferences)
                                .value(appearance::pageMarginsKey)
                                .toObject()
                                .value("top_margin_pixels")
                                .toInt()
                        == 40,
                "Appearance retry rebuilt or lost the failed snapshot");
        book.rememberAppearance();
        waitUntil([&] { return !book.savesPending(); });
        require(storage::readJson(preferences)
                                .value(appearance::pageMarginsKey)
                                .toObject()
                                .value("top_margin_pixels")
                                .toInt()
                        == 48,
                "New appearance did not replace the retried snapshot");

        require(QFile::rename(preferences, backup) && QDir().mkpath(preferences),
                "Obstruct appearance before checkpoint");
        require(book.session()->setVerticalMargins(readingPixels(56)),
                "Set appearance before checkpoint");
        waitUntil([&] { return !book.session()->busy(); });
        book.rememberAppearance();
        storage::drainWriterThread();
        tick();
        require(QDir().rmdir(preferences) && QFile::rename(backup, preferences),
                "Restore appearance before checkpoint");
        require(book.session()->setVerticalMargins(readingPixels(64)),
                "Change appearance for explicit checkpoint");
        waitUntil([&] { return !book.session()->busy(); });
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot), "Checkpoint supersedes appearance retry");
        tick(350);
        require(storage::readJson(preferences)
                                .value(appearance::pageMarginsKey)
                                .toObject()
                                .value("top_margin_pixels")
                                .toInt()
                        == 64,
                "Cancelled appearance retry overwrote checkpoint");

        require(QFile::rename(preferences, backup) && QDir().mkpath(preferences),
                "Obstruct appearance before close");
        require(book.session()->setVerticalMargins(readingPixels(72)),
                "Set appearance before close");
        waitUntil([&] { return !book.session()->busy(); });
        book.rememberAppearance();
        bool closed = false, saved = true;
        book.checkpointForClose([&](bool ok) {
            closed = true;
            saved = ok;
        });
        waitUntil([&] { return closed; });
        require(!saved && !book.savesPending(),
                "Failed appearance retry stranded close or hid its failure");
        require(QDir().rmdir(preferences) && QFile::rename(backup, preferences),
                "Restore appearance after close");
    }
    {
        Calls calls;
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("native-history"), paths,
                         "native-history", hash, std::make_unique<Bridge>(calls), {}, {});
        book.observeReading();
        tick();
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot), "Native history initial checkpoint");
        require(storage::profileTransaction(
                        paths.directory,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            return query.exec("CREATE TABLE native_history_audit(value INTEGER)")
                                    && query.exec(
                                            "CREATE TRIGGER audit_native_history AFTER UPDATE ON reading_sessions "
                                            "BEGIN INSERT INTO native_history_audit VALUES(1); END");
                        }),
                "Native history write audit");
        for (int turn = 0; turn < 3; ++turn) {
            require(book.session()->turn(1), "Native history page turn");
            waitUntil([&] { return !book.session()->busy(); });
            book.observeReading();
            require(book.checkpoint(&snapshot, true), "Native history routine checkpoint");
            storage::drainWriterThread();
        }
        require(calls.observed == 4, "History displaced native activity reporting");
        require(historyValue("SELECT COUNT(*) FROM native_history_audit").toInt() == 0,
                "Page turns added history transactions");
        require(book.checkpointReadingHistory(), "Native history periodic checkpoint");
        require(historyValue("SELECT COUNT(*) FROM native_history_audit").toInt() == 1,
                "Periodic history save did not use one transaction");
        std::atomic_bool entered{ false }, released{ false };
        storage::RecordWriter blocker;

        struct Release
        {
            std::atomic_bool &released;

            ~Release() { released = true; }
        } release{ released };

        blocker.post(
                [&] {
                    entered = true;
                    while (!released)
                        QThread::msleep(1);
                    return true;
                },
                {});
        waitUntil([&] { return entered.load(); });
        book.stopReading(true);
        const auto duration = book.readingMilliseconds();
        require(!book.savesPending(),
                "Stopping activity should retain its interval until a commit point");
        require(calls.stopped == 1, "History delayed native session stop behind the local writer");
        bool closed = false, saved = false;
        book.checkpointForClose([&](bool ok) {
            closed = true;
            saved = ok;
        });
        tick();
        require(!closed, "Close did not wait for queued history");
        released = true;
        waitUntil([&] { return closed; });
        require(saved, "Native history close checkpoint failed");
        require(historyValue("SELECT COUNT(*) FROM native_history_audit").toInt() == 2,
                "Close rewrote the final history interval");
        require(historyValue(
                        "SELECT SUM(active_ms) FROM reading_sessions WHERE identity='native-history'")
                                .toLongLong()
                        == duration,
                "Native history lost or duplicated reading duration");
        require(historyValue(
                        "SELECT COUNT(*) FROM reading_sessions WHERE identity='native-history' AND status='completed'")
                                .toInt()
                        == 1,
                "Native history did not finish exactly one interval");
        require(storage::profileTransaction(
                        paths.directory,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            return query.exec("DROP TRIGGER audit_native_history")
                                    && query.exec("DROP TABLE native_history_audit");
                        }),
                "Remove native history audit");
        book.observeReading();
        tick();
        storage::drainWriterThread();
        require(storage::profileTransaction(
                        paths.directory,
                        [](QSqlDatabase &db) {
                            QSqlQuery query(db);
                            return query.exec(
                                    "CREATE TRIGGER refuse_native_history BEFORE UPDATE ON reading_sessions "
                                    "BEGIN SELECT RAISE(ABORT,'test'); END");
                        }),
                "Refuse final native history save");
        book.stopReading(true);
        closed = saved = false;
        book.checkpointForClose([&](bool ok) {
            closed = true;
            saved = ok;
        });
        waitUntil([&] { return closed; });
        require(!saved, "Close ignored a failed native history save");
        const auto stopped = book.readingMilliseconds();
        tick();
        require(book.readingMilliseconds() == stopped, "Failed close kept counting reading time");
        require(storage::profileTransaction(paths.directory,
                                            [](QSqlDatabase &db) {
                                                QSqlQuery query(db);
                                                return query.exec(
                                                        "DROP TRIGGER refuse_native_history");
                                            }),
                "Release final native history save");
        closed = saved = false;
        book.checkpointForClose([&](bool ok) {
            closed = true;
            saved = ok;
        });
        waitUntil([&] { return closed; });
        require(saved
                        && historyValue(
                                   "SELECT SUM(active_ms) FROM reading_sessions WHERE identity='native-history'")
                                        .toLongLong()
                                == stopped,
                "History retry lost or duplicated native reading time");
        book.releaseNative(false);
    }
    {
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("save-order"), paths, "save-order",
                         hash, nullptr, {}, {});
        std::atomic_bool entered{ false }, released{ false };
        storage::RecordWriter blocker;

        struct Release
        {
            std::atomic_bool &released;

            ~Release() { released = true; }
        } release{ released };

        blocker.post(
                [&] {
                    entered = true;
                    while (!released)
                        QThread::msleep(1);
                    return true;
                },
                {});
        waitUntil([&] { return entered.load(); });
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot, true), "Queue an older explicit checkpoint");
        require(book.session()->turn(1), "First save-order turn");
        waitUntil([&] { return !book.session()->busy(); });
        book.saveReadingActivity();
        require(book.session()->turn(1), "Second save-order turn");
        waitUntil([&] { return !book.session()->busy(); });
        const auto newest = book.session()->state().value("position").toString();
        require(book.checkpoint(&snapshot, true), "Queue a newer explicit choice behind activity");
        released = true;
        waitUntil([&] { return !book.savesPending(); });
        require(storage::readBookState(paths, "save-order").object.value("position").toString()
                        == newest,
                "An older activity snapshot replaced a newer explicit choice");
    }
    {
        // With the platform's change signals, the reader reads saved progress
        // only after a signal, never on the minute fallback.
        Calls calls;
        calls.signalled = true;
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("signalled-id"), paths, "signalled-id",
                         hash, std::make_unique<Bridge>(calls), calls.current, calls.current);
        book.pollProgress();
        const int reads = calls.reads;
        require(reads > 0 && !book.pendingProgress(),
                "The first progress check did not read saved progress");
        ++calls.current.modified;
        calls.current.location = "chapter.xhtml#remote";
        // Waking invalidates the minute check; without a signal it still must not read.
        book.invalidateProgress();
        book.pollProgress();
        require(calls.reads == reads && !book.pendingProgress(),
                "A signalled bridge was read without a change");
        // A read that fails right after the signal is repeated at the next check.
        const auto remote = calls.current;
        calls.current.valid = false;
        calls.changed = true;
        book.pollProgress();
        require(calls.reads == reads + 1 && !book.pendingProgress(),
                "A failed signalled read was not attempted");
        calls.current = remote;
        book.pollProgress();
        require(calls.reads == reads + 2 && book.pendingProgress(),
                "A failed signalled read was not repeated");
        book.pollProgress();
        require(calls.reads == reads + 2, "A successful read kept repeating");
        // A position that never reads stops after three retries per signal.
        book.acknowledge(book.candidate(), false);
        calls.current.valid = false;
        calls.changed = true;
        for (int poll = 0; poll < 6; ++poll)
            book.pollProgress();
        require(calls.reads == reads + 6,
                "An unreadable position was not retried three times after a signal");
        book.releaseNative(false);
    }
    {
        Calls calls;
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("store-id"), paths, "store-id", hash,
                         std::make_unique<Bridge>(calls), calls.current, calls.current);
        require(book.usesNativeCloudSync(), "Downloaded native-library titles exclude KOsync");
        book.observeReading();
        require(calls.observed == 1, "Platform activity was not recorded");
        require(book.tracksReadingHistory(), "Native bridge excluded Kobalt session history");
        book.saveReadingActivity();
        waitUntil([&] { return !book.savesPending(); });
        storage::drainWriterThread();
        require(historyValue("SELECT COUNT(*) FROM reading_sessions WHERE identity='store-id'")
                                .toInt()
                        == 1,
                "Native bridge did not record Kobalt session history");

        calls.providesEstimates = true;
        calls.estimate = ReadingEstimates{ 12000, 45000, false };
        require(book.estimates().chapterMilliseconds == 12000
                        && book.estimates().bookMilliseconds == 45000
                        && !book.estimates().learningPace,
                "Platform estimates did not replace the session learning delay");
        calls.estimate = ReadingEstimates{};
        require(book.estimates().bookMilliseconds == -1 && !book.estimates().learningPace,
                "Unavailable platform counts became a local learning estimate");
        calls.providesEstimates = false;
        calls.transition = true;
        book.observeReading();
        require(book.deviceTransition() && !book.canWriteNative() && calls.observed == 1
                        && calls.stopped == 1,
                "Device transition did not stop platform activity");
        require(book.writeNative("position", {}),
                "Deferred platform write blocked local checkpoint");
        require(book.nativeSaveResult() == ReadingBook::NativeSave::Failed,
                "Unavailable native storage was reported as intentional local-only progress");
        require(calls.writes == 0, "Platform write ran during transition");
        calls.transition = false;
        ++calls.current.modified;
        calls.current.location = "chapter.xhtml#remote";
        book.pollProgress();
        require(book.pendingProgress(), "External progress change did not create a conflict");
        require(!book.writeNative("position", {}) && calls.writes == 0,
                "Conflict was overwritten without a choice");
        require(book.nativeSaveResult() == ReadingBook::NativeSave::Conflict,
                "Native conflict lost its result");
        book.acknowledge(book.candidate(), false);
        require(book.session()->turn(1), "Cannot turn before saving native progress");
        waitUntil([&] { return !book.session()->busy(); });
        require(book.writeNative("position", {}) && calls.writes == 1
                        && calls.location == "position",
                "Acknowledged progress was not written");
        require(calls.observed == 1 && calls.writtenActivity.page == book.session()->page()
                        && calls.writtenActivity.position
                                == book.session()->state().value("position").toString(),
                "Native estimate used the preceding observed page when saving a completed turn");
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot), "Platform checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == calls.current.json()
                        && snapshot.value("native_writeback") == QStringLiteral("verified"),
                "Verified native baseline changed");
        calls.current.location = "chapter.xhtml#remote:0";
        const int beforeEqualTimeSave = calls.writes;
        require(!book.writeNative("position", {})
                        && book.nativeSaveResult() == ReadingBook::NativeSave::Conflict
                        && calls.writes == beforeEqualTimeSave,
                "Equal-time native change became an unrecoverable save failure");
        book.acknowledge(book.candidate(), false);
        require(book.writeNative("position", {})
                        && book.nativeSaveResult() == ReadingBook::NativeSave::Saved
                        && calls.writes == beforeEqualTimeSave + 1,
                "Keeping the local position did not recover equal-time writeback");
        require(book.checkpoint(&snapshot)
                        && snapshot.value("native_writeback_error").toString().isEmpty(),
                "Recovered writeback retained an error");
        calls.current.location = "chapter.xhtml#older:4";
        --calls.current.modified;
        const int beforeOlderSave = calls.writes;
        require(calls.current.modified > 0 && !book.writeNative("position", {})
                        && book.nativeSaveResult() == ReadingBook::NativeSave::Conflict
                        && calls.writes == beforeOlderSave,
                "Older authoritative native position became an unrecoverable save failure");
        book.acknowledge(book.candidate(), false);
        require(book.writeNative("position", {})
                        && book.nativeSaveResult() == ReadingBook::NativeSave::Saved
                        && calls.writes == beforeOlderSave + 1,
                "Keeping local progress did not recover an older authoritative position");
        calls.current.location.clear();
        calls.current.status = 0;
        calls.current.modified = 0;
        const int beforeUnreadSave = calls.writes;
        require(!book.writeNative("position", {})
                        && book.nativeSaveResult() == ReadingBook::NativeSave::Conflict
                        && calls.writes == beforeUnreadSave,
                "Unread native reset became an unrecoverable save failure");
        book.acknowledge(book.candidate(), false);
        require(book.writeNative("position", {})
                        && book.nativeSaveResult() == ReadingBook::NativeSave::Saved
                        && calls.writes == beforeUnreadSave + 1,
                "Keeping local progress did not recover an unread reset");
        // A same-time KEPUB offset round trip must preserve the exact local
        // anchor and allow the next native save without a conflict choice.
        const auto localPosition = book.session()->state().value("position").toString();
        const QString precise = "chapter.xhtml#kobo.50.2:105";
        for (const auto &normalized : { QStringLiteral("chapter.xhtml#kobo.50.2:0"),
                                        QStringLiteral("chapter.xhtml#kobo.50.2") }) {
            require(book.writeNative(precise, {}), "Cannot establish a precise native baseline");
            calls.current.location = normalized;
            book.observeProgress();
            require(!book.pendingProgress()
                            && book.session()->state().value("position").toString()
                                    == localPosition,
                    "Dropped native offset moved local progress or opened a choice");
            const int beforeRoundTripSave = calls.writes;
            require(book.writeNative(precise, {})
                            && book.nativeSaveResult() == ReadingBook::NativeSave::Saved
                            && calls.writes == beforeRoundTripSave + 1 && calls.location == precise
                            && book.session()->state().value("position").toString()
                                    == localPosition,
                    "Normalized native baseline blocked or changed the precise local save");
        }
        const auto committed = calls.current;
        calls.failWrite = true;
        require(book.writeNative("position-two", {}),
                "Ordinary platform failure blocked local persistence");
        require(book.checkpoint(&snapshot), "Failed-write local checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == committed.json(),
                "Failed native write changed the confirmed baseline");
        require(snapshot.value("native_writeback") == QStringLiteral("failed"),
                "Failed native write lost its result");
        require(snapshot.value("native_writeback_error")
                        == QStringLiteral("synthetic write failure"),
                "Failed native write lost its error");
        require(book.nativeSaveResult() == ReadingBook::NativeSave::Failed,
                "Failed native save allowed clean close");
        const int writes = calls.writes;
        require(book.writeNative({}, "unmappable store position") && calls.writes == writes,
                "Store book accepted a time-only write");
        require(book.nativeSaveResult() == ReadingBook::NativeSave::LocalOnly,
                "Unsupported store position was reported as a storage failure");
        const auto changeSql = [&](const QString &sql) {
            return storage::profileTransaction(paths.directory, [&](QSqlDatabase &db) {
                QSqlQuery query(db);
                return query.exec(sql);
            });
        };
        require(changeSql(
                        "CREATE TRIGGER fail_checkpoint BEFORE UPDATE ON book_state BEGIN SELECT RAISE(ABORT,'test'); END"),
                "Cannot obstruct checkpoint transaction");
        require(!book.checkpoint(&snapshot),
                "Checkpoint reported success when storage refused the write");
        require(changeSql("DROP TRIGGER fail_checkpoint") && book.checkpoint(&snapshot),
                "Failed checkpoint was cached instead of retried");
        require(storage::readBookState(paths, "store-id").object == persistedBook(snapshot),
                "Retried checkpoint lost the current state");
        {
            QTemporaryDir logs;
            require(logs.isValid(), "Cannot create close checkpoint diagnostics");
            initializeDiagnostics(logs.path());
            setDiagnosticsSync(false);
            std::atomic<int> checkpoints{ 0 };
            setDiagnosticObserver([&](const QJsonObject &event) {
                if (event.value("event") == QStringLiteral("profile-checkpoint"))
                    ++checkpoints;
            });
            const auto closeCheckpoint = [&] {
                bool done = false, saved = false;
                book.checkpointForClose([&](bool result) {
                    saved = result;
                    done = true;
                });
                waitUntil([&] { return done; });
                return saved;
            };
            require(closeCheckpoint() && checkpoints == 0,
                    "Already durable close checkpoint folded WAL again");
            const auto routineRecord = [&](int value) {
                bool saved = false;
                storage::runOnWriterThread([&] {
                    storage::RoutineWrites routine;
                    saved = storage::writeRecord(paths.directory, "close-sync-check", {},
                                                 { { "value", value } });
                });
                require(saved && storage::profileHasUnsyncedWrites(),
                        "Routine close fixture was already synced");
            };
            routineRecord(1);
            require(closeCheckpoint() && checkpoints == 1 && !storage::profileHasUnsyncedWrites(),
                    "Unchanged close record did not sync earlier routine writes");
            routineRecord(2);
            calls.failWrite = false;
            require(book.writeNative("position-after-routine", {}), "Cannot update close baseline");
            require(closeCheckpoint() && checkpoints == 1 && !storage::profileHasUnsyncedWrites(),
                    "Synced close record redundantly checkpointed earlier routine writes");
            const auto finalState = storage::readBookState(paths, "store-id").object;
            require(finalState.value("nickel_progress").toObject() == calls.current.json(),
                    "Close checkpoint lost the verified native baseline");
            require(book.writeNative("position-before-failed-close", {}),
                    "Cannot update failed close baseline");
            require(changeSql(
                            "CREATE TRIGGER fail_close_checkpoint BEFORE UPDATE ON book_state BEGIN SELECT RAISE(ABORT,'test'); END"),
                    "Cannot obstruct asynchronous close checkpoint");
            require(!closeCheckpoint(),
                    "Failed close record was accepted because the profile was synced");
            require(changeSql("DROP TRIGGER fail_close_checkpoint") && closeCheckpoint(),
                    "Failed asynchronous close checkpoint did not recover");
            setDiagnosticObserver({});
            setDiagnosticsSync(true);
            storage::closeProfile();
            require(storage::readRecord(paths.directory, "close-sync-check", {})
                                            .object.value("value")
                                    == 2
                            && storage::readBookState(paths, "store-id")
                                            .object.value("nickel_progress")
                                            .toObject()
                                    == calls.current.json(),
                    "Reopened profile lost routine writes or the final close baseline");
        }
        book.releaseNative();
        require(book.usesNativeCloudSync(),
                "Releasing native services preserves the sync provider");
        require(calls.destroyed == 1 && !book.canWriteNative(),
                "Release retained platform services");
        book.pollProgress();
        book.observeProgress();
        book.observeReading();
        require(calls.destroyed == 1 && !book.offerFinish(),
                "Released book accessed its bridge again");
    }
    bool drained = false;
    ReaderSession::drain([&] { drained = true; });
    waitUntil([&] { return drained; });
    bool disconnected = false;
    releaseResourceConnections([&] { disconnected = true; });
    waitUntil([&] { return disconnected; });
    std::puts(
            "PASS: local reading state and optional platform activity, conflict, writeback and release");
    // ReaderSession stops its worker on application shutdown. Nested waits
    // above do not emit aboutToQuit; leave through the application event loop.
    qt::later(0, &app, [&] { app.quit(); });
    return app.exec();
}
