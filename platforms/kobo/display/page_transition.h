#pragma once
#include "page_animation.h"
#include <QBackingStore>
#include <QImage>
#include <QRegion>
#include <QVariant>
#include <QWidget>

namespace kobalt {
// Turns with large pictures or colour skip the sweep and refresh the whole
// screen once with the ordinary full-mode policy, as Nickel turns, without a
// flash. Both pages contribute regions, so leaving an illustration redraws it
// too. Only a failed sweep falls back to the `repair` cleanup.
template <class Submit>
bool submitPageTransition(QBackingStore *store, QWidget *reader, int direction, bool dark,
                          quint64 ordinary, quint64 repair, Submit submit)
{
    if (!direction || !pageAnimationsAvailable())
        return false;
    const QRect screen = reader->rect();
    const bool color = pageAnimationPanel() == AnimationPanel::Color;
    const QRegion regions = reader->property(color ? "reader_image_repairs" : "reader_mono_repairs")
                                    .value<QRegion>()
            & screen;
    if (!regions.isEmpty() || (color && reader->property("reader_colour_regions").toBool())) {
        submit(QRegion(screen), ordinary);
        return true;
    }
    auto *device = store->paintDevice();
    if (!device || device->devType() != QInternal::Image) {
        return false;
    }
    auto *pixels = static_cast<QImage *>(device);
    if (pixels->size() != screen.size()
        || (pixels->format() != QImage::Format_RGB32 && pixels->format() != QImage::Format_ARGB32
            && pixels->format() != QImage::Format_ARGB32_Premultiplied
            && pixels->format() != QImage::Format_RGBX8888
            && pixels->format() != QImage::Format_RGBA8888
            && pixels->format() != QImage::Format_RGBA8888_Premultiplied)) {
        return false;
    }
    armPageAnimation(direction, screen, dark);
    submit(QRegion(screen), ordinary);
    const bool swept = pageAnimationSucceeded();
    const bool failed = pageAnimationResult() == PageAnimationResult::Fallback;
    cancelPageAnimation();
    if (!swept && failed)
        submit(QRegion(screen), repair);
    return true;
}
} // namespace kobalt
