#include "build_info.h"
#include "qt_compat.h"
#include "diagnostics.h"
#include "crash_context.h"
#include "main_thread.h"
#include "strings.h"
#include <QDateTime>
#include <QCoreApplication>
#include <QThread>
#include <QDir>
#include <QHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QVector>
#include <QWaitCondition>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <dirent.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#  include <mach/mach.h>
#  include <sys/resource.h>
#endif

namespace kobalt {
namespace {
QMutex mutex;
std::atomic<int> retainedFontBytes{ 0 }, nativeFontBytes{ 0 }, previewBytes{ 0 };
QString logPath;
bool paused = false, detailed = true;
bool syncing = true;
std::function<void(const QJsonObject &)> observer;
constexpr qint64 normalLimit = 128 * 1024;
constexpr qint64 verboseLimit = 16 * 1024 * 1024;
constexpr int verboseHistory = 5;
constexpr qint64 batchLimit = 128 * 1024;
constexpr std::size_t queueLimit = 2048;
constexpr qint64 quietMilliseconds = 1000, maximumDelay = 5000;
// Routine activity in normal mode waits longer, so reading does not write the
// log every few seconds. Warnings, errors, verbose mode and forced flushes at
// sleep, close, USB and power-off keep the short delay.
constexpr qint64 routineQuietMilliseconds = 30000, routineMaximumDelay = 60000;

qint64 monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

struct Entry
{
    QString path;
    QByteArray event;
    QJsonObject fields;
    DiagnosticSeverity severity;
    qint64 monotonic, utc;
    qint64 boot = -1;
    quintptr thread = 0;
    bool mainThread = false;
    qint64 fileLimit = normalLimit;
    bool keepContext = false;
    int history = 1;
    std::function<void(QJsonObject)> memorySample = {};
    std::function<void()> sample = {};
};

// The worker lives until process exit. The fatal handler never accesses it.
std::deque<Entry> &queue = *new std::deque<Entry>;
QWaitCondition &wake = *new QWaitCondition, &idle = *new QWaitCondition;
bool writerStarted = false, writing = false, forceFlush = false;
qint64 firstPending = 0, lastInput = 0;
void addMemory(QJsonObject &fields);

QJsonObject record(const Entry &entry)
{
    auto fields = entry.fields;
    fields["event"] = QString::fromLatin1(entry.event);
    fields["severity"] = QString::fromLatin1(entry.severity == DiagnosticSeverity::Error ? "error"
                                                     : entry.severity == DiagnosticSeverity::Warning
                                                     ? "warning"
                                                     : "activity");
    fields["monotonic_ms"] = double(entry.monotonic);
    fields["utc"] =
            QDateTime::fromMSecsSinceEpoch(entry.utc).toUTC().toString("yyyy-MM-ddTHH:mm:ss.zzz")
            + 'Z';
    fields["pid"] = int(getpid());
    if (entry.boot >= 0)
        fields["boot_ms"] = double(entry.boot);
    fields["thread"] = QString::number(entry.thread, 16);
    fields["main_thread"] = entry.mainThread;
    return fields;
}

// One descriptor held in reserve: with none left, listing them or opening the
// log would fail. Whoever needs the slot closes the reserve, uses the slot and
// takes the reserve back at once.
std::atomic<int> descriptorReserve{ -1 };
std::atomic<qint64> descriptorReported{ -1 };

void takeDescriptorReserve()
{
    if (descriptorReserve.load() >= 0)
        return;
    int reserve = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    // Keep standard input, output and error free for their usual numbers.
    if (reserve >= 0 && reserve < 3) {
        const int moved = ::fcntl(reserve, F_DUPFD_CLOEXEC, 3);
        ::close(reserve);
        reserve = moved;
    }
    int expected = -1;
    if (reserve >= 0 && !descriptorReserve.compare_exchange_strong(expected, reserve))
        ::close(reserve);
}

bool releaseDescriptorReserve()
{
    const int reserve = descriptorReserve.exchange(-1);
    if (reserve < 0)
        return false;
    ::close(reserve);
    return true;
}

void append(const QString &name, const QByteArray &bytes, bool sync, qint64 fileLimit, int history)
{
    const auto path = QFile::encodeName(name);
    const int flags = O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    int fd = ::open(path.constData(), flags, 0600);
    // Out of descriptors, the log uses the reserve's slot.
    if (fd < 0 && (errno == EMFILE || errno == ENFILE) && releaseDescriptorReserve())
        fd = ::open(path.constData(), flags, 0600);
    struct stat info{};
    if (fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        if (info.st_size + bytes.size() > fileLimit) {
            ::close(fd);
            fd = -1;
            const auto previous = [&path](int generation) {
                return path + ".previous"
                        + (generation == 1 ? QByteArray() : '.' + QByteArray::number(generation));
            };
            bool shifted = true;
            for (int generation = history; generation > 1; --generation) {
                if (::rename(previous(generation - 1).constData(), previous(generation).constData())
                            < 0
                    && errno != ENOENT) {
                    shifted = false;
                    break;
                }
            }
            if (shifted && ::rename(path.constData(), previous(1).constData()) == 0)
                fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                            0600);
        }
        if (fd >= 0) {
            qint64 written = 0;
            while (written < bytes.size()) {
                const auto count = ::write(fd, bytes.constData() + written, bytes.size() - written);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    break;
                written += count;
            }
            if (sync)
                ::fdatasync(fd);
        }
    }
    if (fd >= 0)
        ::close(fd);
    takeDescriptorReserve();
}

void enqueue(Entry entry)
{
    if (queue.size() >= queueLimit)
        return;
    if (queue.empty())
        firstPending = monotonicMilliseconds();
    queue.push_back(std::move(entry));
}

// Called with the mutex held. No formatting, sampling or I/O holds it.
void startWriter()
{
    if (writerStarted)
        return;
    writerStarted = true;
    std::thread([] {
        QMutexLocker lock(&mutex);
        qint64 lastMemory = 0;
        QString contextPath;
        QJsonObject context;
        for (;;) {
            while (queue.empty())
                wake.wait(&mutex);
            const auto now = monotonicMilliseconds();
            const bool urgent =
                    detailed || std::any_of(queue.begin(), queue.end(), [](const Entry &entry) {
                        return entry.severity != DiagnosticSeverity::Activity;
                    });
            const auto quiet = urgent ? quietMilliseconds : routineQuietMilliseconds;
            const auto longest = urgent ? maximumDelay : routineMaximumDelay;
            const auto delay =
                    std::min(firstPending + longest, std::max(firstPending, lastInput) + quiet)
                    - now;
            const bool sampling = std::any_of(queue.begin(), queue.end(), [](const Entry &entry) {
                return bool(entry.memorySample) || bool(entry.sample);
            });
            if (!forceFlush && !sampling && delay > 0) {
                wake.wait(&mutex, static_cast<unsigned long>(delay));
                continue;
            }
            std::deque<Entry> batch;
            batch.swap(queue);
            const bool sync = syncing
                    && (forceFlush
                        || std::any_of(batch.begin(), batch.end(), [](const Entry &entry) {
                               return entry.severity != DiagnosticSeverity::Activity;
                           }));
            const bool sample = now - lastMemory >= 1000;
            const bool writeSample = detailed;
            writing = true;
            lock.unlock();
            QString path;
            QByteArray bytes;
            qint64 fileLimit = normalLimit;
            int history = 1;
            bool sampled = false;
            for (const auto &entry : batch) {
                if (entry.sample) {
                    entry.sample();
                    continue;
                }
                if (entry.memorySample) {
                    QJsonObject fields;
                    addMemory(fields);
                    entry.memorySample(std::move(fields));
                    continue;
                }
                if (entry.event.isEmpty()) {
                    if (!bytes.isEmpty()) {
                        append(path, bytes, sync, fileLimit, history);
                        bytes.clear();
                    }
                    append(entry.path, {}, sync, entry.fileLimit, entry.history);
                    continue;
                }
                auto fields = record(entry);
                if (sample && !sampled) {
                    // Normal logs keep the sample out of the line. Taking it still
                    // refreshes the crash report's resource snapshot and raises
                    // memory-pressure warnings.
                    if (writeSample) {
                        addMemory(fields);
                    } else {
                        QJsonObject unwritten;
                        addMemory(unwritten);
                    }
                    sampled = true;
                    lastMemory = now;
                }
                // Keep build and power capabilities independently of event rotation.
                // Driver inventory also survives rotation. All context writes run here.
                if (entry.keepContext
                    && (entry.event == "startup" || entry.event == "power-observer"
                        || entry.event == "standby-input-device"
                        || entry.event == "standby-diagnostic-capabilities"
                        || entry.event == "standby-kernel-capabilities")) {
                    if (contextPath != entry.path || entry.event == "startup") {
                        contextPath = entry.path;
                        context = {};
                    }
                    if (entry.event == "standby-input-device") {
                        auto devices = context.value("input_devices").toObject();
                        devices[fields.value("event_device").toString()] = fields;
                        context["input_devices"] = devices;
                    } else {
                        const auto key = entry.event == "startup"              ? "startup"
                                : entry.event == "standby-kernel-capabilities" ? "standby_kernel"
                                : entry.event == "power-observer"              ? "power_observer"
                                                                  : "standby_diagnostics";
                        context[key] = fields;
                    }
                    QSaveFile snapshot(
                            QFileInfo(entry.path).dir().filePath("session-context.json"));
                    const auto data = QJsonDocument(context).toJson();
                    if (snapshot.open(QIODevice::WriteOnly) && snapshot.write(data) == data.size())
                        snapshot.commit();
                }
                const auto line = QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n';
                if (line.size() > 4096)
                    continue;
                if (!bytes.isEmpty()
                    && (path != entry.path || fileLimit != entry.fileLimit
                        || history != entry.history || bytes.size() + line.size() > batchLimit)) {
                    append(path, bytes, sync, fileLimit, history);
                    bytes.clear();
                }
                path = entry.path;
                fileLimit = entry.fileLimit;
                history = entry.history;
                bytes += line;
            }
            if (!bytes.isEmpty())
                append(path, bytes, sync, fileLimit, history);
            lock.relock();
            writing = false;
            if (queue.empty()) {
                forceFlush = false;
                idle.wakeAll();
            }
        }
    }).detach();
}

bool drain(int milliseconds)
{
    // A queued boundary also syncs bytes from earlier unsynced batches when
    // the queue is empty. All file access remains on the writer thread.
    if (!logPath.isEmpty()) {
        Entry boundary{ logPath,
                        {},
                        {},
                        DiagnosticSeverity::Activity,
                        monotonicMilliseconds(),
                        QDateTime::currentMSecsSinceEpoch() };
        boundary.fileLimit = detailed ? verboseLimit : normalLimit;
        boundary.history = detailed ? verboseHistory : 1;
        enqueue(std::move(boundary));
    }
    forceFlush = true;
    startWriter();
    wake.wakeAll();
    QElapsedTimer timer;
    timer.start();
    while (!queue.empty() || writing) {
        if (milliseconds < 0) {
            idle.wait(&mutex);
            continue;
        }
        const auto left = milliseconds - timer.elapsed();
        if (left <= 0 || !idle.wait(&mutex, (unsigned long)left))
            return queue.empty() && !writing;
    }
    forceFlush = false;
    return true;
}

// Resident memory when the first book began to open, in KiB, or -1.
std::atomic<qint64> memoryBase{ -1 };

// Resident and peak resident memory in KiB, or -1 where unknown, and the
// part above the base once it is known.
void addMemory(QJsonObject &fields)
{
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count)
        == KERN_SUCCESS)
        fields["rss_kib"] = double(info.resident_size / 1024);
    rusage usage{};
    // The Mac reports the peak in bytes, Linux in KiB.
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        fields["peak_rss_kib"] = double(usage.ru_maxrss / 1024);
#else
    // These are worker-time samples, not readings taken at the input event.
    fields["sample_monotonic_ms"] = double(monotonicMilliseconds());
    QFile frequency("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (frequency.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const auto khz = frequency.read(64).trimmed().toLongLong(&ok);
        if (ok && khz > 0)
            fields["cpu_khz"] = double(khz);
    }
    QFile memory("/proc/self/status");
    if (memory.open(QIODevice::ReadOnly)) {
        for (const auto &line : memory.readAll().split('\n')) {
            if (line.startsWith("VmSize:") || line.startsWith("VmData:")
                || line.startsWith("Threads:")) {
                const auto parts = line.simplified().split(' ');
                if (parts.size() >= 2)
                    fields[line.startsWith("VmSize:")           ? "virtual_kib"
                                   : line.startsWith("VmData:") ? "data_kib"
                                                                : "threads"] = parts[1].toDouble();
            }
            if (line.startsWith("VmRSS:") || line.startsWith("VmHWM:")) {
                const auto parts = line.simplified().split(' ');
                if (parts.size() >= 2)
                    fields[line.startsWith("VmRSS:") ? "rss_kib" : "peak_rss_kib"] =
                            parts[1].toDouble();
            }
        }
    }
