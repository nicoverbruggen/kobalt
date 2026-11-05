#pragma once
#include <QElapsedTimer>
#include <QJsonObject>

namespace kobalt {
// When a timed reader action began, and the stages measured so far in ms.
struct ActionTiming {
    // Qt 5.2 leaves a default-constructed timer looking valid.
    ActionTiming() { started.invalidate(); }
    QElapsedTimer started;
    QJsonObject stages;
    // Adds the time since `timer` started to `stage`, so repeated work sums up.
    void add(const char *stage, const QElapsedTimer &timer);
};
// Writes "book-close" with the stages, `sync_ms` as the sum of the storage
// stages and `total_ms` since the close began. Async stages measure the time
// the close waits for them, including queued work. Native session saving can
// overlap local saving; session_ms includes only its remaining wait so the
// sum does not count that overlap twice. Does nothing without a start
// or while diagnostics are off.
void reportBookClose(const ActionTiming &timing);
}
