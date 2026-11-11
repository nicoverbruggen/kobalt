#pragma once
#include <QImage>
#include <QPainter>

namespace kobalt {
// Keep the policy independent of NickelHook so another mod can reuse it.
inline bool needsPremultipliedDisplay(QImage::Format format)
{
    return format == QImage::Format_RGBA8888;
}

// Every nested call sets its own eligibility, including unrelated windows.
// Restoring the outer value must not let a nested stock window inherit it.
class DisplayAllocationScope
{
public:
    DisplayAllocationScope(bool &state, bool eligible) : state(state), previous(state)
    {
        state = eligible;
    }

    ~DisplayAllocationScope() { state = previous; }

    DisplayAllocationScope(const DisplayAllocationScope &) = delete;
    DisplayAllocationScope &operator=(const DisplayAllocationScope &) = delete;

private:
    bool &state;
    bool previous;
};

inline bool displayFormatPaintCheck()
{
    auto draw = [](QImage::Format format, QColor paper) {
        QImage image(32, 24, format);
        image.fill(paper);
        QPainter painter(&image);
        painter.fillRect(2, 3, 21, 17, QColor(173, 41, 97, 128));
        painter.fillRect(9, 7, 19, 13, QColor(21, 137, 203, 192));
        // Kobalt highlights use Multiply/Screen, and selection uses Difference.
        for (auto mode : { QPainter::CompositionMode_Multiply, QPainter::CompositionMode_Screen,
                           QPainter::CompositionMode_Difference }) {
            painter.setCompositionMode(mode);
            painter.fillRect(5, 4, 17, 15, QColor(91, 153, 201, 160));
        }
        painter.end();
        return image.convertToFormat(QImage::Format_RGBA8888);
    };
    for (QColor paper : { QColor(Qt::white), QColor(Qt::black) }) {
        const QImage reference = draw(QImage::Format_RGBA8888, paper);
        const QImage candidate = draw(QImage::Format_RGBA8888_Premultiplied, paper);
        if (reference.isNull() || candidate.isNull())
            return false;
        for (int y = 0; y < reference.height(); ++y) {
            for (int x = 0; x < reference.width(); ++x) {
                const QRgb a = reference.pixel(x, y), b = candidate.pixel(x, y);
                // The two raster paths may round an intermediate channel differently.
                if (qAbs(qRed(a) - qRed(b)) > 1 || qAbs(qGreen(a) - qGreen(b)) > 1
                    || qAbs(qBlue(a) - qBlue(b)) > 1 || qAlpha(a) != qAlpha(b))
                    return false;
            }
        }
    }
    return true;
}

} // namespace kobalt
