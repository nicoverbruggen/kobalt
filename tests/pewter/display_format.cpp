#include "platforms/kobo/display/image_format.h"
#include "platforms/kobo/display/opaque_copy.h"
#include <cstdio>
#include <cstdlib>

static void require(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static void opaqueCopies()
{
    using namespace kobalt;
    for (auto from :
         { QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBX8888,
           QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied, QImage::Format_RGB32 }) {
        for (auto to : { QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied,
                         QImage::Format_RGBX8888, QImage::Format_ARGB32,
                         QImage::Format_ARGB32_Premultiplied, QImage::Format_RGB32 }) {
            for (auto mode :
                 { QPainter::CompositionMode_SourceOver, QPainter::CompositionMode_Source }) {
                QImage source(257, 31, from);
                for (int y = 0; y < source.height(); ++y)
                    for (int x = 0; x < source.width(); ++x)
                        source.setPixelColor(x, y, QColor(x % 256, (x + y) % 256, (x * 17) % 256));
                QImage expected(273, 45, to);
                expected.fill(QColor(19, 37, 71, 128));
                QImage actual = expected.copy();
                const QImage original = source.copy();
                const QRectF input(3, 2, 251, 27), output(9, 7, 251, 27);
                QPainter reference(&expected), candidate(&actual);
                reference.setCompositionMode(mode);
                candidate.setCompositionMode(mode);
                const QRegion clip = QRegion(11, 9, 200, 22) - QRegion(31, 13, 12, 11);
                reference.setClipRegion(clip);
                candidate.setClipRegion(clip);
                reference.drawImage(output, source, input);
                const bool copied =
                        copyOpaqueDisplayImage(candidate, output, source, input, Qt::AutoColor);
                const bool sameLayout =
                        (from >= QImage::Format_RGBX8888) == (to >= QImage::Format_RGBX8888);
                const bool alreadyFast = from == to
                        && (mode == QPainter::CompositionMode_Source
                            || from == QImage::Format_RGBX8888 || from == QImage::Format_RGB32);
                require(copied == (sameLayout && !alreadyFast),
                        "opaque region selected the wrong copy path");
                if (!copied)
                    candidate.drawImage(output, source, input);
                require(candidate.compositionMode() == mode, "copy changed the painter state");
                reference.end();
                candidate.end();
                require(actual == expected, "opaque copy changed pixels or clipping");
                require(source == original, "opaque copy changed its source");
            }
        }
    }
    QImage source(33, 12, QImage::Format_RGBA8888);
    QImage destination(33, 12, QImage::Format_RGBA8888);
    destination.fill(Qt::white);
    QPainter painter(&destination);
    auto copy = [&](const QRectF &target, const QRectF &input) {
        return copyOpaqueDisplayImage(painter, target, source, input, Qt::AutoColor);
    };
    const QRectF rect(source.rect());
    for (int alpha = 0; alpha < 255; ++alpha) {
        source.fill(Qt::white);
        source.setPixelColor(32, 11, QColor(171, 31, 97, alpha));
        require(!copy(rect, rect), "translucent pixel used the opaque copy path");
        const QImage premultiplied = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        require(!copyOpaqueDisplayImage(painter, rect, premultiplied, rect, Qt::AutoColor),
                "translucent Source conversion used the opaque copy path");
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    require(destination.pixelColor(32, 11) == QColor(Qt::white),
            "a rejected copy changed the destination");
    // Exercise every SIMD lane, consecutive vector blocks and the scalar tail.
    for (int alphaByte : { 0, 3 }) {
        QImage image(33, 12, QImage::Format_RGBA8888);
        for (int x = 0; x < image.width(); ++x) {
            image.fill(Qt::white);
            image.scanLine(11)[x * 4 + alphaByte] = 127;
            require(!opaqueDisplayPixels(image, image.rect(), alphaByte),
                    "opacity check missed a vector lane or tail pixel");
        }
    }
    source.fill(Qt::white);
    painter.setOpacity(.5);
    require(!copy(rect, rect), "painter opacity was ignored");
    painter.setOpacity(1);
    painter.translate(1, 0);
    require(!copy(rect, rect), "painter transform was ignored");
    painter.resetTransform();
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    require(!copy(rect, rect), "composition mode was ignored");
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    require(!copy(QRectF(0, 0, 8, 6), rect), "scaling used the copy path");
    require(!copy(rect.translated(.5, 0), rect), "fractional position used the copy path");
    require(!copy(rect, rect.translated(-1, 0)), "source bounds were ignored");
    require(!copy(rect.translated(1, 0), rect), "destination bounds were ignored");
    require(!copyOpaqueDisplayImage(painter, rect, destination, rect, Qt::AutoColor),
            "aliased pixels used the copy path");
    source.setDevicePixelRatio(2);
    require(!copy(rect, rect), "source pixel ratio was ignored");
    painter.end();
}

int main()
{
    using namespace kobalt;
    opaqueCopies();
    require(needsPremultipliedDisplay(QImage::Format_RGBA8888),
            "straight RGBA needs the workaround");
    for (auto format : { QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBX8888,
                         QImage::Format_RGB16, QImage::Format_RGB32, QImage::Format_Invalid })
        require(!needsPremultipliedDisplay(format),
                "fixed or unsupported formats must be untouched");
    require(displayFormatPaintCheck(), "premultiplied drawing changed the rendered colours");
    bool eligible = false;
    {
        DisplayAllocationScope reader(eligible, true);
        require(eligible, "reader allocation scope");
        {
            DisplayAllocationScope stock(eligible, false);
            require(!eligible, "nested stock allocation must not inherit reader eligibility");
        }
        require(eligible, "nested scope must restore reader eligibility");
    }
    require(!eligible, "reader eligibility must end with its native call");
    return 0;
}
