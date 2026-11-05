#include "standby_diagnostics.h"
#include "standby_input.h"
#include "support/diagnostics.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#  include <sys/klog.h>
#endif

namespace kobalt {
namespace {
struct Clocks
{
    qint64 monotonic = -1, boot = -1, realtime = -1;
};

struct PreserveErrno
{
    int saved = errno;

    ~PreserveErrno() { errno = saved; }
};

qint64 milliseconds(clockid_t clock)
{
    timespec time{};
    return clock_gettime(clock, &time) == 0 ? qint64(time.tv_sec) * 1000 + time.tv_nsec / 1000000
                                            : -1;
}

Clocks clocks()
{
    Clocks result;
    result.monotonic = milliseconds(CLOCK_MONOTONIC);
#ifdef CLOCK_BOOTTIME
    result.boot = milliseconds(CLOCK_BOOTTIME);
#endif
    // Old kernels lack CLOCK_BOOTTIME. Wall time is useful evidence, but
    // clock corrections prevent treating its difference as exact sleep time.
    if (result.boot < 0)
        result.realtime = milliseconds(CLOCK_REALTIME);
    return result;
}

struct Write
{
    Clocks enter, exit;
    ssize_t result = 0;
    size_t count = 0;
    int fd = -1, error = 0;
    bool verified = false;
};

struct Trace
{
    bool active = false;
    QByteArray path;
    quint64 attempt = 0;
    unsigned count = 0, omitted = 0;
    Write writes[8];
};

thread_local Trace trace;
std::atomic<quint64> writingAttempt{ 0 };
std::atomic<unsigned> pendingSamples{ 0 };
std::atomic<bool> evidenceEnabled{ false };

// Never read /sys/power/wakeup_count: the kernel can block its handshake
// until wakeup events finish. Per-device counters do not use that handshake.
QByteArray readEvidence(const QString &path, int limit, QJsonObject &fields)
{
    const auto begin = clocks();
    fields["read_enter_monotonic_ms"] = double(begin.monotonic);
    fields["read_enter_boot_ms"] = double(begin.boot);
    fields["read_enter_realtime_ms"] = double(begin.realtime);
    auto finished = [&] {
        const auto end = clocks();
        fields["read_exit_monotonic_ms"] = double(end.monotonic);
        fields["read_exit_boot_ms"] = double(end.boot);
        fields["read_exit_realtime_ms"] = double(end.realtime);
    };
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fields["read_errno"] = errno;
        finished();
        return {};
    }
    QByteArray bytes(limit + 1, '\0');
    int used = 0, error = 0;
    while (used <= limit) {
        const auto size = ::read(fd, bytes.data() + used, size_t(limit + 1 - used));
        if (size < 0) {
            error = errno;
            break;
        }
        if (!size)
            break;
        used += int(size);
    }
    ::close(fd);
    finished();
    fields["read_errno"] = error;
    fields["truncated"] = used > limit;
    bytes.resize(qMin(used, limit));
    return bytes;
}

QJsonObject sampleFields(quint64 attempt, const char *phase, quint64 sample)
{
    const auto now = clocks();
    return qt::object({ { "attempt", QString::number(attempt) },
                        { "phase", phase },
                        { "sample", QString::number(sample) },
                        { "sample_monotonic_ms", double(now.monotonic) },
                        { "sample_boot_ms", double(now.boot) },
                        { "sample_realtime_ms", double(now.realtime) } });
}

void kernelCapabilities(const QString &root)
{
    QJsonObject releaseStatus, mountStatus;
    const auto release =
            readEvidence(root + "/proc/sys/kernel/osrelease", 128, releaseStatus).trimmed();
    const auto mounts = readEvidence(root + "/proc/mounts", 16384, mountStatus);
    bool mounted = false;
    const auto lines = mounts.split('\n');
    for (int i = 0; i + 1 < lines.size(); ++i) {
        const auto columns = lines[i].simplified().split(' ');
        if (columns.value(1) == "/sys/kernel/debug" && columns.value(2) == "debugfs")
            mounted = true;
    }
    const auto now = clocks();
    diagnostic("standby-kernel-capabilities",
               { { "kernel_release", QString::fromLatin1(release) },
                 { "release_read_errno", releaseStatus.value("read_errno") },
                 { "release_truncated", releaseStatus.value("truncated") },
                 { "debugfs_mounted", mounted },
                 { "mounts_read_errno", mountStatus.value("read_errno") },
                 { "mounts_truncated", mountStatus.value("truncated") },
                 { "monotonic_clock", now.monotonic >= 0 },
                 { "boot_clock", now.boot >= 0 },
                 { "realtime_fallback", now.realtime >= 0 } },
               false);
}

void inputWakeupCounters(const QString &root, quint64 attempt, const char *phase, quint64 sample)
{
    const QDir inputs(root + "/sys/class/input");
    const auto devices = inputs.entryList(QStringList() << "event*",
                                          QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const auto sys = QFileInfo(root + "/sys").canonicalFilePath();
    auto summary = sampleFields(attempt, phase, sample);
    summary["devices"] = devices.size();
    summary["truncated"] = devices.size() > 32;
    diagnostic("standby-input-wakeups", summary, false);
    for (const auto &device : devices.mid(0, 32)) {
        auto fields = sampleFields(attempt, phase, sample);
        fields["event_device"] = device;
        // Some 2.6 kernels expose a global count at every device path. Even
        // distinct paths must not be presented as independent wake causes.
        fields["counter_scope"] = QStringLiteral("kernel-dependent");
        QString path = QFileInfo(inputs.filePath(device) + "/device").canonicalFilePath();
        bool found = false;
        for (int depth = 0; depth < 8 && !sys.isEmpty() && path.startsWith(sys + '/'); ++depth) {
            if (QFile::exists(path + "/power/wakeup_count")) {
                fields["parent_depth"] = depth;
                QJsonObject counters;
                for (const auto *name : { "wakeup_count", "wakeup_active_count", "wakeup_hit_count",
                                          "wakeup_abort_count", "wakeup_expire_count",
                                          "wakeup_active", "wakeup_total_time_ms" }) {
                    QJsonObject status;
                    const auto bytes = readEvidence(path + "/power/" + name, 64, status).trimmed();
                    bool numeric = false;
                    bytes.toULongLong(&numeric);
                    counters[QString::fromLatin1(name)] =
                            qt::object({ { "value", QString::fromLatin1(bytes) },
                                         { "numeric", numeric },
                                         { "read_errno", status.value("read_errno") },
                                         { "truncated", status.value("truncated") } });
                }
                fields["counters"] = counters;
                found = true;
                break;
            }
            path = QFileInfo(path).path();
        }
        fields["counter_parent_found"] = found;
        const auto end = clocks();
        fields["sample_exit_monotonic_ms"] = double(end.monotonic);
        fields["sample_exit_boot_ms"] = double(end.boot);
        fields["sample_exit_realtime_ms"] = double(end.realtime);
        diagnostic("standby-input-wakeup", fields, false);
    }
}

void inventory(const QString &root)
{
    const QDir inputs(root + "/sys/class/input");
    const auto devices = inputs.entryList(QStringList() << "event*",
                                          QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    diagnostic("standby-input-inventory",
               { { "available", inputs.exists() },
                 { "devices", devices.size() },
                 { "truncated", devices.size() > 32 } },
               false);
    for (const auto &device : devices.mid(0, 32)) {
        const auto input = inputs.filePath(device) + "/device";
        auto fields = qt::object({ { "event_device", device } });
        auto read = [&](const QString &path) {
            QJsonObject status;
            const auto bytes = readEvidence(path, 256, status);
            return qt::object(
                    { { "value", QString::fromLatin1(bytes.trimmed()) }, { "status", status } });
        };
        fields["name"] = read(input + "/name");
        fields["capabilities_abs"] = read(input + "/capabilities/abs");
        fields["capabilities_ev"] = read(input + "/capabilities/ev");
        // The driver and wakeup setting usually belong to the I2C parent,
        // not the input class node. Never collect phys, uniq or serial fields.
        QString path = QFileInfo(input).canonicalFilePath();
        const auto sys = QFileInfo(root + "/sys").canonicalFilePath();
        for (int depth = 0; depth < 8 && !sys.isEmpty() && path.startsWith(sys + '/'); ++depth) {
            const auto driver = QFileInfo(path + "/driver").symLinkTarget();
            if (!driver.isEmpty() && !fields.contains("driver"))
                fields["driver"] = QFileInfo(driver).fileName();
            if (QFile::exists(path + "/power/wakeup") && !fields.contains("wakeup"))
                fields["wakeup"] = read(path + "/power/wakeup");
            path = QFileInfo(path).path();
        }
        fields["driver_found"] = fields.contains("driver");
        diagnostic("standby-input-device", fields, false);
    }
}
} // namespace

ssize_t (*nativePowerWrite)(int, const void *, size_t) = nullptr;
ssize_t (*nativeTouchRead)(int, void *, size_t) = nullptr;

quint64 activePowerWrite()
{
    return writingAttempt.load(std::memory_order_relaxed);
}

extern "C" __attribute__((visibility("default"))) ssize_t kobalt_touch_read(int fd, void *bytes,
                                                                            size_t count)
{
    const bool observing = diagnosticsActive();
    const int priorError = errno;
    const auto enter = observing ? clocks() : Clocks{};
    const auto enterAttempt = activePowerWrite();
    errno = priorError;
    const auto result = nativeTouchRead(fd, bytes, count);
    const int error = errno;
    // This import belongs only to the touchscreen plugin. Count every
    // successful read before mapping, which another mod can replace. Qt6
    // has no guarded helper and remains observation-only.
    StandbyAcquisition acquisition;
    if (result > 0 && nativePowerStringToFile)
        acquisition = acquireStandbyTouch();
    if (!observing) {
        errno = error;
        return result;
    }
    const auto exit = clocks();
    const auto exitAttempt = activePowerWrite();
    if (acquisition.sequence)
        recordStandbyAcquisition(acquisition, "touch-read");
    if (result > 0) {
        static std::atomic<quint64> sequence{ 0 };
        auto fields = qt::object(
                { { "sequence", QString::number(++sequence) },
                  { "fd", fd },
                  { "requested_bytes", double(count) },
                  { "result", double(result) },
                  { "read_exit_after_wake_request", acquisition.cancellationAttempt != 0 },
                  { "read_enter_monotonic_ms", double(enter.monotonic) },
                  { "read_enter_boot_ms", double(enter.boot) },
                  { "read_exit_monotonic_ms", double(exit.monotonic) },
                  { "read_exit_boot_ms", double(exit.boot) },
                  { "read_enter_realtime_ms", double(enter.realtime) },
                  { "read_exit_realtime_ms", double(exit.realtime) },
                  { "power_write_attempt_enter", QString::number(enterAttempt) },
                  { "power_write_attempt_exit", QString::number(exitAttempt) } });
        // Identify only input-device descriptors; never record input payloads
        // or arbitrary paths. Verification is diagnostic work, after the
        // input-thread cancellation request.
        bool verified = false;
#ifdef Q_OS_LINUX
        char link[64], target[128];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const auto length = ::readlink(link, target, sizeof(target));
        const QByteArray path(target, length > 0 ? int(length) : 0);
        const QByteArray prefix("/dev/input/event");
        if (length > prefix.size() && length < ssize_t(sizeof(target)) && path.startsWith(prefix)) {
            verified = true;
            for (int i = prefix.size(); i < path.size(); ++i)
                if (path[i] < '0' || path[i] > '9')
                    verified = false;
            if (verified)
                fields["event_device"] = QString::fromLatin1(path.mid(11));
        }
#endif
        fields["fd_path_verified"] = verified;
        diagnostic("input-driver-read", fields, false);
    }
    errno = error;
    return result;
}

PowerWriteTrace::PowerWriteTrace(const QString &path, quint64 attempt)
{
    const PreserveErrno preserve;
    if (trace.active || !diagnosticsActive())
        return;
    trace = {};
    trace.path = path.toLatin1();
    trace.attempt = attempt;
    trace.active = active = true;
}

PowerWriteTrace::~PowerWriteTrace()
{
    if (!active)
        return;
    const int savedError = errno;
    trace.active = false;
    for (unsigned i = 0; i < trace.count; ++i) {
        const auto &write = trace.writes[i];
        diagnostic("power-write",
                   { { "attempt", QString::number(trace.attempt) },
                     { "path", QString::fromLatin1(trace.path) },
                     { "index", int(i) },
                     { "fd", write.fd },
                     { "fd_path_verified", write.verified },
                     { "bytes", double(write.count) },
                     { "result", double(write.result) },
                     { "errno", write.result < 0 ? write.error : 0 },
                     { "write_enter_monotonic_ms", double(write.enter.monotonic) },
                     { "write_enter_boot_ms", double(write.enter.boot) },
                     { "write_exit_monotonic_ms", double(write.exit.monotonic) },
                     { "write_exit_boot_ms", double(write.exit.boot) },
                     { "write_enter_realtime_ms", double(write.enter.realtime) },
                     { "write_exit_realtime_ms", double(write.exit.realtime) } },
                   false);
    }
    diagnostic("power-write-observation",
               { { "attempt", QString::number(trace.attempt) },
                 { "path", QString::fromLatin1(trace.path) },
                 { "hook_available", bool(nativePowerWrite) },
                 { "writes", int(trace.count) },
                 { "omitted", int(trace.omitted) } },
               false);
    errno = savedError;
}

extern "C" __attribute__((visibility("default"))) ssize_t kobalt_power_write(int fd,
                                                                             const void *bytes,
                                                                             size_t count)
{
    if (!trace.active)
        return nativePowerWrite(fd, bytes, count);
    if (trace.count == 8) {
        ++trace.omitted;
        return nativePowerWrite(fd, bytes, count);
    }
    auto &record = trace.writes[trace.count++];
    record.fd = fd;
    record.count = count;
    const int priorError = errno;
#ifdef Q_OS_LINUX
    char link[64], target[128];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const auto length = ::readlink(link, target, sizeof(target));
    record.verified = length == trace.path.size()
            && std::memcmp(target, trace.path.constData(), size_t(length)) == 0;
#endif
    writingAttempt.store(trace.attempt, std::memory_order_relaxed);
    record.enter = clocks();
    errno = priorError;
    record.result = nativePowerWrite(fd, bytes, count);
    record.error = errno;
    record.exit = clocks();
    writingAttempt.store(0, std::memory_order_relaxed);
    errno = record.error;
    return record.result;
}

void collectStandbyEvidence(const QString &root, quint64 attempt, const char *phase)
{
    static std::atomic<quint64> sequence{ 0 };
    const auto sample = ++sequence;
    if (!attempt) {
        kernelCapabilities(root);
        inventory(root);
    }
    auto fields = sampleFields(attempt, phase, sample);
    const auto bytes = readEvidence(root + "/sys/kernel/debug/wakeup_sources", 32768, fields);
    diagnostic("standby-wakeup-sources", fields, false);
    // Report complete rows as named counters. A missing file is evidence of
    // unavailable instrumentation, not proof that there were no wakeups.
    const auto lines = bytes.split('\n');
    const auto header = lines.value(0).simplified().split(' ');
    const bool valid = header.value(0) == "name" && header.contains("event_count")
            && (header.contains("wakeup_count") || header.contains("hit_count"));
    auto format = sampleFields(attempt, phase, sample);
    format["header_valid"] = valid;
    format["input_counter_fallback"] = !valid || fields.value("read_errno").toInt() != 0;
    format["rows_truncated"] = lines.size() > 130;
    int skipped = 0;
    if (valid) {
        for (int row = 1; row < lines.size() - 1 && row <= 128; ++row) {
            const auto values = lines[row].simplified().split(' ');
            if (values.size() != header.size() || values[0].size() > 128) {
                ++skipped;
                continue;
            }
            auto source = sampleFields(attempt, phase, sample);
            source["name"] = QString::fromLatin1(values[0]);
            bool rowValid = true;
            for (int col = 1; col < header.size() && col < 20; ++col) {
                bool numeric = false;
                values[col].toULongLong(&numeric);
                if (!numeric || header[col].size() > 48) {
                    rowValid = false;
                    break;
                }
                source[QString::fromLatin1(header[col])] = QString::fromLatin1(values[col]);
            }
            if (rowValid)
                diagnostic("standby-wakeup-source", source, false);
            else
                ++skipped;
        }
    }
    format["rows_skipped"] = skipped;
    diagnostic("standby-wakeup-format", format, false);
    if (format.value("input_counter_fallback").toBool())
        inputWakeupCounters(root, attempt, phase, sample);
    for (const auto *path : { "/sys/power/pm_wakeup_irq", "/sys/kernel/debug/suspend_stats",
                              "/sys/kernel/wakeup_reasons/last_resume_reason",
                              "/sys/kernel/wakeup_reasons/last_suspend_time" }) {
        auto status = sampleFields(attempt, phase, sample);
        const auto value = readEvidence(root + QString::fromLatin1(path), 2048, status);
        status["path"] = QString::fromLatin1(path);
        status["value"] = QString::fromLatin1(value.trimmed());
        diagnostic("standby-kernel-evidence", status, false);
    }
#ifdef Q_OS_LINUX
    if (root.isEmpty()) {
        // READ_ALL is non-destructive. Do not use READ_CLEAR or change dmesg
        // permissions. Old kernels may expose neither debugfs nor wakeup IRQ.
        collectStandbyKernelLog(attempt, phase, sample, klogctl);
    }
#endif
}

void collectStandbyKernelLog(quint64 attempt, const char *phase, quint64 sample,
                             int (*readLog)(int, char *, int))
{
    const PreserveErrno preserve;
    QByteArray log(65536, '\0');
    const auto enter = clocks();
    const int size = readLog(3, log.data(), log.size());
    const int error = size < 0 ? errno : 0;
    const auto exit = clocks();
    auto status = sampleFields(attempt, phase, sample);
    status["read_enter_monotonic_ms"] = double(enter.monotonic);
    status["read_enter_boot_ms"] = double(enter.boot);
    status["read_enter_realtime_ms"] = double(enter.realtime);
    status["read_exit_monotonic_ms"] = double(exit.monotonic);
    status["read_exit_boot_ms"] = double(exit.boot);
    status["read_exit_realtime_ms"] = double(exit.realtime);
    status["read_errno"] = error;
    status["bytes"] = size;
    status["read_limit_bytes"] = log.size();
    // READ_ALL drops whole oldest records to fit. A short read cannot prove
    // completeness, and the kernel ring may already have overwritten lines.
    status["completeness"] = QStringLiteral("unknown");
    log.resize(qBound(0, size, int(log.size())));
    recordStandbyKernelLog(log, status);
}

void recordStandbyKernelLog(const QByteArray &log, QJsonObject status)
{
    QJsonArray rejected;
    const auto messages = standbyKernelMessages(log, &status, &rejected);
    status["messages_truncated"] = messages.size() > 32;
    diagnostic("standby-kernel-log", status, false);
    for (const auto &message : messages.mid(qMax(0, messages.size() - 32))) {
        auto line = status;
        line["message"] = message;
        diagnostic("standby-kernel-message", line, false);
    }
    for (const auto &value : rejected) {
        auto line = value.toObject();
        for (auto it = status.constBegin(); it != status.constEnd(); ++it)
            line[it.key()] = it.value();
        diagnostic("standby-kernel-rejected", line, false);
    }
}

QStringList standbyKernelMessages(const QByteArray &log, QJsonObject *status,
                                  QJsonArray *rejectedLines)
{
    // MediaTek inserts a printk state/CPU/task prefix after the timestamp:
    // " .(0)[343:nickel]", " (0)[343:nickel]" or "-(0)" during suspension.
    // Strip only this grammar, never search for a PM substring to emit a line.
    static const QRegularExpression prefix(QStringLiteral(
            "^(?:<[0-9]{1,3}>)?\\s*(?:\\[\\s*[0-9]+(?:\\.[0-9]+)?\\]\\s*)?"
            "(?:[.-]?\\([0-9a-fA-F]{1,8}\\)(?:\\[[0-9]{1,10}:[^\\]\\x00-\\x1f]{1,16}\\])?\\s*)?"));
    QStringList result;
    if (rejectedLines)
        *rejectedLines = QJsonArray();
    int examined = 0, oversized = 0, rejected = 0;
    auto reject = [&](const QByteArray &line, const char *reason) {
        ++rejected;
        if (!rejectedLines)
            return;
        // Keep only the latest eight candidate lines, each at most 256 bytes.
        // Hex preserves control bytes in the prefix without logging a second
        // full copy. JSON escapes control bytes in the readable excerpt.
        if (rejectedLines->size() == 8)
            rejectedLines->removeAt(0);
        // Qt 5.2 otherwise converts QByteArray through a NUL-terminated pointer.
        rejectedLines->append(qt::object(
                { { "message", QString::fromLatin1(line.constData(), qMin(line.size(), 256)) },
                  { "prefix_hex", QString::fromLatin1(line.left(64).toHex()) },
                  { "line_bytes", line.size() },
                  { "line_truncated", line.size() > 256 },
                  { "reason", reason } }));
    };
    for (const auto &line : log.split('\n')) {
        if (line.isEmpty())
            continue;
        ++examined;
        static const char *const prefixes[] = { "PM: ",
                                                "Freezing user space processes",
                                                "Restarting tasks",
                                                "Resume caused by IRQ",
                                                "Resume caused by more than ",
                                                "active wakeup source: ",
                                                "last active wakeup source: ",
                                                "Pending Wakeup Sources: ",
                                                "Last active Wakeup Source: " };
        bool candidate = line.contains("PM:");
        for (const auto *text : prefixes)
            candidate = candidate || line.contains(text);
        if (line.size() > 512) {
            ++oversized;
            if (candidate)
                reject(line, "oversized");
            continue;
        }
        auto message = QString::fromLatin1(line.trimmed());
        const auto match = prefix.match(message);
        message.remove(0, match.capturedLength());
        bool accepted = false;
        for (const auto *text : prefixes)
            accepted = accepted || message.startsWith(QLatin1String(text));
        if (accepted)
            result.append(QString::fromLatin1(line));
        else if (candidate)
            reject(line, "filter");
    }
    if (status) {
        (*status)["lines_examined"] = examined;
        (*status)["oversized_lines"] = oversized;
        (*status)["candidate_lines_rejected"] = rejected;
        (*status)["messages_matched"] = result.size();
        (*status)["rejected_lines_captured"] = rejectedLines ? rejectedLines->size() : 0;
        (*status)["rejected_lines_omitted"] =
                rejected - (rejectedLines ? rejectedLines->size() : 0);
        (*status)["prefix_pattern_valid"] = prefix.isValid();
        if (!prefix.isValid())
            (*status)["prefix_pattern_error"] = prefix.errorString();
    }
    return result;
}

void sampleStandbyEvidence(quint64 attempt, const char *phase)
{
    const PreserveErrno preserve;
    if (!evidenceEnabled || !diagnosticsActive())
        return;
    const auto requested = clocks();
    // Do not accumulate work behind slow debugfs. No reader waits for it.
    if (pendingSamples.fetch_add(1) >= 4) {
        --pendingSamples;
        diagnostic("standby-evidence-skipped",
                   { { "attempt", QString::number(attempt) }, { "phase", phase } }, false);
        return;
    }
    const QByteArray phaseName(phase);
    if (!sampleDiagnostics([attempt, phaseName, requested] {
            diagnostic("standby-evidence-request",
                       { { "attempt", QString::number(attempt) },
                         { "phase", QString::fromLatin1(phaseName) },
                         { "requested_monotonic_ms", double(requested.monotonic) },
                         { "requested_boot_ms", double(requested.boot) },
                         { "requested_realtime_ms", double(requested.realtime) } },
                       false);
            collectStandbyEvidence({}, attempt, phaseName.constData());
            --pendingSamples;
        }))
        --pendingSamples;
}

void initializeStandbyDiagnostics()
{
    if (!diagnosticsActive())
        return;
    diagnostic("standby-diagnostic-capabilities",
               { { "qtcore_write_hook", bool(nativePowerWrite) },
                 { "touch_read_hook", bool(nativeTouchRead) },
                 { "power_helper_hook",
                   bool(nativePowerStringToFile || nativeDiagnosticPowerStringToFile) },
                 { "input_wakeup_bridge", bool(nativePowerStringToFile) },
                 { "input_wakeup_timeout_ms", nativePowerStringToFile ? 1000 : 0 },
                 { "samples_asynchronous", true } },
               false);
    evidenceEnabled = true;
    sampleStandbyEvidence(0, "startup");
}
} // namespace kobalt
