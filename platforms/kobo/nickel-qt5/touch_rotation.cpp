#include "touch_rotation.h"
#include "../common/standby_input.h"

#include <QGuiApplication>
#include <QFile>
#include <QScreen>

#include <atomic>

namespace kobalt::nickel_qt5 {
QPoint (*nativeMapFromDevice)(const void *, const QPoint &);
QPoint (*nativeTouchTransform)(const QTransform *, const QPoint &);

namespace {
std::atomic<int> rotation{ 0 };
// Written once before rotation is enabled, then immutable for the input thread.
int portraitWidth = 0, portraitHeight = 0;
thread_local bool mappingTouch = false;
} // namespace

bool reversedTouchForVersion(const QByteArray &record)
{
    if (record.isEmpty() || record.size() > 1024 || record.contains('\0')) {
        return false;
    }
    QByteArray line = record;
    while (line.endsWith('\r') || line.endsWith('\n')) {
        line.chop(1);
    }
    if (line.contains('\r') || line.contains('\n')) {
        return false;
    }
    const auto fields = line.split(',');
    if (fields.size() != 6) {
        return false;
    }
    const auto version = fields[2].split('.');
    if (version.size() != 3 || version[0] != "4") {
        return false;
    }
    for (int field : { 1, 2 }) {
        if (version[field].isEmpty()) {
            return false;
        }
        for (const char ch : version[field]) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
    }
    bool minorOk = false, patchOk = false;
    const uint minor = version[1].toUInt(&minorOk);
    (void)version[2].toUInt(&patchOk);
    if (!minorOk || !patchOk) {
        return false;
    }
    const auto id = fields[5];
    // Only Clara BW and Clara Colour need the replacement transform.
    // Clara HD must retain the driver's native orientation mapping.
    return minor >= 45
            && (id == "00000000-0000-0000-0000-000000000391"
                || id == "00000000-0000-0000-0000-000000000395"
                || id == "00000000-0000-0000-0000-000000000393");
}

QPoint rotatedTouchPoint(QPoint point, Qt::ScreenOrientation orientation, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return point;
    }
    point.setX(qBound(0, point.x(), width - 1));
    point.setY(qBound(0, point.y(), height - 1));
    switch (orientation) {
    case Qt::LandscapeOrientation:
        return { height - 1 - point.y(), point.x() };
    case Qt::InvertedPortraitOrientation:
        return { width - 1 - point.x(), height - 1 - point.y() };
    case Qt::InvertedLandscapeOrientation:
        return { point.y(), width - 1 - point.x() };
    default:
        return point;
    }
}

void configureTouchRotation(bool enabled, Qt::ScreenOrientation orientation, int width, int height)
{
    // The panel dimensions do not change when the screen rotates.
    if (!portraitWidth && width > 0 && height > 0) {
        portraitWidth = width;
        portraitHeight = height;
    }
    rotation.store(enabled ? int(orientation) : 0, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) QPoint
kobalt_touch_map_from_device(const void *self, const QPoint &point)
{
    standbyTouchAcquired();

    struct Scope
    {
        bool saved = mappingTouch;

        Scope() { mappingTouch = true; }

        ~Scope() { mappingTouch = saved; }
    } scope;

    return nativeMapFromDevice(self, point);
}

extern "C" __attribute__((visibility("default"))) QPoint
kobalt_touch_transform(const QTransform *transform, const QPoint &point)
{
    const int current = rotation.load(std::memory_order_acquire);
    if (!mappingTouch || !current || !nativeMapFromDevice || !nativeTouchTransform) {
        return nativeTouchTransform(transform, point);
    }
    // The driver has already mapped the panel axes to portrait coordinates.
    // Replace only its final orientation transform, including stale transforms
    // after returning to portrait. This also composes with NickelRotate's hook.
    if (current == Qt::PortraitOrientation) {
        return point;
    }
    return rotatedTouchPoint(point, static_cast<Qt::ScreenOrientation>(current), portraitWidth,
                             portraitHeight);
}
} // namespace kobalt::nickel_qt5

namespace kobalt::nickel_qt5 {
void ownReaderTouchRotation(bool owned)
{
    static bool initialized = false, supported = false, active = false;
    auto *screen = QGuiApplication::primaryScreen();
    if (!initialized && screen) {
        initialized = true;
        QFile version("/mnt/onboard/.kobo/version");
        QSize size = screen->size();
        if (size.width() > size.height()) {
            size.transpose();
        }
        supported = version.open(QIODevice::ReadOnly) && reversedTouchForVersion(version.read(1025))
                && size == QSize(1072, 1448)
                && screen->nativeOrientation() == Qt::PortraitOrientation;
        configureTouchRotation(false, screen->orientation(), size.width(), size.height());
        QObject::connect(screen, &QScreen::orientationChanged, QGuiApplication::instance(),
                         [](Qt::ScreenOrientation orientation) {
                             configureTouchRotation(active && supported, orientation, 0, 0);
                         });
    }
    active = owned;
    if (screen) {
        configureTouchRotation(active && supported, screen->orientation(), 0, 0);
    }
}
} // namespace kobalt::nickel_qt5
