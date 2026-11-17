#include "resources.h"

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <libproc.h>
#  include <mach/mach.h>
#  include <malloc/malloc.h>
#elif defined(__linux__)
#  include <cstdio>
#  include <cstring>
#  include <dirent.h>
#  if defined(__GLIBC__)
#    include <malloc.h>
#  endif
#endif

namespace kobalt::scenario {
namespace {
double milliseconds(const timeval &time)
{
    return time.tv_sec * 1000.0 + time.tv_usec / 1000.0;
}

#if defined(__APPLE__)
double milliseconds(const time_value_t &time)
{
    return time.seconds * 1000.0 + time.microseconds / 1000.0;
}

// User and system time of one thread, or -1.
double threadCpu(thread_act_t thread)
{
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(thread, THREAD_BASIC_INFO, thread_info_t(&info), &count) != KERN_SUCCESS)
        return -1;
    return milliseconds(info.user_time) + milliseconds(info.system_time);
}

void sampleSystem(Resources &sample)
{
    const auto self = mach_thread_self();
    sample.mainCpuMs = threadCpu(self);
    mach_port_deallocate(mach_task_self(), self);

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t threadCount = 0;
    if (task_threads(mach_task_self(), &threads, &threadCount) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < threadCount; ++i) {
            const double cpu = threadCpu(threads[i]);
            thread_extended_info_data_t extended;
            mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
            QString name;
            if (thread_info(threads[i], THREAD_EXTENDED_INFO, thread_info_t(&extended), &count)
                == KERN_SUCCESS)
                name = QString::fromUtf8(extended.pth_name);
            if (cpu >= 0)
                sample.threadCpuMs[name.isEmpty() ? QStringLiteral("unnamed") : name] += cpu;
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), vm_address_t(threads), threadCount * sizeof(thread_act_t));
    }

    mach_task_basic_info_data_t task;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&task), &count)
        == KERN_SUCCESS)
        sample.rssKib = task.resident_size / 1024.0;

    malloc_statistics_t heap;
    malloc_zone_statistics(nullptr, &heap);
    sample.heapKib = heap.size_in_use / 1024.0;

    rusage_info_v2 usage;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t *>(&usage)) == 0)
        sample.diskWrittenBytes = double(usage.ri_diskio_byteswritten);
}
#elif defined(__linux__)
// Reads a small /proc file into `buffer`, or returns false.
bool readProc(const char *path, char *buffer, size_t size)
{
    FILE *file = std::fopen(path, "r");
    if (!file)
        return false;
    const size_t length = std::fread(buffer, 1, size - 1, file);
    std::fclose(file);
    buffer[length] = 0;
    return length > 0;
}

// The value after `key` in a "key: value" file, or -1.
double field(const char *text, const char *key)
{
    const char *at = std::strstr(text, key);
    if (!at)
        return -1;
    unsigned long long value = 0;
    return std::sscanf(at + std::strlen(key), " %llu", &value) == 1 ? double(value) : -1;
}

void sampleSystem(Resources &sample)
{
    rusage thread;
    if (getrusage(RUSAGE_THREAD, &thread) == 0)
        sample.mainCpuMs = milliseconds(thread.ru_utime) + milliseconds(thread.ru_stime);

    const double tick = 1000.0 / double(sysconf(_SC_CLK_TCK));
    if (DIR *tasks = opendir("/proc/self/task")) {
        while (const dirent *entry = readdir(tasks)) {
            if (entry->d_name[0] == '.')
                continue;
            // Room for "/proc/self/task/", any directory name and "/stat".
            char path[sizeof("/proc/self/task//stat") + sizeof(entry->d_name)], text[1024];
            std::snprintf(path, sizeof path, "/proc/self/task/%s/stat", entry->d_name);
            if (!readProc(path, text, sizeof text))
                continue;
            // "tid (name) state ..." with utime and stime as fields 14 and 15.
            const char *open = std::strchr(text, '('), *close = std::strrchr(text, ')');
            if (!open || !close || close < open)
                continue;
            unsigned long user = 0, system = 0;
            if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &user,
                            &system)
                != 2)
                continue;
            const auto name = QString::fromUtf8(open + 1, int(close - open - 1));
            sample.threadCpuMs[name.isEmpty() ? QStringLiteral("unnamed") : name] +=
                    (user + system) * tick;
        }
        closedir(tasks);
    }

    char text[512];
    long pages = 0, resident = 0;
    if (readProc("/proc/self/statm", text, sizeof text)
        && std::sscanf(text, "%ld %ld", &pages, &resident) == 2)
        sample.rssKib = resident * double(sysconf(_SC_PAGESIZE)) / 1024.0;

    // Needs the kernel's task I/O accounting, which some Kobo kernels lack.
    if (readProc("/proc/self/io", text, sizeof text)) {
        sample.writeCallBytes = field(text, "wchar:");
        sample.writeCalls = field(text, "syscw:");
        sample.diskWrittenBytes = field(text, "\nwrite_bytes:");
    }

