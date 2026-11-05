#include "support/qt_compat.h"
#include "support/diagnostics.h"
#include "support/crash_context.h"
#include "support/strings.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <vector>
#include <thread>
#include <atomic>
#include <dlfcn.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <QJsonArray>

// Count actual sync requests, while still performing the filesystem sync.
static std::atomic<int> syncCalls{ 0 };

extern "C" int fdatasync(int descriptor)
{
    ++syncCalls;
    static const auto sync = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
    if (!sync)
        qFatal("Cannot resolve the filesystem sync for the log test");
    return sync(descriptor);
}

static QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read log");
    return file.readAll();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto device = kobalt::diagnosticDeviceFields(
            "PRIVATE-SERIAL,4.9.77,4.45.23646,4.9.77,4.9.77,00000000-0000-0000-0000-000000000390\n");
    if (device.value("hardware_uuid").toString() != "00000000-0000-0000-0000-000000000390"
        || device.value("firmware").toString() != "4.45.23646" || device.size() != 2
        || QJsonDocument(device).toJson().contains("PRIVATE-SERIAL")) {
        qFatal("Device diagnostics lost model or disclosed serial");
    }
    if (!kobalt::diagnosticDeviceFields("PRIVATE-SERIAL,broken").isEmpty()
        || !kobalt::diagnosticDeviceFields("PRIVATE-SERIAL,x,private firmware,x,x,PRIVATE-SERIAL")
                    .isEmpty()) {
        qFatal("Malformed device fields disclosed arbitrary data");
    }
    const auto code = QStringLiteral("error.native_publication_transfer_timed_out");
    if (kobalt::diagnosticErrorCode(kobalt::strings::get(code.toLatin1().constData())) != code
        || kobalt::diagnosticErrorCode(code) != code || !kobalt::diagnosticErrorCode({}).isEmpty()
        || kobalt::diagnosticErrorCode("Cannot read /private/book.epub") != "unknown")
        qFatal("Error codes lost or arbitrary error text disclosed");
    QTemporaryDir directory;
    if (!directory.isValid())
        qFatal("No temporary directory");
    if (kobalt::diagnosticsActive())
        qFatal("Diagnostics reported active before initialization");
    kobalt::initializeDiagnostics(directory.path());
    if (!kobalt::diagnosticsActive())
        qFatal("Initialized diagnostics reported inactive");
    QJsonObject startedOperation, endedOperation;
    kobalt::setDiagnosticObserver([&](const QJsonObject &entry) {
        if (entry.value("operation").toString() != "test-operation")
            return;
        if (entry.value("event").toString() == "operation-start")
            startedOperation = entry;
        if (entry.value("event").toString() == "operation-end")
            endedOperation = entry;
    });
    {
        kobalt::DiagnosticOperation operation("test-operation");
        if (startedOperation.isEmpty() || !endedOperation.isEmpty())
            qFatal("Operation start was not recorded before work");
    }
    if (endedOperation.value("operation_id") != startedOperation.value("operation_id")
        || endedOperation.value("elapsed_ms").toDouble(-1) < 0
        || !startedOperation.value("main_thread").toBool()
        || startedOperation.value("thread").toString().isEmpty())
        qFatal("Operation correlation, duration or thread was lost");
    kobalt::setDiagnosticObserver({});
    kobalt::flushDiagnostics();
    // Normal logs keep the small production limit, including across a queued
    // profile/mode switch. Verbose retention is checked separately below.
    kobalt::initializeDiagnostics(directory.path(), false);
    const auto path = (directory.path() + "/" + "events.jsonl");
    auto writer = [] {
        for (int i = 0; i < 600; ++i) {
            errno = EDOM;
            // Padding makes the concurrent records pass the rotation limit
            // independently of optional memory samples.
            kobalt::diagnostic("render", { { "sequence", i }, { "padding", QString(64, 'p') } },
                               false);
            if (errno != EDOM)
                qFatal("Logging changed errno");
        }
    };
    std::thread first(writer), second(writer);
    first.join();
    second.join();
    kobalt::flushDiagnostics();
    for (const auto &file : { path, path + ".previous" }) {
        const auto bytes = read(file);
        if (bytes.isEmpty() || bytes.size() > 128 * 1024 || !bytes.endsWith('\n'))
            qFatal("Unbounded or incomplete log");
        for (const auto &line : bytes.split('\n')) {
            if (line.isEmpty())
                continue;
            const auto object = QJsonDocument::fromJson(line).object();
            if (!object.contains("event") || !object.contains("monotonic_ms")
                || !object.contains("pid"))
                qFatal("Invalid record");
        }
    }
    const auto before = read(path);
    kobalt::pauseDiagnostics(true);
    if (kobalt::diagnosticsActive())
        qFatal("Paused diagnostics reported active");
    kobalt::diagnostic("exported-storage");
    kobalt::flushDiagnostics();
    if (read(path) != before)
        qFatal("Wrote diagnostics during USB export");
    kobalt::pauseDiagnostics(false);
    kobalt::diagnostic("remounted-storage");
    kobalt::flushDiagnostics();
    if (read(path) == before)
        qFatal("Did not resume diagnostics after USB");
    const auto resumed = read(path);
    kobalt::diagnostic("oversized", { { "value", QString(5000, 'x') } });
    kobalt::flushDiagnostics();
    if (read(path) != resumed)
        qFatal("Oversized entry accepted");
    QFile::remove(path);
    const auto target = (directory.path() + "/" + "untouched");
    QFile sentinel(target);
    if (!sentinel.open(QIODevice::WriteOnly) || sentinel.write("sentinel") != 8)
        qFatal("Cannot write symlink target fixture");
    sentinel.close();
    if (::symlink(QFile::encodeName(target), QFile::encodeName(path)) != 0)
        qFatal("Cannot create test symlink");
    kobalt::diagnostic("must-not-follow");
    kobalt::flushDiagnostics();
    if (read(target) != "sentinel")
        qFatal("Followed log symlink");
    kobalt::initializeDiagnostics(target + "/not-a-directory");
    kobalt::diagnostic("unavailable");
    kobalt::flushDiagnostics();
    if (read(target) != "sentinel")
        qFatal("Logging failure damaged file");
    const int beforeNormal = syncCalls.load();
    QTemporaryDir quiet;
    kobalt::initializeDiagnostics(quiet.path(), false);
    const auto quietPath = quiet.path() + "/events.jsonl";
    if (kobalt::diagnosticsActive())
        qFatal("Quiet logging enabled expensive instrumentation");
    for (int i = 0; i < 20; ++i)
        kobalt::diagnostic("turn", { { "sequence", i } });
    // Ordinary activity in normal mode waits for the routine delay, so reading
    // does not write the log every few seconds. The error and the lifecycle
    // flush below write it.
    QThread::msleep(1500);
    if (QFile::exists(quietPath))
        qFatal("Normal activity was written before the routine delay");
    QThread::msleep(50);
    if (syncCalls.load() != beforeNormal)
        qFatal("Normal activity requested a storage sync");
    QJsonObject observed;
    kobalt::setDiagnosticObserver([&](const QJsonObject &entry) { observed = entry; });
    if (!kobalt::diagnosticsActive())
        qFatal("Normal disk policy disabled the evaluation observer");
    kobalt::diagnostic("observed", { { "measurement", 42 } });
    if (observed.value("measurement").toInt() != 42 || observed.contains("rss_kib"))
        qFatal("Observer lost fields or sampled memory on the caller");
    kobalt::setDiagnosticObserver({});
    kobalt::setDiagnosticFontBytes(123456);
    kobalt::setDiagnosticNativeFontBytes(654321);
    QJsonObject sampledMemory;
    std::atomic<bool> sampled{ false };
    kobalt::sampleDiagnosticMemory([&](QJsonObject fields) {
        if (QThread::currentThread() == app.thread())
            qFatal("Memory sampled on the main thread");
        sampledMemory = std::move(fields);
        sampled.store(true);
    });
    QElapsedTimer sampleWait;
    sampleWait.start();
    while (!sampled.load() && sampleWait.elapsed() < 3000)
        QThread::msleep(10);
    if (!sampled.load() || !sampledMemory.value("rss_kib").isDouble()
        || !sampledMemory.value("peak_rss_kib").isDouble())
        qFatal("Explicit performance memory sample was lost");
    if (sampledMemory.value("private_font_bytes").toInt() != 123456
        || sampledMemory.value("native_fallback_font_bytes").toInt() != 654321
        || sampledMemory.value("fd_soft_limit").toDouble() <= 0
        || !sampledMemory.value("resource_sample_monotonic_ms").isDouble())
        qFatal("Resource sample lost published font counters, descriptor limit or timestamp");
