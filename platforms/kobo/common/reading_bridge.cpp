#include "reading_bridge.h"
#include "progress_policy.h"
#include "native_progress.h"
#include "native_reading_session.h"
#include "native_lifecycle.h"

namespace kobalt::nickel {
namespace {
class Bridge final : public ReadingBridge
{
public:
    Bridge(QString identity, std::unique_ptr<NativeProgress> progress, bool koboCloud)
        : progress_(std::move(progress)),
          allowUnmapped_(nickel::allowsUnmappedPosition(identity)),
          syncPolicy_(nickel::nativeSyncPolicy(identity, koboCloud)),
          reading_(std::move(identity))
    {
        reading_.estimatesChanged = [this] {
            if (estimatesChanged) {
                estimatesChanged();
            }
        };
    }

    bool deviceTransition() const override { return progress_->deviceTransition(); }

    bool writable() const override
    {
        return NativeLifecycle::nativeProgressSaveAllowed() && !progress_->deviceTransition();
    }

    ProgressReconciler::Normalization positionNormalization() const override
    {
        return acceptsPositionNormalization;
    }

    bool allowsUnmappedPosition() const override { return allowUnmapped_; }

    NativeSyncPolicy nativeSyncPolicy() const override { return syncPolicy_; }

    ReadingProgress read(QString *error) const override { return progress_->read(error); }

    bool takeChange() override { return progress_->takeChange(); }

    bool signalsChanges() const override { return progress_->signalsChanges(); }

    NativeSaveResult write(const ReadingProgress &expected, const QString &location, double percent,
                           bool finished, const ReadingActivity &activity) override
    {
        ReadingEstimates estimates;
        const bool available = reading_.estimates(activity, &estimates);
        return progress_->write(expected, location, percent, finished,
                                available ? &estimates : nullptr, &activity);
    }

    NativeActivityResult markReading() override
    {
        return progress_->markReading() ? NativeActivityResult::Completed
                                        : NativeActivityResult::Failed;
    }

    void markRecent() override { progress_->markRecent(); }

    void observeReading(const ReadingActivity &activity) override
    {
        useRecord();
        reading_.start(activity);
        reading_.page(activity);
    }

    bool estimates(const ReadingActivity &activity, ReadingEstimates *result) override
    {
        return reading_.estimates(activity, result);
    }

    void minuteTick() override { reading_.minuteTick(); }

    void stopReading(const ReadingActivity &activity) override
    {
        useRecord();
        if (!activity.position.isEmpty() && activity.pages > 0 && activity.page >= 0)
            reading_.page(activity);
        reading_.stop();
    }

    void stopReading() override { reading_.stop(); }

    void finishReading(std::function<void(NativeActivityResult)> done) override
    {
        reading_.finish([done = std::move(done)](bool saved) {
            done(saved ? NativeActivityResult::Completed : NativeActivityResult::Failed);
        });
    }

    bool readingSavePending() const override { return reading_.savePending(); }

    void retryFailedReadingSave() override { reading_.retryFailedSave(); }

private:
    // Page events keep one lookup of Nickel's record until Nickel signals a
    // sync, an account update or the end of a USB session. Without those
    // signals every page event looks the record up.
    void useRecord() { reading_.useRecord(progress_->signalsChanges(), progress_->changes()); }

    std::unique_ptr<NativeProgress> progress_;
    const bool allowUnmapped_;
    const NativeSyncPolicy syncPolicy_;
    NativeReadingSession reading_;
};
} // namespace

std::unique_ptr<ReadingBridge>
makeReadingBridge(QString identity, std::unique_ptr<NativeProgress> progress, bool koboCloud)
{
    if (!progress)
        return {};
    return std::make_unique<Bridge>(std::move(identity), std::move(progress), koboCloud);
}
} // namespace kobalt::nickel
