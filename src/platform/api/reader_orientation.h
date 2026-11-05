#pragma once
#include <QJsonObject>
#ifndef KOBALT_NO_SENSORS
#  include <QOrientationSensor>
#else
#  include <QObject>
class QOrientationSensor;
#endif
#include <QPointer>
#include <QTimer>
#include <functional>

class QScreen;

namespace kobalt {
// The orientation that the screen shows now. Qt 6 reports Qt::PrimaryOrientation
// until the first orientation change, and Nickel makes none at boot. Turning to
// PrimaryOrientation selects the panel's own rotation, which is landscape on the
// Libra Colour. Resolve it from the screen's shape, as Qt 5 does. The result is
// PrimaryOrientation only without a screen.
Qt::ScreenOrientation shownOrientation(const QScreen *screen);

class ReaderOrientation : public QObject
#ifndef KOBALT_NO_SENSORS
    ,
                          private QOrientationFilter
#endif
{
public:
    // Sensorless desktop builds accept nullptr and retain manual rotation and lifecycle.
    // Ownership begins before the first turn and ends after stop() prepares
    // Nickel's orientation. Repeated start/stop calls do not repeat callbacks.
    ReaderOrientation(QOrientationSensor *sensor, std::function<void(Qt::ScreenOrientation)> turn,
                      std::function<void(bool)> rotationOwnership = {},
                      std::function<bool()> nativeRotationAllowed = {});
    ~ReaderOrientation() override;
    void configure(const QJsonObject &settings);
    // Start in a permitted sensor or shown orientation. After a sleep handoff,
    // resume without replacing the original nativeOrientation with Nickel's
    // temporary wake orientation. Restore the reading orientation saved by
    // freeze(), then follow fresh awake readings. Discard sleep samples and
    // the stale public sensor cache; the shared sensor has no polling API.
    void start(Qt::ScreenOrientation nativeOrientation);
    // Restore nativeOrientation, unless it is PrimaryOrientation. Sleep keeps
    // a same-axis orientation until wake or a final stop(), including when the
    // reader closes or is destroyed while hidden. Quarter turns restore native.
    // An awake close also publishes the latest valid reading to a borrowed
    // sensor, so its native owner can select the current permitted orientation.
    void stop(bool keepForSleep = false);
    // Cancel pending turns and discard readings from the sleep request onward.
    // Keep the filter installed so Nickel cannot rotate the sleep screen.
    // The host can permit native sensor delivery while its awake PIN screen
    // is visible, without resuming the reader or replacing its saved orientation.
    void freeze();
    // Ignore manual turns while frozen so wake can restore the reading orientation.
    void rotate();
    bool setAutomatic(bool enabled);
    void setBlocked(bool blocked);

    bool automaticAvailable() const { return sensor != nullptr; }

    // Auto permits every sensor orientation. With Auto off, sensor devices
    // still follow both orientations matching manual(), normalized to an axis.
    // Devices without a sensor keep the exact manual orientation.
    bool automatic() const { return automaticRotation; }

    Qt::ScreenOrientation manual() const { return manualOrientation; }

    QJsonObject settings() const;

private:
    std::function<void(bool)> rotationOwnership;
    std::function<bool()> nativeRotationAllowed;
#ifndef KOBALT_NO_SENSORS
    QPointer<QOrientationSensor> sensor;
#else
    QOrientationSensor *sensor = nullptr;
#endif
    std::function<void(Qt::ScreenOrientation)> turn;
    QTimer debounce;
    Qt::ScreenOrientation manualOrientation = Qt::PortraitOrientation;
    Qt::ScreenOrientation nativeOrientation = Qt::PortraitOrientation;
    Qt::ScreenOrientation currentOrientation = Qt::PortraitOrientation;
    Qt::ScreenOrientation pending = Qt::PrimaryOrientation;
    // The filter rejects samples, so sensor->reading() is stale while active.
    Qt::ScreenOrientation heldOrientation = Qt::PrimaryOrientation;
#ifndef KOBALT_NO_SENSORS
    QOrientationReading::Orientation latestReading = QOrientationReading::Undefined;
#endif
    quint64 latestTimestamp = 0;
    bool frozen = false;
    bool active = false, sleeping = false, blocked = false, automaticRotation = false,
         startedSensor = false;
#ifndef KOBALT_NO_SENSORS
    bool filter(QOrientationReading *reading) override;
#endif
    bool startSensor();
    bool permits(Qt::ScreenOrientation orientation) const;
    void follow();
    void deliver();

    void apply(Qt::ScreenOrientation value)
    {
        currentOrientation = value;
        turn(value);
    }
};
} // namespace kobalt
