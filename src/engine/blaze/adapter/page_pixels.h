#pragma once
#include "render/page_refresh_regions.h"
#include <QImage>
#include <QVector>
#if defined(__arm__) && defined(__ARM_NEON)
#  include <arm_neon.h>
#endif

namespace kobalt {
// CREngine supplies 0x00RRGGBB pixels. Convert them to opaque Qt pixels and
// record every coloured tile, including faint antialiased edges. With
// `rgbaBytes`, the pixels are stored in RGBX8888 byte order instead of RGB32,
// so a screen that stores RGBA bytes can copy the page without converting it.
// The image keeps its RGB32 format; the caller reinterprets it.
inline bool normalizePagePixels(QImage &image, bool dark, QVector<uchar> *colorTiles,
                                int tileColumns, bool rgbaBytes = false)
{
    const int width = image.width(), height = image.height();
    quint32 colorBits = 0;
    const quint32 inversion = dark ? 0x00ffffffU : 0;
    // Keep the compiler-generated loop on ARM64.
#if defined(__arm__) && defined(__ARM_NEON)
    uint32x4_t vectorColorBits = vdupq_n_u32(0);
    const uint32x4_t vectorInversion = vdupq_n_u32(inversion);
    const uint32x4_t opaque = vdupq_n_u32(0xff000000U);
    const uint32x4_t greenMask = vdupq_n_u32(0x0000ff00U), byteMask = vdupq_n_u32(0x000000ffU);
#endif
    for (int y = 0; y < height; ++y) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
        int x = 0;
#if defined(__arm__) && defined(__ARM_NEON)
        for (; x + 4 <= width; x += 4) {
            const uint32x4_t pixel = vld1q_u32(pixels + x);
            const uint32x4_t green = vshrq_n_u32(pixel, 8);
            vectorColorBits = vorrq_u32(
                    vectorColorBits,
                    vorrq_u32(veorq_u32(vshrq_n_u32(pixel, 16), green), veorq_u32(green, pixel)));
            uint32x4_t shown = veorq_u32(pixel, vectorInversion);
            if (rgbaBytes)
                shown = vorrq_u32(vorrq_u32(vandq_u32(shown, greenMask),
                                            vandq_u32(vshrq_n_u32(shown, 16), byteMask)),
                                  vshlq_n_u32(vandq_u32(shown, byteMask), 16));
            vst1q_u32(pixels + x, vorrq_u32(shown, opaque));
        }
#endif
        for (; x < width; ++x) {
            const quint32 pixel = pixels[x];
            colorBits |= ((pixel >> 16) ^ (pixel >> 8)) | ((pixel >> 8) ^ pixel);
            quint32 shown = pixel ^ inversion;
            if (rgbaBytes)
                shown = (shown & 0x0000ff00U) | ((shown >> 16) & 0xffU) | ((shown & 0xffU) << 16);
            pixels[x] = shown | 0xff000000U;
        }
    }
#if defined(__arm__) && defined(__ARM_NEON)
    colorBits |= vgetq_lane_u32(vectorColorBits, 0) | vgetq_lane_u32(vectorColorBits, 1)
            | vgetq_lane_u32(vectorColorBits, 2) | vgetq_lane_u32(vectorColorBits, 3);
#endif
    // Grayscale pages need no colour regions. Keep their pixel conversion
    // free of tile writes and per-pixel colour thresholds. The checks below
    // treat red and blue alike, so they also hold for RGBA bytes.
    if (!(colorBits & 0xffU))
        return false;
    bool hasColor = false;
    auto *tiles = colorTiles ? colorTiles->data() : nullptr;
    for (int y = 0; y < height; ++y) {
        const auto *pixels = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = pixels[x];
            if (qRed(pixel) == qGreen(pixel) && qGreen(pixel) == qBlue(pixel))
                continue;
            if (hasVisibleColor(pixel))
                hasColor = true;
            if (tiles)
                tiles[(y / 32) * tileColumns + x / 32] = 1;
        }
    }
    return hasColor;
}
} // namespace kobalt
