#include "refresh_queue.h"
#include <chrono>
#include <utility>

namespace kindle {
RefreshQueue::RefreshQueue(Present present, QObject *parent)
    : QObject(parent), present(std::move(present)), worker([this] { run(); }) {}
RefreshQueue::~RefreshQueue() { stop(); }
quint64 RefreshQueue::epoch() const {
    std::lock_guard<std::mutex> lock(mutex);
    return epoch_;
}
void RefreshQueue::cancelAnimation() {
    std::lock_guard<std::mutex> lock(mutex);
    if (pending)
        pending->animationDirection = 0;
}
bool RefreshQueue::enqueue(RefreshFrame frame) {
    quint64 replaced = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || !frame.serial || frame.epoch != epoch_ || frame.pixels.isNull())
            return false;
        if (pending) {
            replaced = pending->serial;
            frame.damage += pending->damage;
            frame.full = frame.full || pending->full;
            // Several frames no longer prove one adjacent displayed-page turn.
            frame.animationDirection = 0;
        }
        if (frame.full)
            frame.animationDirection = 0;
        pending = std::move(frame);
    }
    if (replaced)
        emit discarded(replaced);
    wake.notify_one();
    return true;
}
quint64 RefreshQueue::invalidate() {
    quint64 dropped = 0, next = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!++epoch_)
            ++epoch_;
        next = epoch_;
        needsFull = true;
        if (pending) {
            dropped = pending->serial;
            pending.reset();
        }
    }
    if (dropped)
        emit discarded(dropped);
    wake.notify_one();
    return next;
}
void RefreshQueue::stop() {
    quint64 dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        if (pending) {
            dropped = pending->serial;
            pending.reset();
        }
    }
    if (dropped)
        emit discarded(dropped);
    wake.notify_one();
    if (worker.joinable())
        worker.join();
}
void RefreshQueue::run() {
    for (;;) {
        RefreshFrame frame;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [this] { return stopping || pending.has_value(); });
            if (stopping)
                return;
            frame = std::move(*pending);
            pending.reset();
            frame.full = frame.full || needsFull;
            if (frame.full)
                frame.animationDirection = 0;
        }
        unsigned attempts = 0;
        for (;;) {
            const auto result = present(frame);
            if (result.ok) {
                bool current;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    current = !stopping && frame.epoch == epoch_;
                    if (current)
                        needsFull = false;
                }
                if (current)
                    emit completed(frame.serial, frame.epoch);
                else
                    emit discarded(frame.serial);
                break;
            }
            quint64 dropped = 0;
            bool abort = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                needsFull = true;
                if (stopping || frame.epoch != epoch_) {
                    lock.unlock();
                    emit discarded(frame.serial);
                    break;
                }
                abort = result.fatal || ++attempts == 3;
                if (abort) {
                    stopping = true;
                    if (pending) {
                        dropped = pending->serial;
                        pending.reset();
                    }
                } else {
                    // Retry even without another paint. Keep retries bounded
                    // and allow shutdown/invalidation to interrupt the delay.
                    wake.wait_for(lock, std::chrono::milliseconds(50),
                                  [this, &frame] { return stopping || frame.epoch != epoch_; });
                    if (stopping || frame.epoch != epoch_) {
                        lock.unlock();
                        emit discarded(frame.serial);
                        break;
                    }
                    if (pending) {
                        dropped = frame.serial;
                        auto next = std::move(*pending);
                        pending.reset();
                        next.damage += frame.damage;
                        frame = std::move(next);
                    }
                    frame.full = true;
                    frame.animationDirection = 0;
                }
            }
            if (dropped)
                emit discarded(dropped);
            if (abort) {
                emit discarded(frame.serial);
                emit failed(result.error);
                return;
            }
        }
    }
}
}
