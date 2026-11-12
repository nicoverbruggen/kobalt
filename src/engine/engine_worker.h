#pragma once
#include <atomic>
#include <memory>
#include <functional>
#include <QElapsedTimer>

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
// Inside an engine job: waits for the work done since the last call, so that
// long work is slowed evenly and a stop request is seen as late as on the
// device, not later. Does nothing outside a job or without a slowdown.
void engineSlowdownPause();

// Desktop test control. Only explicitly scoped rendering work is delayed.
void setEngineSlowdownEnabled(bool enabled);
bool engineSlowdownEnabled();

class EngineSlowdownScope
{
public:
    explicit EngineSlowdownScope(bool enabled = true, std::function<bool()> cancelled = {});
    ~EngineSlowdownScope();
    EngineSlowdownScope(const EngineSlowdownScope &) = delete;
    EngineSlowdownScope &operator=(const EngineSlowdownScope &) = delete;
    void checkpoint();

    double addedMilliseconds() const { return delayed / 1e6; }

private:
    QElapsedTimer timer;
    std::function<bool()> cancelled;
    qint64 accounted = 0, delayed = 0;
    bool owner = false, waiting = false;
};

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