#endif
    fields["private_font_bytes"] = retainedFontBytes.load();
    fields["native_fallback_font_bytes"] = nativeFontBytes.load();
    fields["font_preview_bitmap_bytes"] = previewBytes.load();
    fields["resource_sample_monotonic_ms"] = double(monotonicMilliseconds());
#ifdef __linux__
    QFile systemMemory("/proc/meminfo");
    if (systemMemory.open(QIODevice::ReadOnly)) {
        for (const auto &line : systemMemory.readAll().split('\n')) {
            const auto parts = line.simplified().split(' ');
            if (parts.size() < 2)
                continue;
            if (parts[0] == "MemAvailable:")
                fields["available_kib"] = parts[1].toDouble();
            if (parts[0] == "MemFree:")
                fields["free_kib"] = parts[1].toDouble();
        }
    }
    // Do not count the sampler's own proc/sysfs handles as application usage.
    systemMemory.close();
    memory.close();
    frequency.close();
    if (auto *directory = opendir("/proc/self/fd")) {
        int count = 0, highest = -1;
        while (const auto *entry = readdir(directory)) {
            char *end = nullptr;
            const long fd = std::strtol(entry->d_name, &end, 10);
            if (!entry->d_name[0] || *end || fd < 0 || fd == dirfd(directory))
                continue;
            ++count;
            highest = qMax(highest, int(fd));
        }
        closedir(directory);
        fields["fd_count"] = count;
        fields["fd_highest"] = highest;
    }
