#include "platforms/kobo/display/page_animation.h"
#include "check.h"
#include <QCoreApplication>
#include <QRegion>
#include <array>
#include <cstring>
#include <vector>
#include <cerrno>

namespace {
QByteArray driver;
bool enabled = true;

struct Call
{
    unsigned long request;
    std::array<uint32_t, 18> data{};
};

std::vector<Call> calls;
int failAt = -1;

int submit(int, unsigned long request, void *argument)
{
    Call call{ request, {} };
    const size_t bytes = request == kobalt::PageAnimation::sendImx ? 72
            : request == kobalt::PageAnimation::send               ? 36
                                                                   : 4;
    if (argument)
        std::memcpy(call.data.data(), argument, bytes);
    calls.push_back(call);
    if (int(calls.size()) == failAt) {
        errno = EIO;
        return -1;
    }
    return 0;
}
} // namespace

namespace kobalt {
PageAnimationSupport pageAnimationSupport()
{
    return animationSupport(driver, enabled, nextAnimationIoctl != nullptr);
}

AnimationPanel pageAnimationPanel()
{
    return animationPanel("", driver);
}
} // namespace kobalt

// Checks the page-animation support predicate and band submission for each Kobo display driver, with fake display calls.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace kobalt;
    for (const auto &backend :
         { QByteArray("hwtcon"), QByteArray("mxc_epdc_fb"), QByteArray("sunxi"), QByteArray() }) {
        driver = backend;
        for (bool built : { false, true })
            for (bool hook : { false, true }) {
                enabled = built;
                nextAnimationIoctl = hook ? submit : nullptr;
                const auto expected = !built || !hook ? PageAnimationSupport::Unavailable
                        : backend == "hwtcon"         ? PageAnimationSupport::Supported
                        : backend == "mxc_epdc_fb"    ? PageAnimationSupport::Uncertain
                                                      : PageAnimationSupport::Unavailable;
                require(pageAnimationSupport() == expected,
                        "Backend, build flag or missing hook misclassified");
                require(pageAnimationsAvailable() == pageAnimationsAvailable(expected),
                        "Runtime and setting eligibility disagree");
                calls.clear();
                armPageAnimation(1, QRect(0, 0, 1072, 1448), false);
                std::array<uint32_t, 18> update{
                    0,  0, 1072, 1448, backend == "hwtcon" ? 4u : 6u, backend == "hwtcon" ? 1u : 0u,
                    123
                };
                if (hook) {
                    kobalt_animation_ioctl(
                            1, backend == "hwtcon" ? PageAnimation::send : PageAnimation::sendImx,
                            update.data());
                    require((calls.size() > 1) == pageAnimationsAvailable(expected),
                            "Unavailable backend submitted a sweep");
                }
                cancelPageAnimation();
            }
    }
    for (const auto *product : { "monza", "monzaTolino", "spaColour", "spaColourTolino" })
        require(animationPanel(product, "hwtcon") == AnimationPanel::Color,
                "Supported Kobo/Tolino colour panel lost");
    for (const auto *product : { "spaBW", "spaBWTolino", "goldfinch", "io", "cadmus", "unknown" }) {
        require(animationPanel(product, "hwtcon") == AnimationPanel::Mono,
                "MTK runtime policy depends on model list");
        require(animationPanel(product, "mxc_epdc_fb") == AnimationPanel::Mono,
                "i.MX mono panel misclassified");
        require(animationPanel(product, "sunxi") == AnimationPanel::None,
                "Unsupported backend accepted by model name");
    }
    // Exercise the real submission path. mxcfb copies 72 bytes, reuses the
    // reading waveform and never sends the hwtcon submission-wait request.
    for (bool imx : { false, true })
        for (bool dark : { false, true })
            for (int direction : { -1, 1 }) {
                PageAnimation animation;
                const QRect rect(3, 7, 1073, 1449);
                std::array<uint32_t, 18> original{ 7,
                                                   3,
                                                   1073,
                                                   1449,
                                                   imx ? (dark ? 10u : 6u) : (dark ? 9u : 4u),
                                                   imx ? 0u : 1u,
                                                   456,
                                                   0x1234 };
                for (size_t i = 8; i < original.size(); ++i)
                    original[i] = uint32_t(100 + i);
                calls.clear();
                failAt = -1;
                animation.arm(direction, rect, dark, false, imx, imx);
                require(animation.submit(submit, 1,
                                         imx ? PageAnimation::sendImx : PageAnimation::send,
                                         original.data())
                                == 0,
                        "Recognized reading update failed");
                require(animation.result() == PageAnimationResult::Swept,
                        "Reading update did not sweep");
                QRegion coverage;
                int bands = 0, previous = direction > 0 ? rect.right() + 1 : rect.left() - 1;
                for (const auto &call : calls) {
                    require(!imx || call.request != PageAnimation::wait,
                            "mxcfb received an hwtcon wait");
                    if (call.request != (imx ? PageAnimation::sendImx : PageAnimation::send))
                        continue;
                    const auto &data = call.data;
                    const QRect band{ int(data[1]), int(data[0]), int(data[2]), int(data[3]) };
                    require((coverage & band).isEmpty(), "Sweep bands overlap");
                    require(direction > 0 ? band.left() < previous : band.left() > previous,
                            "Wrong sweep direction");
                    previous = band.left();
                    coverage += band;
                    ++bands;
                    require(data[5] == 0, "Sweep band is not partial");
                    if (imx) {
                        require(data[4] == original[4], "mxcfb reading waveform changed");
                        for (size_t i = 7; i < original.size(); ++i)
                            require(data[i] == original[i],
                                    "mxcfb flags or trailing structure fields changed");
                    } else
                        require(data[4] == (dark ? 9u : 8u),
                                "Existing MTK colour waveform changed");
                    if (bands == 10)
                        require(data[6] == original[6], "Final marker was not retained");
                }
                require(bands == 10 && coverage == QRegion(rect),
                        "Sweep does not cover exactly the page");
            }
    for (unsigned waveform : { 0u, 1u, 2u, 4u, 5u, 6u, 7u, 9u, 10u, 257u })
        for (unsigned mode : { 0u, 1u }) {
            PageAnimation animation;
            std::array<uint32_t, 18> update{ 0, 0, 1072, 1448, waveform, mode, 123 };
            animation.arm(1, QRect(0, 0, 1072, 1448), false, false, true, true);
            calls.clear();
            animation.submit(submit, 1, PageAnimation::sendImx, update.data());
            const bool reading = mode == 0
                    && (waveform == 5 || waveform == 6 || waveform == 7 || waveform == 10);
            require((animation.result() == PageAnimationResult::Swept) == reading,
                    "mxcfb flash/menu/unknown waveform restrictions changed");
            if (!reading)
                require(calls.size() == 1 && calls[0].data == update,
                        "Rejected update did not pass through unchanged");
        }
    for (int failure : { 1, 4, 11 }) {
        PageAnimation animation;
        std::array<uint32_t, 18> update{ 0, 0, 1072, 1448, 6, 0, 123 };
        animation.arm(1, QRect(0, 0, 1072, 1448), false, false, true, true);
        calls.clear();
        failAt = failure;
        require(animation.submit(submit, 1, PageAnimation::sendImx, update.data()) < 0,
                "Failed mxcfb submission reported success");
        require(animation.result() == PageAnimationResult::Fallback,
                "Failed sweep did not request repair");
        require(calls[calls.size() - 2].request == PageAnimation::sendImx
                        && calls[calls.size() - 2].data == update,
                "Failed sweep did not resubmit the exact original rectangle");
    }
}
