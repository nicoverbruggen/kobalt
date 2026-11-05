#include "reading_bridge.h"
#include "native_progress.h"
#include "native_reading_session.h"
#include "native_lifecycle.h"

namespace kobalt::nickel {
namespace {
class Bridge final : public ReadingBridge
{
public:
    Bridge(QString identity, std::unique_ptr<NativeProgress> progress)
        : progress_(std::move(progress)), reading_(std::move(identity))
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

    ReadingProgress read(QString *error) const override { return progress_->read(error); }

    bool takeChange() override { return progress_->takeChange(); }

    bool signalsChanges() const override { return progress_->signalsChanges(); }

    bool write(const ReadingProgress &expected, const QString &location, double percent,
               bool finished, const ReadingActivity &activity, ReadingProgress *verified,
               QString *error) override
    {
        ReadingEstimates estimates;
        const bool available = reading_.estimates(activity, &estimates);
        return progress_->write(expected, location, percent, finished, verified, error,
                                available ? &estimates : nullptr, &activity);
    }

    bool markReading() override { return progress_->markReading(); }

    void markRecent() override { progress_->markRecent(); }

    void observeReading(const ReadingActivity &activity) override
    {
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
        if (!activity.position.isEmpty() && activity.pages > 0 && activity.page >= 0)
            reading_.page(activity);
        reading_.stop();
    }

    void stopReading() override { reading_.stop(); }

    void finishReading(std::function<void(bool)> done) override
    {
        reading_.finish(std::move(done));
    }

    bool readingSavePending() const override { return reading_.savePending(); }

    void retryFailedReadingSave() override { reading_.retryFailedSave(); }

private:
    std::unique_ptr<NativeProgress> progress_;
    NativeReadingSession reading_;
};
} // namespace

std::unique_ptr<ReadingBridge> makeReadingBridge(QString identity,
                                                 std::unique_ptr<NativeProgress> progress)
{
    if (!progress)
        return {};
    return std::make_unique<Bridge>(std::move(identity), std::move(progress));
}
} // namespace kobalt::nickel
