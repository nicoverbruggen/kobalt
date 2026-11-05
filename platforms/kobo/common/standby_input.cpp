#include "standby_input.h"
#include "standby_diagnostics.h"
#include "support/diagnostics.h"
#include <qpa/qwindowsysteminterface_p.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace kobalt {
namespace {
std::atomic<quint64> acquired{ 0 };
std::atomic<unsigned> acquiring{ 0 };
std::atomic<qint64> acquiredAt{ -1 };
std::atomic<quint64> attempts{ 0 };
std::atomic<quint64> cancellationAttempt{ 0 };
std::atomic<int> wakeLockError{ 0 };
std::atomic<quint64> wakeRequests{ 0 };
// Only the suspend (GUI) thread emits the normal, bounded evidence.
bool capabilitiesRecorded = false, wakeFailureRecorded = false;
QString fallbackState;
int fallbackError = 0;
quint64 fallbackAttempts = 0;

void recordWakeFailure()
{
    const int error = wakeLockError.load();
    if (!error || wakeFailureRecorded)
        return;
    wakeFailureRecorded = true;
    diagnostic("standby-input-wake-failure", { { "errno", error } }, false);
}

void recordFallback(const char *reason, int error)
{
    if (fallbackState == QLatin1String(reason) && fallbackError == error) {
        ++fallbackAttempts;
        return;
    }
    diagnostic("standby-input-fallback",
               { { "reason", reason },
                 { "errno", error },
                 { "previous_reason", fallbackState },
                 { "attempts_since_last_event", QString::number(fallbackAttempts) } },
               false);
    fallbackState = QLatin1String(reason);
    fallbackError = error;
    fallbackAttempts = 1;
}

struct Attempt
{
    unsigned depth = 0;
    bool protect = false;
    bool wakeupChecked = false;
    quint64 acquired = 0;
    quint64 id = 0;
};

thread_local Attempt attempt;

qint64 milliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

bool inputQueued()
{
    using Events = QWindowSystemInterfacePrivate;
    return Events::peekWindowSystemEvent(Events::Touch)
            || Events::peekWindowSystemEvent(Events::Mouse)
            || Events::peekWindowSystemEvent(Events::Key)
            || Events::peekWindowSystemEvent(Events::Wheel);
}

QJsonObject wakeupCountFields(const QString &value)
{
    bool valid = false;
    const auto count = value.toULongLong(&valid);
    auto fields = qt::object(
            { { "wakeup_count_present", !value.isEmpty() }, { "wakeup_count_numeric", valid } });
    if (valid)
        fields["saved_wakeup_count"] = QString::number(count);
    return fields;
}

int inputWakeLock(bool request)
{
    // Keep one descriptor until process exit. Closing it when an attempt
    // returns would race an input thread that already observed its epoch.
    static std::atomic<int> descriptor{ -1 };
    int fd = descriptor.load();
    if (fd < 0 && !request) {
        const int opened = ::open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
        if (opened < 0)
            return errno;
        if (descriptor.compare_exchange_strong(fd, opened))
            fd = opened;
        else
            ::close(opened);
    }
    if (fd < 0)
        return ENODEV;
    int error = 0;
    if (request) {
        // The timeout is in nanoseconds. Expiry also advances the kernel's
        // wakeup count, so an armed suspend remains cancelled after expiry.
        // Never write an untimed lock or unlock a later input's wake event.
        static const char command[] = "kobalt-input 1000000000";
        const auto result = ::write(fd, command, sizeof(command) - 1);
        if (result < 0)
            error = errno;
        else if (result != ssize_t(sizeof(command) - 1))
            error = EIO;
    }
    return error;
}
} // namespace

bool (*nativePowerStringToFile)(const QString &, const QString &);
bool (*nativeDiagnosticPowerStringToFile)(const QString &, const QString &);
int (*standbyInputWakeLock)(bool) = inputWakeLock;

