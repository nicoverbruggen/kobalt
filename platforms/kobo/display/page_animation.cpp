#include "page_animation.h"
#include "framebuffer_driver.h"
#include "support/diagnostics.h"
#include <array>
#include <vector>
#include <cerrno>
#include <cstring>
#include <thread>

namespace kobalt {
PageAnimation::Ioctl nextAnimationIoctl = nullptr;

namespace {
thread_local PageAnimation animation;
}

void PageAnimation::arm(int direction, const QRect &update, bool dark, bool vertical, bool mono,
                        bool imx)
{
    direction_ = direction;
    update_ = update;
    result_ = PageAnimationResult::None;
    dark_ = dark;
    vertical_ = vertical;
    mono_ = mono;
    imx_ = imx;
    armed_ = std::chrono::steady_clock::now();
}

PageAnimationSupport animationSupport(const QByteArray &driver, bool enabled, bool hookAvailable)
{
    if (!enabled || !hookAvailable)
        return PageAnimationSupport::Unavailable;
    if (mtkFramebuffer(driver))
        return PageAnimationSupport::Supported;
    if (imxFramebuffer(driver))
        return PageAnimationSupport::Uncertain;
    return PageAnimationSupport::Unavailable;
}

AnimationPanel animationPanel(const QByteArray &product, const QByteArray &driver)
{
    const auto support = animationSupport(driver, true, true);
    if (support == PageAnimationSupport::Unavailable)
        return AnimationPanel::None;
    if (support == PageAnimationSupport::Supported
        && (product.startsWith("monza")
            || (product.startsWith("spa") && product.contains("Colour"))))
        return AnimationPanel::Color;
    return AnimationPanel::Mono;
}

bool pageAnimationsAvailable()
{
    return pageAnimationsAvailable(pageAnimationSupport());
}

void armPageAnimation(int direction, const QRect &update, bool dark, bool vertical)
{
    if (pageAnimationsAvailable())
        animation.arm(direction, update, dark, vertical,
                      pageAnimationPanel() == AnimationPanel::Mono,
                      pageAnimationSupport() == PageAnimationSupport::Uncertain);
    else
        animation.cancel();
}

void cancelPageAnimation()
{
    animation.cancel();
}

PageAnimationResult pageAnimationResult()
{
    return animation.result();
}

bool pageAnimationSucceeded()
{
    return pageAnimationResult() == PageAnimationResult::Swept;
}

void beginPageRepair()
{
    animation.beginRepair();
}

bool endPageRepair()
{
    return animation.endRepair(nextAnimationIoctl);
}

void PageAnimation::beginRepair()
{
    repairing_ = true;
    repairFailed_ = repairSent_ = false;
    repairFd_ = -1;
    repairMarker_ = 0;
}

bool PageAnimation::endRepair(Ioctl next)
{
    const bool sent = repairSent_, failed = repairFailed_;
    const int fd = repairFd_;
    const uint32_t marker = repairMarker_;
    repairing_ = repairFailed_ = repairSent_ = false;
    if (!sent || !next)
        return false;
    uint32_t completion[2] = { marker, 0 };
    return next(fd, waitComplete, completion) >= 0 && !failed;
}

int PageAnimation::submit(Ioctl next, int fd, unsigned long request, void *argument)
{
    if (!next) {
        errno = ENOSYS;
        return -1;
    }
    if (repairing_ && (request == send || request == sendImx)) {
        const int result = next(fd, request, argument);
        if (result < 0 || !argument)
            repairFailed_ = true;
        else {
            repairSent_ = true;
            repairFd_ = fd;
            // The driver writes the update marker in the seventh word.
            std::memcpy(&repairMarker_, static_cast<const char *>(argument) + 6 * sizeof(uint32_t),
                        sizeof(repairMarker_));
        }
        return result;
    }
    const auto sendRequest = imx_ ? sendImx : send;
    if (request != sendRequest || !argument || !direction_)
        return next(fd, request, argument);
    const int direction = direction_;
    direction_ = 0;
    result_ = PageAnimationResult::Passthrough;
    std::array<uint32_t, 18> original{};
    const size_t bytes = imx_ ? 72 : 36;
    std::memcpy(original.data(), argument, bytes);
    const QRect submitted{ int(original[1]), int(original[0]), int(original[2]), int(original[3]) };
    // This request must belong to the completed page-turn flush. Never sweep
    // a clean/color update, another rectangle, or a later frame.
    // Each armed turn logs why it did or did not animate.
    const bool imxReading = imx_ && original[5] == 0
            && (original[4] == 5 || original[4] == 6 || original[4] == 7 || original[4] == 10);
    const bool nativeLight = !imx_ && !dark_ && original[4] == 4 && original[5] == 1;
    const bool nativeDark = !imx_ && dark_ && original[4] == 9 && original[5] == 1;
    const char *refused = std::chrono::steady_clock::now() - armed_ > std::chrono::milliseconds(500)
            ? "expired"
            : submitted != update_                            ? "rectangle"
            : !nativeLight && !nativeDark && !imxReading      ? "policy"
            : update_.width() < 128 || update_.height() < 128 ? "size"
                                                              : nullptr;
    if (refused) {
        // Per-turn details belong to detailed diagnostics only.
        if (diagnosticsActive())
            diagnostic("page-animation",
                       { { "result", refused }, { "waveform", int(original[4]) } }, false);
        return next(fd, request, argument);
    }
    std::vector<QRect> parts;
    // Start the sweep immediately. A separate header/footer submission would
    // make the body wait for those updates before its first band can begin.
    const int length = vertical_ ? update_.height() : update_.width();
    const int count = qMax(submitted.width(), submitted.height()) >= 1600 ? 12 : 10;
    for (int i = 0; i < count; ++i) {
        const int column = direction > 0 ? count - 1 - i : i;
        const int start = length * column / count, end = length * (column + 1) / count;
        const QRect band = vertical_
                ? QRect(update_.left(), update_.top() + start, update_.width(), end - start)
                : QRect(update_.left() + start, update_.top(), end - start, update_.height());
        parts.push_back(band);
    }
    int failed = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto &rect = parts[i];
        auto band = original;
        band[0] = uint32_t(rect.top());
        band[1] = uint32_t(rect.left());
        band[2] = uint32_t(rect.width());
        band[3] = uint32_t(rect.height());
        // Colour panels use GCC16 and GCK16. The Clara BW keeps greyscale
        // page-turn waveforms, GLR16 and GLKW16; 8 is a flash there.
        band[5] = 0; // Animation bands use partial mode, as in NickelDissolve.
        if (!imx_)
            band[4] = mono_ ? (dark_ ? 9 : 4) : (dark_ ? 9 : 8);
        if (!imx_ && !mono_)
            band[7] |= 0x8000; // HWTCON_FLAG_CFA_SKIP, text only.
        const bool last = i + 1 == parts.size();
        band[6] = last ? original[6] : marker_++;
        if (next(fd, sendRequest, band.data()) < 0 || (!imx_ && next(fd, wait, &band[6]) < 0)) {
            failed = errno ? errno : EIO;
            break;
        }
        // mxcfb has no submission wait. Use NickelDissolve's default pacing.
        if (imx_ && !last)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (!failed) {
        // Submission wait does not mean the panel has finished its last band.
        uint32_t completion[2] = { original[6], 0 };
        DiagnosticOperation operation("display-wait-complete");
        const int completed = next(fd, waitComplete, completion);
        if (completed < 0)
            failed = errno ? errno : EIO;
        if (diagnosticsActive())
            diagnostic("display-complete",
                       { { "marker", double(original[6]) }, { "ok", completed >= 0 } }, false);
    }
    if (failed) {
        result_ = PageAnimationResult::Fallback;
        // A failed sweep may have painted some bands. Repaint the full area.
        if (next(fd, sendRequest, argument) >= 0) {
            uint32_t completion[2] = { original[6], 0 };
            next(fd, waitComplete, completion);
        }
        diagnostic("page-animation",
                   { { "result", "fallback" },
                     { "bands", int(parts.size()) },
                     { "failed", failed },
                     { "mono", mono_ } },
                   DiagnosticSeverity::Warning);
        errno = failed;
        return -1;
    }
    result_ = PageAnimationResult::Swept;
    if (diagnosticsActive())
        diagnostic("page-animation",
                   { { "result", "swept" }, { "bands", int(parts.size()) }, { "mono", mono_ } },
                   false);
    return 0;
}

int submitPageAnimation(int fd, unsigned long request, void *argument)
{
    return animation.submit(nextAnimationIoctl, fd, request, argument);
}
} // namespace kobalt

extern "C" __attribute__((visibility("default"))) int
kobalt_animation_ioctl(int fd, unsigned long request, void *argument)
{
    return kobalt::submitPageAnimation(fd, request, argument);
}
