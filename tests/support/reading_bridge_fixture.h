#pragma once
#include "platforms/kobo/common/progress_policy.h"
#include "reading/reading_bridge.h"
#include "support/save_wait.h"
#include <memory>
#include <utility>

namespace kobalt::test {
// The platform's saved reading position, which a test changes as a sync or
// another device would. Writes from the reader are recorded, not stored.
struct HostProgress
{
    ReadingProgress current;
    ReadingEstimates remaining;
    ReadingActivity observedActivity;
    bool hasEstimates = false;
    std::function<void()> notifyEstimates;
    bool changed = false, failWrite = false, writeBlocked = false;
    bool busyBeforeWrite = false;
    QString writeError = "The database is busy.";
    int reads = 0, writes = 0, writeAttempts = 0, openingAttempts = 0;
    bool holdFinish = false, failFinish = false, finishPending = false;
    int finishAttempts = 0;
    // The write counts when the last finish began, so a test can check
    // that the progress was written before the reading-time finish.
    int writesAtFinish = -1, writeAttemptsAtFinish = -1;
    int saveRetries = 0;
    std::function<void(bool)> retryDone;
    int stopAttempts = 0, destroyed = 0;
    // Minute ticks that asked the platform for its reading pace.
    int minuteTicks = 0;
    std::function<void(bool)> finishDone;

    void completeFinish(bool saved)
    {
        finishPending = false;
        saves::settled();
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

    ProgressReconciler::Normalization positionNormalization() const override
    {
        return nickel::acceptsPositionNormalization;
    }

    bool allowsUnmappedPosition() const override
    {
        return nickel::allowsUnmappedPosition(progress->current.book);
    }

    NativeSyncPolicy nativeSyncPolicy() const override
    {
        return nickel::nativeSyncPolicy(progress->current.book);
    }

    ReadingProgress read(QString *) const override
    {
        ++progress->reads;
        return progress->current;
    }

    bool takeChange() override { return std::exchange(progress->changed, false); }

    NativeSaveResult write(const ReadingProgress &expected, const QString &location, double percent,
                           bool finished, const ReadingActivity &) override
    {
        ++progress->writeAttempts;
        if (progress->failWrite) {
            return progress->busyBeforeWrite
                    ? NativeSaveResult::busyBeforeWrite(progress->writeError)
                    : NativeSaveResult::unverified(progress->writeError);
        }
        if (!(expected == progress->current)) {
            return NativeSaveResult::progressChanged("The saved position changed.");
        }
        ++progress->writes;
        progress->percent = percent;
        if (!location.isEmpty())
            progress->current.location = location;
        progress->current.status = finished ? 2 : 1;
        progress->current.modified += 1;
        progress->current.valid = true;
        return NativeSaveResult::verified(progress->current, location.isEmpty());
    }

    NativeActivityResult markReading() override
    {
        ++progress->openingAttempts;
        if (progress->failWrite)
            return NativeActivityResult::Failed;
        progress->current.status = 1;
        return NativeActivityResult::Completed;
    }

    void observeReading(const ReadingActivity &activity) override
    {
        progress->observedActivity = activity;
    }

    void minuteTick() override { ++progress->minuteTicks; }

    void stopReading() override { ++progress->stopAttempts; }

    void finishReading(std::function<void(NativeActivityResult)> done) override
    {
        ++progress->finishAttempts;
        progress->writesAtFinish = progress->writes;
        progress->writeAttemptsAtFinish = progress->writeAttempts;
        progress->finishPending = true;
        progress->finishDone = [done = std::move(done)](bool saved) {
            done(saved ? NativeActivityResult::Completed : NativeActivityResult::Failed);
        };
        if (!progress->holdFinish)
            progress->completeFinish(!progress->failFinish);
    }

    void retryFailedReadingSave(std::function<void(NativeActivityResult)> done,
                                ReadingSaveRetry) override
    {
        if (done) {
            ++progress->saveRetries;
            progress->retryDone = [done = std::move(done)](bool saved) {
                done(saved ? NativeActivityResult::Completed : NativeActivityResult::Failed);
            };
        }
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
