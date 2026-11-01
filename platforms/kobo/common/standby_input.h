#pragma once
#include <QString>

namespace kobalt {
// The touch driver calls this before it queues input for the GUI thread.
void standbyTouchAcquired();
class StandbyInputScope {
public:
    explicit StandbyInputScope(bool protect, const QString &wakeupCount = {});
    ~StandbyInputScope();
    StandbyInputScope(const StandbyInputScope &) = delete;
    StandbyInputScope &operator=(const StandbyInputScope &) = delete;
};
extern bool (*nativePowerStringToFile)(const QString &, const QString &);
}
extern "C" bool kobalt_power_string_to_file(const QString &path, const QString &value);
