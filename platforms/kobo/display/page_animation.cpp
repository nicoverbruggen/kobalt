#include "page_animation.h"
#include "support/diagnostics.h"
#include <array>
#include <vector>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace kobalt {
PageAnimation::Ioctl nextAnimationIoctl = nullptr;
namespace {
thread_local PageAnimation animation;
}
void PageAnimation::arm(int direction, const QRect &update, bool dark, bool vertical, bool mono) {
    direction_ = direction;
    update_ = update;
    result_ = PageAnimationResult::None;
    dark_ = dark;
    vertical_ = vertical;
    mono_ = mono;
    armed_ = std::chrono::steady_clock::now();
}
AnimationPanel animationPanel(const QByteArray &product, const QByteArray &driver) {
    if (driver != "hwtcon")
        return AnimationPanel::None;
    if (product.startsWith("monza"))
        return AnimationPanel::Colour;
    if (product.startsWith("spa")) {
        if (product.contains("Colour"))
            return AnimationPanel::Colour;
        if (product.contains("BW"))
            return AnimationPanel::Mono;
    }
    return AnimationPanel::None;
}
AnimationPanel pageAnimationPanel() {
    static const AnimationPanel panel = [] {
        QByteArray driver;
        const int fd = ::open("/dev/fb0", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            fb_fix_screeninfo info{};
            if (::ioctl(fd, FBIOGET_FSCREENINFO, &info) == 0)
                driver = QByteArray(info.id, int(strnlen(info.id, sizeof(info.id))));
            ::close(fd);
        }
        return animationPanel(qgetenv("PRODUCT"), driver);
    }();
    return panel;
}
bool pageAnimationsAvailable() {
    return nextAnimationIoctl && pageAnimationPanel() != AnimationPanel::None;
}
void armPageAnimation(int direction, const QRect &update, bool dark, bool vertical) {
    if (pageAnimationsAvailable())
        animation.arm(direction, update, dark, vertical, pageAnimationPanel() == AnimationPanel::Mono);
    else
        animation.cancel();
}
void cancelPageAnimation() {
    animation.cancel();
}
PageAnimationResult pageAnimationResult() {
    return animation.result();
}
bool pageAnimationSucceeded() {
    return pageAnimationResult() == PageAnimationResult::Swept;
}
void beginPageRepair() {
    animation.beginRepair();
}
bool endPageRepair() {
    return animation.endRepair(nextAnimationIoctl);
}
void PageAnimation::beginRepair() {
    repairing_ = true;
    repairFailed_ = repairSent_ = false;
    repairFd_ = -1;
    repairMarker_ = 0;
}
bool PageAnimation::endRepair(Ioctl next) {
    const bool sent = repairSent_, failed = repairFailed_;
    const int fd = repairFd_;
    const uint32_t marker = repairMarker_;
    repairing_ = repairFailed_ = repairSent_ = false;
    if (!sent || !next)
        return false;
    uint32_t completion[2] = {marker, 0};
    return next(fd, waitComplete, completion) >= 0 && !failed;
}
int PageAnimation::submit(Ioctl next, int fd, unsigned long request, void *argument) {
    if (!next) {
        errno = ENOSYS;
        return -1;
    }
    if (repairing_ && request == send) {
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
    if (request != send || !argument || !direction_)
        return next(fd, request, argument);
    const int direction = direction_;
    direction_ = 0;
    result_ = PageAnimationResult::Passthrough;
    std::array<uint32_t, 9> original;
    std::memcpy(original.data(), argument, sizeof(original));
    const QRect submitted{int(original[1]), int(original[0]), int(original[2]), int(original[3])};
    // This request must belong to the completed page-turn flush. Never sweep
    // a clean/color update, another rectangle, or a later frame.
    // Each armed turn logs why it did or did not animate.
    const bool nativeLight = !dark_ && original[4] == 4 && original[5] == 1;
    const bool nativeDark = dark_ && original[4] == 9 && original[5] == 1;
    const char *refused = std::chrono::steady_clock::now() - armed_ > std::chrono::milliseconds(500) ? "expired"
                          : submitted != update_                                                     ? "rectangle"
                          : !nativeLight && !nativeDark                                              ? "policy"
                          : update_.width() < 128 || update_.height() < 128                          ? "size"
                                                                                                     : nullptr;
    if (refused) {
        diagnostic("page-animation", {{"result", refused}, {"waveform", int(original[4])}}, false);
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
        const QRect band = vertical_ ? QRect(update_.left(), update_.top() + start, update_.width(), end - start)
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
        band[4] = mono_ ? (dark_ ? 9 : 4) : (dark_ ? 9 : 8);
        if (!mono_)
            band[7] |= 0x8000; // HWTCON_FLAG_CFA_SKIP, text only.
        const bool last = i + 1 == parts.size();
        band[6] = last ? original[6] : marker_++;
        if (next(fd, send, band.data()) < 0 || next(fd, wait, &band[6]) < 0) {
            failed = errno ? errno : EIO;
            break;
        }
    }
    if (!failed) {
        // Submission wait does not mean the panel has finished its last band.
        uint32_t completion[2] = {original[6], 0};
        if (next(fd, waitComplete, completion) < 0)
            failed = errno ? errno : EIO;
    }
    if (failed) {
        result_ = PageAnimationResult::Fallback;
        // A failed sweep may have painted some bands. Repaint the full area.
        if (next(fd, send, argument) >= 0) {
            uint32_t completion[2] = {original[6], 0};
            next(fd, waitComplete, completion);
        }
        diagnostic("page-animation",
                   {{"result", "fallback"}, {"bands", int(parts.size())}, {"failed", failed}, {"mono", mono_}}, false);
        errno = failed;
        return -1;
    }
    result_ = PageAnimationResult::Swept;
    diagnostic("page-animation", {{"result", "swept"}, {"bands", int(parts.size())}, {"mono", mono_}}, false);
    return 0;
}
int submitPageAnimation(int fd, unsigned long request, void *argument) {
    return animation.submit(nextAnimationIoctl, fd, request, argument);
}
}
extern "C" __attribute__((visibility("default"))) int kobalt_animation_ioctl(int fd, unsigned long request,
                                                                             void *argument) {
    return kobalt::submitPageAnimation(fd, request, argument);
}
