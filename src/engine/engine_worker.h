#pragma once
#include <atomic>
#include <memory>

class QObject;
class QThread;

namespace kobalt {
// Shared queue for reader sessions and their coordinate probes. Separate
// documents must not access CREngine's global fonts and caches concurrently.
QObject *engineWorker();

// For judging speed on a desktop: KOBALT_KOBO_EMULATION=20 in the environment
// makes every engine job take 20 times as long, which is about the speed of
// a Kobo. The engine does the work, then the worker waits for the rest of
// the time. One without the variable.
int engineSlowdown();

struct EngineWorker
{
    QThread *thread = nullptr;
    QObject *object = nullptr;
    std::atomic_bool stopped{ false };
};

// One worker serializes CREngine's process-global font state across closing
// sessions and new openings. The state outlives the worker so retained
// documents can still be released during application destruction.
const std::shared_ptr<EngineWorker> &engineWorkerState();
} // namespace kobalt
