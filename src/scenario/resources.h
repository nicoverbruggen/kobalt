#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>

namespace kobalt::scenario {
// What the operating system counts for this process at one moment. The
// scenario player takes one at `mark` and one at the end of a sample, and
// reports the difference. Nothing here adds counters to the reader: every
// value comes from the kernel. A value the system does not expose is -1.
struct Resources
{
    // User and system CPU time of the whole process, and of the calling
    // thread, which is the GUI thread for the player.
    double cpuMs = -1, mainCpuMs = -1;
    // CPU time of each live thread, by the thread's system name. Threads
    // with the same name add up. Threads that ended are missing here but
    // remain in cpuMs.
    QMap<QString, double> threadCpuMs;
    // Resident memory now and its high-water mark, in KiB.
    double rssKib = -1, peakKib = -1;
    // Bytes in use from the allocator, in KiB.
    double heapKib = -1;
    // Bytes the process caused to be written to storage. On Linux this is
    // `write_bytes` of /proc/self/io; on macOS the disk I/O of
    // proc_pid_rusage.
    double diskWrittenBytes = -1;
    // Block output operations from getrusage.
    double blockWrites = -1;
    // write(2)-family calls and their bytes, to any file, pipe or socket
    // (Linux `syscw` and `wchar`). macOS does not expose them.
    double writeCalls = -1, writeCallBytes = -1;
};

Resources sampleResources();

// The change from `start` to `end` as sample stages: `cpu_ms`,
// `main_cpu_ms`, `background_cpu_ms`, `rss_end_kib`, `rss_growth_kib`,
// `peak_kib`, `peak_rise_kib`, `heap_end_kib`, `heap_growth_kib`,
// `disk_written_bytes`, `block_writes`, `write_calls` and
// `write_call_bytes`. A value one side lacks is left out.
QJsonObject resourceStages(const Resources &start, const Resources &end);

// The CPU time of each thread name between the two samples, in ms, for
// threads that used any.
QJsonObject threadStages(const Resources &start, const Resources &end);
} // namespace kobalt::scenario