#ifdef __linux__
    if (sampledMemory.value("fd_count").toInt() <= 0
        || sampledMemory.value("fd_highest").toInt() < 0)
        qFatal("Resource sample lost descriptor usage");
#endif
    kobalt::setDiagnosticFontBytes(0);
    kobalt::setDiagnosticNativeFontBytes(0);
    if (syncCalls.load() != beforeNormal)
        qFatal("Memory sampling requested a storage sync");
    {
        // A normal batch does not write its resource sample, but it still
        // refreshes the crash report's snapshot. Wait out the one-second sample
        // interval so the next batch takes a sample. Flush first, so the
        // batch with the explicit sample above cannot overwrite the marker.
        kobalt::flushDiagnostics();
        const char noSample[] = "none\n";
        kobalt::crashContext::resources.store(noSample, sizeof(noSample) - 1);
        QThread::msleep(1100);
        kobalt::diagnostic("sample-probe");
        kobalt::flushDiagnostics();
        if (!QByteArray(kobalt::crashContext::resources.data,
                        int(kobalt::crashContext::resources.size))
                     .contains("resource_sample_monotonic_ms"))
            qFatal("A normal batch did not refresh the crash report's resource sample");
        if (read(quietPath).contains("rss_kib"))
            qFatal("A normal batch wrote its resource sample");
    }
    {
        // Run out of descriptors for real: the report must still list them.
        QJsonObject observed;
        kobalt::setDiagnosticObserver([&](const QJsonObject &entry) { observed = entry; });
        kobalt::reportDescriptorExhaustion("not-exhausted", EACCES);
        if (!observed.isEmpty())
            qFatal("A different open error reported descriptor exhaustion");
        rlimit original{};
        if (getrlimit(RLIMIT_NOFILE, &original) != 0)
            qFatal("Cannot read the descriptor limit");
        rlimit lowered = original;
        lowered.rlim_cur = 96;
        if (setrlimit(RLIMIT_NOFILE, &lowered) != 0)
            qFatal("Cannot lower the descriptor limit");
        std::vector<int> opened;
        int error = 0;
        for (int i = 0; i < 1000; ++i) {
            const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                error = errno;
                break;
            }
            opened.push_back(fd);
        }
        if (error != EMFILE)
            qFatal("The lowered limit did not give EMFILE");
        errno = EMFILE;
        kobalt::reportDescriptorExhaustion("test-open", error);
        const int after = errno;
        // A second report within a minute is skipped.
        auto first = observed;
        observed = {};
        kobalt::reportDescriptorExhaustion("test-open-again", error);
        // The line reaches the log while descriptors are still exhausted.
        const int spare = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (spare >= 0)
            opened.push_back(spare);
        kobalt::flushDiagnostics();
        for (const int fd : opened)
            ::close(fd);
        if (setrlimit(RLIMIT_NOFILE, &original) != 0)
            qFatal("Cannot restore the descriptor limit");
        const bool logged = read(quietPath).contains("\"descriptor-exhaustion\"");
        if (opened.size() < 32)
            qFatal("The test did not open enough descriptors to exhaust the limit");
        if (!logged)
            qFatal("The descriptor report did not reach the log while descriptors were exhausted");
        if (after != EMFILE)
            qFatal("The descriptor report changed errno");
        if (!observed.isEmpty())
            qFatal("A second descriptor report within a minute was logged");
        if (first.value("event").toString() != "descriptor-exhaustion"
            || first.value("context").toString() != "test-open"
            || first.value("errno").toInt() != EMFILE || !first.value("listed").toBool()
            || first.value("open").toInt() < int(opened.size()) - 1)
            qFatal("The descriptor report did not list the open descriptors");
