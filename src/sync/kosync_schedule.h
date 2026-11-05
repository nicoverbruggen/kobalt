#pragma once
#include <QVector>

namespace kobalt::kosync {
// When automatic sync runs. While reading, an open book uploads a changed
// position every `minutes`: 0 follows page turns with the 30-second check and
// `neverWhileReading` turns periodic syncs off. The default is 15 minutes.
// Periodic syncs turn on Wi-Fi only with wakeWifi. Opening, sleep and wake,
// and closing each have their own switch.
inline constexpr int neverWhileReading = -1;

struct Schedule
{
    int minutes = 15;
    bool wakeWifi = false;
    bool onOpen = true, onSleepWake = true, onClose = true;

    bool operator==(const Schedule &other) const
    {
        return minutes == other.minutes && wakeWifi == other.wakeWifi && onOpen == other.onOpen
                && onSleepWake == other.onSleepWake && onClose == other.onClose;
    }

    bool operator!=(const Schedule &other) const { return !(*this == other); }
};

inline const QVector<int> &scheduleMinutes()
{
    static const QVector<int> values{ 0, 1, 5, 15, 30, 60, neverWhileReading };
    return values;
}

inline bool validScheduleMinutes(int minutes)
{
    return scheduleMinutes().contains(minutes);
}

inline bool syncsWhileReading(const Schedule &schedule)
{
    return schedule.minutes != neverWhileReading;
}

inline int scheduleIntervalMilliseconds(const Schedule &schedule)
{
    return schedule.minutes > 0 ? schedule.minutes * 60000 : 30000;
}

// Failed background connections stop after this many until the device wakes,
// the user syncs or Wi-Fi is found connected.
inline constexpr int backgroundConnectionLimit = 3;
} // namespace kobalt::kosync
