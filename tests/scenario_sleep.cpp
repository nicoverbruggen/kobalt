#include "scenario/player.h"
#include "support/diagnostics.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

// This check drives the player's failure paths without a reader or hardware.
namespace kobalt {
void setDiagnosticsSync(bool) { }

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

// Checks the scenario player's sleep benchmark, KOSync account, `native-sync`, `wifi` and event steps, real-time waits and keep-awake, with a controlled host.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
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
    radio.wifiUp = [&] { return up; };
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
    radio.wifiOff = [] { return QString(); };
    auto stuck = wifiScenario;
    stuck["steps"] = QJsonArray{ QJsonObject{ { "wifi", "off" }, { "timeout", 50 } },
                                 QJsonObject{ { "mark", true } } };
    results = kobalt::scenario::run(QJsonObject{ { "scenarios", QJsonArray{ stuck } } }, radio,
                                    options);
    require(results[0].value("status") == "failed"
                    && results[0].value("reason").toString().contains("Wi-Fi stayed on"),
            "a radio that stays up fails the sample");
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
    store.nativeSyncing = [&] { return syncing; };
    store.nativeSync = [&] {
        startedWhileSyncing = syncing;
        ++starts;
        // The first save ends before the page turn and is passed over; the
        // second ends after the step that waits for the turn.
        kobalt::observer(save(11));
        QTimer::singleShot(20, [] {
            kobalt::observer(QJsonObject{ { "event", "page-turn-painted" }, { "elapsed_ms", 5 } });
        });
        QTimer::singleShot(150, [&] { kobalt::observer(save(22)); });
        QTimer::singleShot(200, [] {
            kobalt::observer(QJsonObject{ { "event", "native-sync-end" }, { "elapsed_ms", 33 } });
        });
        return QString();
    };
    QTimer::singleShot(30, [&] { syncing = false; });
    results = kobalt::scenario::run(syncFile(), store, options);
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
}
