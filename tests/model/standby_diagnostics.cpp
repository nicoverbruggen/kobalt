#include "platforms/kobo/common/standby_diagnostics.h"
#include "platforms/kobo/common/standby_input.h"
#include "support/diagnostics.h"
#include "check.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QTemporaryDir>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>

namespace {
int calls = 0;
ssize_t result = 3;
int writeError = EIO;
const char payload[] = "mem";
quint64 expectedAttempt = 0;
int readCalls = 0, powerCalls = 0;
ssize_t readResult = 4;
char readBuffer[4] = {};
bool readDuringWrite = false;
int observedPowerCalls = 0;
bool observedPowerResult = true;
bool failKernelLog = false;

int kernelLog(int operation, char *data, int limit)
{
    require(operation == 3 && limit == 65536, "Kernel-log capture changed its operation or bound");
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    errno = EPERM;
    if (failKernelLog)
        return -1;
    const char line[] = "PM: suspend exit\n";
    std::memcpy(data, line, sizeof(line) - 1);
    return sizeof(line) - 1;
}

bool observedPower(const QString &, const QString &)
{
    ++observedPowerCalls;
    errno = EINTR;
    return observedPowerResult;
}

ssize_t read(int fd, void *bytes, size_t count)
{
    ++readCalls;
    require(fd == 42 && bytes == readBuffer && count == sizeof(readBuffer),
            "Tracing changed read arguments");
    if (readResult > 0)
        std::memset(bytes, 'x', size_t(readResult));
    errno = EINTR;
    return readResult;
}

bool power(const QString &, const QString &)
{
    ++powerCalls;
    return true;
}

ssize_t write(int fd, const void *bytes, size_t count)
{
    ++calls;
    require(fd == 42 && bytes == payload && count == 3, "Tracing changed write arguments");
    require(kobalt::activePowerWrite() == expectedAttempt,
            "Input cannot correlate the active write");
    if (readDuringWrite)
        kobalt_touch_read(42, readBuffer, sizeof(readBuffer));
    errno = writeError;
    return result;
}

void fixture(const QString &path, const QByteArray &bytes)
{
    require(QDir().mkpath(QFileInfo(path).path()), "Cannot create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "Cannot write fixture");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create test directory");
    kobalt::initializeDiagnostics(temporary.path() + "/logs");
    QMutex mutex;
    std::vector<QJsonObject> events;
    kobalt::setDiagnosticObserver([&](const QJsonObject &event) {
        QMutexLocker lock(&mutex);
        events.push_back(event);
    });
    kobalt::nativePowerWrite = write;
    require(kobalt_power_write(42, payload, 3) == 3 && errno == EIO && calls == 1,
            "Unscoped write behavior changed");
    for (const auto outcome : { ssize_t(3), ssize_t(1), ssize_t(-1) }) {
        result = outcome;
        writeError = outcome < 0 ? EINTR : ENOSPC;
        const int before = calls;
        expectedAttempt = 17;
        {
            kobalt::PowerWriteTrace trace("/sys/power/state", expectedAttempt);
            require(kobalt_power_write(42, payload, 3) == outcome && errno == writeError,
                    "Raw write result or errno changed");
            require(kobalt::activePowerWrite() == 0, "Write stayed active after return");
        }
        require(errno == writeError && calls == before + 1,
                "Tracing retried a short or interrupted write");
    }
    expectedAttempt = 23;
    {
        kobalt::PowerWriteTrace trace("/sys/power/wakeup_count", expectedAttempt);
        kobalt::PowerWriteTrace nested("/unrelated", 24);
        for (int i = 0; i < 10; ++i) {
            // Calls beyond the record limit still pass through exactly once.
            if (i >= 8)
                expectedAttempt = 0;
            kobalt_power_write(42, payload, 3);
        }
    }
    require(calls == 14, "Record limit swallowed a native write");
    kobalt::nativeTouchRead = read;
    kobalt::nativePowerStringToFile = power;
    for (const auto outcome : { ssize_t(4), ssize_t(1), ssize_t(0), ssize_t(-1) }) {
        readResult = outcome;
        const int before = readCalls;
        kobalt::StandbyInputScope scope(false);
        require(kobalt_touch_read(42, readBuffer, sizeof(readBuffer)) == outcome && errno == EINTR,
                "Touch tracing changed read result or errno");
        require(readCalls == before + 1,
                "Touch tracing retried a short, empty or interrupted read");
        if (outcome > 0)
            require(readBuffer[0] == 'x', "Touch tracing changed input bytes");
        require(kobalt_power_string_to_file("/sys/power/state", "mem"),
                "Touch read changed unguarded native suspend");
    }
    require(powerCalls == 4, "Diagnostic touch observation suppressed a power helper call");
    readResult = 4;
    readDuringWrite = true;
    expectedAttempt = 31;
    {
        kobalt::PowerWriteTrace trace("/sys/power/state", expectedAttempt);
        kobalt_power_write(42, payload, 3);
    }
    readDuringWrite = false;
    kobalt::nativeDiagnosticPowerStringToFile = observedPower;
    {
        kobalt::StandbyInputScope scope(true);
        kobalt::standbyTouchAcquired();
        for (bool outcome : { true, false }) {
            observedPowerResult = outcome;
            require(kobalt_observe_power_string_to_file("/sys/power/state", "mem") == outcome
                            && errno == EINTR,
                    "Observation-only power hook changed result or errno");
        }
    }
    require(observedPowerCalls == 2,
            "Observation-only hook guarded input or retried a native call");
    for (bool failure : { false, true }) {
        failKernelLog = failure;
        errno = EINTR;
        kobalt::collectStandbyKernelLog(80 + int(failure), "returned", 1, kernelLog);
        require(errno == EINTR, "Kernel log capture changed caller errno");
    }
    {
        QMutexLocker lock(&mutex);
        int samples = 0;
        for (const auto &event : events) {
            if (event.value("event") != "standby-kernel-log")
                continue;
            const auto attempt = event.value("attempt").toString().toInt();
            if (attempt != 80 && attempt != 81)
                continue;
            ++samples;
            require(event.value("read_exit_monotonic_ms").toDouble()
                            >= event.value("read_enter_monotonic_ms").toDouble() + 2,
                    "Kernel log read duration was not bracketed");
            require(event.contains("read_enter_boot_ms") && event.contains("read_exit_boot_ms")
                            && event.contains("read_enter_realtime_ms")
                            && event.contains("read_exit_realtime_ms"),
                    "Kernel log capture lost fallback clocks");
            require(event.value("read_errno").toInt() == (attempt == 81 ? EPERM : 0)
                            && event.value("completeness") == "unknown"
                            && !event.contains("buffer_full")
                            && event.value("read_limit_bytes").toInt() == 65536,
                    "Short or failed kernel log read was presented as complete");
        }
        require(samples == 2, "Kernel log capture omitted a result");
    }
    const auto root = temporary.path() + "/kernel";
    fixture(root + "/proc/sys/kernel/osrelease", "2.6.35.3\n");
    fixture(root + "/proc/mounts",
            "none /sys sysfs rw 0 0\n/private/book /private/mount ext4 rw 0 0\n");
    fixture(root + "/sys/kernel/debug/wakeup_sources",
            "name active_count event_count wakeup_count expire_count active_since total_time\n"
            "touchscreen 4 5 6 0 0 123\n"
            "broken 1 2\n"
            "private bad bad bad bad bad bad\n");
    fixture(root + "/sys/power/pm_wakeup_irq", "42\n");
    const auto device = root + "/sys/devices/i2c/touch";
    fixture(device + "/input/input0/name", "Example touch\n");
    fixture(device + "/input/input0/uniq", "DO-NOT-LOG-SERIAL");
    fixture(device + "/input/input0/capabilities/abs", "6600000\n");
    fixture(device + "/input/input0/capabilities/ev", "b\n");
    fixture(device + "/power/wakeup", "enabled\n");
    fixture(device + "/power/wakeup_count", "123\n");
    fixture(device + "/power/wakeup_hit_count", "7\n");
    fixture(device + "/power/wakeup_active", "invalid\n");
    fixture(device + "/power/wakeup_total_time_ms", QByteArray(100, '9'));
    fixture(root + "/sys/power/wakeup_count", "DO-NOT-READ-GLOBAL-HANDSHAKE\n");
    fixture(root + "/sys/kernel/wakeup_reasons/last_resume_reason", "42 touchscreen\n");
    fixture(root + "/sys/kernel/wakeup_reasons/last_suspend_time", "0.025 12.500\n");
    require(QDir().mkpath(root + "/sys/bus/i2c/drivers/example-touch"),
            "Cannot create driver fixture");
    require(QFile::link(root + "/sys/bus/i2c/drivers/example-touch", device + "/driver"),
            "Cannot link driver");
    require(QDir().mkpath(root + "/sys/class/input/event0"), "Cannot create input fixture");
    require(QFile::link(device + "/input/input0", root + "/sys/class/input/event0/device"),
            "Cannot link input");
    std::atomic<bool> onWorker{ false };
    const auto caller = std::this_thread::get_id();
    require(kobalt::sampleDiagnostics([&] {
                onWorker = std::this_thread::get_id() != caller;
                kobalt::collectStandbyEvidence(root, 0, "startup");
            }),
            "Sampling was not queued");
    require(kobalt::flushDiagnostics() && onWorker, "Evidence did not run on the worker");
    // Oversize files are bounded; an incomplete last row is never parsed.
    fixture(root + "/sys/kernel/debug/wakeup_sources", QByteArray(40000, 'x'));
    require(kobalt::sampleDiagnostics(
                    [&] { kobalt::collectStandbyEvidence(root, 18, "returned"); }),
            "Cannot queue oversized sample");
    require(kobalt::flushDiagnostics(), "Oversized sample did not finish");
    // Linux 3.0 exports hit_count instead of wakeup_count. Preserve that name.
    fixture(root + "/sys/kernel/debug/wakeup_sources",
            "name active_count event_count hit_count active_since total_time max_time last_change\n"
            "legacy-touch 1 2 3 0 4 5 6\n");
    require(kobalt::sampleDiagnostics([&] { kobalt::collectStandbyEvidence(root, 19, "returned"); })
                    && kobalt::flushDiagnostics(),
            "Cannot collect legacy wakeup table");
    require(QFile::remove(root + "/sys/kernel/debug/wakeup_sources"),
            "Cannot remove wakeup fixture");
    require(kobalt::sampleDiagnostics([&] { kobalt::collectStandbyEvidence(root, 20, "returned"); })
                    && kobalt::flushDiagnostics(),
            "Cannot collect missing-debugfs fallback");
    bool raw = false, omitted = false, source = false, missing = false, truncated = false,
         driver = false;
    bool legacySource = false, fallback = false, capabilities = false, resumeReason = false;
    int touchReads = 0;
    bool touchDuringWrite = false;
    {
        QMutexLocker lock(&mutex);
        for (const auto &event : events) {
            const auto name = event.value("event").toString();
            require(!QJsonDocument(event).toJson().contains("DO-NOT-LOG-SERIAL"),
                    "Inventory leaked serial data");
            require(!QJsonDocument(event).toJson().contains("DO-NOT-READ-GLOBAL-HANDSHAKE")
                            && !QJsonDocument(event).toJson().contains("/private/book"),
                    "Fallback leaked unrelated kernel data");
            if (name == "standby-kernel-capabilities") {
                capabilities = event.value("kernel_release") == "2.6.35.3"
                        && !event.value("debugfs_mounted").toBool();
                require(event.value("boot_clock").toBool()
                                || event.value("realtime_fallback").toBool(),
                        "Missing boot clock has no declared fallback");
            }
            if (name == "standby-wakeup-source" && event.value("name") == "legacy-touch")
                legacySource = event.value("hit_count") == "3" && !event.contains("wakeup_count");
            if (name == "standby-input-wakeup" && event.value("attempt") == "20") {
                const auto counters = event.value("counters").toObject();
                fallback = event.value("counter_parent_found").toBool()
                        && event.value("parent_depth").toInt() == 2
                        && event.value("counter_scope") == "kernel-dependent"
                        && counters.value("wakeup_count").toObject().value("value") == "123"
                        && counters.value("wakeup_hit_count").toObject().value("value") == "7";
                require(counters.value("wakeup_abort_count").toObject().value("read_errno").toInt()
                                        == ENOENT
                                && !counters.value("wakeup_active")
                                            .toObject()
                                            .value("numeric")
                                            .toBool()
                                && counters.value("wakeup_total_time_ms")
                                           .toObject()
                                           .value("truncated")
                                           .toBool(),
                        "Fallback hid missing, malformed or oversized counters");
                require(QJsonDocument(event).toJson(QJsonDocument::Compact).size() < 4096,
                        "Fallback exceeded the diagnostic record limit");
            }
            if (name == "standby-kernel-evidence"
                && event.value("path") == "/sys/kernel/wakeup_reasons/last_resume_reason")
                resumeReason = event.value("value") == "42 touchscreen";
            if (name == "input-driver-read") {
                ++touchReads;
                require(event.value("result").toInt() > 0
                                && !event.value("fd_path_verified").toBool(),
                        "Touch trace claimed an empty read or an unverified device");
                require(event.value("read_exit_monotonic_ms").toDouble()
                                >= event.value("read_enter_monotonic_ms").toDouble(),
                        "Touch acquisition clocks are reversed");
                require(!event.contains("payload") && !event.contains("coordinates"),
                        "Touch trace collected input contents");
                if (event.value("power_write_attempt_enter") == "31"
                    && event.value("power_write_attempt_exit") == "31")
                    touchDuringWrite = true;
            }
            if (name == "power-write") {
                raw = true;
                require(!event.value("fd_path_verified").toBool(),
                        "Invalid fd was reported as verified sysfs");
                require(event.value("write_exit_monotonic_ms").toDouble()
                                >= event.value("write_enter_monotonic_ms").toDouble(),
                        "Write clocks are reversed");
            }
            if (name == "power-write-observation" && event.value("omitted").toInt() == 2)
                omitted = true;
            if (name == "standby-wakeup-source" && event.value("name") == "touchscreen")
                source = event.value("wakeup_count") == "6" && event.value("event_count") == "5";
            if (name == "standby-kernel-evidence" && event.value("read_errno").toInt() == ENOENT)
                missing = true;
            if (name == "standby-wakeup-sources" && event.value("truncated").toBool()) {
                truncated = true;
                require(event.value("read_exit_monotonic_ms").toDouble()
                                >= event.value("read_enter_monotonic_ms").toDouble(),
                        "Evidence read clocks are reversed");
            }
            if (name == "standby-input-device")
                driver = event.value("driver") == "example-touch"
                        && event.value("wakeup").toObject().value("value") == "enabled";
        }
    }
    require(raw && omitted && source && missing && truncated && driver,
            "Write, driver or wakeup evidence missing");
    require(legacySource && fallback && capabilities && resumeReason,
            "Legacy counters, capabilities or resume evidence missing");
    require(touchReads == 3 && touchDuringWrite,
            "Touch trace missed acquisition/write correlation or logged empty/error reads");
    const auto messages =
            kobalt::standbyKernelMessages("<6>[ 123.4] PM: suspend entry 2026-11-03\n"
                                          "<6>[ 123.5] PM: Syncing filesystems ... done.\n"
                                          "<6>[ 123.6] unrelated private data PM: suspend\n"
                                          "<6>[ 124.0] Restarting tasks ... done.\n");
    require(messages.size() == 3 && messages[0].contains("123.4"),
            "PM filtering lost clocks or included unrelated kernel data");
    const auto mtkMessages = kobalt::standbyKernelMessages(
            "<6>[ 123.4]  (0)[343:nickel]PM: suspend entry 2026-11-03\n"
            "<3>[ 123.5] -(1)[343:nickel]PM: Some devices failed to suspend, or early wake event detected\n"
            "<6>[ 124.0] -(0)Restarting tasks ... done.\n");
    require(mtkMessages.size() == 3, "MediaTek CPU/task prefixes hide PM messages");
    // Source-derived examples from Kobo's Sage/Elipsa 4.9 wakeup.c. Their
    // production kernels disable CONFIG_PRINTK_TIME; no timestamp is required.
    QJsonObject sunxiStatus;
    QJsonArray sunxiRejected;
    const auto sunxi =
            kobalt::standbyKernelMessages("<6>PM: Wakeup pending, aborting suspend\n"
                                          "<6>active wakeup source: elants_i2c\n"
                                          "<6>last active wakeup source: cyttsp5\n"
                                          "<4>Resume caused by more than 32 IRQs\n"
                                          "unknown-prefix active wakeup source: elants_i2c\n"
                                          "unrelated private message\n",
                                          &sunxiStatus, &sunxiRejected);
    require(sunxi.size() == 4 && sunxiRejected.size() == 1
                    && sunxiStatus.value("candidate_lines_rejected").toInt() == 1,
            "Sunxi wake-source messages disappeared or unrelated lines were accepted");
    // Captured on Clara BW firmware 4.46.23852. Preserve the original lines,
    // including the printk dot, timestamps, task names and trailing spaces.
    QFile capturedFile(
            QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("../fixtures/clara-bw-pm.json"));
    require(capturedFile.open(QIODevice::ReadOnly), "Cannot read captured Clara PM fixture");
    const auto capturedLines = QJsonDocument::fromJson(capturedFile.readAll()).array();
    require(capturedLines.size() == 16, "Captured PM fixture is incomplete");
    QStringList expectedMessages;
    for (const auto &line : capturedLines)
        expectedMessages.append(line.toString());
    const auto capturedLog = expectedMessages.join('\n').toLatin1() + '\n';
    QJsonObject capturedStatus;
    QJsonArray capturedRejected;
    const auto capturedMessages =
            kobalt::standbyKernelMessages(capturedLog, &capturedStatus, &capturedRejected);
    require(capturedMessages == expectedMessages && capturedRejected.isEmpty()
                    && capturedStatus.value("messages_matched").toInt() == 16
                    && capturedStatus.value("candidate_lines_rejected").toInt() == 0,
            "The Clara's dot prefix hides or alters captured PM messages");
    QJsonObject filter;
    const auto filtered = kobalt::standbyKernelMessages(
            "<3>[ 124.1]  (0)[343:nickel]PM: Device 14000000.hwtcon failed to suspend: error -1\n"
            "<6>[ 124.2] -(1)Resume caused by IRQ 90\n"
            "<6>[ 124.3]  (0)[343:nickel]unrelated PM: suspend\n"
            "<bad>[ 124.4] PM: suspend\n"
            "<6>[ 124.5] -(not-cpu)PM: suspend\n"
            "<6>[ 124.6]  (0)[343:nickel PM: suspend\n"
            "<6>[ 124.7] x(0)[343:nickel]PM: suspend\n"
            "<6>[ 124.8] ..(0)[343:nickel]PM: suspend\n"
            "PM: " + QByteArray(600, 'x')
                    + "\n",
            &filter);
    require(filtered.size() == 2 && filtered[0].contains("14000000.hwtcon")
                    && filtered[1].contains("IRQ 90"),
            "PM filter lost failure/wakeup evidence or accepted unrelated messages");
    require(filter.value("lines_examined").toInt() == 9
                    && filter.value("oversized_lines").toInt() == 1
                    && filter.value("candidate_lines_rejected").toInt() == 7
                    && filter.value("messages_matched").toInt() == 2,
            "PM filter rejection counts are missing or incorrect");
    QByteArray rejectedLog("<6>[ 125.0] PM: suspend entry\nUNRELATED-PRIVATE-TEXT\n");
    for (int i = 0; i < 12; ++i)
        rejectedLog += "unknown-" + QByteArray::number(i) + " PM: suspend\n";
    rejectedLog += QByteArray(1, '\0') + QByteArray(240, '\1') + "PM: control bytes\n";
    rejectedLog += "PM: " + QByteArray(700, 'x') + '\n';
    QJsonArray rejected;
    const auto accepted = kobalt::standbyKernelMessages(rejectedLog, &filter, &rejected);
    require(accepted.size() == 1 && rejected.size() == 8
                    && filter.value("candidate_lines_rejected").toInt() == 14
                    && filter.value("rejected_lines_captured").toInt() == 8
                    && filter.value("rejected_lines_omitted").toInt() == 6,
            "Rejected PM capture exceeded its limit or lost omission counts");
    require(rejected[0].toObject().value("message").toString().startsWith("unknown-6"),
            "Rejected PM capture did not retain the latest candidates");
    const auto control = rejected[6].toObject(), oversizedLine = rejected[7].toObject();
    require(control.value("prefix_hex").toString().startsWith("0001")
                    && control.value("message").toString().size() == 256
                    && control.value("message").toString()[0].unicode() == 0
                    && control.value("line_truncated").toBool(),
            "Rejected PM prefix bytes were lost or unbounded");
    require(oversizedLine.value("message").toString().size() == 256
                    && oversizedLine.value("line_bytes").toInt() == 704
                    && oversizedLine.value("reason") == "oversized"
                    && oversizedLine.value("line_truncated").toBool(),
            "Oversized PM candidate was not bounded and identified");
    require(kobalt::sampleDiagnostics([&] {
                kobalt::recordStandbyKernelLog(
                        rejectedLog,
                        kobalt::qt::object({ { "attempt", "99" }, { "phase", "returned" } }));
            }) && kobalt::flushDiagnostics(),
            "Rejected PM capture did not finish on the logging worker");
    {
        QMutexLocker lock(&mutex);
        int captured = 0;
        for (const auto &event : events) {
            if (event.value("attempt") != "99")
                continue;
            const auto serialized = QJsonDocument(event).toJson(QJsonDocument::Compact);
            require(serialized.size() < 4096 && !serialized.contains("UNRELATED-PRIVATE-TEXT"),
                    "Rejected PM capture exceeded the log record limit or included unrelated text");
            require(!event.value("main_thread").toBool(),
                    "Rejected PM capture ran on the caller thread");
            if (event.value("event") == "standby-kernel-rejected")
                ++captured;
        }
        require(captured == 8, "Rejected PM records did not reach the diagnostic writer");
    }
    rejected.append(QJsonObject());
    kobalt::standbyKernelMessages("unrelated\n", &filter, &rejected);
    require(rejected.isEmpty() && filter.value("rejected_lines_captured").toInt() == 0,
            "Rejected PM capture leaked entries from a previous sample");
    QFile context(temporary.path() + "/logs/session-context.json");
    require(context.open(QIODevice::ReadOnly), "Cannot read session context");
    const auto contextBytes = context.readAll();
    require(contextBytes.contains("example-touch") && contextBytes.contains("standby_kernel")
                    && contextBytes.contains("2.6.35.3"),
            "Driver or kernel capabilities lost from session context");
    kobalt::setDiagnosticObserver({});
    kobalt::pauseDiagnostics(true);
    require(!kobalt::sampleDiagnostics([] { }), "Paused diagnostics still sample the kernel");
    observedPowerResult = false;
    require(!kobalt_observe_power_string_to_file("/sys/power/state", "mem") && errno == EINTR
                    && observedPowerCalls == 3,
            "Disabled power observation changed native behavior");
    readResult = -1;
    const int before = readCalls;
    require(kobalt_touch_read(42, readBuffer, sizeof(readBuffer)) == -1 && errno == EINTR
                    && readCalls == before + 1,
            "Disabled touch tracing changed native read behavior");
}
