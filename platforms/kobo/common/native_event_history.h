#pragma once
#include <QVariantMap>

namespace kobalt {
extern void (*nativeEventSetExtraData)(void *, QVariantMap);

namespace nickel {
constexpr int pageHistoryTrigger = 10000;
constexpr int pageHistoryRetained = 5000;

// Return the number of oldest page samples to remove. A session start must
// separate them from the retained suffix. Ambiguous histories stay intact.
int pageHistoryPrefix(const QVariantMap &pages, const QVariantMap &starts, const QVariantMap &ends);
} // namespace nickel
} // namespace kobalt

extern "C" __attribute__((visibility("default"))) void
kobalt_event_set_extra_data(void *event, QVariantMap data);
