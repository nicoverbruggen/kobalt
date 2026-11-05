#pragma once

#include <QByteArray>
#include <QPoint>
#include <QTransform>

namespace kobalt::nickel_qt5 {
// Adapted from Nico Verbruggen's NickelRotate. See resources/notices/NickelRotate-MIT.
void ownReaderTouchRotation(bool owned);
bool reversedTouchForVersion(const QByteArray &record);
QPoint rotatedTouchPoint(QPoint point, Qt::ScreenOrientation orientation, int width, int height);
extern QPoint (*nativeMapFromDevice)(const void *, const QPoint &);
extern QPoint (*nativeTouchTransform)(const QTransform *, const QPoint &);
// Publish only GUI-thread state. The input thread never reads a QScreen.
void configureTouchRotation(bool enabled, Qt::ScreenOrientation orientation, int width, int height);
} // namespace kobalt::nickel_qt5

// NickelHook resolves unmangled names, but these functions retain Qt's C++ ABI.
#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
extern "C"
        __attribute__((visibility("default"))) QPoint kobalt_touch_map_from_device(const void *,
                                                                                   const QPoint &);
extern "C" __attribute__((visibility("default"))) QPoint kobalt_touch_transform(const QTransform *,
                                                                                const QPoint &);
#ifdef __clang__
#  pragma clang diagnostic pop
#endif
