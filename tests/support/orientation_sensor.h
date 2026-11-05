#pragma once
#include <QOrientationSensor>
#include <QSensorBackend>
#include <QSensorManager>

namespace kobalt::test {
class Backend : public QSensorBackend
{
public:
    QOrientationReading reading;
    quint64 timestamp = 0;

    explicit Backend(QSensor *sensor) : QSensorBackend(sensor) { setReading(&reading); }

    void start() override { }

    void stop() override { }

    void sample(QOrientationReading::Orientation value)
    {
        reading.setOrientation(value);
        reading.setTimestamp(++timestamp);
        newReadingAvailable();
    }
};

class Factory : public QSensorBackendFactory
{
public:
    Backend *backend = nullptr;

    QSensorBackend *createBackend(QSensor *sensor) override
    {
        return backend = new Backend(sensor);
    }
};
} // namespace kobalt::test