StandbyAcquisition acquireStandbyTouch()
{
    const int savedError = errno;
    StandbyAcquisition input;
    acquiring.fetch_add(1);
    // Pair sequence-cst acquisition/epoch operations with the final check:
    // either the GUI sees this acquisition, or this thread sees the epoch.
    input.sequence = acquired.fetch_add(1) + 1;
    input.cancellationAttempt = cancellationAttempt.load();
    if (input.cancellationAttempt) {
        wakeRequests.fetch_add(1);
        input.wakeError = standbyInputWakeLock(true);
        int noError = 0;
        if (input.wakeError)
            wakeLockError.compare_exchange_strong(noError, input.wakeError);
    }
    input.completedAt = milliseconds();
    acquiredAt.store(input.completedAt, std::memory_order_relaxed);
    // A new preparation can take its baseline during this function. Until
    // its quiet-window timestamp is published, it must see input in progress.
    acquiring.fetch_sub(1);
    errno = savedError;
    return input;
}

void recordStandbyAcquisition(const StandbyAcquisition &input, const char *stage)
{
    if (!diagnosticsActive())
        return;
    if (input.cancellationAttempt)
        diagnostic("standby-input-wake-request",
                   { { "attempt", QString::number(input.cancellationAttempt) },
                     { "sequence", QString::number(input.sequence) },
                     { "ok", input.wakeError == 0 },
                     { "errno", input.wakeError },
                     { "timeout_ms", 1000 },
                     { "stage", stage },
                     { "completed_monotonic_ms", double(input.completedAt) } },
                   false);
    diagnostic("input-acquired",
               { { "sequence", double(input.sequence) },
                 { "stage", stage },
                 { "power_write_attempt", QString::number(activePowerWrite()) } },
               false);
}

void standbyTouchAcquired()
{
    const int savedError = errno;
    recordStandbyAcquisition(acquireStandbyTouch(), "touch-map");
    errno = savedError;
}

StandbyInputScope::StandbyInputScope(bool protect, const QString &wakeupCount)
{
    if (attempt.depth++ == 0) {
        attempt.protect = protect;
        attempt.wakeupChecked = false;
        attempt.acquired = acquired.load(std::memory_order_acquire);
        attempt.id = ++attempts;
        if (diagnosticsActive()) {
            auto fields = wakeupCountFields(wakeupCount);
            fields["guarded"] = protect && bool(nativePowerStringToFile);
            fields["acquired_before"] = double(attempt.acquired);
            fields["attempt"] = QString::number(attempt.id);
            diagnostic("standby-preparation", fields, false);
            sampleStandbyEvidence(attempt.id, "preparation");
        }
    }
}

StandbyInputScope::~StandbyInputScope()
{
    if (--attempt.depth == 0) {
        attempt.protect = false;
        sampleStandbyEvidence(attempt.id, "returned");
    }
}

// Qt6 has no input guard installed. Observe its helper without introducing
// the Qt5 veto or changing native calls when diagnostics are disabled.
extern "C" __attribute__((visibility("default"))) bool
kobalt_observe_power_string_to_file(const QString &path, const QString &value)
{
    const bool wakeupCheck = path == QLatin1String("/sys/power/wakeup_count");
    if (!diagnosticsActive() || !attempt.depth
        || (!wakeupCheck
            && (path != QLatin1String("/sys/power/state") || value != QLatin1String("mem"))))
        return nativeDiagnosticPowerStringToFile(path, value);
    bool saved;
    {
        PowerWriteTrace trace(path, attempt.id);
        saved = nativeDiagnosticPowerStringToFile(path, value);
    }
    const int error = errno;
    auto fields = wakeupCheck ? wakeupCountFields(value) : QJsonObject();
    fields["ok"] = saved;
    fields["errno"] = saved ? 0 : error;
    fields["guarded"] = false;
    fields["attempt"] = QString::number(attempt.id);
    diagnostic(wakeupCheck ? "standby-wakeup-check" : "kernel-suspend-result", fields, false);
    errno = error;
    return saved;
}

