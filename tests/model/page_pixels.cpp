#include "support/qt_compat.h"
#include "engine/crengine/page_pixels.h"
#include <random>
#define CHECK(x)                                 \
    do {                                         \
        if (!(x))                                \
            qFatal("line %d: %s", __LINE__, #x); \
    } while (0)

int main()
{
    std::mt19937 random(42);
    QImage empty;
    CHECK(!kobalt::normalizePagePixels(empty, false, nullptr, 0));
    for (int width : { 1, 2, 3, 4, 5, 7, 31, 32, 33, 65, 1072 }) {
        for (int pattern = 0; pattern < 5; ++pattern)
            for (bool dark : { false, true })
                for (bool tiles : { false, true }) {
                    QImage original(width, 65, QImage::Format_RGB32);
                    for (int y = 0; y < original.height(); ++y) {
                        auto *pixels = reinterpret_cast<QRgb *>(original.scanLine(y));
                        for (int x = 0; x < width; ++x) {
                            const quint32 value = random();
                            if (pattern == 0)
                                pixels[x] = (value & 255U) * 0x010101U;
                            else if (pattern == 1)
                                pixels[x] = value;
                            else if (pattern == 2)
                                pixels[x] = 0xff808080U;
                            else if (pattern == 3)
                                pixels[x] = x == width - 1 && y == 64 ? 0x00808081U : 0x00808080U;
                            else
                                pixels[x] = x == 0 && y == 0 ? 0x00818080U : 0x00808080U;
                        }
                    }
                    auto expected = original.copy();
                    bool color = false;
                    const int columns = (width + 31) / 32;
                    QVector<uchar> expectedTiles(columns * 3, 0), actualTiles = expectedTiles;
                    for (int y = 0; y < expected.height(); ++y) {
                        auto *pixels = reinterpret_cast<QRgb *>(expected.scanLine(y));
                        for (int x = 0; x < width; ++x) {
                            const QRgb pixel = pixels[x];
                            color |= kobalt::hasVisibleColor(pixel);
                            if (tiles
                                && (qRed(pixel) != qGreen(pixel) || qGreen(pixel) != qBlue(pixel)))
                                expectedTiles[(y / 32) * columns + x / 32] = 1;
                            pixels[x] = (pixel ^ (dark ? 0x00ffffffU : 0)) | 0xff000000U;
                        }
                    }
                    // A shared source must detach before the first vector store.
                    auto actual = original;
                    const auto unchanged = original.copy();
                    CHECK(kobalt::normalizePagePixels(actual, dark, tiles ? &actualTiles : nullptr,
                                                      columns)
                          == color);
                    CHECK(actualTiles == expectedTiles);
                    CHECK(actual == expected);
                    CHECK(original == unchanged);
                    // RGBA byte order keeps the colours and their classification.
                    auto rgba = original;
                    QVector<uchar> rgbaTiles(columns * 3, 0);
                    CHECK(kobalt::normalizePagePixels(rgba, dark, tiles ? &rgbaTiles : nullptr,
                                                      columns, true)
                          == color);
                    CHECK(rgbaTiles == expectedTiles);
                    rgba.reinterpretAsFormat(QImage::Format_RGBX8888);
                    CHECK(rgba.convertToFormat(QImage::Format_RGB32) == expected);
                    CHECK(original == unchanged);
                }
    }
    qInfo("Page pixel checks passed");
}
