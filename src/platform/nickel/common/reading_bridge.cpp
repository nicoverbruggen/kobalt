#include "reading_bridge.h"
#include "native_progress.h"
#include "native_reading_session.h"
#include "native_lifecycle.h"

namespace kobalt::nickel {
namespace {
class Bridge final : public ReadingBridge {
public:
    Bridge(QString identity, std::unique_ptr<NativeProgress> progress)
        : progress_(std::move(progress)), reading_(std::move(identity)) {}
    bool deviceTransition() const override {
        return progress_->deviceTransition();
    }
    bool writable() const override {
        return (!NativeLifecycle::suspended() || NativeLifecycle::usbSaveAllowed() ||
                NativeLifecycle::sleepSaveAllowed()) &&
               !progress_->deviceTransition();
    }
    ReadingProgress read(QString *error) const override {
        return progress_->read(error);
    }
    bool takeChange() override {
        return progress_->takeChange();
    }
    bool write(const ReadingProgress &expected, const QString &location, double percent, bool finished,
               const ReadingActivity &activity, ReadingProgress *verified, QString *error) override {
        ReadingEstimates estimates;
        const bool available = reading_.estimates(activity, &estimates);
        return progress_->write(expected, location, percent, finished, verified, error,
                                available ? &estimates : nullptr);
    }
    void observeReading(const ReadingActivity &activity) override {
        reading_.start(activity);
        reading_.page(activity);
    }
    bool estimates(const ReadingActivity &activity, ReadingEstimates *result) override {
        return reading_.estimates(activity, result);
    }
    void stopReading() override {
        reading_.stop();
    }

private:
    std::unique_ptr<NativeProgress> progress_;
    NativeReadingSession reading_;
};
}
std::unique_ptr<ReadingBridge> makeReadingBridge(QString identity, std::unique_ptr<NativeProgress> progress) {
    if (!progress)
        return {};
    return std::make_unique<Bridge>(std::move(identity), std::move(progress));
}
}
