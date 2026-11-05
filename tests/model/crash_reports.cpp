#include "platforms/kobo/common/crash_reports.h"
#include "support/diagnostics.h"
#include "support/crash_context.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

// Each crash runs in a child that executes this test again. macOS aborts a
// forked child that uses Qt's file APIs without exec once the parent has used
// them. The child first installs a handler that stands for Nickel's own crash
// handler: it exits with 100 plus the signal. That checks the chaining, and no
// crash reaches the host's crash reporter.
namespace {
const char *self = nullptr;

void hostHandler(int signal)
{
    _exit(100 + signal);
}

extern "C" __attribute__((noinline)) void kobalt_test_crash_here()
{
    volatile int *volatile target = nullptr;
    *target = 1;
}

int crash(const char *kind, const char *directory, const char *mount)
{
    for (const int signal : { SIGSEGV, SIGBUS, SIGABRT })
        std::signal(signal, hostHandler);
    kobalt::installCrashReports(QString::fromLocal8Bit(directory), QString::fromLocal8Bit(mount));
    kobalt::diagnostic("font-load-start", { { "file_bytes", 123456 }, { "retained_bytes", 7890 } },
                       false);
    const char resource[] = "{\"resource_sample_monotonic_ms\":1,\"rss_kib\":456}\n";
    kobalt::crashContext::resources.store(resource, sizeof(resource) - 1);
    // A writer interrupted while owning a slot must not deadlock the handler.
    kobalt::crashContext::events[15].busy.test_and_set();
    if (std::strcmp(kind, "sent") == 0) {
        kill(getpid(), SIGSEGV);
        for (;;)
            pause();
    }
    if (std::strcmp(kind, "abort") == 0)
        std::abort();
    kobalt_test_crash_here();
    return 1;
}

int crashChild(const QString &directory, const QString &mount, const char *kind)
{
    const auto directoryName = QFile::encodeName(directory), mountName = QFile::encodeName(mount);
    const pid_t child = fork();
    if (child < 0)
        qFatal("Cannot fork");
    if (child == 0) {
        execlp(self, self, "--crash", kind, directoryName.constData(), mountName.constData(),
               nullptr);
        _exit(2);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child)
        qFatal("Cannot wait for the crashing child");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

QStringList reports(const QString &directory)
{
    return QDir(directory).entryList(
            { QStringLiteral("crash-*.log"), QStringLiteral("crash-*.txt") }, QDir::Files,
            QDir::Name);
}

QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read crash report");
    return file.readAll();
}

bool faultStatus(int status)
{
    return status == 100 + SIGSEGV || status == 100 + SIGBUS;
}
} // namespace

// Crashes child processes with a fault and with `abort`. On Linux it also sends
// `SIGSEGV` and verifies sender fields instead of a fault address. Reports
// include a bounded resource snapshot and recent operation breadcrumbs,
// including events that have not reached the asynchronous log. An interrupted
// breadcrumb writer must not block the handler. Each report must name the
// signal and contain the backtrace and symbol sections, and the fault report
// must name the crashing function. The previous handler must still run after
// each report. A full set of reports must keep the newest ones and leave other
// files alone. Without the mounted storage, no report may be written. Register
// capture and stack scanning for ARM Linux run only on a Kobo; read a report on
// a device after a crash to check them.
int main(int argc, char **argv)
{
    self = argv[0];
    if (argc == 5 && std::strcmp(argv[1], "--crash") == 0)
        return crash(argv[2], argv[3], argv[4]);
    QTemporaryDir root;
    if (!root.isValid())
        qFatal("No temporary directory");

    const auto directory = root.path() + "/crash-reporting";
    if (!faultStatus(crashChild(directory, {}, "fault")))
        qFatal("A fault did not reach the previous handler");
    auto names = reports(directory);
    if (names.size() != 1)
        qFatal("A fault did not write one report");
    auto report = read(directory + '/' + names.first());
    if (!report.startsWith("Kobalt crash report\n")
        || !(report.contains("\nsignal: SIGSEGV (") || report.contains("\nsignal: SIGBUS ("))
        || !report.contains("\nbacktrace:\n") || !report.contains("\nsymbols:\n")
        || !report.endsWith("end\n"))
        qFatal("The fault report lost its signal, backtrace or symbols:\n%s", report.constData());
    if (!report.contains("event=font-load-start") || !report.contains("retained_bytes=7890")
        || !report.contains("resource_sample_monotonic_ms"))
        qFatal("Crash context lost an unflushed event or resource sample");
    if (!report.contains("kobalt_test_crash_here"))
        qFatal("The fault report does not name the crashing function:\n%s", report.constData());

    if (crashChild(directory, {}, "abort") != 100 + SIGABRT)
        qFatal("An abort did not reach the previous handler");
    names = reports(directory);
    if (names.size() != 2)
        qFatal("An abort did not add a second report");
    bool aborted = false;
    for (const auto &name : names)
        aborted = aborted || read(directory + '/' + name).contains("\nsignal: SIGABRT (");
    if (!aborted)
        qFatal("The abort report lost its signal");

    // A full set keeps the newest reports and leaves other files alone.
    const auto full = root.path() + "/full";
    QDir().mkpath(full);
    for (int i = 0; i < kobalt::crashReportLimit + 5; ++i) {
        QFile file(
                full
                + QStringLiteral("/crash-20000101-0000%1-1.txt").arg(i, 2, 10, QLatin1Char('0')));
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Cannot create an old report");
    }
    QFile notes(full + "/notes.txt");
    if (!notes.open(QIODevice::WriteOnly))
        qFatal("Cannot create an unrelated file");
    notes.close();
    if (!faultStatus(crashChild(full, {}, "fault")))
        qFatal("A fault with a full set did not reach the previous handler");
    names = reports(full);
    if (names.size() != kobalt::crashReportLimit || names.first() != "crash-20000101-000006-1.txt"
        || names.last().startsWith("crash-20000101-") || !QFile::exists(full + "/notes.txt"))
        qFatal("A full set did not keep the newest reports: %s", qPrintable(names.join(", ")));

#ifdef __linux__
    // Darwin reports kill(SIGSEGV) as SEGV_ACCERR without sender information.
    // Linux supplies SI_USER, which is the Kobo report format under test.
    const auto sent = root.path() + "/sent";
    const auto sentStatus = crashChild(sent, {}, "sent");
    if (sentStatus != 100 + SIGSEGV)
        qFatal("Sent signal did not chain: %d", sentStatus);
    const auto sentReport = read(sent + '/' + reports(sent).first());
    if (!sentReport.contains("sender_pid:") || !sentReport.contains("sender_uid:")
        || sentReport.contains("\naddress:"))
        qFatal("Sent signal was reported as a fault address");
#endif

    // Without the mounted storage, the report is skipped and the crash still chains.
    const auto unmounted = root.path() + "/unmounted";
    QDir().mkpath(unmounted);
    const auto guarded = root.path() + "/guarded";
    if (!faultStatus(crashChild(guarded, unmounted, "fault")))
        qFatal("A fault without storage did not reach the previous handler");
    if (QFileInfo::exists(guarded))
        qFatal("A report was written without the mounted storage");
    return 0;
}
