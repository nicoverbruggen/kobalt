#include "scenario/player.h"
#include "support/diagnostics.h"
#include <QApplication>
#include <QWidget>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTimer>
#include <QVector>
#include <cstdio>
#include <cstdlib>
#include <thread>

// This check drives the player's failure paths without a reader or hardware.
namespace kobalt {
void setDiagnosticsSync(bool) { }

static bool fileWrites = true;
bool setDiagnosticFileWrites(bool enabled)
{
    const bool previous = fileWrites;
    fileWrites = enabled;
    return previous;
}
bool diagnosticWriterIdle() { return true; }

static std::function<void(const QJsonObject &)> observer;

void setDiagnosticObserver(std::function<void(const QJsonObject &)> value)
{
    observer = std::move(value);
}

void diagnostic(const char *, QJsonObject, bool) { }

void sampleDiagnosticMemory(std::function<void(QJsonObject)> done)
{
    done({});
}

namespace kosync {
static bool paused = false;
static QJsonObject account;

bool automaticPaused()
{
    return paused;
}

void pauseAutomatic(bool value)
{
    paused = value;
}

void overrideAccount(const QJsonObject &value)
{
    account = value;
}
} // namespace kosync

namespace renderingMode {
static QVector<int> overrides;

void setOverride(int mode)
{
    overrides.append(mode);
}
} // namespace renderingMode
} // namespace kobalt

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