#endif
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        fields["fd_soft_limit"] = limit.rlim_cur == RLIM_INFINITY ? -1.0 : double(limit.rlim_cur);
        fields["fd_hard_limit"] = limit.rlim_max == RLIM_INFINITY ? -1.0 : double(limit.rlim_max);
    }
    // Keep pressure evidence even in release logs. The available value is not
    // interchangeable with MemFree; old kernels may provide only the latter.
    const bool low = fields.contains("available_kib")
            && fields.value("available_kib").toDouble() < 16 * 1024;
    if (fields.contains("available_kib"))
        fields["memory_pressure"] = low;
    static qint64 lastWarning = -60000;
    const auto now = monotonicMilliseconds();
    if (low && now - lastWarning >= 60000) {
        lastWarning = now;
        diagnostic("memory-pressure", { { "available_kib", fields.value("available_kib") } },
                   DiagnosticSeverity::Warning);
    }
    QJsonObject resources;
    for (const auto *key :
         { "resource_sample_monotonic_ms", "rss_kib", "peak_rss_kib", "virtual_kib", "data_kib",
           "available_kib", "free_kib", "threads", "fd_count", "fd_highest", "fd_soft_limit",
           "fd_hard_limit", "memory_pressure", "private_font_bytes", "native_fallback_font_bytes",
           "font_preview_bitmap_bytes" })
        if (fields.contains(key))
            resources[key] = fields.value(key);
    const auto snapshot = QJsonDocument(resources).toJson(QJsonDocument::Compact) + '\n';
    crashContext::resources.store(snapshot.constData(), std::size_t(snapshot.size()));
    const qint64 base = memoryBase;
    if (base < 0)
        return;
    if (fields.value("rss_kib").isDouble())
        fields["kobalt_rss_kib"] = fields.value("rss_kib").toDouble() - double(base);
    if (fields.value("peak_rss_kib").isDouble())
        fields["kobalt_peak_kib"] = fields.value("peak_rss_kib").toDouble() - double(base);
}
} // namespace

