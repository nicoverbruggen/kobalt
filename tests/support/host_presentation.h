#pragma once
#include "platform/api/window_presentation.h"
#include <QDialog>
#include <memory>
#include <utility>

namespace kobalt::test {
struct SleepPresentationState
{
    QImage image;
    sleep::Snapshot saved;
    int savedCount = 0;
    Qt::ScreenOrientation orientation = Qt::PrimaryOrientation;
    int presented = 0;
    int updated = 0;
    QRect damage;
};

// A desktop test window has no library backing store to pause or cover, so a
// handoff accepts the dialog directly.
class HostPresentation final : public WindowPresentation
{
public:
    explicit HostPresentation(std::shared_ptr<SleepPresentationState> sleep = {})
        : sleep(std::move(sleep))
    {
    }

    void pauseBackground(bool) override { }

    void resumeBackground() override
    {
        if (resumed)
            resumed();
    }

    std::function<void()> resumed;
    std::function<void()> repaint;

    void resumeAndRepaintBackground() override
    {
        if (repaint) {
            repaint();
        }
    }

    bool supportsSleepScreen() const override { return bool(sleep); }

    bool presentSleepScreen(QImage image, Qt::ScreenOrientation orientation) override
    {
        if (!sleep) {
            return false;
        }
        sleep->image = std::move(image);
        sleep->orientation = orientation;
        ++sleep->presented;
        return true;
    }

    void saveSleepScreen(const sleep::Snapshot &snapshot) override
    {
        if (sleep) {
            sleep->saved = snapshot;
            ++sleep->savedCount;
        }
    }

    void clearSleepScreen() override
    {
        if (sleep) {
            sleep->image = {};
        }
    }

    bool updateSleepScreen(QImage image, const QRect &damage) override
    {
        if (!sleep || sleep->image.isNull()) {
            return false;
        }
        sleep->image = std::move(image);
        sleep->damage = damage;
        ++sleep->updated;
        return true;
    }

    void acceptHandoff(QDialog &, const std::function<void()> &accept) override { accept(); }

private:
    std::shared_ptr<SleepPresentationState> sleep;
};

inline std::unique_ptr<WindowPresentation> makeHostPresentation(QWidget *)
{
    return std::make_unique<HostPresentation>();
}
} // namespace kobalt::test
