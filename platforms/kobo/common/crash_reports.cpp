#include "crash_reports.h"
#include "build_info.h"
#include "support/diagnostics.h"
#include "support/crash_context.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#  include <sys/prctl.h>
#  include <sys/syscall.h>
#endif

// Everything that runs inside the signal handler uses only system calls and
// static buffers: no allocation, no locks, no Qt and no stdio. The handler
// writes the registers, stack and mappings first and syncs them. Only then does
// it run the unwinder and dladdr, which are not async-signal-safe and can fault
// or deadlock. Either way the previous handler still receives the crash.
namespace kobalt {
namespace {
const int fatalSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
constexpr int signalCount = sizeof(fatalSignals) / sizeof(fatalSignals[0]);
struct sigaction previous[signalCount];
std::atomic<bool> reporting{ false };
char directory[512], mountPoint[512], mountParent[512], oldest[1024];
char header[2048];
size_t headerSize = 0;
alignas(16) char alternateStack[64 * 1024];

// The mappings are read at crash time, because libraries load after startup.
char maps[192 * 1024];

struct Region
{
    uintptr_t start, end, offset;
    const char *path;
    size_t pathSize;
    bool readable, executable;
};

Region regions[1024];
int regionCount = 0;
constexpr int candidateLimit = 64;
uintptr_t candidates[candidateLimit];
int candidateCount = 0;
void *frames[64];
int frameCount = 0;

class Output
{
public:
    void open(int descriptor)
    {
        fd = descriptor;
        used = 0;
    }

    Output &operator<<(const char *text) { return put(text, std::strlen(text)); }

    Output &put(const char *text, size_t size)
    {
        while (size) {
            if (used == sizeof(buffer))
                flush();
            const size_t count = size < sizeof(buffer) - used ? size : sizeof(buffer) - used;
            std::memcpy(buffer + used, text, count);
            used += count;
            text += count;
            size -= count;
        }
        return *this;
    }

