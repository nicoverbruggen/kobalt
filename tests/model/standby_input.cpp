#include "platforms/kobo/common/standby_input.h"
#include "platforms/kobo/common/standby_diagnostics.h"
#include "support/diagnostics.h"
#include "check.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <qpa/qwindowsysteminterface_p.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

namespace {
int writes = 0, wakeRequests = 0, probeError = 0, requestError = 0;
bool nativeResult = true, lateInput = false, kernelWakeEvent = false;
ssize_t readResult = 4;
char bytes[4] = {};
bool holdWake = false;
std::promise<void> requestEntered, releaseRequest;
auto requestEnteredFuture = requestEntered.get_future();
auto releaseRequestFuture = releaseRequest.get_future();
std::thread delayedInput;

int wakeLock(bool request)
{
    if (!request)
        return probeError;
    ++wakeRequests;
    if (!requestError)
        kernelWakeEvent = true;
    if (holdWake) {
        requestEntered.set_value();
        releaseRequestFuture.wait();
    }
    errno = ENOSPC;
    return requestError;
}

ssize_t read(int fd, void *data, size_t count)
{
    require(fd == 42 && data == bytes && count == sizeof(bytes), "Touch read arguments changed");
    if (readResult > 0)
        std::memset(data, 'x', size_t(readResult));
    errno = EINTR;
    return readResult;
}

void input()
{
    require(kobalt_touch_read(42, bytes, sizeof(bytes)) == readResult && errno == EINTR,
            "Touch result or errno changed");
}

bool write(const QString &path, const QString &)
{
    ++writes;
    if (lateInput && path == QLatin1String("/sys/power/state")) {
        // The GUI is blocked here. Neither acquisition may need a GUI callback
        // to register the kernel event, and both reads must reach the driver.
        std::thread worker([] {
            input();
            input();
        });
        worker.join();
        if (kernelWakeEvent) {
            errno = EBUSY;
            return false;
        }
    }
    errno = nativeResult ? EINTR : EIO;
    return nativeResult;
}

bool suspend()
{
    return kobalt_power_string_to_file("/sys/power/state", "mem");
}

bool returnDuringAcquisition(const QString &path, const QString &value)
{
    if (path != QLatin1String("/sys/power/state"))
        return write(path, value);
    delayedInput = std::thread(input);
    require(requestEnteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "Input thread did not reach the kernel request");
    return true;
}

void arm()
{
    require(kobalt_power_string_to_file("/sys/power/wakeup_count", "123"),
            "Native handshake failed");
}

void settle()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
}
} // namespace

QList<QJsonObject> events(const QString &directory, const char *name)
{
    require(kobalt::flushDiagnostics(), "Diagnostic flush failed");
    QFile file(directory + "/events.jsonl");
    require(file.open(QIODevice::ReadOnly), "Diagnostic log missing");
    QList<QJsonObject> result;
    while (!file.atEnd()) {
        const auto event = QJsonDocument::fromJson(file.readLine()).object();
        if (event.value("event") == QLatin1String(name))
            result.append(event);
    }
    return result;
}

