#pragma once
#include "platform/api/page_animation_support.h"
#include <QByteArray>
#include <QSize>
#include <QRect>
#include <chrono>
#include <cstdint>

namespace kobalt {
enum class PageAnimationResult { None, Passthrough, Swept, Fallback };

// MTK and i.MX band submission adapted from NickelDissolve (MIT; see its notice).
class PageAnimation
{
public:
    using Ioctl = int (*)(int, unsigned long, void *);
    static constexpr unsigned long send = 0x4024462e, wait = 0x40044637, waitComplete = 0xc008462f;
    static constexpr unsigned long sendImx = 0x4048462e;
    // A mono panel sweeps with greyscale waveforms; a colour panel with the
    // colour ones.
    void arm(int direction, const QRect &update, bool dark, bool vertical = false,
             bool mono = false, bool imx = false);

    void cancel()
    {
        direction_ = 0;
        result_ = PageAnimationResult::None;
    }

    int submit(Ioctl next, int fd, unsigned long request, void *argument);
    void beginRepair();
    bool endRepair(Ioctl next);

    // Read this after the armed flush. Cancel clears it before the next flush.
    PageAnimationResult result() const { return result_; }

private:
    int direction_ = 0;
    PageAnimationResult result_ = PageAnimationResult::None;
    QRect update_;
    bool dark_ = false, vertical_ = false, mono_ = false, imx_ = false;
    std::chrono::steady_clock::time_point armed_;
    uint32_t marker_ = 0xe0000000;
    bool repairing_ = false, repairFailed_ = false, repairSent_ = false;
    int repairFd_ = -1;
    uint32_t repairMarker_ = 0;
};

// NickelDissolve's runtime policy: hwtcon is supported, mxcfb is best-effort.
// Unknown backends and unavailable display hooks leave animations disabled.
PageAnimationSupport animationSupport(const QByteArray &driver, bool enabled, bool hookAvailable);
PageAnimationSupport pageAnimationSupport();
enum class AnimationPanel { None, Mono, Color };
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
} // namespace kobalt

extern "C" int kobalt_animation_ioctl(int fd, unsigned long request, void *argument);