void setDiagnosticFontBytes(qint64 bytes)
{
    retainedFontBytes.store(int(bytes));
}

void setDiagnosticPreviewBytes(int bytes)
{
    previewBytes.store(bytes);
}

void setDiagnosticNativeFontBytes(qint64 bytes)
{
    nativeFontBytes.store(int(bytes));
}

void setDiagnosticsSync(bool enabled)
{
    QMutexLocker lock(&mutex);
    syncing = enabled;
}

void setDiagnosticObserver(std::function<void(const QJsonObject &)> value)
{
    QMutexLocker lock(&mutex);
    observer = std::move(value);
}

QJsonObject diagnosticDeviceFields(const QByteArray &version)
{
    const auto fields = version.trimmed().split(',');
    if (fields.size() != 6) {
        return {};
    }
    QJsonObject result;
    const auto firmware = QString::fromLatin1(fields[2].trimmed());
    const auto hardware = QString::fromLatin1(fields[5].trimmed());
    // Persist only model and firmware fields. The first field is the serial.
    static const QRegularExpression firmwarePattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+){2,}$"));
    static const QRegularExpression hardwarePattern(
            QStringLiteral("^00000000-0000-0000-0000-[0-9]{12}$"));
    if (firmwarePattern.match(firmware).hasMatch()) {
        result["firmware"] = firmware;
    }
    if (hardwarePattern.match(hardware).hasMatch()) {
        result["hardware_uuid"] = hardware;
    }
    return result;
}

