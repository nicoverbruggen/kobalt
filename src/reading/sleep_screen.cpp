#include "sleep_screen.h"
#include "render/page_refresh_regions.h"

namespace kobalt::sleep {
bool needsImageCleanup(const QImage &frame, bool colorDisplay)
{
    if (frame.isNull())
        return true;
    const int columns = qMin(64, frame.width()), rows = qMin(64, frame.height());
    const int samples = columns * rows;
    const auto midtone = [](QRgb pixel) {
        const int gray = qGray(pixel);
        return gray >= 32 && gray <= 223;
    };
    int color = 0, midtones = 0;
    for (int row = 0; row < rows; ++row) {
        const int y = (2 * row + 1) * qint64(frame.height()) / (2 * rows);
        for (int col = 0; col < columns; ++col) {
            // Stagger alternate rows to avoid following the same text stems.
            const int x = (4 * col + 1 + 2 * (row % 2)) * qint64(frame.width()) / (4 * columns);
            const QRgb pixel = frame.pixel(x, y);
            if (qAlpha(pixel) != 255)
                return true;
            if (colorDisplay && hasVisibleColor(pixel))
                ++color;
            // Text edges contain gray too. Look for gray that extends across
            // and down the screen instead of counting isolated edge pixels.
            if (midtone(pixel) && midtone(frame.pixel(qMin(x + 1, frame.width() - 1), y))
                && midtone(frame.pixel(x, qMin(y + 1, frame.height() - 1))))
                ++midtones;
        }
    }
    // A color thumbnail or a sizable area of gray warrants a clean refresh.
    // Ordinary black text, its antialiased edges and small icons do not.
    return color * 100 >= samples || midtones * 10 >= samples;
}
} // namespace kobalt::sleep
