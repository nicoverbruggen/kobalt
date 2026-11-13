#pragma once
#include <pthread.h>
#include <mutex>

namespace kobalt {
// Only the engine worker enters jobs. The GUI requests a pause without waiting
// for that worker, which can itself be waiting for Nickel's resource service.
class EngineActivity
{
public:
    ~EngineActivity() { pthread_cond_destroy(&changed); }

    void pause(bool value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        paused = value;
        if (!paused)
            pthread_cond_broadcast(&changed);
    }

    void enter()
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++active;
        checkpoint(lock);
    }

    void leave()
    {
        std::lock_guard<std::mutex> lock(mutex);
        --active;
    }

    void checkpoint()
    {
        std::unique_lock<std::mutex> lock(mutex);
        checkpoint(lock);
    }

    bool quiescent()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return drains == 0 && active == parked;
    }

private:
    friend class EngineDrain;
    std::mutex mutex;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    bool paused = false;
    unsigned active = 0, parked = 0, drains = 0;

    void beginDrain()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++drains;
        pthread_cond_broadcast(&changed);
    }

    void endDrain()
    {
        std::lock_guard<std::mutex> lock(mutex);
        --drains;
    }

    void checkpoint(std::unique_lock<std::mutex> &lock)
    {
        if (!paused || drains)
            return;
        ++parked;
        while (paused && !drains)
            pthread_cond_wait(&changed, lock.mutex()->native_handle());
        --parked;
    }
};

inline EngineActivity &engineActivity()
{
    static EngineActivity activity;
    return activity;
}

// Accepted saves can need several queued engine queries before storage writes.
// Let that chain finish even when sleep requests a pause. The worker is serial,
// so jobs ahead of the save must run too. Suspend stays blocked until the last
// owner releases the drain and running engine work reaches a pause checkpoint.
class EngineDrain
{
public:
    explicit EngineDrain(EngineActivity &activity = engineActivity()) : activity(activity)
    {
        activity.beginDrain();
    }

    ~EngineDrain() { activity.endDrain(); }

    EngineDrain(const EngineDrain &) = delete;
    EngineDrain &operator=(const EngineDrain &) = delete;

private:
    EngineActivity &activity;
};

class EngineJob
{
public:
    EngineJob() { engineActivity().enter(); }

    ~EngineJob() { engineActivity().leave(); }

    EngineJob(const EngineJob &) = delete;
    EngineJob &operator=(const EngineJob &) = delete;
};
} // namespace kobalt
