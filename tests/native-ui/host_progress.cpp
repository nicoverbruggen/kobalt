#include "host_progress.h"
#include <utility>

namespace kobalt::test {
namespace {
class Bridge final : public ReadingBridge {
public:
    explicit Bridge(std::shared_ptr<HostProgress> progress) : progress(std::move(progress)) {}
    bool deviceTransition() const override {
        return false;
    }
    bool writable() const override {
        return true;
    }
    ReadingProgress read(QString *) const override {
        return progress->current;
    }
    bool takeChange() override {
        return std::exchange(progress->changed, false);
    }
    bool write(const ReadingProgress &expected, const QString &location, double percent, bool finished,
               const ReadingActivity &, ReadingProgress *verified, QString *error) override {
        if (progress->failWrite) {
            if (error) {
                *error = "The database is busy.";
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
    void observeReading(const ReadingActivity &) override {}
    void stopReading() override {}

private:
    std::shared_ptr<HostProgress> progress;
};
}
std::unique_ptr<ReadingBridge> hostReadingBridge(std::shared_ptr<HostProgress> progress) {
    return std::make_unique<Bridge>(std::move(progress));
}
}