QString diagnosticErrorCode(const QString &reason)
{
    if (reason.isEmpty())
        return {};
    int count = 0;
    const auto *entries = strings::entries(&count);
    // Never persist arbitrary error text: it may contain a private file path.
    for (int i = 0; i < count; ++i) {
        const auto &entry = entries[i];
        if (std::strncmp(entry.key, "error.", 6) == 0
            && (reason == QString::fromUtf8(entry.value) || reason == QLatin1String(entry.key)))
            return QString::fromLatin1(entry.key);
    }
    return QStringLiteral("unknown");
}

void pauseDiagnostics(bool value)
{
    // Storage can be exported next. Finish the queued events first.
    if (value)
        mainthread::blocking("diagnostic-log-flush");
    QMutexLocker lock(&mutex);
    paused = value;
    if (value)
        drain(-1);
}

bool diagnosticsActive()
{
    QMutexLocker lock(&mutex);
    return !paused && (detailed || bool(observer)) && !logPath.isEmpty();
}

bool diagnosticsRequested(const QString &profile)
{
#ifdef KOBALT_VERBOSE_DIAGNOSTICS
    Q_UNUSED(profile);
    return true;
#else
    return qgetenv("KOBALT_DIAGNOSTICS") == "1" || QFile::exists(profile + "/diagnostics-enabled");
#endif
}

void initializeDiagnostics(const QString &directory, bool verbose)
{
    {
        QMutexLocker lock(&mutex);
        if (!QDir().mkpath(directory))
            return;
        logPath = directory + "/events.jsonl";
        detailed = verbose;
        takeDescriptorReserve();
        lastInput = monotonicMilliseconds();
    }
    auto fields = kobalt::qt::object({ { "build", __DATE__ " " __TIME__ }, { "qt", qVersion() } });
#ifdef KOBALT_VERSION
    fields["version"] = QString::fromLatin1(KOBALT_VERSION);
#endif
#ifdef KOBALT_READER_HARDWARE
    QFile version("/mnt/onboard/.kobo/version");
    if (version.open(QIODevice::ReadOnly)) {
        const auto device = diagnosticDeviceFields(version.read(512));
        for (auto it = device.begin(); it != device.end(); ++it) {
            fields[it.key()] = it.value();
        }
    }
#endif
    fields["source_commit"] = QString::fromLatin1(kobaltCommit);
    fields["source_modified"] = bool(KOBALT_COMMIT_MODIFIED);
    fields["verbose"] = verbose;
    fields["log_limit_bytes"] = double(verbose ? verboseLimit : normalLimit);
    fields["log_history_files"] = verbose ? verboseHistory : 1;
    diagnostic("startup", fields);
}

void diagnosticInput()
{
    QMutexLocker lock(&mutex);
    lastInput = monotonicMilliseconds();
}

void sampleDiagnosticMemory(std::function<void(QJsonObject)> done)
{
    QMutexLocker lock(&mutex);
    if (!done || queue.size() >= queueLimit)
        return;
    if (queue.empty())
        firstPending = monotonicMilliseconds();
    queue.push_front({ {},
                       {},
                       {},
                       DiagnosticSeverity::Activity,
                       0,
                       0,
                       -1,
                       0,
                       false,
                       normalLimit,
                       false,
                       1,
                       std::move(done) });
    startWriter();
    wake.wakeAll();
}