    Output &hex(uintptr_t value)
    {
        char digits[2 + 2 * sizeof(uintptr_t)];
        size_t at = sizeof(digits);
        do {
            digits[--at] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while (value);
        digits[--at] = 'x';
        digits[--at] = '0';
        return put(digits + at, sizeof(digits) - at);
    }

    Output &dec(long long value)
    {
        char digits[24];
        size_t at = sizeof(digits);
        const bool negative = value < 0;
        unsigned long long magnitude =
                negative ? 0ULL - static_cast<unsigned long long>(value) : value;
        do {
            digits[--at] = char('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude);
        if (negative)
            digits[--at] = '-';
        return put(digits + at, sizeof(digits) - at);
    }

    void flush()
    {
        size_t written = 0;
        while (written < used) {
            const auto count = ::write(fd, buffer + written, used - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            written += size_t(count);
        }
        used = 0;
    }

private:
    int fd = -1;
    char buffer[4096];
    size_t used = 0;
};

Output out;

const char *signalName(int signal)
{
    switch (signal) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return "signal";
    }
}

struct Clock
{
    int year, month, day, hour, minute, second;
};

// Civil date from days since 1970, without the time zone or locale state of
// gmtime. Kobo keeps local wall time in the system clock, so the result reads
// as local time there.
Clock clockFields(long long seconds)
{
    long long days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = unsigned(days - era * 146097);
    const unsigned yearOfEra =
            (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned shifted = (5 * dayOfYear + 2) / 153;
    const unsigned month = shifted < 10 ? shifted + 3 : shifted - 9;
    return { int(yearOfEra + era * 400 + (month <= 2)),
             int(month),
             int(dayOfYear - (153 * shifted + 2) / 5 + 1),
             int(rest / 3600),
             int(rest / 60 % 60),
             int(rest % 60) };
}

char *digits(char *at, int value, int width)
{
    for (int i = width - 1; i >= 0; --i, value /= 10)
        at[i] = char('0' + value % 10);
    return at + width;
}

bool append(char *target, size_t capacity, size_t &used, const char *text)
{
    const size_t size = std::strlen(text);
    if (used + size >= capacity)
        return false;
    std::memcpy(target + used, text, size + 1);
    used += size;
    return true;
}

uintptr_t parseHex(const char *&at, const char *end)
{
    uintptr_t value = 0;
    for (; at < end; ++at) {
        const char c = *at;
        if (c >= '0' && c <= '9')
            value = value * 16 + uintptr_t(c - '0');
        else if (c >= 'a' && c <= 'f')
            value = value * 16 + uintptr_t(c - 'a' + 10);
        else
            break;
    }
    return value;
}

void readMappings()
{
    regionCount = 0;
    const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    size_t size = 0;
    while (size < sizeof(maps) - 1) {
        const auto count = ::read(fd, maps + size, sizeof(maps) - 1 - size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        size += size_t(count);
    }
    ::close(fd);
    const char *at = maps, *end = maps + size;
    // start-end perms offset device inode [path]
    while (at < end && regionCount < int(sizeof(regions) / sizeof(regions[0]))) {
        const char *line = at;
        const char *lineEnd = static_cast<const char *>(std::memchr(at, '\n', size_t(end - at)));
        if (!lineEnd)
            lineEnd = end;
        Region region{};
        region.start = parseHex(at, lineEnd);
        if (at < lineEnd && *at == '-')
            ++at;
        region.end = parseHex(at, lineEnd);
        if (lineEnd - at > 5) {
            region.readable = at[1] == 'r';
            region.executable = at[3] == 'x';
            at += 6;
            region.offset = parseHex(at, lineEnd);
        }
        for (int field = 0; field < 2 && at < lineEnd; ++field) {
            while (at < lineEnd && *at == ' ')
                ++at;
            while (at < lineEnd && *at != ' ')
                ++at;
        }
        while (at < lineEnd && *at == ' ')
            ++at;
        region.path = at;
        region.pathSize = size_t(lineEnd - at);
        if (region.end > region.start && at > line)
            regions[regionCount++] = region;
        at = lineEnd + 1;
    }
}

const Region *regionFor(uintptr_t address)
{
    for (int i = 0; i < regionCount; ++i)
        if (address >= regions[i].start && address < regions[i].end)
            return &regions[i];
    return nullptr;
}

void describe(uintptr_t address)
{
    out.hex(address);
    const auto *region = regionFor(address);
    if (region && region->executable) {
        out << " ";
        if (region->pathSize)
            out.put(region->path, region->pathSize);
        else
            out << "[anonymous]";
        out << "+";
        out.hex(address - region->start + region->offset);
    }
    out << "\n";
}

struct Registers
{
    uintptr_t pc = 0, lr = 0, sp = 0;
};

Registers writeRegisters(void *context)
{
    Registers result;
    if (!context)
        return result;
    out << "\nregisters:\n";
#if defined(__linux__) && defined(__arm__)
    const auto &m = static_cast<ucontext_t *>(context)->uc_mcontext;
    const unsigned long values[] = { m.arm_r0, m.arm_r1, m.arm_r2, m.arm_r3, m.arm_r4,  m.arm_r5,
                                     m.arm_r6, m.arm_r7, m.arm_r8, m.arm_r9, m.arm_r10, m.arm_fp,
                                     m.arm_ip, m.arm_sp, m.arm_lr, m.arm_pc, m.arm_cpsr };
    const char *names[] = { "r0", "r1",  "r2", "r3", "r4", "r5", "r6", "r7",  "r8",
                            "r9", "r10", "fp", "ip", "sp", "lr", "pc", "cpsr" };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        out << "  " << names[i] << " ";
        out.hex(values[i]);
        out << "\n";
    }
    result = { m.arm_pc, m.arm_lr, m.arm_sp };
#elif defined(__linux__) && defined(__aarch64__)
    const auto &m = static_cast<ucontext_t *>(context)->uc_mcontext;
    for (int i = 0; i < 31; ++i) {
        out << "  x";
        out.dec(i);
        out << " ";
        out.hex(m.regs[i]);
        out << "\n";
    }
    out << "  sp ";
    out.hex(m.sp);
    out << "\n  pc ";
    out.hex(m.pc);
    out << "\n";
    result = { m.pc, m.regs[30], m.sp };
#elif defined(__APPLE__) && defined(__aarch64__)
    const auto &s = static_cast<ucontext_t *>(context)->uc_mcontext->__ss;
    result = { uintptr_t(__darwin_arm_thread_state64_get_pc(s)),
               uintptr_t(__darwin_arm_thread_state64_get_lr(s)),
               uintptr_t(__darwin_arm_thread_state64_get_sp(s)) };
    out << "  sp ";
    out.hex(result.sp);
    out << "\n  lr ";
    out.hex(result.lr);
    out << "\n  pc ";
    out.hex(result.pc);
    out << "\n";
#elif defined(__APPLE__) && defined(__x86_64__)
    const auto &s = static_cast<ucontext_t *>(context)->uc_mcontext->__ss;
    result = { uintptr_t(s.__rip), 0, uintptr_t(s.__rsp) };
    out << "  rsp ";
    out.hex(result.sp);
    out << "\n  rip ";
    out.hex(result.pc);
    out << "\n";
#else
    out << "  unavailable on this architecture\n";
#endif
    return result;
}

// Words on the stack that point into executable mappings. Most are return
// addresses, so they show the call chain where the unwinder cannot.
void writeStack(uintptr_t sp)
{
    candidateCount = 0;
    const auto *region = regionFor(sp);
    if (!region || !region->readable)
        return;
    out << "\nstack return-address candidates:\n";
    const auto *word = reinterpret_cast<const uintptr_t *>(sp & ~uintptr_t(sizeof(uintptr_t) - 1));
    const size_t available = (region->end - uintptr_t(word)) / sizeof(uintptr_t);
    const size_t count = available < 2048 ? available : 2048;
    for (size_t i = 0; i < count && candidateCount < candidateLimit; ++i) {
        const auto *target = regionFor(word[i]);
        if (!target || !target->executable)
            continue;
        candidates[candidateCount++] = word[i];
        out << "  sp+";
        out.hex(i * sizeof(uintptr_t));
        out << " ";
        describe(word[i]);
    }
}

void writeSymbol(uintptr_t address)
{
    out << "  ";
    out.hex(address);
    Dl_info info{};
    if (address && dladdr(reinterpret_cast<void *>(address), &info) && info.dli_fname) {
        out << " " << info.dli_fname << "+";
        out.hex(address - uintptr_t(info.dli_fbase));
        if (info.dli_sname) {
            out << " " << info.dli_sname << "+";
            out.hex(address - uintptr_t(info.dli_saddr));
        }
    }
    out << "\n";
}

volatile sig_atomic_t crashSignal = 0;

void restorePrevious(int signal)
{
    for (int i = 0; i < signalCount; ++i)
        if (fatalSignals[i] == signal)
            sigaction(signal, &previous[i], nullptr);
}

void timedOut(int)
{
    restorePrevious(crashSignal);
    raise(crashSignal);
}

void writeReport(int signal, siginfo_t *info, void *context)
{
    if (mountPoint[0]) {
        struct stat mounted{}, parent{};
        if (::stat(mountPoint, &mounted) != 0 || ::stat(mountParent, &parent) != 0
            || mounted.st_dev == parent.st_dev)
            return;
    }
    ::mkdir(directory, 0700);
    if (oldest[0])
        ::unlink(oldest);

    struct timespec now{}, monotonic{};
    clock_gettime(CLOCK_REALTIME, &now);
    clock_gettime(CLOCK_MONOTONIC, &monotonic);
    const auto clock = clockFields(now.tv_sec);
    char stamp[16], *at = stamp;
    at = digits(at, clock.year, 4);
    at = digits(at, clock.month, 2);
    at = digits(at, clock.day, 2);
    *at++ = '-';
    at = digits(at, clock.hour, 2);
    at = digits(at, clock.minute, 2);
    at = digits(at, clock.second, 2);
    *at = 0;
    char pid[16], *pidEnd = pid + sizeof(pid) - 1;
    *pidEnd = 0;
    for (auto value = (unsigned long)getpid(); value || pidEnd == pid + sizeof(pid) - 1;
         value /= 10)
        *--pidEnd = char('0' + value % 10);
    char path[1024];
    size_t used = 0;
    path[0] = 0;
    if (!append(path, sizeof(path), used, directory) || !append(path, sizeof(path), used, "/crash-")
        || !append(path, sizeof(path), used, stamp) || !append(path, sizeof(path), used, "-")
        || !append(path, sizeof(path), used, pidEnd) || !append(path, sizeof(path), used, ".log"))
        return;
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    out.open(fd);

    out << "Kobalt crash report\n";
    out.put(header, headerSize);
    out << "time: ";
    char display[] = "0000-00-00 00:00:00";
    digits(display, clock.year, 4);
    digits(display + 5, clock.month, 2);
    digits(display + 8, clock.day, 2);
    digits(display + 11, clock.hour, 2);
    digits(display + 14, clock.minute, 2);
    digits(display + 17, clock.second, 2);
    out << display << "\nmonotonic_ms: ";
    out.dec(monotonic.tv_sec * 1000LL + monotonic.tv_nsec / 1000000);
    out << "\npid: " << pidEnd;
#ifdef __linux__
    out << "\nthread: ";
    out.dec(syscall(SYS_gettid));
    char name[17] = {};
    if (prctl(PR_GET_NAME, name, 0, 0, 0) == 0)
        out << " " << name;
#endif
    out << "\nsignal: " << signalName(signal) << " (";
    out.dec(signal);
    out << ")\n";
    if (info) {
        out << "code: ";
        out.dec(info->si_code);
        if (info->si_code <= 0 || info->si_code == SI_USER || info->si_code == SI_QUEUE) {
            out << "\nsender_pid: ";
            out.dec(info->si_pid);
            out << "\nsender_uid: ";
            out.dec(info->si_uid);
        } else if (signal == SIGSEGV || signal == SIGBUS || signal == SIGILL || signal == SIGFPE) {
            out << "\naddress: ";
            out.hex(uintptr_t(info->si_addr));
        }
        out << "\n";
    }

    out.flush();
    crashContext::writeTo(fd);
    readMappings();
    const auto registers = writeRegisters(context);
    out << "\npc ";
    describe(registers.pc);
    out << "lr ";
    describe(registers.lr);
    // The scan reads only inside the stack's own mapping, so it cannot fault.
    writeStack(registers.sp);

    out << "\nexecutable mappings:\n";
    for (int i = 0; i < regionCount; ++i) {
        if (!regions[i].executable)
            continue;
        out << "  ";
        out.hex(regions[i].start);
        out << "-";
        out.hex(regions[i].end);
        out << " ";
        out.hex(regions[i].offset);
        out << " ";
        out.put(regions[i].path, regions[i].pathSize);
        out << "\n";
    }
    out.flush();
    ::fsync(fd);

    // The facts so far are on storage. The unwinder can fault on a damaged
    // stack, and it and dladdr can wait for a loader lock that the crashed
    // code holds. A nested fault reaches the previous handler through
    // SA_NODEFER. The alarm hands a stuck lookup to it with the crash signal.
    struct sigaction timeout{}, previousAlarm{};
    timeout.sa_handler = timedOut;
    sigemptyset(&timeout.sa_mask);
    ::sigaction(SIGALRM, &timeout, &previousAlarm);
    ::alarm(3);
    out << "\nbacktrace:\n";
    frameCount = backtrace(frames, int(sizeof(frames) / sizeof(frames[0])));
    for (int i = 0; i < frameCount; ++i) {
        out << "  #";
        out.dec(i);
        out << " ";
        describe(uintptr_t(frames[i]));
    }
    out.flush();
    out << "\nsymbols:\n";
    writeSymbol(registers.pc);
    writeSymbol(registers.lr);
    for (int i = 0; i < frameCount; ++i)
        writeSymbol(uintptr_t(frames[i]));
    for (int i = 0; i < candidateCount; ++i)
        writeSymbol(candidates[i]);
    out << "end\n";
    out.flush();
    ::alarm(0);
    ::sigaction(SIGALRM, &previousAlarm, nullptr);
    ::fsync(fd);
    ::close(fd);
}

// A fault from the faulting instruction happens again once the handler
// returns, now in the previous handler. A signal sent with kill, raise or
// abort does not, so it is raised again for the previous handler.
bool repeatsOnReturn(int signal, const siginfo_t *info)
{
    if (signal == SIGABRT || !info || info->si_code <= 0)
        return false;
    if (info->si_code == SI_USER || info->si_code == SI_QUEUE)
        return false;
    return true;
}

void handle(int signal, siginfo_t *info, void *context)
{
    const int savedErrno = errno;
    // A second crash while writing, in this or another thread, only chains.
    if (!reporting.exchange(true)) {
        crashSignal = signal;
        writeReport(signal, info, context);
    }
    restorePrevious(signal);
    if (!repeatsOnReturn(signal, info)) {
        // SA_NODEFER would deliver the raised signal at once, nested on this
        // handler's stack. Block it, so it arrives after the return, on the
        // thread's own stack. The return restores the thread's signal mask.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, signal);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
        raise(signal);
    }
    errno = savedErrno;
}

bool copy(const QString &value, char *target, size_t capacity)
{
    const auto bytes = QFile::encodeName(value);
    if (size_t(bytes.size()) >= capacity)
        return false;
    std::memcpy(target, bytes.constData(), size_t(bytes.size()) + 1);
    return true;
}

void addHeader(const char *key, const QByteArray &value)
{
    const auto line = QByteArray(key) + ": " + value + '\n';
    if (headerSize + size_t(line.size()) >= sizeof(header))
        return;
    std::memcpy(header + headerSize, line.constData(), size_t(line.size()));
    headerSize += size_t(line.size());
}

// Keeps room for one new report: the handler removes the oldest of a full set.
void prune(const QString &path)
{
    oldest[0] = 0;
    // Reports end in .log, so Nickel does not import them as TXT books. Earlier
    // builds wrote .txt; the same names sort by time, so both count here.
    auto names =
            QDir(path).entryList({ QStringLiteral("crash-*.log"), QStringLiteral("crash-*.txt") },
                                 QDir::Files | QDir::NoSymLinks, QDir::Name);
    while (names.size() > crashReportLimit)
        QFile::remove(path + '/' + names.takeFirst());
    if (names.size() == crashReportLimit
        && !copy(path + '/' + names.first(), oldest, sizeof(oldest)))
        oldest[0] = 0;
}
} // namespace

void installCrashReports(const QString &path, const QString &mount)
{
    static bool installed = false;
    if (installed)
        return;
    if (!copy(path, directory, sizeof(directory)) || !copy(mount, mountPoint, sizeof(mountPoint))
        || !copy(mount.isEmpty() ? QString() : QFileInfo(mount).absolutePath(), mountParent,
                 sizeof(mountParent)))
        return;
    installed = true;
#ifdef KOBALT_VERSION
    addHeader("version", KOBALT_VERSION);
#endif
    addHeader("commit", QByteArray(kobaltCommit) + (KOBALT_COMMIT_MODIFIED ? " (modified)" : ""));
    addHeader("build", __DATE__ " " __TIME__);
    addHeader("qt", qVersion());
#ifdef KOBALT_READER_HARDWARE
    QFile version("/mnt/onboard/.kobo/version");
    if (version.open(QIODevice::ReadOnly)) {
        const auto device = diagnosticDeviceFields(version.read(512));
        for (auto it = device.begin(); it != device.end(); ++it)
            addHeader(it.key().toLatin1().constData(), it.value().toString().toLatin1());
    }
#endif
    prune(path);
    // The first backtrace loads the unwinder. Do that now, not in the handler.
    void *frame = nullptr;
    backtrace(&frame, 1);
    stack_t stack{};
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof(alternateStack);
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = handle;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < signalCount; ++i)
        sigaction(fatalSignals[i], &action, &previous[i]);
}
} // namespace kobalt
