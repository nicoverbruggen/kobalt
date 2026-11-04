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
void setDiagnosticsSync(bool) {}
static std::function<void(const QJsonObject &)> observer;
void setDiagnosticObserver(std::function<void(const QJsonObject &)> value) { observer = std::move(value); }
void diagnostic(const char *, QJsonObject, bool) {}
void sampleDiagnosticMemory(std::function<void(QJsonObject)> done) { done({}); }
namespace kosync {
static bool paused = false;
static QJsonObject account;
bool automaticPaused() { return paused; }
void pauseAutomatic(bool value) { paused = value; }
void overrideAccount(const QJsonObject &value) { account = value; }
}
}
namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto scenario = QJsonObject{{"id", "sleep.test"}, {"opt_in", true}, {"repeat", 1},
        {"needs", QJsonArray{"sleep-persistence"}},
        {"setup", QJsonArray{QJsonObject{{"sleep_persistence", QJsonObject{{"action", "configure"}}}}}},
        {"steps", QJsonArray{QJsonObject{{"sleep_persistence", QJsonObject{{"action", "drain"}}}}}},
        {"sample", QJsonObject{{"event", "sleep-persistence"}, {"field", "total_ms"}, {"stages", true}}}};
    const auto file = [&] { return QJsonObject{{"scenarios", QJsonArray{scenario}}}; };
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
        return fail ? QJsonObject{{"error", "write failed"}} :
            QJsonObject{{"total_ms", 5}, {"encode_write_ms", 4}, {"bytes", 1234}};
    };
    require(kobalt::scenario::run(file(), host).isEmpty(), "benchmark is opt-in");
    require(cleanups == 0, "excluded benchmark has no side effects");
    kobalt::scenario::Options options;
    options.filter = "sleep";
    auto results = kobalt::scenario::run(file(), host, options);
    require(results.size() == 1 && results[0].value("status") == "failed", "write failure retained");
    require(!overrideActive && cleanups == 1, "failure restores overrides");
    require(results[0].value("details").toArray().first().toObject().value("status") == "failed",
            "failed trial retained");
    fail = false;
    scenario["warmup"] = 1;
    results = kobalt::scenario::run(file(), host, options);
    require(results[0].value("status") == "completed" && !overrideActive && cleanups == 2,
            "success restores overrides");
    require(results[0].value("samples").toArray().size() == 1 &&
            results[0].value("details").toArray().size() == 2, "warmup details excluded from samples");
    require(!results[0].value("stages").toObject().contains("bytes"), "byte count is not a timing stage");
    host.sleepPersistence = {};
    results = kobalt::scenario::run(file(), host, options);
    require(results[0].value("status") == "skipped", "unsupported host skips benchmark");

    // A KOSync scenario: an in-memory account, a wait, and a sample picked
    // from the matching event only.
    const QJsonObject fake{{"server", "https://127.0.0.1:9"}, {"automatic", true}};
    QJsonObject kosyncScenario{{"id", "kosync.test"}, {"opt_in", true}, {"repeat", 1},
        {"needs", QJsonArray{"kosync", "wifi-control"}},
        {"setup", QJsonArray{QJsonObject{{"kosync", fake}}}},
        {"steps", QJsonArray{QJsonObject{{"wait_ms", 60}},
                             QJsonObject{{"event", "main-thread-wait"}, {"where", QJsonObject{{"operation", "wifi-off"}}}}}},
        {"sample", QJsonObject{{"event", "main-thread-wait"}, {"field", "elapsed_ms"}}}};
    const auto kosyncFile = [&] { return QJsonObject{{"scenarios", QJsonArray{kosyncScenario}}}; };
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
        kobalt::observer(QJsonObject{{"event", "main-thread-wait"}, {"operation", "json-write"}, {"elapsed_ms", 7}});
        kobalt::observer(QJsonObject{{"event", "main-thread-wait"}, {"operation", "wifi-off"}, {"elapsed_ms", 42}});
    });
    const bool pausedBefore = kobalt::kosync::paused;
    QElapsedTimer elapsed;
    elapsed.start();
    results = kobalt::scenario::run(kosyncFile(), network, options);
    require(results[0].value("status") == "completed", "the KOSync scenario completes");
    require(elapsed.elapsed() >= 60, "wait_ms lets the time pass");
    require(accountDuringRun && syncDuringRun, "the scenario account and automatic sync are active during the run");
    require(results[0].value("samples").toArray() == QJsonArray{42}, "the sample comes from the matching event only");
    require(kobalt::kosync::account.isEmpty() && kobalt::kosync::paused == pausedBefore,
            "the account ends with the scenario and the run restores the pause");
    // A failed step clears the account before the next scenario starts.
    kosyncScenario["steps"] = QJsonArray{QJsonObject{{"event", "main-thread-wait"}, {"timeout", 20}}};
    const QJsonObject next{{"id", "kosync.next"}, {"opt_in", true}, {"repeat", 1},
        {"steps", QJsonArray{QJsonObject{{"mark", true}}, QJsonObject{{"wait_ms", 400}}}},
        {"sample", QJsonObject{{"timer", true}}}};
    bool clearedBeforeNext = false;
    // The failing scenario ends after about 20 ms; the next one waits 400 ms.
    QTimer::singleShot(200, [&] {
        clearedBeforeNext = kobalt::kosync::account.isEmpty() && kobalt::kosync::paused;
    });
    results = kobalt::scenario::run(QJsonObject{{"scenarios", QJsonArray{kosyncScenario, next}}}, network, options);
    require(results.size() == 2 && results[0].value("status") == "failed" && results[1].value("status") == "completed",
            "a missing event fails the scenario and the next one still runs");
    require(clearedBeforeNext, "a failure clears the account and pauses sync before the next scenario");
    require(kobalt::kosync::account.isEmpty() && kobalt::kosync::paused == pausedBefore,
            "the run restores the pause");
}
