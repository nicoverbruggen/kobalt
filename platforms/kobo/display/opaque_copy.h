#pragma once
#include <QImage>
#include <QPainter>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#  include <arm_neon.h>
#endif

namespace kobalt {
inline bool opaqueDisplayPixels(const QImage &image, const QRect &rect, int alphaByte)
{
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *pixel = image.constScanLine(y) + rect.left() * 4;
        int remaining = rect.width();
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        uint8x16_t alpha = vdupq_n_u8(255);
        while (remaining >= 4) {
            alpha = vandq_u8(alpha, vld1q_u8(pixel));
            pixel += 16;
            remaining -= 4;
        }
        // Alpha occupies byte 0 or 3 in each supported pixel layout. Plain
        // loads avoid deinterleaving all four channels just to check one.
        const auto lanes = alphaByte == 3 ? vextq_u8(alpha, alpha, 3) : alpha;
        if ((vgetq_lane_u8(lanes, 0) & vgetq_lane_u8(lanes, 4) & vgetq_lane_u8(lanes, 8)
             & vgetq_lane_u8(lanes, 12))
            != 255)
            return false;
#endif
        while (remaining--) {
            if (pixel[alphaByte] != 255)
                return false;
            pixel += 4;
        }
    }
    return true;
}

// The MTK compositor uses straight RGBA even for an opaque reader. Qt's generic
// composition path converts it through RGBA64. For opaque, unscaled pixels the
// layouts within each family are identical, so Source can copy them without
// conversion. BW has the same problem with its native ARGB32 background window.
inline bool copyOpaqueDisplayImage(QPainter &painter, const QRectF &target, const QImage &image,
                                   const QRectF &source, Qt::ImageConversionFlags flags)
{
    auto layout = [](QImage::Format format) {
        switch (format) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
            return 1; // Native-endian ARGB words.
        case QImage::Format_RGBX8888:
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
            return 2; // Byte-ordered RGBA.
        default:
            return 0;
        }
    };
    const int sourceLayout = layout(image.format());
    const auto mode = painter.compositionMode();
    auto *device = painter.device();
    if (!device || device->devType() != QInternal::Image || !sourceLayout
        || (mode != QPainter::CompositionMode_SourceOver
            && mode != QPainter::CompositionMode_Source)
        || painter.opacity() != 1 || !painter.combinedTransform().isIdentity()
        || painter.testRenderHint(QPainter::SmoothPixmapTransform) || image.devicePixelRatio() != 1
        || source.isEmpty() || source != QRectF(source.toRect())
        || target != QRectF(target.toRect()) || source.size() != target.size()
        || !QRectF(image.rect()).contains(source))
        return false;
    const auto *destination = static_cast<const QImage *>(device);
    if (layout(destination->format()) != sourceLayout || destination->devicePixelRatio() != 1
        || !QRectF(destination->rect()).contains(target))
        return false;
    // Qt already blits these cases, including transparency in Source mode.
    if (image.format() == destination->format()
        && (mode == QPainter::CompositionMode_Source || !image.hasAlphaChannel()))
        return false;
    const auto begin = reinterpret_cast<quintptr>(image.constBits());
    const auto end = begin + image.sizeInBytes();
    const auto destinationBegin = reinterpret_cast<quintptr>(destination->constBits());
    const auto destinationEnd = destinationBegin + destination->sizeInBytes();
    if (begin < destinationEnd && destinationBegin < end)
        return false;
    const QRect rect = source.toRect();
    const int alphaByte = sourceLayout == 1 && Q_BYTE_ORDER == Q_BIG_ENDIAN ? 0 : 3;
    if (!opaqueDisplayPixels(image, rect, alphaByte))
        return false;
    // Only the checked rectangle is read. Its bytes have the same meaning in
    // the selected family; neither the source nor its format is modified.
    QImage view(image.constBits(), image.width(), image.height(), image.bytesPerLine(),
                destination->format());
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(target, view, source, flags);
    painter.setCompositionMode(mode);
    return true;
}
} // namespace kobalt
