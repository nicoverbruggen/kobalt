#include "reader_orientation.h"

#include "support/qt_compat.h"

#include <QScreen>

namespace kobalt {
namespace {
Qt::ScreenOrientation validOrientation(int value)
{
    switch (value) {
    case Qt::PortraitOrientation:
    case Qt::LandscapeOrientation:
    case Qt::InvertedPortraitOrientation:
    case Qt::InvertedLandscapeOrientation:
        return static_cast<Qt::ScreenOrientation>(value);
    default:
        return Qt::PortraitOrientation;
    }
}

Qt::ScreenOrientation orientationAxis(Qt::ScreenOrientation orientation)
{
    switch (orientation) {
    case Qt::PortraitOrientation:
    case Qt::InvertedPortraitOrientation:
        return Qt::PortraitOrientation;
    case Qt::LandscapeOrientation:
    case Qt::InvertedLandscapeOrientation:
        return Qt::LandscapeOrientation;
    default:
        return Qt::PrimaryOrientation;
    }
}
#ifndef KOBALT_NO_SENSORS
Qt::ScreenOrientation readingOrientation(const QOrientationReading *reading)
{
    switch (reading->orientation()) {
    case QOrientationReading::TopUp:
        return Qt::PortraitOrientation;
    case QOrientationReading::TopDown:
        return Qt::InvertedPortraitOrientation;
    case QOrientationReading::LeftUp:
        return Qt::LandscapeOrientation;
    case QOrientationReading::RightUp:
        return Qt::InvertedLandscapeOrientation;
    default:
        return Qt::PrimaryOrientation;
    }
}
#endif
} // namespace

Qt::ScreenOrientation shownOrientation(const QScreen *screen)
{
    if (!screen)
        return Qt::PrimaryOrientation;
    const auto orientation = screen->orientation();
    return orientation == Qt::PrimaryOrientation ? screen->primaryOrientation() : orientation;
}

ReaderOrientation::ReaderOrientation(QOrientationSensor *sensor,
                                     std::function<void(Qt::ScreenOrientation)> turn,
                                     std::function<void(bool)> rotationOwnership,
                                     std::function<bool()> nativeRotationAllowed)
    : rotationOwnership(std::move(rotationOwnership)),
      nativeRotationAllowed(std::move(nativeRotationAllowed)),
#ifndef KOBALT_NO_SENSORS
      sensor(sensor),
#endif
      turn(std::move(turn))
{
#ifdef KOBALT_NO_SENSORS
    Q_ASSERT(!sensor);
    Q_UNUSED(sensor);
#endif
    // The member timer belongs to this object and has a name, so tests can
    // check and run it instead of waiting for it.
    debounce.setParent(this);
    debounce.setObjectName("rrOrientationDebounce");
    debounce.setSingleShot(true);
    debounce.setInterval(250);
    connect(&debounce, &QTimer::timeout, this, [this] { deliver(); });
}

ReaderOrientation::~ReaderOrientation()
{
    stop();
}

void ReaderOrientation::configure(const QJsonObject &settings)
{
    manualOrientation =
            validOrientation(settings.value("orientation").toInt(Qt::PortraitOrientation));
    if (sensor) {
        manualOrientation = orientationAxis(manualOrientation);
    }
    const bool automatic = settings.value("auto_rotate").toBool(false);
    if (active && !frozen
        && (!sensor || (!automatic && orientationAxis(currentOrientation) != manualOrientation))) {
        apply(manualOrientation);
    }
    setAutomatic(automatic);
}

void ReaderOrientation::start(Qt::ScreenOrientation orientation)
{
    if (active) {
        // Wake can arrive before the sleep view hides the reader. Native PIN
        // rotation may still have changed the screen while we were frozen.
        if (frozen) {
            frozen = false;
            apply(currentOrientation);
        }
        return;
    }
    const bool waking = sleeping;
    if (!waking) {
        nativeOrientation = orientation;
        heldOrientation = Qt::PrimaryOrientation;
#ifndef KOBALT_NO_SENSORS
        latestReading = QOrientationReading::Undefined;
#endif
        latestTimestamp = 0;
    }
    sleeping = false;
    frozen = false;
    active = true;
    if (rotationOwnership) {
        rotationOwnership(true);
    }
    // Quarter-turn sleep restores Nickel's geometry in currentOrientation.
    // freeze() kept the reading orientation separately, so waking does not
    // need another sensor sample to restore it.
    auto initial = waking ? heldOrientation : Qt::PrimaryOrientation;
#ifndef KOBALT_NO_SENSORS
    if (sensor) {
        if (!waking) {
            sensor->addFilter(this);
        }
        const auto *reading = sensor->isActive() ? sensor->reading() : nullptr;
        // Sleep and PIN samples must not replace the saved reading orientation.
        if (reading && !waking) {
            heldOrientation = readingOrientation(reading);
            latestReading = reading->orientation();
            latestTimestamp = reading->timestamp();
            initial = heldOrientation;
        }
    }
#endif
    const auto shown = waking ? currentOrientation : orientation;
    apply(permits(initial) ? initial : permits(shown) ? shown : manualOrientation);
    if (sensor) {
        startSensor();
    }
}

void ReaderOrientation::stop(bool keepForSleep)
{
    if (keepForSleep) {
        freeze();
    }
    if (!active) {
        // A hidden reader can close before waking. It still owes Nickel the
        // orientation saved at the first start(), even after releasing input.
        if (sleeping && !keepForSleep) {
            sleeping = false;
#ifndef KOBALT_NO_SENSORS
            if (sensor) {
                sensor->removeFilter(this);
            }
#endif
            if (nativeOrientation != Qt::PrimaryOrientation) {
                apply(nativeOrientation);
            }
        }
        return;
    }
#ifndef KOBALT_NO_SENSORS
    const bool handback =
            !keepForSleep && !frozen && sensor && !startedSensor && sensor->isActive();
#endif
    active = false;
    debounce.stop();
    pending = Qt::PrimaryOrientation;
#ifndef KOBALT_NO_SENSORS
    if (sensor) {
        if (!keepForSleep) {
            sensor->removeFilter(this);
        }
        if (startedSensor) {
            sensor->stop();
        }
    }
#endif
    startedSensor = false;
    // A half turn keeps the sleep image's geometry. A quarter turn must give
    // Nickel its original geometry back before relinquishing rotation.
    sleeping = keepForSleep;
    const bool sameAxis = orientationAxis(currentOrientation) == orientationAxis(nativeOrientation);
    frozen = keepForSleep;
    if (nativeOrientation != Qt::PrimaryOrientation && (!sleeping || !sameAxis)) {
        apply(nativeOrientation);
    }
#ifndef KOBALT_NO_SENSORS
    if (handback && sensor && sensor->isActive() && latestReading >= QOrientationReading::TopUp
        && latestReading <= QOrientationReading::RightUp) {
        // Rejected samples never reached the borrowed sensor's cache or its
        // native owner. Publish the final sample once, after restoring the
        // native screen mask. Waiting for another movement leaves Home in the
        // opening orientation even when the device is now held upside down.
        if (auto *reading = sensor->reading()) {
            reading->setOrientation(latestReading);
            reading->setTimestamp(latestTimestamp);
            QMetaObject::invokeMethod(sensor.data(), "readingChanged", Qt::DirectConnection);
        }
    }
#endif
    if (rotationOwnership) {
        rotationOwnership(false);
    }
}

void ReaderOrientation::freeze()
{
    if (!active || frozen) {
        return;
    }
    frozen = true;
    debounce.stop();
    pending = Qt::PrimaryOrientation;
    heldOrientation = currentOrientation;
}

void ReaderOrientation::rotate()
{
    if (frozen)
        return;
    setAutomatic(false);
    manualOrientation = currentOrientation == Qt::InvertedLandscapeOrientation
            ? Qt::PortraitOrientation
            : static_cast<Qt::ScreenOrientation>(int(currentOrientation) * 2);
    if (active && !frozen) {
        apply(manualOrientation);
    }
}

bool ReaderOrientation::setAutomatic(bool enabled)
{
    debounce.stop();
    pending = Qt::PrimaryOrientation;
    automaticRotation = enabled && sensor;
    if (active && !frozen && sensor) {
        if (!startSensor()) {
            return false;
        }
        follow();
    }
    return !enabled || automaticRotation;
}
#ifndef KOBALT_NO_SENSORS
bool ReaderOrientation::startSensor()
{
    if (sensor->isActive()) {
        return true;
    }
    startedSensor = sensor->start();
    if (!startedSensor) {
        automaticRotation = false;
    }
    return startedSensor;
}
#else
bool ReaderOrientation::startSensor()
{
    return false;
}
#endif
void ReaderOrientation::setBlocked(bool value)
{
    blocked = value;
    if (!blocked && pending != Qt::PrimaryOrientation && !debounce.isActive())
        debounce.start(0);
}
#ifndef KOBALT_NO_SENSORS
bool ReaderOrientation::filter(QOrientationReading *reading)
{
    if (sleeping || frozen) {
        return nativeRotationAllowed && nativeRotationAllowed();
    }
    if (!active) {
        return true;
    }
    heldOrientation = readingOrientation(reading);
    latestReading = reading->orientation();
    latestTimestamp = reading->timestamp();
    follow();
    // Nickel keeps its connection. Final close releases the filter.
    return false;
}
#endif
bool ReaderOrientation::permits(Qt::ScreenOrientation orientation) const
{
    return orientation != Qt::PrimaryOrientation
            && (automaticRotation || orientation == manualOrientation
                || (sensor && orientationAxis(orientation) == manualOrientation));
}

void ReaderOrientation::follow()
{
    if (!permits(heldOrientation) || heldOrientation == currentOrientation) {
        pending = Qt::PrimaryOrientation;
        debounce.stop();
    } else if (heldOrientation != pending) {
        // Repeated samples at the same angle must not postpone the turn.
        pending = heldOrientation;
        debounce.start(250);
    }
}

void ReaderOrientation::deliver()
{
    if (!active || frozen || !sensor || blocked || pending == Qt::PrimaryOrientation)
        return;
    const auto orientation = pending;
    pending = Qt::PrimaryOrientation;
    apply(orientation);
}

QJsonObject ReaderOrientation::settings() const
{
    return kobalt::qt::object(
            { { "orientation", int(manualOrientation) }, { "auto_rotate", automaticRotation } });
}
} // namespace kobalt
