#include "standby_input.h"
#include "support/diagnostics.h"
#include <qpa/qwindowsysteminterface_p.h>
#include <atomic>
#include <chrono>
#include <cerrno>

namespace kobalt {
namespace {
std::atomic<quint64> acquired{0};
std::atomic<qint64> acquiredAt{-1};
struct Attempt {
    unsigned depth = 0;
    bool protect = false;
    quint64 acquired = 0;
};
thread_local Attempt attempt;
qint64 milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool inputQueued() {
    using Events = QWindowSystemInterfacePrivate;
    return Events::peekWindowSystemEvent(Events::Touch) || Events::peekWindowSystemEvent(Events::Mouse) ||
           Events::peekWindowSystemEvent(Events::Key) || Events::peekWindowSystemEvent(Events::Wheel);
}
QJsonObject wakeupCountFields(const QString &value) {
    bool valid = false;
    const auto count = value.toULongLong(&valid);
    auto fields = qt::object({{"wakeup_count_present", !value.isEmpty()}, {"wakeup_count_numeric", valid}});
    if (valid) fields["saved_wakeup_count"] = QString::number(count);
    return fields;
}
}
bool (*nativePowerStringToFile)(const QString &, const QString &);
void standbyTouchAcquired() {
    acquiredAt.store(milliseconds(), std::memory_order_relaxed);
    const auto sequence = acquired.fetch_add(1, std::memory_order_release) + 1;
    if (diagnosticsActive())
        diagnostic("input-acquired", {{"sequence", double(sequence)}, {"stage", "touch-map"}}, false);
}
StandbyInputScope::StandbyInputScope(bool protect, const QString &wakeupCount) {
    if (attempt.depth++ == 0) {
        attempt.protect = protect;
        attempt.acquired = acquired.load(std::memory_order_acquire);
        if (diagnosticsActive()) {
            auto fields = wakeupCountFields(wakeupCount);
            fields["guarded"] = protect;
            fields["acquired_before"] = double(attempt.acquired);
            diagnostic("standby-preparation", fields, false);
        }
    }
}
StandbyInputScope::~StandbyInputScope() {
    if (--attempt.depth == 0) attempt.protect = false;
}
extern "C" __attribute__((visibility("default"))) bool kobalt_power_string_to_file(const QString &path,
                                                                                  const QString &value) {
    const bool wakeupCheck = path == QLatin1String("/sys/power/wakeup_count");
    if (!attempt.depth || (!wakeupCheck && (path != QLatin1String("/sys/power/state") || value != QLatin1String("mem"))))
        return nativePowerStringToFile(path, value);
    DiagnosticOperation operation(wakeupCheck ? "kernel-wakeup-count" : "kernel-suspend");
    if (!wakeupCheck && attempt.protect && diagnosticsActive())
        diagnostic("standby-input-check", {{"acquired_before", double(attempt.acquired)},
            {"acquired_now", double(acquired.load(std::memory_order_acquire))}, {"input_queued", inputQueued()}}, false);
    // Keep the final input check immediately next to the native state write.
    // A driver acquisition can precede its Qt enqueue; a short quiet window
    // covers that gap even when acquisition began before this attempt.
    if (!wakeupCheck && attempt.protect) {
        const auto sequence = acquired.load(std::memory_order_acquire);
        const auto last = acquiredAt.load(std::memory_order_relaxed);
        const auto age = last < 0 ? qint64(-1) : milliseconds() - last;
        const char *reason = nullptr;
        if (inputQueued()) reason = "queued-input";
        else if (sequence != attempt.acquired) reason = "input-during-preparation";
        else if (age >= 0 && age < 200) reason = "recent-acquisition";
        if (reason) {
            if (diagnosticsActive())
                diagnostic("standby-deferred", {{"reason", reason}, {"acquired_before", double(attempt.acquired)},
                    {"acquired_now", double(sequence)}, {"acquisition_age_ms", double(age)}}, false);
            errno = EAGAIN;
            return false;
        }
    }
    const bool saved = nativePowerStringToFile(path, value);
    const int error = saved ? 0 : errno;
    if (diagnosticsActive()) {
        auto fields = wakeupCheck ? wakeupCountFields(value) : QJsonObject();
        fields["ok"] = saved;
        fields["errno"] = error;
        fields["guarded"] = attempt.protect;
        diagnostic(wakeupCheck ? "standby-wakeup-check" : "kernel-suspend-result", fields, false);
    }
    if (!saved) errno = error;
    return saved;
}
}