// Checks the scenario player's sleep benchmark, KOSync account, `native-sync`, `wifi` and event steps, real-time waits, keep-awake and resource samples, with a controlled host.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    auto scenario = QJsonObject{
        { "id", "sleep.test" },
        { "opt_in", true },
        { "repeat", 1 },
        { "needs", QJsonArray{ "sleep-persistence" } },
        { "setup",
          QJsonArray{ QJsonObject{
                  { "sleep_persistence", QJsonObject{ { "action", "configure" } } } } } },
        { "steps",
          QJsonArray{
                  QJsonObject{ { "sleep_persistence", QJsonObject{ { "action", "drain" } } } } } },
        { "sample",
          QJsonObject{
                  { "event", "sleep-persistence" }, { "field", "total_ms" }, { "stages", true } } }
    };
    const auto file = [&] { return QJsonObject{ { "scenarios", QJsonArray{ scenario } } }; };
    kobalt::scenario::Host host;
    bool overrideActive = false, fail = true;
    int cleanups = 0;
    host.sleepPersistence = [&](const QJsonObject &request) {
        if (request.isEmpty()) {
            overrideActive = false;
            ++cleanups;
            return QJsonObject();
        }
        if (request.value("action") == "configure") {
            overrideActive = true;
            return QJsonObject();
        }
        return fail ? QJsonObject{ { "error", "write failed" } }
                    : QJsonObject{ { "total_ms", 5 }, { "encode_write_ms", 4 }, { "bytes", 1234 } };
    };
    require(kobalt::scenario::run(file(), host).isEmpty(), "benchmark is opt-in");
    require(cleanups == 0, "excluded benchmark has no side effects");
    kobalt::scenario::Options options;
    options.filter = "sleep";
    auto results = kobalt::scenario::run(file(), host, options);
    require(results.size() == 1 && results[0].value("status") == "failed",
            "write failure retained");
    require(!overrideActive && cleanups == 1, "failure restores overrides");
    require(results[0].value("details").toArray().first().toObject().value("status") == "failed",
            "failed trial retained");
    fail = false;
    scenario["warmup"] = 1;
    results = kobalt::scenario::run(file(), host, options);
    require(results[0].value("status") == "completed" && !overrideActive && cleanups == 2,
            "success restores overrides");
    require(results[0].value("samples").toArray().size() == 1
                    && results[0].value("details").toArray().size() == 2,
            "warmup details excluded from samples");
    require(!results[0].value("stages").toObject().contains("bytes"),
            "byte count is not a timing stage");
    host.sleepPersistence = {};
    results = kobalt::scenario::run(file(), host, options);
    require(results[0].value("status") == "skipped", "unsupported host skips benchmark");

    // A KOSync scenario: an in-memory account, a wait, and a sample picked
    // from the matching event only.
    const QJsonObject fake{ { "server", "https://127.0.0.1:9" }, { "automatic", true } };
    QJsonObject kosyncScenario{
        { "id", "kosync.test" },
        { "opt_in", true },
        { "repeat", 1 },
        { "needs", QJsonArray{ "kosync", "wifi-control" } },
        { "setup", QJsonArray{ QJsonObject{ { "kosync", fake } } } },
        { "steps",
          QJsonArray{ QJsonObject{ { "wait_ms", 60 } },
                      QJsonObject{ { "event", "main-thread-wait" },
                                   { "where", QJsonObject{ { "operation", "wifi-off" } } } } } },
        { "sample", QJsonObject{ { "event", "main-thread-wait" }, { "field", "elapsed_ms" } } }
    };
    const auto kosyncFile = [&] {
        return QJsonObject{ { "scenarios", QJsonArray{ kosyncScenario } } };
    };
    options.filter = "kosync";
    kobalt::scenario::Host network;
    results = kobalt::scenario::run(kosyncFile(), network, options);
    require(results.size() == 1 && results[0].value("status") == "skipped",
            "a host without Wi-Fi control skips KOSync scenarios");
    require(kobalt::kosync::account.isEmpty(), "a skipped scenario sets no account");
    network.wifiControl = [] { return true; };
    bool accountDuringRun = false, syncDuringRun = false;
    QTimer::singleShot(10, [&] {
        accountDuringRun = kobalt::kosync::account == fake;
        syncDuringRun = !kobalt::kosync::paused;
        kobalt::observer(QJsonObject{ { "event", "main-thread-wait" },
                                      { "operation", "json-write" },
                                      { "elapsed_ms", 7 } });
        kobalt::observer(QJsonObject{ { "event", "main-thread-wait" },
                                      { "operation", "wifi-off" },
                                      { "elapsed_ms", 42 } });
    });
    const bool pausedBefore = kobalt::kosync::paused;
    QElapsedTimer elapsed;
    elapsed.start();
    results = kobalt::scenario::run(kosyncFile(), network, options);
    require(results[0].value("status") == "completed", "the KOSync scenario completes");
    require(elapsed.elapsed() >= 60, "wait_ms lets the time pass");
    require(accountDuringRun && syncDuringRun,
            "the scenario account and automatic sync are active during the run");
    require(results[0].value("samples").toArray() == QJsonArray{ 42 },
            "the sample comes from the matching event only");
    require(kobalt::kosync::account.isEmpty() && kobalt::kosync::paused == pausedBefore,
            "the account ends with the scenario and the run restores the pause");
    // A failed step clears the account before the next scenario starts.
    kosyncScenario["steps"] =
            QJsonArray{ QJsonObject{ { "event", "main-thread-wait" }, { "timeout", 20 } } };
    const QJsonObject next{ { "id", "kosync.next" },
                            { "opt_in", true },
                            { "repeat", 1 },
                            { "steps",
                              QJsonArray{ QJsonObject{ { "mark", true } },
                                          QJsonObject{ { "wait_ms", 400 } } } },
                            { "sample", QJsonObject{ { "timer", true } } } };
    bool clearedBeforeNext = false;
    // The failing scenario ends after about 20 ms; the next one waits 400 ms.
    QTimer::singleShot(200, [&] {
        clearedBeforeNext = kobalt::kosync::account.isEmpty() && kobalt::kosync::paused;
    });
    results = kobalt::scenario::run(
            QJsonObject{ { "scenarios", QJsonArray{ kosyncScenario, next } } }, network, options);
    require(results.size() == 2 && results[0].value("status") == "failed"
                    && results[1].value("status") == "completed",
            "a missing event fails the scenario and the next one still runs");
    require(clearedBeforeNext,
            "a failure clears the account and pauses sync before the next scenario");
    require(kobalt::kosync::account.isEmpty() && kobalt::kosync::paused == pausedBefore,
            "the run restores the pause");

    // A wifi step turns Wi-Fi off and waits until the radio is down, so a
    // sample starts from a known state. Wi-Fi that is already off is left alone.
    const QJsonObject wifiScenario{ { "id", "wifi.test" },
                                    { "opt_in", true },
                                    { "repeat", 2 },
                                    { "steps",
                                      QJsonArray{ QJsonObject{ { "wifi", "off" } },
                                                  QJsonObject{ { "mark", true } } } },
                                    { "sample", QJsonObject{ { "timer", true } } } };
    const auto wifiFile = [&] {
        return QJsonObject{ { "scenarios", QJsonArray{ wifiScenario } } };
    };
    options.filter = "wifi";
    kobalt::scenario::Host radio;
    results = kobalt::scenario::run(wifiFile(), radio, options);
    require(results.size() == 1 && results[0].value("status") == "skipped"
                    && results[0].value("reason").toString().contains("cannot turn Wi-Fi off"),
            "a host that cannot turn Wi-Fi off skips the scenario");
    bool up = true;
    int offCalls = 0;
    int radioChecks = 0;
    radio.wifiUp = [&] {
        ++radioChecks;
        return up;
    };
    radio.wifiOff = [&] {
        ++offCalls;
        QTimer::singleShot(30, [&] { up = false; });
        return QString();
    };
    results = kobalt::scenario::run(wifiFile(), radio, options);
    require(results[0].value("status") == "completed"
                    && results[0].value("samples").toArray().size() == 2,
            "the wifi step completes once the radio is down");
    require(offCalls == 1, "the step turns Wi-Fi off only while it is up");
    up = true;
    radioChecks = 0;
    radio.wifiOff = [] { return QString(); };
    auto stuck = wifiScenario;
    stuck["steps"] = QJsonArray{ QJsonObject{ { "wifi", "off" }, { "timeout", 50 } },
                                 QJsonObject{ { "mark", true } } };
    results = kobalt::scenario::run(QJsonObject{ { "scenarios", QJsonArray{ stuck } } }, radio,
                                    options);
    require(results[0].value("status") == "failed"
                    && results[0].value("reason").toString().contains("Wi-Fi stayed on"),
            "a radio that stays up fails the sample");
    require(radioChecks <= 8, "a 50 ms radio wait must not poll every millisecond");
    radio.wifiOff = [] { return QStringLiteral("this firmware has no WirelessManager::off()"); };
    results = kobalt::scenario::run(wifiFile(), radio, options);
    require(results[0].value("status") == "failed"
                    && results[0].value("reason").toString().contains("no WirelessManager::off()"),
            "a host that fails to turn Wi-Fi off fails the sample");

    // wait_ms counts real time when the host has a clock that includes
    // standby, and the run keeps the device awake except for a scenario that
    // sets "standby".
    {
        qint64 real = 0;
        QVector<bool> awake;
        kobalt::scenario::Host clocked;
        clocked.realMilliseconds = [&] { return real; };
        clocked.keepAwake = [&](bool on) { awake.append(on); };
        // A suspend: real time jumps ahead while the process waits.
        QTimer::singleShot(20, [&] { real += 60000; });
        const QJsonObject waiting{ { "id", "clock.wait" },
                                   { "opt_in", true },
                                   { "repeat", 1 },
                                   { "steps",
                                     QJsonArray{ QJsonObject{ { "mark", true } },
                                                 QJsonObject{ { "wait_ms", 31000 } } } },
                                   { "sample", QJsonObject{ { "timer", true } } } };
        const QJsonObject asleep{ { "id", "clock.standby" },
                                  { "opt_in", true },
                                  { "repeat", 1 },
                                  { "standby", true },
                                  { "steps", QJsonArray{ QJsonObject{ { "mark", true } } } },
                                  { "sample", QJsonObject{ { "timer", true } } } };
        options.filter = "clock";
        QElapsedTimer elapsed;
        elapsed.start();
        results = kobalt::scenario::run(
                QJsonObject{ { "scenarios", QJsonArray{ waiting, asleep } } }, clocked, options);
        require(results.size() == 2 && results[0].value("status") == "completed"
                        && results[1].value("status") == "completed",
                "the clock scenarios complete");
        require(elapsed.elapsed() < 10000,
                "wait_ms ends once real time has passed, also across a suspend");
        require(awake == QVector<bool>({ true, false, true, false }),
                "the run keeps the device awake, lets a standby scenario suspend, and releases it at the end");
    }

    // A native sync scenario: the step waits for an earlier sync, starts one
    // and does not wait for it. Events that a step passed over stay available
    // to an `earlier` event step, `as` keeps several events of one kind, and
    // `mark_end` ends the timer before the steps that only collect events.
    const auto save = [](double elapsed) {
        return QJsonObject{ { "event", "operation-end" },
                            { "operation", "nickel-events-save" },
                            { "elapsed_ms", elapsed } };
    };
    const QJsonObject saveStep{ { "event", "operation-end" },
                                { "where", QJsonObject{ { "operation", "nickel-events-save" } } },
                                { "earlier", true } };
    auto first = saveStep, second = saveStep;
    first["as"] = "first-save";
    second["as"] = "second-save";
    QJsonObject syncScenario{
        { "id", "native-sync.test" },
        { "opt_in", true },
        { "repeat", 1 },
        { "needs", QJsonArray{ "native-sync" } },
        { "steps",
          QJsonArray{ QJsonObject{ { "native-sync", true } }, QJsonObject{ { "mark", true } },
                      QJsonObject{ { "event", "page-turn-painted" }, { "as", "turn" } },
                      QJsonObject{ { "mark_end", true } }, QJsonObject{ { "wait_ms", 300 } }, first,
                      second,
                      QJsonObject{ { "event", "native-sync-end" },
                                   { "earlier", true },
                                   { "as", "sync" } } } },
        { "sample",
          QJsonObject{
                  { "timer", true },
                  { "stages",
                    QJsonObject{
                            { "turn_ms",
                              QJsonObject{ { "event", "turn" }, { "field", "elapsed_ms" } } },
                            { "first_ms",
                              QJsonObject{ { "event", "first-save" }, { "field", "elapsed_ms" } } },
                            { "second_ms",
                              QJsonObject{ { "event", "second-save" },
                                           { "field", "elapsed_ms" } } },
                            { "sync_ms",
                              QJsonObject{ { "event", "sync" },
                                           { "field", "elapsed_ms" } } } } } } }
    };
    const auto syncFile = [&] {
        return QJsonObject{ { "scenarios", QJsonArray{ syncScenario } } };
    };
    options.filter = "native-sync";
    kobalt::scenario::Host store;
    results = kobalt::scenario::run(syncFile(), store, options);
    require(results.size() == 1 && results[0].value("status") == "skipped"
                    && results[0].value("reason") == "the host cannot start its own sync",
            "a host without its own sync skips the scenario");
    bool syncing = true, startedWhileSyncing = false;
    int starts = 0;
    std::thread worker;
    store.nativeSyncing = [&] { return syncing; };
    store.nativeSync = [&] {
        startedWhileSyncing = syncing;
        ++starts;
        // The first save ends before the page turn and is passed over; the
        // second ends after the step that waits for the turn.
        kobalt::observer(save(11));
        QTimer::singleShot(20, [&] {
            worker = std::thread([notify = kobalt::observer] {
                notify(QJsonObject{ { "event", "page-turn-painted" }, { "elapsed_ms", 5 } });
            });
        });
        QTimer::singleShot(150, [&] { kobalt::observer(save(22)); });
        QTimer::singleShot(200, [] {
            kobalt::observer(QJsonObject{ { "event", "native-sync-end" }, { "elapsed_ms", 33 } });
        });
        return QString();
    };
    QTimer::singleShot(30, [&] { syncing = false; });
    results = kobalt::scenario::run(syncFile(), store, options);
    if (worker.joinable())
        worker.join();
    require(results[0].value("status") == "completed", "the native sync scenario completes");
    require(starts == 1 && !startedWhileSyncing, "the step waits for the earlier sync to end");
    const auto samples = results[0].value("samples").toArray();
    require(samples.size() == 1 && samples[0].toDouble() < 250, "mark_end ends the timer");
    const auto stages = results[0].value("stages").toObject();
    require(stages.value("turn_ms").toArray() == QJsonArray{ 5 }
                    && stages.value("first_ms").toArray() == QJsonArray{ 11 }
                    && stages.value("second_ms").toArray() == QJsonArray{ 22 }
                    && stages.value("sync_ms").toArray() == QJsonArray{ 33 },
            "earlier event steps find passed and later events, each once and in order");
    store.nativeSync = [] { return QStringLiteral("Nickel did not start a sync"); };
    results = kobalt::scenario::run(syncFile(), store, options);
    require(results[0].value("status") == "failed"
                    && results[0].value("reason").toString().endsWith(
                            "Nickel did not start a sync"),
            "a sync that does not start fails the sample");

    // A resource sample: the system's counters and the counted events from
    // `mark` to `mark_end`, the limits for the comparison, and a rendering
    // override that ends with the scenario. The host step writes one profile
    // transaction; the one after `mark_end` is not counted.
    const QJsonObject write{ { "sleep_persistence", QJsonObject{ { "action", "write" } } } };
    const QJsonObject resourceScenario{
        { "id", "resources.test" },
        { "opt_in", true },
        { "repeat", 2 },
        { "rendering", "hybrid" },
        { "expect", QJsonObject{ { "writes", 0 } } },
        { "steps",
          QJsonArray{ QJsonObject{ { "mark", true } }, write, QJsonObject{ { "mark_end", true } },
                      write } },
        { "sample",
          QJsonObject{ { "timer", true },
                       { "resources", true },
                       { "count",
                         QJsonObject{ { "writes", "operation-start/profile-transaction" },
                                      { "ends", "operation-end" } } } } }
    };
    kobalt::scenario::Host writer;
    writer.sleepPersistence = [](const QJsonObject &) {
        require(!kobalt::fileWrites, "Resource samples must keep diagnostic events in memory");
        kobalt::observer(QJsonObject{ { "event", "operation-start" },
                                      { "operation", "profile-transaction" } });
        kobalt::observer(QJsonObject{ { "event", "operation-end" },
                                      { "operation", "profile-transaction" } });
        return QJsonObject();
    };
    options.filter = "resources";
    kobalt::renderingMode::overrides.clear();
    results = kobalt::scenario::run(QJsonObject{ { "scenarios", QJsonArray{ resourceScenario } } },
                                    writer, options);
    require(results.size() == 1 && results[0].value("status") == "completed",
            "the resource scenario completes");
    require(kobalt::renderingMode::overrides == QVector<int>({ 2, -1 }),
            "the rendering override is set for the scenario and removed at its end");
    const auto measured = results[0].value("stages").toObject();
    require(measured.value("writes").toArray() == QJsonArray({ 1, 1 })
                    && measured.value("ends").toArray() == QJsonArray({ 1, 1 }),
            "events are counted from mark to mark_end only");
    for (const auto *stage : { "cpu_ms", "main_cpu_ms", "background_cpu_ms", "rss_end_kib",
                               "peak_kib", "block_writes" })
        require(measured.value(stage).toArray().size() == 2, stage);
    require(kobalt::fileWrites, "Finishing a resource scenario must restore file logging");
    require(results[0].value("resources").toBool()
                    && results[0].value("counts").toArray() == QJsonArray({ "ends", "writes" })
                    && results[0].value("expect").toObject() == QJsonObject{ { "writes", 0 } },
            "the result names its counts and limits for the comparison");
    // The first scenario inherits an already open book. Only reuse a book
    // for an explicit rendering mode after opening it in that mode.
    QWidget reader;
    reader.show();
    QVector<int> openedModes;
    kobalt::scenario::Host library;
    library.reader = [&] { return reader.isVisible() ? &reader : nullptr; };
    library.busy = [] { return false; };
    library.open = [&](const QString &) {
        openedModes.append(kobalt::renderingMode::overrides.last());
        reader.show();
        return true;
    };
    QJsonObject turn{ { "id", "turn" },
                      { "book", "test" },
                      { "repeat", 1 },
                      { "steps",
                        QJsonArray{ QJsonObject{ { "mark", true } },
                                    QJsonObject{ { "mark_end", true } } } },
                      { "sample", QJsonObject{ { "timer", true } } } };
    QJsonArray turns;
    for (auto mode : { "accurate", "hybrid", "hybrid" }) {
        turn["rendering"] = mode;
        turns.append(turn);
    }
    options.filter = "turn";
    options.openBook = "test";
    results = kobalt::scenario::run(QJsonObject{ { "scenarios", turns } }, library, options);
    for (const auto &result : results)
        require(result.value("status") == "completed", "rendering scenario did not complete");
    require(openedModes == QVector<int>({ 0, 2 }),
            "scenario reused an unknown or different rendering mode, or reopened a matching book");

    auto cacheStep = [&](QJsonObject step) {
        auto scenario = turn;
        scenario.remove("book");
        scenario["steps"] = QJsonArray{ step, QJsonObject{ { "mark", true } },
                                        QJsonObject{ { "mark_end", true } } };
        return kobalt::scenario::run(QJsonObject{ { "scenarios", QJsonArray{ scenario } } },
                                     library, options)
                .first()
                .value("status")
                .toString();
    };
    const auto openings = openedModes.size();
    require(cacheStep({ { "open", "test" }, { "fresh", true } }) == "skipped"
                    && openedModes.size() == openings && reader.isVisible(),
            "fresh open changed the reader without a cache removal callback");
    int removals = 0;
    library.forget = [&](const QString &) {
        ++removals;
        return true;
    };
    require(cacheStep({ { "forget", "test" } }) == "failed" && removals == 0 && reader.isVisible(),
            "the player removed caches while the book was open");
    reader.close();
    require(cacheStep({ { "forget", "test" } }) == "completed" && removals == 1,
            "the player did not remove caches after the book closed");
    library.forget = [](const QString &) { return false; };
    require(cacheStep({ { "open", "test" }, { "fresh", true } }) == "failed"
                    && openedModes.size() == openings,
            "a failed cache removal was measured as a cold open");
    require(cacheStep({ { "forget", "test" } }) == "failed",
            "a failed cache removal step reported success");
    library.reader = {};
    require(cacheStep({ { "forget", "test" } }) == "skipped" && removals == 1,
            "the player removed caches without knowing whether a book was open");
    reader.show();

    const QJsonObject opening{ { "scenarios", QJsonArray{ turn } } };
    kobalt::scenario::Host incomplete;
    int attempts = 0;
    incomplete.open = [&](const QString &) {
        ++attempts;
        return true;
    };
    options.openBook.clear();
    try {
        results = kobalt::scenario::run(opening, incomplete, options);
        require(results.first().value("status") == "skipped" && attempts == 0,
                "a host without a reader callback started an opening");
        incomplete.reader = [&] { return &reader; };
        results = kobalt::scenario::run(opening, incomplete, options);
        require(results.first().value("status") == "skipped" && attempts == 0 && reader.isVisible(),
                "a host without a busy callback changed the open reader");
        turn.remove("book");
        turn["steps"] = QJsonArray{ QJsonObject{ { "settle", true } } };
        results = kobalt::scenario::run(QJsonObject{ { "scenarios", QJsonArray{ turn } } },
                                        incomplete, options);
        require(results.first().value("status") == "skipped",
                "settle accepted a host that cannot report work in flight");
    } catch (const std::bad_function_call &) {
        require(false, "scenario player called an absent host callback");
    }
}