extern "C" __attribute__((visibility("default"))) bool
kobalt_power_string_to_file(const QString &path, const QString &value)
{
    const bool wakeupCheck = path == QLatin1String("/sys/power/wakeup_count");
    if (!attempt.depth
        || (!wakeupCheck
            && (path != QLatin1String("/sys/power/state") || value != QLatin1String("mem"))))
        return nativePowerStringToFile(path, value);
    DiagnosticOperation operation(wakeupCheck ? "kernel-wakeup-count" : "kernel-suspend");
    if (!wakeupCheck && attempt.protect && diagnosticsActive())
        diagnostic("standby-input-check",
                   { { "acquired_before", double(attempt.acquired) },
                     { "acquired_now", double(acquired.load(std::memory_order_acquire)) },
                     { "input_queued", inputQueued() } },
                   false);
    quint64 requestsBefore = 0;
    // Keep the final input check immediately next to the native state write.
    // A driver acquisition can precede its Qt enqueue; a short quiet window
    // covers that gap even when acquisition began before this attempt.
    if (!wakeupCheck && attempt.protect) {
        // An input event only cancels manual suspend when Nickel successfully
        // armed the kernel wakeup-count check in this same outer attempt.
        const int requestError = wakeLockError.load();
        const int bridgeError = requestError ? requestError : standbyInputWakeLock(false);
        if (!capabilitiesRecorded) {
            capabilitiesRecorded = true;
            diagnostic("standby-input-capabilities",
                       { { "touch_read_hook", bool(nativeTouchRead) },
                         { "probe_errno", bridgeError },
                         { "numeric_handshake", attempt.wakeupChecked } },
                       false);
        }
        const char *fallback = !attempt.wakeupChecked ? "wakeup-count-not-armed"
                : !nativeTouchRead                    ? "touch-read-unavailable"
                : requestError                        ? "wake-request-failed"
                : bridgeError                         ? "input-wakeup-unavailable"
                                                      : "none";
        recordFallback(fallback, bridgeError);
        recordWakeFailure();
        // An acquisition can outlive the previous native return. Do not
        // attribute its request to this call or publish a new epoch over it.
        const bool alreadyAcquiring = acquiring.load() != 0;
        requestsBefore = wakeRequests.load();
        if (!alreadyAcquiring && attempt.wakeupChecked && !bridgeError && nativeTouchRead)
            cancellationAttempt.store(attempt.id);
        const auto sequence = acquired.load();
        const bool inputInProgress = alreadyAcquiring || acquiring.load() != 0;
        const auto last = acquiredAt.load(std::memory_order_relaxed);
        const auto age = last < 0 ? qint64(-1) : milliseconds() - last;
        const char *reason = nullptr;
        if (inputQueued())
            reason = "queued-input";
        else if (sequence != attempt.acquired)
            reason = "input-during-preparation";
        else if (inputInProgress)
            reason = "input-acquisition-in-progress";
        else if (age >= 0 && age < 200)
            reason = "recent-acquisition";
        if (reason) {
            cancellationAttempt.store(0);
            recordWakeFailure();
            if (diagnosticsActive())
                diagnostic("standby-deferred",
                           { { "reason", reason },
                             { "acquired_before", double(attempt.acquired) },
                             { "acquired_now", double(sequence) },
                             { "acquisition_age_ms", double(age) },
                             { "wakeup_errno", bridgeError } },
                           false);
            errno = EAGAIN;
            return false;
        }
        // The kernel can disarm its event check when it aborts suspend. A
        // retry in this scope must perform a new native count handshake.
        attempt.wakeupChecked = false;
    }
    bool saved;
    {
        PowerWriteTrace trace(path, attempt.id);
        saved = nativePowerStringToFile(path, value);
    }
    const int error = errno;
    if (!wakeupCheck && attempt.protect) {
        cancellationAttempt.store(0);
        const auto requests = wakeRequests.load() - requestsBefore;
        if (requests)
            diagnostic("standby-input-wake-result",
                       { { "attempt", QString::number(attempt.id) },
                         { "requests", QString::number(requests) },
                         { "ok", saved },
                         { "errno", saved ? 0 : error } },
                       false);
        recordWakeFailure();
    }
    if (wakeupCheck) {
        bool numeric = false;
        value.toULongLong(&numeric);
        attempt.wakeupChecked = saved && numeric;
    }
    if (diagnosticsActive()) {
        auto fields = wakeupCheck ? wakeupCountFields(value) : QJsonObject();
        fields["ok"] = saved;
        fields["errno"] = saved ? 0 : error;
        fields["guarded"] = attempt.protect;
        fields["attempt"] = QString::number(attempt.id);
        diagnostic(wakeupCheck ? "standby-wakeup-check" : "kernel-suspend-result", fields, false);
    }
    errno = error;
    return saved;
}
} // namespace kobalt