#ifdef Q_OS_LINUX
        const auto top = first.value("most_opened").toArray();
        if (top.isEmpty() || top.first().toObject().value("path").toString() != "/dev/null"
            || top.first().toObject().value("count").toInt() < int(opened.size()) - 1)
            qFatal("The descriptor report did not name the most opened file");
#endif
        kobalt::setDiagnosticObserver({});
    }
    // The descriptor check above wrote its own error; count from here.
    const int beforeError = syncCalls.load();
    kobalt::diagnostic("save", { { "ok", false } }, kobalt::DiagnosticSeverity::Error);
    QElapsedTimer syncWait;
    syncWait.start();
    while (syncCalls.load() == beforeError && syncWait.elapsed() < 3000)
        QThread::msleep(10);
    if (syncCalls.load() == beforeError)
        qFatal("Error batch did not request a sync");
    kobalt::flushDiagnostics();
    const int beforeBoundary = syncCalls.load();
    kobalt::flushDiagnostics();
    if (syncCalls.load() <= beforeBoundary)
        qFatal("Empty lifecycle flush did not sync prior batches");
    const auto quietBytes = read(quietPath);
    if (quietBytes.count("\"event\":\"turn\"") != 20 || !quietBytes.contains("\"sequence\":19")
        || !QRegularExpression(
                    "\"event\":\"save\"[^\\n]*\"severity\":\"error\"|\"severity\":\"error\"[^\\n]*\"event\":\"save\"")
                    .match(QString::fromUtf8(quietBytes))
                    .hasMatch()
        || quietBytes.contains("rss_kib") || quietBytes.contains("resource_sample_monotonic_ms"))
        qFatal("Normal log lost activity fields or wrote a resource sample");
    if (sampledMemory.contains("kobalt_rss_kib") || sampledMemory.contains("kobalt_peak_kib"))
        qFatal("Kobalt's memory share was reported before its base");
    // The base is taken once; later samples report memory above it.
    kobalt::markMemoryBase();
    kobalt::markMemoryBase();
    kobalt::flushDiagnostics();
    std::vector<char> grown(8 * 1024 * 1024, 1);
    sampled.store(false);
    kobalt::sampleDiagnosticMemory([&](QJsonObject fields) {
        sampledMemory = std::move(fields);
        sampled.store(true);
    });
    sampleWait.restart();
    while (!sampled.load() && sampleWait.elapsed() < 3000)
        QThread::msleep(10);
    if (!sampled.load() || sampledMemory.value("kobalt_rss_kib").toDouble() < 4096
        || sampledMemory.value("kobalt_peak_kib").toDouble() < 4096 || grown.back() != 1)
        qFatal("Kobalt's memory share did not follow its base");
    // Replay more than five minutes at a faster event rate than the device
    // trace. Preserve its first input, render, standby and final save records.
    QTemporaryDir verbose;
    kobalt::initializeDiagnostics(verbose.path(), true);
    const auto verbosePath = verbose.path() + "/events.jsonl";
    // A detailed log writes the resource sample into the batch's first line.
    QThread::msleep(1100);
    kobalt::diagnostic("sample-probe");
    kobalt::flushDiagnostics();
    if (!read(verbosePath).contains("\"resource_sample_monotonic_ms\""))
        qFatal("A detailed batch did not write its resource sample");
    kobalt::diagnostic("power-observer",
                       { { "standby_request", true }, { "standby_resume", true } });
    kobalt::diagnostic("standby-request");
    kobalt::diagnostic("standby-resumed");
    for (int turn = 0; turn < 1200; ++turn) {
        kobalt::diagnostic("retention-turn",
                           { { "sequence", turn },
                             { "test_source_ms", turn * 300 },
                             { "padding", QString(512, 'x') } });
        if (turn % 256 == 255)
            kobalt::flushDiagnostics();
    }
    kobalt::diagnostic("retention-final-save");
    kobalt::flushDiagnostics();
    auto retained = read(verbosePath);
    if (retained.size() <= 256 * 1024 || !retained.contains("standby-request")
        || !retained.contains("standby-resumed") || !retained.contains("\"sequence\":0,")
        || !retained.contains("\"sequence\":1199,") || !retained.contains("retention-final-save"))
        qFatal("Verbose trace lost the beginning of a six-minute replay");
    // Force more than five real 16 MiB rotations in bounded chunks. Context must survive,
    // while each event file stays bounded and ends on a complete JSON record.
    for (int batch = 0; batch < 128; ++batch) {
        for (int i = 0; i < 512; ++i)
            kobalt::diagnostic(
                    "retention-load",
                    { { "batch", batch }, { "row", i }, { "padding", QString(2048, 'x') } });
        kobalt::flushDiagnostics();
    }
    for (int generation = 0; generation <= 5; ++generation) {
        const auto file = verbosePath
                + (generation == 0 ? QString()
                           : generation == 1
                           ? QStringLiteral(".previous")
                           : QStringLiteral(".previous.") + QString::number(generation));
        const auto data = read(file);
        if (data.isEmpty() || data.size() > 16 * 1024 * 1024 || !data.endsWith('\n'))
            qFatal("Verbose rotation exceeded its file bound");
        for (const auto &line : data.split('\n'))
            if (!line.isEmpty() && !QJsonDocument::fromJson(line).isObject())
                qFatal("Verbose rotation broke a JSON record");
    }
    if (QFile::exists(verbosePath + ".previous.6"))
        qFatal("Verbose history exceeded five files");
    const auto oldest = read(verbosePath + ".previous.5");
    if (oldest.contains("retention-turn"))
        qFatal("Oldest history was not replaced after five rotations");
    const auto context =
            QJsonDocument::fromJson(read(verbose.path() + "/session-context.json")).object();
    if (context.value("startup").toObject().value("log_limit_bytes").toInt() != 16 * 1024 * 1024
        || context.value("startup").toObject().value("log_history_files").toInt() != 5
        || !context.value("power_observer").toObject().value("standby_resume").toBool())
        qFatal("Rotation lost startup or power context");
    QTemporaryDir deadline;
    kobalt::initializeDiagnostics(deadline.path(), false);
    const auto deadlinePath = deadline.path() + "/events.jsonl";
    kobalt::diagnosticInput();
    kobalt::diagnostic("deadline", {}, kobalt::DiagnosticSeverity::Warning);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!QFile::exists(deadlinePath) && elapsed.elapsed() < 6000) {
        kobalt::diagnosticInput();
        QThread::msleep(20);
    }
    if (!QFile::exists(deadlinePath) || elapsed.elapsed() < 900)
        qFatal("Input did not defer diagnostics or prevented the maximum-delay flush");
    kobalt::pauseDiagnostics(true);
    qInfo("PASS: bounded concurrent logs, normal activity, error batches, observers, batching and safe files");
}
