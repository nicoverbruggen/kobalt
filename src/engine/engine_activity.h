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
        return active == parked;
    }

private:
    std::mutex mutex;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    bool paused = false;
    unsigned active = 0, parked = 0;

    void checkpoint(std::unique_lock<std::mutex> &lock)
    {
        if (!paused)
            return;
        ++parked;
        while (paused)
            pthread_cond_wait(&changed, lock.mutex()->native_handle());
        --parked;
    }
};

inline EngineActivity &engineActivity()
{
    static EngineActivity activity;
    return activity;
}

class EngineJob
{
public:
    EngineJob() { engineActivity().enter(); }

    ~EngineJob() { engineActivity().leave(); }

    EngineJob(const EngineJob &) = delete;
    EngineJob &operator=(const EngineJob &) = delete;
};
} // namespace kobalt
