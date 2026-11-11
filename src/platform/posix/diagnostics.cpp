#include "diagnostics.h"
#include "support/qt_compat.h"
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#  include <mach/mach.h>
#endif

namespace kobalt::platform {
QString diagnosticDescriptorName(const QString &path)
{
    static const char *const kept[] = { "/dev/", "/proc/", "/sys/", "/usr/",
                                        "/lib/", "/etc/",  "/bin/" };
    for (const auto *prefix : kept)
        if (path.startsWith(QLatin1String(prefix)))
            return path;
    const auto name = path.section('/', -1);
    const auto suffix = name.contains('.') ? name.section('.', -1) : QString();
    static const QRegularExpression plain("^[A-Za-z0-9]{1,8}$");
    return plain.match(suffix).hasMatch() ? QStringLiteral("other file .") + suffix.toLower()
                                          : QStringLiteral("other file");
}

namespace {
QJsonObject processSample(qint64 monotonic)
{
    QJsonObject fields;
    (void)monotonic;
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
#elif defined(__linux__)
    // These are worker-time samples, not readings taken at the input event.
    fields["sample_monotonic_ms"] = double(monotonic);
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
    return fields;
}

QJsonObject descriptorSample(const DescriptorName &name)
{
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
    QHash<QString, int> named;
    for (auto it = targets.cbegin(); it != targets.cend(); ++it)
        named[name ? name(it.key()) : diagnosticDescriptorName(it.key())] += it.value();
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
    return qt::object({ { "listed", listed },
                        { "open", total },
                        { "files", files },
                        { "sockets", sockets },
                        { "pipes", pipes },
                        { "inotify", inotify },
                        { "other", other },
                        { "most_opened", top } });
}
} // namespace

DiagnosticSystem processDiagnosticSystem(DescriptorName name)
{
    DiagnosticSystem system;
    system.sample = processSample;
    system.descriptors = [name = std::move(name)] { return descriptorSample(name); };
#ifdef __linux__
    system.bootMilliseconds = [] {
        timespec boot{};
        return clock_gettime(CLOCK_BOOTTIME, &boot) == 0
                ? qint64(boot.tv_sec) * 1000 + boot.tv_nsec / 1000000
                : qint64(-1);
    };
#endif
    return system;
}
} // namespace kobalt::platform
