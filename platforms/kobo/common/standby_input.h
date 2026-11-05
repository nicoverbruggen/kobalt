#pragma once
#include <QString>

namespace kobalt {
// The touch driver calls this before it queues input for the GUI thread.
void standbyTouchAcquired();

struct StandbyAcquisition
{
    quint64 sequence = 0, cancellationAttempt = 0;
    qint64 completedAt = -1;
    int wakeError = 0;
};

// Keep kernel registration ahead of clock syscalls and diagnostic work.
StandbyAcquisition acquireStandbyTouch();
void recordStandbyAcquisition(const StandbyAcquisition &input, const char *stage);
// Kernel adapter: probe the wake-lock file, or request a timed wake event.
// Returns zero on success and an errno value on failure.
extern int (*standbyInputWakeLock)(bool request);

class StandbyInputScope
{
public:
    explicit StandbyInputScope(bool protect, const QString &wakeupCount = {});
    ~StandbyInputScope();
    StandbyInputScope(const StandbyInputScope &) = delete;
    StandbyInputScope &operator=(const StandbyInputScope &) = delete;
};

extern bool (*nativePowerStringToFile)(const QString &, const QString &);
extern bool (*nativeDiagnosticPowerStringToFile)(const QString &, const QString &);
} // namespace kobalt

extern "C" bool kobalt_power_string_to_file(const QString &path, const QString &value);
extern "C" bool kobalt_observe_power_string_to_file(const QString &path, const QString &value);