#  if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const auto heap = mallinfo2();
    sample.heapKib = (heap.uordblks + heap.hblkhd) / 1024.0;
#  elif defined(__GLIBC__)
    const auto heap = mallinfo();
    sample.heapKib = (unsigned(heap.uordblks) + unsigned(heap.hblkhd)) / 1024.0;
#  endif
}
#else
void sampleSystem(Resources &) { }
#endif

void add(QJsonObject &stages, const char *key, double value)
{
    stages[QLatin1String(key)] = value;
}

// Adds `end - start` when both are known.
void difference(QJsonObject &stages, const char *key, double start, double end)
{
    if (start >= 0 && end >= 0)
        add(stages, key, end - start);
}
} // namespace

Resources sampleResources()
{
    Resources sample;
    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        sample.cpuMs = milliseconds(usage.ru_utime) + milliseconds(usage.ru_stime);
#if defined(__APPLE__)
        // macOS reports the high-water mark in bytes, Linux in KiB.
        sample.peakKib = usage.ru_maxrss / 1024.0;
#else
        sample.peakKib = double(usage.ru_maxrss);
#endif
        sample.blockWrites = double(usage.ru_oublock);
    }
    sampleSystem(sample);
    return sample;
}

QJsonObject resourceStages(const Resources &start, const Resources &end)
{
    QJsonObject stages;
    difference(stages, "cpu_ms", start.cpuMs, end.cpuMs);
    difference(stages, "main_cpu_ms", start.mainCpuMs, end.mainCpuMs);
    if (start.cpuMs >= 0 && end.cpuMs >= 0 && start.mainCpuMs >= 0 && end.mainCpuMs >= 0)
        add(stages, "background_cpu_ms",
            (end.cpuMs - start.cpuMs) - (end.mainCpuMs - start.mainCpuMs));
    if (end.rssKib >= 0)
        add(stages, "rss_end_kib", end.rssKib);
    difference(stages, "rss_growth_kib", start.rssKib, end.rssKib);
    if (end.peakKib >= 0)
        add(stages, "peak_kib", end.peakKib);
    difference(stages, "peak_rise_kib", start.peakKib, end.peakKib);
    if (end.heapKib >= 0)
        add(stages, "heap_end_kib", end.heapKib);
    difference(stages, "heap_growth_kib", start.heapKib, end.heapKib);
    difference(stages, "disk_written_bytes", start.diskWrittenBytes, end.diskWrittenBytes);
    difference(stages, "block_writes", start.blockWrites, end.blockWrites);
    difference(stages, "write_calls", start.writeCalls, end.writeCalls);
    difference(stages, "write_call_bytes", start.writeCallBytes, end.writeCallBytes);
    return stages;
}

QJsonObject threadStages(const Resources &start, const Resources &end)
{
    QJsonObject threads;
    for (auto it = end.threadCpuMs.begin(); it != end.threadCpuMs.end(); ++it) {
        // A thread that started during the sample used all of its time here.
        const double used = it.value() - start.threadCpuMs.value(it.key(), 0);
        if (used > 0)
            threads[it.key()] = used;
    }
    return threads;
}
} // namespace kobalt::scenario