bool sampleDiagnostics(std::function<void()> sample)
{
    QMutexLocker lock(&mutex);
    if (!sample || paused || (!detailed && !observer) || logPath.isEmpty()
        || queue.size() >= queueLimit)
        return false;
    enqueue({ {},
              {},
              {},
              DiagnosticSeverity::Activity,
              0,
              0,
              -1,
              0,
              false,
              normalLimit,
              false,
              1,
              {},
              std::move(sample) });
    startWriter();
    wake.wakeAll();
    return true;
}

void markMemoryBase()
{
    static std::atomic_bool marked{ false };
    if (marked.exchange(true))
        return;
    sampleDiagnosticMemory([](QJsonObject fields) {
        const auto rss = fields.value("rss_kib");
        if (!rss.isDouble())
            return;
        memoryBase = qint64(rss.toDouble());
        // Not `rss_kib`: a detailed log adds its own sample under that name.
        QJsonObject base;
        base["base_rss_kib"] = rss;
        diagnostic("memory-base", base, DiagnosticSeverity::Activity);
    });
}

void diagnostic(const char *event, QJsonObject fields, bool)
{
    diagnostic(event, std::move(fields), DiagnosticSeverity::Activity);
}

void diagnostic(const char *event, QJsonObject fields, DiagnosticSeverity severity)
{
    const int savedErrno = errno;
    // Record only event names and resource counters, never arbitrary paths,
    // book identifiers or user text. This survives an unflushed event queue.
    char recent[1024];
    int used = std::snprintf(recent, sizeof(recent), "event=%.*s monotonic_ms=%lld", 96, event,
                             static_cast<long long>(monotonicMilliseconds()));
    for (const auto *key :
         { "file_bytes", "retained_bytes", "budget_bytes", "released_bytes", "id" }) {
        if (!fields.contains(key) || used < 0 || used >= int(sizeof(recent)) - 2)
            continue;
        const int n = std::snprintf(recent + used, sizeof(recent) - std::size_t(used), " %s=%.0f",
                                    key, fields.value(key).toDouble());
        if (n > 0)
            used = qMin(int(sizeof(recent)) - 2, used + n);
    }
    if (used > 0 && used < int(sizeof(recent)) - 1) {
        recent[used++] = '\n';
        crashContext::event(recent, std::size_t(used));
    }
    Entry entry{ {},       QByteArray(event),       std::move(fields),
                 severity, monotonicMilliseconds(), QDateTime::currentMSecsSinceEpoch() };
#ifdef __linux__
    timespec boot{};
    if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0)
        entry.boot = qint64(boot.tv_sec) * 1000 + boot.tv_nsec / 1000000;
#endif
    entry.thread = reinterpret_cast<quintptr>(QThread::currentThreadId());
    entry.mainThread = QCoreApplication::instance()
            && QThread::currentThread() == QCoreApplication::instance()->thread();
    std::function<void(const QJsonObject &)> notify;
    {
        QMutexLocker lock(&mutex);
        if (!paused && !logPath.isEmpty()) {
            entry.path = logPath;
            entry.fileLimit = detailed ? verboseLimit : normalLimit;
            entry.keepContext = detailed;
            entry.history = detailed ? verboseHistory : 1;
            notify = observer;
            enqueue(entry);
            startWriter();
            wake.wakeAll();
        }
    }
    // Evaluation observes the original fields regardless of disk policy.
    if (notify)
        notify(record(entry));
    errno = savedErrno;
}

DiagnosticOperation::DiagnosticOperation(const char *name) : operation(name)
{
    if (!name || !diagnosticsActive())
        return;
    static std::atomic<quint64> sequence{ 0 };
    id = ++sequence;
    elapsed.start();
    diagnostic("operation-start",
               { { "operation", QString::fromLatin1(operation) }, { "operation_id", double(id) } },
               false);
}

DiagnosticOperation::~DiagnosticOperation()
{
    if (!id)
        return;
    diagnostic("operation-end",
               { { "operation", QString::fromLatin1(operation) },
                 { "operation_id", double(id) },
                 { "elapsed_ms", elapsed.nsecsElapsed() / 1e6 } },
               false);
}

