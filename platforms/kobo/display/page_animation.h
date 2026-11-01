#pragma once
#include <QByteArray>
#include <QSize>
#include <QRect>
#include <chrono>
#include <cstdint>

namespace kobalt {
enum class PageAnimationResult { None, Passthrough, Swept, Fallback };
// MTK band submission adapted from NickelDissolve (MIT; see its notice).
class PageAnimation {
public:
    using Ioctl = int (*)(int, unsigned long, void *);
    static constexpr unsigned long send = 0x4024462e, wait = 0x40044637, waitComplete = 0xc008462f;
    // A mono panel sweeps with greyscale waveforms; a colour panel with the
    // colour ones.
    void arm(int direction, const QRect &update, bool dark, bool vertical = false, bool mono = false);
    void cancel() {
        direction_ = 0;
        result_ = PageAnimationResult::None;
    }
    int submit(Ioctl next, int fd, unsigned long request, void *argument);
    void beginRepair();
    bool endRepair(Ioctl next);
    // Read this after the armed flush. Cancel clears it before the next flush.
    PageAnimationResult result() const {
        return result_;
    }

private:
    int direction_ = 0;
    PageAnimationResult result_ = PageAnimationResult::None;
    QRect update_;
    bool dark_ = false, vertical_ = false, mono_ = false;
    std::chrono::steady_clock::time_point armed_;
    uint32_t marker_ = 0xe0000000;
    bool repairing_ = false, repairFailed_ = false, repairSent_ = false;
    int repairFd_ = -1;
    uint32_t repairMarker_ = 0;
};
// Page turn animations follow NickelDissolve on the Clara Colour, Libra Colour,
// Clara BW and their Tolino counterparts, which use MediaTek's hwtcon driver.
// The firmware names the model in PRODUCT. The Elipsa 2E also uses hwtcon but
// is not supported.
enum class AnimationPanel { None, Mono, Colour };
AnimationPanel animationPanel(const QByteArray &product, const QByteArray &driver);
// This device's panel, read once from PRODUCT and the framebuffer driver.
AnimationPanel pageAnimationPanel();
bool pageAnimationsAvailable();
void armPageAnimation(int direction, const QRect &update, bool dark, bool vertical = false);
void cancelPageAnimation();
PageAnimationResult pageAnimationResult();
bool pageAnimationSucceeded();
void beginPageRepair();
bool endPageRepair();
extern PageAnimation::Ioctl nextAnimationIoctl;
}
extern "C" int kobalt_animation_ioctl(int fd, unsigned long request, void *argument);
