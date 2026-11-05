#include "platforms/kobo/display/page_transition.h"
#include "check.h"
#include <QApplication>
#include <QWindow>
#include <vector>

namespace {
kobalt::PageAnimationResult result = kobalt::PageAnimationResult::None;
bool success = true, available = true;
int arms = 0, repairs = 0;
kobalt::AnimationPanel panel = kobalt::AnimationPanel::Color;
} // namespace

namespace kobalt {
void beginPageRepair()
{
    ++repairs;
}

bool endPageRepair()
{
    ++repairs;
    return false;
}

AnimationPanel pageAnimationPanel()
{
    return panel;
}

bool pageAnimationsAvailable()
{
    return available;
}

void armPageAnimation(int, const QRect &, bool, bool)
{
    ++arms;
    result = PageAnimationResult::None;
}

void cancelPageAnimation()
{
    result = PageAnimationResult::None;
}

PageAnimationResult pageAnimationResult()
{
    return result;
}

bool pageAnimationSucceeded()
{
    return result == PageAnimationResult::Swept;
}
} // namespace kobalt

// Checks native page turns: text turns sweep and keep the final pixels, picture and colour turns get one full refresh, failures clean the page.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QWidget reader;
    reader.resize(300, 400);
    reader.show();
    app.processEvents();
    QBackingStore store(reader.windowHandle());
    store.resize(reader.size());
    const QRect image(24, 70, 180, 100);
    for (const auto format :
         { QImage::Format_RGB32, QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied })
        for (bool mono : { false, true })
            for (bool dark : { false, true })
                for (int direction : { -1, 1 })
                    for (int content : { 0, 1, 2, 3 })
                        for (bool succeeds : { false, true }) {
                            // The reader publishes the union of outgoing and incoming regions.
                            // Also cover colour content on its own and colour on a mono panel.
                            panel = mono ? kobalt::AnimationPanel::Mono
                                         : kobalt::AnimationPanel::Color;
                            success = succeeds;
                            arms = repairs = 0;
                            reader.setProperty("reader_image_repairs",
                                               QVariant::fromValue(content == 1 || content == 2
                                                                           ? QRegion(image)
                                                                           : QRegion()));
                            reader.setProperty(
                                    "reader_mono_repairs",
                                    QVariant::fromValue(content == 1 ? QRegion(image) : QRegion()));
                            reader.setProperty("reader_colour_regions", content == 3);
                            store.beginPaint(QRegion(reader.rect()));
                            auto *pixels = static_cast<QImage *>(store.paintDevice());
                            pixels->fill(mono ? qRgb(100, 100, 100) : qRgb(200, 20, 40));
                            *pixels = pixels->convertToFormat(format);
                            store.endPaint();
                            const QImage expected = pixels->copy();
                            std::vector<QRegion> requests;
                            std::vector<quint64> policies;
                            require(kobalt::submitPageTransition(
                                            &store, &reader, direction, dark, 10, 20,
                                            [&](const QRegion &regions, quint64 policy) {
                                                requests.push_back(regions);
                                                policies.push_back(policy);
                                                require(*pixels == expected,
                                                        "Transition altered the final backing-store pixels");
                                                if (policy == 10)
                                                    result = success
                                                            ? kobalt::PageAnimationResult::Swept
                                                            : kobalt::PageAnimationResult::Fallback;
                                                else
                                                    require(policy == 20, "Unknown cleanup policy");
                                            }),
                                    "Valid backing store did not submit a transition");
                            const bool quality =
                                    content == 1 || (!mono && (content == 2 || content == 3));
                            require(requests.size() == (quality || success ? 1u : 2u),
                                    "Turn submitted an extra refresh");
                            for (const auto &region : requests)
                                require(region == QRegion(reader.rect()),
                                        "A turn refresh was restricted to an image region");
                            require(arms == (quality ? 0 : 1),
                                    "Picture turn armed a sweep or text turn skipped it");
                            // Picture turns refresh like Nickel, with the ordinary policy and no cleanup flash.
                            require(policies[0] == 10u, "Turn selected the wrong refresh policy");
                            if (!quality && !success)
                                require(policies[1] == 20,
                                        "Failed sweep did not clean the entire page");
                            require(repairs == 0, "Turn attempted a post-sweep regional repair");
                            require(*pixels == expected,
                                    "Transition changed retained backing-store pixels");
                        }
    available = false;
    bool submitted = false;
    require(!kobalt::submitPageTransition(&store, &reader, 1, false, 10, 20,
                                          [&](const QRegion &, quint64) { submitted = true; })
                    && !submitted,
            "Unsupported device submitted an animation");
    available = true;
    require(!kobalt::submitPageTransition(&store, &reader, 0, false, 10, 20,
                                          [&](const QRegion &, quint64) { submitted = true; })
                    && !submitted,
            "Non-turn submitted an animation");
    std::puts(
            "PASS: picture turns use one ordinary full refresh, text sweeps preserve final pixels and failures clean the whole page");
}
