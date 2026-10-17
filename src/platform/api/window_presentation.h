#pragma once
#include <functional>
#include <memory>
#include <QImage>
#include <QRect>
class QDialog;
class QWidget;

namespace kobalt {
class WindowPresentation {
public:
    virtual ~WindowPresentation() = default;
    virtual void pauseBackground(bool waking = false) = 0;
    virtual void resumeBackground() = 0;
    virtual void resumeAndRepaintBackground() = 0;
    virtual bool supportsSleepScreen() const {
        return false;
    }
    // The host owns the frame until its native sleep view closes. Failure
    // leaves the stock sleep screen in place. Native unlocking stays with the host.
    virtual bool presentSleepScreen(QImage, Qt::ScreenOrientation) {
        return false;
    }
    // Updates an existing sleep frame only. Never creates a view after wake.
    virtual bool updateSleepScreen(QImage, const QRect &) {
        return false;
    }
    virtual void clearSleepScreen() {}
    // The continuation accepts the dialog. The host keeps the appropriate
    // backing store covered while that continuation installs its reader.
    virtual void acceptHandoff(QDialog &dialog, const std::function<void()> &accept) = 0;
};
using PresentationFactory = std::function<std::unique_ptr<WindowPresentation>(QWidget *)>;
}
