#include "reader_inputs.h"
#include "check.h"
#include "reading/reading_book.h"
#include "reading/appearance_settings.h"
#include "interfaces/opening_message.h"
#include "reading/reader_session.h"
#include "storage/reader_storage.h"
#include "publication/native_resources.h"
#include "support/qt_compat.h"
#include "publication/resource_server.h"
#include "publication/native_archive.h"
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTimer>
#include <utility>

using namespace kobalt;
namespace {
void waitUntil(const std::function<bool()> &ready) {
    if (ready())
        return;
    QEventLoop loop;
    QTimer poll, timeout;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [] {
        require(false, "ReadingBook check timed out");
    });
    poll.start(1);
    timeout.start(60000);
    loop.exec();
}
void tick() {
    QEventLoop loop;
    qt::later(10, &loop, [&] {
        loop.quit();
    });
    loop.exec();
}
struct Calls {
    int reads = 0, writes = 0, observed = 0, stopped = 0, destroyed = 0;
    bool writable = true, transition = false, changed = true, failWrite = false;
    bool finished = false;
    QString location;
    ReadingProgress current{"store-id", "chapter.xhtml#one", 1, 1, true};
    ReadingEstimates estimate;
    ReadingActivity writtenActivity;
    bool providesEstimates = false;
};
class Bridge final : public ReadingBridge {
public:
    explicit Bridge(Calls &calls) : calls(calls) {}
    ~Bridge() override {
        ++calls.destroyed;
    }
    bool deviceTransition() const override {
        return calls.transition;
    }
    bool writable() const override {
        return calls.writable && !calls.transition;
    }
    ReadingProgress read(QString *) const override {
        ++calls.reads;
        return calls.current;
    }
    bool takeChange() override {
        return std::exchange(calls.changed, false);
    }
    bool write(const ReadingProgress &expected, const QString &location, double, bool finished,
               const ReadingActivity &activity, ReadingProgress *verified, QString *error) override {
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
    void observeReading(const ReadingActivity &) override {
        ++calls.observed;
    }
    bool estimates(const ReadingActivity &, ReadingEstimates *result) override {
        *result = calls.estimate;
        return calls.providesEstimates;
    }
    void stopReading() override {
        ++calls.stopped;
    }

private:
    Calls &calls;
};
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    require(argc == 2, "Expected a synthetic EPUB");
    const auto path = QString::fromLocal8Bit(argv[1]);
    native::ResourceServer server;
    require(server.open(path,
                        [path] {
                            return native::openPlainArchive(path);
                        }),
            "Cannot open synthetic publication");
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create state directory");
    storage::Paths paths{temporary.path()};
    const auto open = [&] {
        auto session = std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        session->open(
            {"test-book", path, QSize(758, 900), {}, {}, {}, lease, nativeResourceFactory(path, server.port(), lease)},
            [&](const OpeningResult &result) {
                require(result.ready(), qPrintable(openingMessage(result)));
                ready = true;
            });
        waitUntil([&] {
            return ready;
        });
        return session;
    };
    for (const auto &original : {qt::object({{"line_spacing", 122}}), QJsonObject(),
                                 qt::object({{"font_pixels", 31},
                                             {"font_face", "Libron"},
                                             {"line_spacing", QJsonValue(QJsonValue::Null)},
                                             {"dark_mode", true}})}) {
        auto migratedOriginal = original;
        appearance::migrateLineSpacing(migratedOriginal);
        const QString identity = "file:///mnt/onboard/kobalt-benchmark.kepub.epub";
        auto session = std::make_unique<ReaderSession>(test::readerFonts(), test::readerPlaceholder());
        bool ready = false;
        auto lease = std::make_shared<SourceLease>();
        OpenRequest request{identity, path, QSize(758, 900), original,
                            {},       {},   lease,           nativeResourceFactory(path, server.port(), lease)};
        request.temporaryAppearance = appearance::benchmarkTypography(identity);
        request.originalAppearance = original;
        session->open(std::move(request), [&](const OpeningResult &result) {
            require(result.ready(), qPrintable(openingMessage(result)));
            ready = true;
        });
        waitUntil([&] {
            return ready;
        });
        require(session->temporaryTypography() && session->fontFace().isEmpty() &&
                    session->fontSize() == defaultFontPixels() && session->margin() == readingPixels(60) &&
                    session->topMargin() == readingPixels(60) && !session->state().value("dark_mode").toBool(),
                "Temporary appearance was lost during cold opening");
        require(!session->style(30, 100, "Libron", 0, "left") && !session->setWeight(2) &&
                    !session->setHyphenation("on") && !session->setVerticalMargins(0),
                "Temporary appearance accepted a typography edit");
        session->setDarkMode(true);
        session->apply(original);
        waitUntil([&] {
            return !session->busy();
        });
        const auto hash = session->sourceHash();
        const auto statePath = paths.bookState(identity);
        std::unique_ptr<ReaderSession> retained;
        {
            ReadingBook benchmark(std::move(session), statePath, paths, identity, hash, nullptr, {}, {});
            QJsonObject snapshot;
            require(benchmark.checkpoint(&snapshot), "Benchmark checkpoint failed");
            require(snapshot.value("font_pixels").toInt() == defaultFontPixels(),
                    "Live snapshot lost benchmark appearance");
            const auto persisted = storage::readJson(statePath);
            require(appearance::same(persisted, migratedOriginal),
                    "Checkpoint persisted temporary appearance or added absent keys");
            require(benchmark.session()->turn(1), "Benchmark cannot turn");
            waitUntil([&] {
                return !benchmark.session()->busy();
            });
            require(benchmark.checkpoint(&snapshot), "Benchmark turn checkpoint failed");
            require(appearance::same(storage::readJson(statePath), migratedOriginal),
                    "Turn checkpoint changed original appearance");
            require(!storage::readJson(paths.preferences()).contains(appearance::lastTextLayoutKey),
                    "Temporary benchmark typography became the next book's style");
            retained = benchmark.session()->retained();
            require(bool(retained), "Benchmark could not retain its parsed document");
        }
        require(appearance::same(storage::readJson(statePath), migratedOriginal),
                "Closing changed original appearance");
        ready = false;
        lease = std::make_shared<SourceLease>();
        retained->open({identity,
                        path,
                        QSize(758, 900),
                        storage::readJson(statePath),
                        {},
                        {},
                        lease,
                        nativeResourceFactory(path, server.port(), lease)},
                       [&](const OpeningResult &result) {
                           require(result.ready(), qPrintable(openingMessage(result)));
                           ready = true;
                       });
        waitUntil([&] {
            return ready;
        });
        require(!retained->temporaryTypography() &&
                    retained->fontSize() == original.value("font_pixels").toInt(defaultFontPixels()) &&
                    retained->spacing() == migratedOriginal.value("line_spacing").toInt(defaultLineSpacing),
                "Normal reopening retained temporary typography");
        auto ordinary = open();
        require(!ordinary->temporaryTypography(), "Unmatched opening inherited benchmark typography");
    }
    {
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook local(std::move(session), paths.bookState("local-id"), paths, "local-id", hash, nullptr, {}, {});
        require(local.saveLayoutPreferences(
                    qt::object({{"show_clock", false}, {"header_text_size", "large"}, {"orientation", 2}})),
                "Cannot save book layout");
        require(local.saveDictionaryChoice("test-dictionary"), "Cannot save dictionary choice");
        QJsonObject layoutSnapshot;
        require(local.checkpoint(&layoutSnapshot), "Cannot checkpoint book layout");
        const auto savedLayout = storage::readJson(paths.bookState("local-id"));
        require(!savedLayout["show_clock"].toBool() && savedLayout["header_text_size"].toString() == "large" &&
                    savedLayout["orientation"].toInt() == 2,
                "Checkpoint lost book layout");
        require(savedLayout["dictionary_id"].toString() == "test-dictionary", "Checkpoint lost dictionary choice");
        require(!storage::readJson(paths.preferences()).contains("show_clock"),
                "Book layout changed global preferences");
        require(!local.canWriteNative() && !local.deviceTransition(), "Local book acquired platform services");
        require(local.estimates().learningPace && local.estimates().bookMilliseconds == -1,
                "Standalone book stopped using its session pace");
        local.pollProgress();
        local.observeProgress();
        require(!local.readProgress().valid && !local.pendingProgress(), "Local book invented external progress");
        local.observeReading();
        tick();
        local.stopReading();
        const auto active = local.readingMilliseconds();
        require(active > 0, "Local reading time did not run without a bridge");
        tick();
        require(local.readingMilliseconds() == active, "Stopped local reading time kept running");
        require(local.session()->go(local.session()->pages() - 1), "Cannot reach the final page");
        waitUntil([&] {
            return !local.session()->busy();
        });
        require(local.offerFinish(), "Local book cannot be marked finished without a bridge");
        local.markFinished();
        require(!local.offerFinish(), "Local book repeatedly offered the same finish position");
        require(local.writeNative({}, "no native mapping"), "Local-only save required a platform position");
        QJsonObject snapshot;
        require(local.checkpoint(&snapshot), "Local checkpoint failed");
        require(snapshot == storage::readJson(paths.bookState("local-id")), "Local checkpoint fields changed on disk");
        require(snapshot.value("book") == QStringLiteral("local-id") && snapshot.value("schema") == 1 &&
                    snapshot.value("fingerprint").toString() == QString::fromLatin1(hash) &&
                    !snapshot.value("finished_position").toString().isEmpty() &&
                    snapshot.value("native_writeback") == QStringLiteral("local-only") &&
                    snapshot.value("native_writeback_error").toString().isEmpty(),
                "Local checkpoint lost identity, finish or writeback state");
        local.releaseNative();
        local.observeReading();
        tick();
        require(local.readingMilliseconds() == active && !local.offerFinish(),
                "Released local book became active again");
    }
    {
        Calls calls;
        auto session = open();
        const auto hash = session->sourceHash();
        ReadingBook book(std::move(session), paths.bookState("store-id"), paths, "store-id", hash,
                         std::make_unique<Bridge>(calls), calls.current, calls.current);
        book.observeReading();
        require(calls.observed == 1, "Platform activity was not recorded");
        calls.providesEstimates = true;
        calls.estimate = ReadingEstimates{12000, 45000, false};
        require(book.estimates().chapterMilliseconds == 12000 && book.estimates().bookMilliseconds == 45000 &&
                    !book.estimates().learningPace,
                "Platform estimates did not replace the session learning delay");
        calls.estimate = ReadingEstimates{};
        require(book.estimates().bookMilliseconds == -1 && !book.estimates().learningPace,
                "Unavailable platform counts became a local learning estimate");
        calls.providesEstimates = false;
        calls.transition = true;
        book.observeReading();
        require(book.deviceTransition() && !book.canWriteNative() && calls.observed == 1 && calls.stopped == 1,
                "Device transition did not stop platform activity");
        require(book.writeNative("position", {}), "Deferred platform write blocked local checkpoint");
        require(calls.writes == 0, "Platform write ran during transition");
        calls.transition = false;
        ++calls.current.modified;
        calls.current.location = "chapter.xhtml#remote";
        book.pollProgress();
        require(book.pendingProgress(), "External progress change did not create a conflict");
        require(!book.writeNative("position", {}) && calls.writes == 0, "Conflict was overwritten without a choice");
        book.acknowledge(book.candidate(), false);
        require(book.session()->turn(1), "Cannot turn before saving native progress");
        waitUntil([&] {
            return !book.session()->busy();
        });
        require(book.writeNative("position", {}) && calls.writes == 1 && calls.location == "position",
                "Acknowledged progress was not written");
        require(calls.observed == 1 && calls.writtenActivity.page == book.session()->page() &&
                    calls.writtenActivity.position == book.session()->state().value("position").toString(),
                "Native estimate used the preceding observed page when saving a completed turn");
        QJsonObject snapshot;
        require(book.checkpoint(&snapshot), "Platform checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == calls.current.json() &&
                    snapshot.value("native_writeback") == QStringLiteral("verified"),
                "Verified native baseline changed");
        const auto committed = calls.current;
        calls.failWrite = true;
        require(book.writeNative("position-two", {}), "Ordinary platform failure blocked local persistence");
        require(book.checkpoint(&snapshot), "Failed-write local checkpoint failed");
        require(snapshot.value("nickel_progress").toObject() == committed.json() &&
                    snapshot.value("native_writeback") == QStringLiteral("local-only") &&
                    snapshot.value("native_writeback_error") == QStringLiteral("synthetic write failure"),
                "Failed native write changed the confirmed baseline");
        const int writes = calls.writes;
        require(book.writeNative({}, "unmappable store position") && calls.writes == writes,
                "Store book accepted a time-only write");
        const auto statePath = paths.bookState("store-id");
        require(QFile::remove(statePath) && QDir().mkpath(statePath), "Cannot obstruct checkpoint destination");
        require(!book.checkpoint(&snapshot), "Checkpoint reported success when storage refused the write");
        require(QDir().rmdir(statePath) && book.checkpoint(&snapshot),
                "Failed checkpoint was cached instead of retried");
        require(storage::readJson(statePath) == snapshot, "Retried checkpoint lost the current state");
        book.releaseNative();
        require(calls.destroyed == 1 && !book.canWriteNative(), "Release retained platform services");
        book.pollProgress();
        book.observeProgress();
        book.observeReading();
        require(calls.destroyed == 1 && !book.offerFinish(), "Released book accessed its bridge again");
    }
    bool drained = false;
    ReaderSession::drain([&] {
        drained = true;
    });
    waitUntil([&] {
        return drained;
    });
    bool disconnected = false;
    releaseResourceConnections([&] {
        disconnected = true;
    });
    waitUntil([&] {
        return disconnected;
    });
    std::puts("PASS: local reading state and optional platform activity, conflict, writeback and release");
    // ReaderSession stops its worker on application shutdown. Nested waits
    // above do not emit aboutToQuit; leave through the application event loop.
    qt::later(0, &app, [&] {
        app.quit();
    });
    return app.exec();
}
