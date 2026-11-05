#include "support/qt_compat.h"
#include "render/image_damage.h"
#include <QCoreApplication>
#include <QPainter>
#include <random>
#define CHECK(x)                                 \
    do {                                         \
        if (!(x))                                \
            qFatal("line %d: %s", __LINE__, #x); \
    } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using kobalt::render::imageDamage;
    QImage base(107, 73, QImage::Format_RGB32);
    base.fill(Qt::white);
    CHECK(imageDamage(base, base).isEmpty());
    CHECK(imageDamage(base, base.copy()).isEmpty());
    CHECK(imageDamage({}, base) == base.rect());
    CHECK(imageDamage(base, {}) == base.rect());
    auto changed = base.copy();
    changed.setPixel(0, 0, QColor(Qt::red).rgba());
    changed.setPixel(106, 72, QColor(Qt::blue).rgba());
    CHECK(imageDamage(base, changed) == base.rect());
    changed = base.copy();
    changed.setPixel(52, 32, QColor(Qt::red).rgba());
    CHECK(imageDamage(base, changed) == QRect(52, 32, 1, 1));
    auto alpha = base.convertToFormat(QImage::Format_ARGB32);
    auto transparent = alpha.copy();
    transparent.setPixel(4, 7, QColor(255, 255, 255, 0).rgba());
    CHECK(imageDamage(alpha, transparent) == QRect(4, 7, 1, 1));
    // Qt 5.2 has no 8-bit grayscale format. Indexed 8-bit is another format with a different depth.
    auto gray = base.convertToFormat(QImage::Format_Indexed8);
    CHECK(imageDamage(base, gray) == base.rect());
    auto scaled = base.copy();
    scaled.setDevicePixelRatio(2);
    CHECK(imageDamage(base, scaled) == base.rect());
    // Later rows can extend either edge or stay inside bounds found earlier.
    changed = base.copy();
    changed.setPixel(52, 0, QColor(Qt::black).rgba());
    changed.setPixel(20, 10, QColor(Qt::black).rgba());
    changed.setPixel(80, 20, QColor(Qt::black).rgba());
    changed.setPixel(40, 72, QColor(Qt::black).rgba());
    CHECK(imageDamage(base, changed) == QRect(20, 0, 61, 73));
    changed = base.copy();
    for (int y = 0; y < changed.height(); ++y)
        changed.setPixel(52, y, QColor(Qt::black).rgba());
    CHECK(imageDamage(base, changed) == QRect(52, 0, 1, 73));
    // Qt 5.2 has no QRandomGenerator. A seeded standard engine is as repeatable.
    std::mt19937 random(42);
    // Applying only the returned rectangle must reconstruct the destination,
    // including erasure and colour changes at arbitrary image boundaries.
    for (int run = 0; run < 100; ++run) {
        changed = base.copy();
        for (int i = 0; i < 20; ++i)
            changed.setPixel(int(random() % 107u), int(random() % 73u),
                             quint32(random()) | 0xff000000);
        auto patched = base.copy();
        const auto damage = imageDamage(base, changed);
        {
            QPainter painter(&patched);
            painter.setClipRect(damage);
            painter.drawImage(0, 0, changed);
        }
        CHECK(patched == changed);
        CHECK(imageDamage(base, changed) == imageDamage(changed, base));
    }
    qInfo("Image damage checks passed");
}