bool flushDiagnostics(int milliseconds)
{
    mainthread::blocking("diagnostic-log-flush");
    QMutexLocker lock(&mutex);
    return drain(milliseconds);
}

namespace {
// Event logs never carry book paths. System files, Kobalt's and other mods'
// folders and the files directly in Nickel's .kobo folder keep their path. Its
// subfolders name store books by their IDs, and other platforms keep imported
// books beside the profile, so those and any other file are named only by
// their extension.
QString descriptorTarget(const QString &path)
{
    static const char *const kept[] = { "/dev/", "/proc/", "/sys/", "/usr/",
                                        "/lib/", "/etc/",  "/bin/", "/mnt/onboard/.adds/" };
    for (const auto *prefix : kept)
        if (path.startsWith(QLatin1String(prefix)))
            return path;
    static const QLatin1String nickel("/mnt/onboard/.kobo/");
    if (path.startsWith(nickel) && !path.mid(nickel.size()).contains('/'))
        return path;
    const auto name = path.section('/', -1);
    const auto suffix = name.contains('.') ? name.section('.', -1) : QString();
    static const QRegularExpression plain("^[A-Za-z0-9]{1,8}$");
    return plain.match(suffix).hasMatch() ? QStringLiteral("other file .") + suffix.toLower()
                                          : QStringLiteral("other file");
}
} // namespace

void reportDescriptorExhaustion(const char *context, int error)
{
    if (error != EMFILE && error != ENFILE)
        return;
    const int savedError = errno;
    const auto now = monotonicMilliseconds();
    auto last = descriptorReported.load();
    if ((last >= 0 && now - last < 60000)
        || !descriptorReported.compare_exchange_strong(last, now)) {
        errno = savedError;
        return;
    }
    releaseDescriptorReserve();
    int total = 0, files = 0, sockets = 0, pipes = 0, inotify = 0, other = 0;
    bool listed = false;
    QHash<QString, int> targets;
#ifdef Q_OS_MACOS
    // macOS lists descriptors without link targets, so it reports only the total.
    const char *directoryPath = "/dev/fd";
#else
    const char *directoryPath = "/proc/self/fd";
#endif
    if (DIR *directory = ::opendir(directoryPath)) {
        listed = true;
        const int own = ::dirfd(directory);
        while (const auto *entry = ::readdir(directory)) {
            if (entry->d_name[0] == '.' || std::atoi(entry->d_name) == own)
                continue;
            ++total;
            char target[512];
            const auto link = QByteArray(directoryPath) + '/' + entry->d_name;
            const auto length = ::readlink(link.constData(), target, sizeof(target) - 1);
            if (length <= 0) {
                ++other;
                continue;
            }
            target[length] = 0;
            if (std::strncmp(target, "socket:", 7) == 0)
                ++sockets;
            else if (std::strncmp(target, "pipe:", 5) == 0)
                ++pipes;
            else if (std::strcmp(target, "anon_inode:inotify") == 0)
                ++inotify;
            else if (target[0] == '/') {
                ++files;
                ++targets[QString::fromLocal8Bit(target)];
            } else
                ++other;
        }
        ::closedir(directory);
    }
    // Take the slot back at once, before whatever leaks takes it.
    takeDescriptorReserve();
    QHash<QString, int> named;
    for (auto it = targets.cbegin(); it != targets.cend(); ++it)
        named[descriptorTarget(it.key())] += it.value();
    QVector<QPair<int, QString>> ranked;
    for (auto it = named.cbegin(); it != named.cend(); ++it)
        ranked.append({ it.value(), it.key() });
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                  return a.first != b.first ? a.first > b.first : a.second < b.second;
              });
    QJsonArray top;
    for (int i = 0; i < ranked.size() && i < 5; ++i)
        top.append(qt::object({ { "path", ranked[i].second }, { "count", ranked[i].first } }));
    diagnostic("descriptor-exhaustion",
               qt::object({ { "context", QString::fromLatin1(context) },
                            { "errno", error },
                            { "listed", listed },
                            { "open", total },
                            { "files", files },
                            { "sockets", sockets },
                            { "pipes", pipes },
                            { "inotify", inotify },
                            { "other", other },
                            { "most_opened", top } }),
               DiagnosticSeverity::Error);
    errno = savedError;
}
} // namespace kobalt