void normalLogging()
{
    QTemporaryDir logs;
    kobalt::initializeDiagnostics(logs.path(), false);
    require(!kobalt::diagnosticsActive(), "Normal logging test enabled verbose diagnostics");
    kobalt::nativePowerStringToFile = write;
    kobalt::nativeTouchRead = read;
    kobalt::standbyInputWakeLock = wakeLock;
    {
        kobalt::StandbyInputScope scope(true, "123");
        for (int retry = 0; retry < 100; ++retry)
            require(suspend() && errno == EINTR, "Fallback prevented quiet native standby");
        const auto capabilities = events(logs.path(), "standby-input-capabilities");
        require(capabilities.size() == 1 && capabilities[0].value("touch_read_hook").toBool()
                        && capabilities[0].value("probe_errno").toInt() == 0
                        && !capabilities[0].value("numeric_handshake").toBool(),
                "First protected capability evidence is wrong");
        require(events(logs.path(), "standby-input-fallback").size() == 1,
                "Fallback logged every retry");
        arm();
        require(suspend(), "Fresh handshake prevented quiet native standby");
        const auto fallback = events(logs.path(), "standby-input-fallback");
        require(fallback.size() == 2 && fallback.last().value("reason") == "none"
                        && fallback.last().value("previous_reason") == "wakeup-count-not-armed"
                        && fallback.last().value("attempts_since_last_event") == "100",
                "Fallback recovery lost the retry count");
    }
    requestError = EPERM;
    lateInput = true;
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        require(suspend() && errno == EINTR && wakeRequests == 2,
                "Failed requests changed the native return");
    }
    lateInput = false;
    settle();
    for (int retry = 0; retry < 100; ++retry) {
        kobalt::StandbyInputScope scope(true);
        arm();
        require(suspend() && wakeRequests == 2,
                "Failed bridge blocked standby or retried a wake request");
    }
    const auto failures = events(logs.path(), "standby-input-wake-failure");
    const auto results = events(logs.path(), "standby-input-wake-result");
    const auto fallback = events(logs.path(), "standby-input-fallback");
    require(failures.size() == 1 && failures[0].value("errno").toInt() == EPERM,
            "Normal wake failure missing or repeated");
    require(results.size() == 1 && results[0].value("requests") == "2"
                    && results[0].value("ok").toBool() && results[0].value("errno").toInt() == 0,
            "Normal native return evidence is wrong");
    require(fallback.size() == 3 && fallback.last().value("reason") == "wake-request-failed",
            "Sticky failure logged every retry");
    require(events(logs.path(), "standby-input-capabilities").size() == 1, "Capabilities repeated");
    for (const auto *name : { "standby-input-capabilities", "standby-input-fallback",
                              "standby-input-wake-failure", "standby-input-wake-result" })
        for (const auto &event : events(logs.path(), name))
            require(event.value("main_thread").toBool(),
                    "Normal evidence was logged on the input thread");
    require(events(logs.path(), "input-acquired").isEmpty()
                    && events(logs.path(), "standby-input-wake-request").isEmpty(),
            "Normal logging emitted per-tap evidence");
    kobalt::pauseDiagnostics(true);
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (app.arguments().contains("--normal-logging")) {
        normalLogging();
        return 0;
    }
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), { "--normal-logging" });
    const bool finished = child.waitForFinished(10000);
    if (!finished) {
        child.kill();
        child.waitForFinished();
    }
    require(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
            ("Normal logging regression failed: " + child.readAllStandardError()).constData());
    QTemporaryDir logs;
    kobalt::initializeDiagnostics(logs.path());
    std::atomic<int> acquisitionEvents{ 0 }, wakeEvents{ 0 }, wakeResultEvents{ 0 };
    std::atomic<bool> singleRequestResult{ false };
    std::atomic<bool> acquiredOnWorker{ false }, noHandshake{ false }, unavailable{ false },
            failedWake{ false };
    kobalt::setDiagnosticObserver([&](const QJsonObject &event) {
        if (event.value("event") == "input-acquired") {
            ++acquisitionEvents;
            if (!event.value("main_thread").toBool())
                acquiredOnWorker = true;
        }
        if (event.value("event") == "standby-input-wake-request") {
            ++wakeEvents;
            if (event.value("errno").toInt() == EPERM)
                failedWake = true;
        }
        if (event.value("event") == "standby-input-wake-result") {
            ++wakeResultEvents;
            singleRequestResult = event.value("requests") == "1";
        }
        if (event.value("event") == "standby-input-fallback") {
            if (event.value("reason") == "wakeup-count-not-armed")
                noHandshake = true;
            if (event.value("reason") == "input-wakeup-unavailable")
                unavailable = true;
        }
    });
    kobalt::nativePowerStringToFile = write;
    kobalt::nativeTouchRead = read;
    kobalt::standbyInputWakeLock = wakeLock;
    require(suspend() && writes == 1 && errno == EINTR, "Unscoped native writes changed");
    {
        kobalt::StandbyInputScope scope(true, "123");
        require(suspend() && errno == EINTR && writes == 2,
                "Missing handshake blocked native standby");
        arm();
        require(suspend() && errno == EINTR && writes == 4 && wakeRequests == 0,
                "Quiet standby changed or acquired a wake lock");
        require(suspend() && writes == 5, "Consumed handshake blocked native standby");
        arm();
        require(suspend(), "Fresh native handshake did not permit a retry");
        require(kobalt_power_string_to_file("/sys/power/state-extended", "0"),
                "Native cleanup changed");
        require(kobalt_power_string_to_file("/sys/power/state", "freeze"),
                "Unrelated sleep mode changed");
    }
    {
        kobalt::StandbyInputScope scope(true);
        require(suspend(), "New attempt without a handshake blocked native standby");
        require(kobalt_power_string_to_file("/sys/power/wakeup_count", "not-a-count"),
                "Native helper result changed");
        require(suspend(), "Nonnumeric count blocked native standby");
    }
    {
        kobalt::StandbyInputScope outer(true);
        arm();
        std::thread worker(input);
        worker.join();
        settle();
        // Nested alarm preparation must retain the outer baseline and count.
        kobalt::StandbyInputScope inner(true);
        const int before = writes;
        require(!suspend() && errno == EAGAIN && writes == before,
                "Input during preparation was slept over");
    }
    input();
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        require(!suspend(), "Input just before entry was slept over before Qt enqueue");
    }
    settle();
    for (const auto outcome : { ssize_t(0), ssize_t(-1) }) {
        readResult = outcome;
        kobalt::StandbyInputScope scope(true);
        arm();
        input();
        require(suspend(), "Empty or failed reads prevented standby");
    }
    readResult = 4;
    using Events = QWindowSystemInterfacePrivate;
    for (bool handshake : { false, true })
        for (auto type : { Events::Touch, Events::Mouse, Events::Key, Events::Wheel }) {
            auto *event = new Events::WindowSystemEvent(type);
            Events::windowSystemEventQueue.append(event);
            {
                kobalt::StandbyInputScope scope(true);
                if (handshake)
                    arm();
                require(!suspend() && errno == EAGAIN, "Queued platform input was slept over");
                require(Events::peekWindowSystemEvent(type) == event,
                        "Guard consumed queued input");
            }
            Events::removeWindowSystemEvent(event);
        }
    {
        auto *event = new Events::WindowSystemEvent(Events::Expose);
        Events::windowSystemEventQueue.append(event);
        kobalt::StandbyInputScope scope(true);
        arm();
        require(suspend(), "Non-input events prevented standby");
        Events::removeWindowSystemEvent(event);
    }
    {
        kobalt::StandbyInputScope scope(false);
        input();
        require(suspend() && wakeRequests == 0, "Stock or sleep-screen suspension changed");
    }
    settle();
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        nativeResult = false;
        require(!suspend() && errno == EIO, "Native suspend failure changed");
        require(!kobalt_power_string_to_file("/sys/power/wakeup_count", "124") && errno == EIO,
                "Native wakeup-count failure changed");
        nativeResult = true;
        require(suspend() && errno == EINTR, "Failed handshake blocked native standby");
    }
    probeError = ENOENT;
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        const int before = writes;
        require(suspend() && errno == EINTR && writes == before + 1,
                "Unavailable wake lock blocked native standby");
    }
    probeError = 0;
    kobalt::nativeTouchRead = nullptr;
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        const int before = writes;
        require(suspend() && writes == before + 1, "Missing raw-read hook blocked native standby");
    }
    kobalt::nativeTouchRead = read;
    // Reproduce input inside the native write, with diagnostics both enabled
    // and disabled. The request must run on the input thread in either case.
    for (bool logging : { true, false }) {
        if (!logging)
            kobalt::pauseDiagnostics(true);
        settle();
        kernelWakeEvent = false;
        lateInput = true;
        const auto before = wakeRequests;
        {
            kobalt::StandbyInputScope scope(true);
            arm();
            const int nativeBefore = writes;
            require(!suspend() && errno == EBUSY && writes == nativeBefore + 1,
                    "Touch inside the blocked native write did not cancel suspend");
            require(wakeRequests == before + 2 && bytes[0] == 'x' && bytes[3] == 'x',
                    "Late taps were lost or modified");
            lateInput = false;
            settle();
            const int after = writes;
            require(!suspend() && errno == EAGAIN && writes == after,
                    "A native retry reused a consumed wakeup check");
        }
        lateInput = false;
        input();
        require(wakeRequests == before + 2, "Cancellation epoch survived native return");
        settle();
        {
            kobalt::StandbyInputScope scope(true);
            arm();
            require(suspend() && wakeRequests == before + 2,
                    "Quiet standby did not resume after cancellation");
        }
    }
    kobalt::pauseDiagnostics(false);
    holdWake = true;
    kobalt::nativePowerStringToFile = returnDuringAcquisition;
    const int resultsBeforeHeldRequest = wakeResultEvents;
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        require(suspend(), "Native return during acquisition changed");
        require(wakeResultEvents == resultsBeforeHeldRequest + 1 && singleRequestResult,
                "Native return did not report the single unfinished wake request");
    }
    kobalt::nativePowerStringToFile = write;
    {
        // This baseline includes the new sequence, but its quiet timestamp
        // is not published yet. Only the in-progress check catches it.
        kobalt::StandbyInputScope scope(true);
        arm();
        const int before = writes;
        require(!suspend() && writes == before, "New preparation missed unfinished acquisition");
        require(wakeResultEvents == resultsBeforeHeldRequest + 1,
                "Deferred call reported the previous call's unfinished wake request");
    }
    releaseRequest.set_value();
    delayedInput.join();
    holdWake = false;
    settle();
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        require(suspend(), "Completed acquisition kept the in-progress veto active");
    }
    // Mapping still tracks acquisition in addition to the installed read hook.
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        kobalt::standbyTouchAcquired();
        require(!suspend(), "Mapping fallback stopped tracking acquisition");
    }
    settle();
    // Qt6 observes raw reads, but must not enable the Qt5 mitigation.
    kobalt::nativePowerStringToFile = nullptr;
    kobalt::nativeDiagnosticPowerStringToFile = write;
    {
        kobalt::StandbyInputScope scope(true);
        const int before = wakeRequests;
        input();
        require(kobalt_observe_power_string_to_file("/sys/power/state", "mem")
                        && wakeRequests == before,
                "Qt6 observation enabled the Qt5 mitigation");
    }
    kobalt::nativePowerStringToFile = write;
    // A late write failure cannot undo a syscall already in progress. Record
    // it and fall back to the pre-write input checks on later attempts.
    requestError = EPERM;
    kernelWakeEvent = false;
    lateInput = true;
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        require(suspend(), "Failed cancellation changed the native result");
    }
    lateInput = false;
    settle();
    {
        kobalt::StandbyInputScope scope(true);
        arm();
        const int before = wakeRequests;
        require(suspend() && wakeRequests == before,
                "Failed wake request blocked standby or kept the bridge armed");
    }
    {
        kobalt::StandbyInputScope scope(false);
        require(suspend(), "Wake request failure disabled stock sleep");
    }
    require(acquisitionEvents > 0 && acquiredOnWorker && wakeEvents >= 4 && noHandshake
                    && unavailable && failedWake,
            "Acquisition, cancellation or fallback evidence is missing");
    kobalt::setDiagnosticObserver({});
    kobalt::pauseDiagnostics(true);
}
