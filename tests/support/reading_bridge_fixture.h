#pragma once
#include "reading/reading_bridge.h"
#include <memory>
#include <utility>

namespace kobalt::test {
// The platform's saved reading position, which a test changes as a sync or
// another device would. Writes from the reader are recorded, not stored.
struct HostProgress
{
    ReadingProgress current;
    ReadingEstimates remaining;
    bool hasEstimates = false;
    std::function<void()> notifyEstimates;
    bool changed = false, failWrite = false, writeBlocked = false;
    QString writeError = "The database is busy.";
    int writes = 0, writeAttempts = 0, openingAttempts = 0;
    bool holdFinish = false, failFinish = false, finishPending = false;
    bool rejectWriteWhileFinishing = false;
    int finishAttempts = 0;
    int stopAttempts = 0, destroyed = 0;
    std::function<void(bool)> finishDone;

    void completeFinish(bool saved)
    {
        finishPending = false;
        auto done = std::move(finishDone);
        finishDone = {};
        if (done)
            done(saved);
    }

    double percent = 0;

    void publish(ReadingProgress progress)
    {
        current = std::move(progress);
        changed = true;
    }
};

class HostReadingBridge final : public ReadingBridge
{
public:
    explicit HostReadingBridge(std::shared_ptr<HostProgress> progress)
        : progress(std::move(progress))
    {
        this->progress->notifyEstimates = [this] {
            if (estimatesChanged) {
                estimatesChanged();
            }
        };
    }

    ~HostReadingBridge() override
    {
        ++progress->destroyed;
        progress->notifyEstimates = {};
        progress->finishDone = {};
    }

    bool estimates(const ReadingActivity &, ReadingEstimates *result) override
    {
        if (!progress->hasEstimates) {
            return false;
        }
        *result = progress->remaining;
        return true;
    }

    bool deviceTransition() const override { return false; }

    bool writable() const override { return !progress->writeBlocked; }

    ReadingProgress read(QString *) const override { return progress->current; }

    bool takeChange() override { return std::exchange(progress->changed, false); }

    bool write(const ReadingProgress &expected, const QString &location, double percent,
               bool finished, const ReadingActivity &, ReadingProgress *verified,
               QString *error) override
    {
        ++progress->writeAttempts;
        if (progress->failWrite
            || (progress->rejectWriteWhileFinishing && progress->finishPending)) {
            if (error) {
                *error = progress->writeError;
            }
            return false;
        }
        if (!(expected == progress->current)) {
            if (error)
                *error = "The saved position changed.";
            return false;
        }
        ++progress->writes;
        progress->percent = percent;
        if (!location.isEmpty())
            progress->current.location = location;
        progress->current.status = finished ? 2 : 1;
        progress->current.modified += 1;
        progress->current.valid = true;
        if (verified)
            *verified = progress->current;
        return true;
    }

    bool markReading() override
    {
        ++progress->openingAttempts;
        if (progress->failWrite)
            return false;
        progress->current.status = 1;
        return true;
    }

    void observeReading(const ReadingActivity &) override { }

    void stopReading() override
    {
        ++progress->stopAttempts;
        if (progress->rejectWriteWhileFinishing && progress->holdFinish)
            progress->finishPending = true;
    }

    void finishReading(std::function<void(bool)> done) override
    {
        ++progress->finishAttempts;
        progress->finishPending = true;
        progress->finishDone = std::move(done);
        if (!progress->holdFinish)
            progress->completeFinish(!progress->failFinish);
    }

    bool readingSavePending() const override { return progress->finishPending; }

private:
    std::shared_ptr<HostProgress> progress;
};

inline std::unique_ptr<ReadingBridge> hostReadingBridge(std::shared_ptr<HostProgress> progress)
{
    return std::make_unique<HostReadingBridge>(std::move(progress));
}
} // namespace kobalt::test
