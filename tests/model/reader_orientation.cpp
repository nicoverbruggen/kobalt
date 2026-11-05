#ifndef KOBALT_NO_SENSORS
#  include "platform/api/reader_orientation.h"
#  include "orientation_sensor.h"

#  include <QCoreApplication>
#  include <QEventLoop>
#  include <QSensorBackend>
#  include <QSensorManager>
#  include <QTimer>

#  include <cstdio>
#  include <cstdlib>

using namespace kobalt;

namespace {
using test::Backend;
using test::Factory;

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

void settle()
{
    QEventLoop loop;
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();
}

class Fixture
{
public:
    QOrientationSensor sensor;
    QList<Qt::ScreenOrientation> turns;
    QList<bool> ownership;
    ReaderOrientation rotation{ &sensor,
                                [this](Qt::ScreenOrientation value) { turns.append(value); },
                                [this](bool owned) { ownership.append(owned); } };
    Backend *backend;

    explicit Fixture(Factory &factory, bool borrowed = true)
    {
        sensor.setIdentifier("kobalt-lock-test");
        require(sensor.connectToBackend(), "Connect test sensor");
        backend = factory.backend;
        if (borrowed) {
            require(sensor.start(), "Start borrowed sensor");
        }
    }

    void sample(QOrientationReading::Orientation value)
    {
        backend->sample(value);
        settle();
    }
};

void checkSleepRequest(Factory &factory)
{
    Fixture f(factory);
    f.backend->sample(QOrientationReading::TopUp);
    f.rotation.start(Qt::PortraitOrientation);
    f.backend->sample(QOrientationReading::TopDown);
    f.rotation.freeze();
    settle();
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::PortraitOrientation },
            "Sleep request cancels pending rotation");
    f.sample(QOrientationReading::TopDown);
    require(f.turns.size() == 1, "Sleep request ignores samples before the sleep view is ready");
    f.rotation.stop(true);
    f.sample(QOrientationReading::TopDown);
    require(f.sensor.reading()->orientation() == QOrientationReading::TopUp,
            "Sleep does not leak sensor samples to Nickel");
    f.rotation.start(Qt::PortraitOrientation);
    settle();
    require(f.turns.last() == Qt::PortraitOrientation,
            "Wake ignores samples received during sleep");
    f.sample(QOrientationReading::TopDown);
    require(f.turns.last() == Qt::InvertedPortraitOrientation,
            "Fresh awake samples resume rotation");
    f.rotation.stop();
    f.sample(QOrientationReading::TopDown);
    require(f.sensor.reading()->orientation() == QOrientationReading::TopDown,
            "Final close releases the sensor filter");
}

void checkNativeHandoff(Factory &factory)
{
    Fixture f(factory);
    f.backend->sample(QOrientationReading::TopUp);
    auto nativeShown = Qt::PortraitOrientation;
    int nativeNotifications = 0;
    QObject::connect(&f.sensor, &QSensor::readingChanged, [&] {
        ++nativeNotifications;
        const auto reading = f.sensor.reading()->orientation();
        if (reading == QOrientationReading::TopUp) {
            nativeShown = Qt::PortraitOrientation;
        } else if (reading == QOrientationReading::TopDown) {
            nativeShown = Qt::InvertedPortraitOrientation;
        }
    });
    f.rotation.start(Qt::PortraitOrientation);
    f.sample(QOrientationReading::TopDown);
    require(nativeNotifications == 0, "Reader does not notify native rotation while active");
    f.rotation.stop();
    require(nativeNotifications == 1 && nativeShown == Qt::InvertedPortraitOrientation,
            "Close gives native rotation the current reading without another movement");
    require(f.sensor.reading()->timestamp() == f.backend->reading.timestamp(),
            "Native handoff keeps sample timestamp");
    f.rotation.stop();
    require(nativeNotifications == 1, "Repeated close does not repeat the native notification");
    f.backend->sample(QOrientationReading::TopUp);
    require(nativeNotifications == 2 && nativeShown == Qt::PortraitOrientation,
            "Native rotation continues after handoff");
    f.rotation.start(Qt::PortraitOrientation);
    f.sample(QOrientationReading::TopDown);
    f.rotation.freeze();
    f.rotation.stop(true);
    const auto count = nativeNotifications;
    f.rotation.stop();
    require(nativeNotifications == count,
            "Closing asleep does not replay a sensor turn into the sleep screen");
}

void checkPinScreenHandoff(Factory &factory)
{
    QOrientationSensor sensor;
    sensor.setIdentifier("kobalt-lock-test");
    require(sensor.start(), "Start native sensor for PIN handoff");
    auto *backend = factory.backend;
    backend->sample(QOrientationReading::TopUp);
    bool pin = false;
    QList<Qt::ScreenOrientation> turns;
    ReaderOrientation rotation(
            &sensor, [&](Qt::ScreenOrientation shown) { turns.append(shown); }, {},
            [&] { return pin; });
    rotation.configure({ { "auto_rotate", true } });
    int nativeNotifications = 0;
    QObject::connect(&sensor, &QSensor::readingChanged, [&] { ++nativeNotifications; });
    rotation.start(Qt::PortraitOrientation);
    backend->sample(QOrientationReading::LeftUp);
    settle();
    require(turns.last() == Qt::LandscapeOrientation, "Reader rotates before PIN handoff");
    rotation.freeze();
    turns.clear();
    pin = true;
    backend->sample(QOrientationReading::RightUp);
    require(nativeNotifications == 1 && turns.isEmpty(),
            "PIN can rotate before the reader sleep handoff");
    pin = false;
    rotation.start(Qt::InvertedLandscapeOrientation);
    require(turns == QList<Qt::ScreenOrientation>{ Qt::LandscapeOrientation },
            "Unlock before handoff restores the reading orientation");
    for (int cycle = 0; cycle < 2; ++cycle) {
        rotation.freeze();
        rotation.stop(true);
        const auto beforeSleep = nativeNotifications;
        backend->sample(QOrientationReading::TopDown);
        require(nativeNotifications == beforeSleep,
                "Sleep screen still blocks native sensor delivery");
        turns.clear();
        pin = true;
        backend->sample(QOrientationReading::RightUp);
        require(nativeNotifications == beforeSleep + 1
                        && sensor.reading()->orientation() == QOrientationReading::RightUp,
                "Awake PIN screen receives native sensor readings");
        require(turns.isEmpty(), "PIN rotation does not turn or resume Kobalt");
        pin = false;
        backend->sample(QOrientationReading::TopDown);
        require(nativeNotifications == beforeSleep + 1,
                "Returning to sleep blocks native readings again");
        rotation.start(Qt::InvertedLandscapeOrientation);
        require(turns == QList<Qt::ScreenOrientation>{ Qt::LandscapeOrientation },
                "Unlock restores held reading orientation instead of the PIN orientation");
        backend->sample(QOrientationReading::LeftUp);
        require(nativeNotifications == beforeSleep + 1,
                "Unlocked reader owns sensor delivery again");
    }
    rotation.stop(true);
    pin = true;
    backend->sample(QOrientationReading::TopDown);
    rotation.stop();
    require(turns.last() == Qt::PortraitOrientation,
            "Closing behind PIN restores the original native orientation");
    const auto beforeClose = nativeNotifications;
    backend->sample(QOrientationReading::TopUp);
    require(nativeNotifications == beforeClose + 1,
            "Closing behind PIN releases the sensor filter");
}

void checkOpening(Factory &factory)
{
    for (const auto lock : { Qt::PortraitOrientation, Qt::LandscapeOrientation }) {
        const auto inverted = Qt::ScreenOrientation(int(lock) * 4);
        const auto reading = lock == Qt::PortraitOrientation ? QOrientationReading::TopDown
                                                             : QOrientationReading::RightUp;
        Fixture f(factory);
        f.rotation.configure({ { "orientation", int(lock) } });
        f.backend->sample(reading);
        f.rotation.start(inverted);
        settle();
        require(f.turns == QList<Qt::ScreenOrientation>{ inverted },
                "Opening inverted does not turn away and back");
        f.rotation.stop();
        f.backend->sample(QOrientationReading::Undefined);
        f.turns.clear();
        f.rotation.start(inverted);
        settle();
        require(f.turns == QList<Qt::ScreenOrientation>{ inverted },
                "Undefined sensor keeps permitted shown orientation");
    }
    Fixture f(factory);
    f.rotation.configure({});
    f.backend->sample(QOrientationReading::LeftUp);
    f.rotation.start(Qt::LandscapeOrientation);
    settle();
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::PortraitOrientation },
            "Opening obeys the portrait lock");
}

void checkSleepLifecycle(Factory &factory)
{
    for (const bool borrowed : { false, true }) {
        for (const auto opening : { Qt::PortraitOrientation, Qt::InvertedPortraitOrientation,
                                    Qt::LandscapeOrientation, Qt::InvertedLandscapeOrientation }) {
            const bool portrait = opening == Qt::PortraitOrientation
                    || opening == Qt::InvertedPortraitOrientation;
            const auto opposite =
                    Qt::ScreenOrientation(int(opening) < 4 ? int(opening) * 4 : int(opening) / 4);
            const auto sample = opposite == Qt::PortraitOrientation ? QOrientationReading::TopUp
                    : opposite == Qt::InvertedPortraitOrientation   ? QOrientationReading::TopDown
                    : opposite == Qt::LandscapeOrientation          ? QOrientationReading::LeftUp
                                                                    : QOrientationReading::RightUp;
            Fixture f(factory, borrowed);
            f.rotation.configure(
                    { { "orientation",
                        int(portrait ? Qt::PortraitOrientation : Qt::LandscapeOrientation) } });
            f.rotation.start(opening);
            f.sample(sample);
            for (int cycle = 0; cycle < 2; ++cycle) {
                f.turns.clear();
                f.rotation.stop(true);
                f.rotation.stop(true);
                require(f.turns.isEmpty(),
                        "Repeated sleep handoff keeps the same-axis orientation");
                require(f.sensor.isActive() == borrowed,
                        "Sleep stops only a reader-started sensor");
                f.rotation.start(Qt::PortraitOrientation);
                settle();
                require(f.turns == QList<Qt::ScreenOrientation>{ opposite },
                        "Wake keeps orientation without a new sample");
            }
            f.turns.clear();
            f.rotation.stop();
            f.rotation.stop();
            require(f.turns == QList<Qt::ScreenOrientation>{ opening },
                    "Closing after repeated wake restores opening");
            require(f.ownership == QList<bool>{ true, false, true, false, true, false },
                    "Ownership follows active intervals");
        }
    }
    Fixture f(factory);
    f.rotation.start(Qt::PortraitOrientation);
    f.sample(QOrientationReading::TopDown);
    f.rotation.stop(true);
    f.turns.clear();
    f.rotation.stop();
    f.rotation.stop();
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::PortraitOrientation },
            "Closing while asleep restores opening once");
    f.rotation.start(Qt::LandscapeOrientation);
    f.rotation.stop();
    require(f.turns.last() == Qt::LandscapeOrientation,
            "A new book captures its own opening orientation");
    f.rotation.configure({ { "auto_rotate", true } });
    f.rotation.start(Qt::PortraitOrientation);
    f.sample(QOrientationReading::LeftUp);
    f.turns.clear();
    f.rotation.stop(true);
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::PortraitOrientation },
            "Quarter-turn sleep restores native geometry");
    f.rotation.start(Qt::PortraitOrientation);
    settle();
    f.rotation.stop();
    require(f.turns.last() == Qt::PortraitOrientation,
            "Quarter-turn sleep keeps the original restoration target");
    f.backend->sample(QOrientationReading::LeftUp);
    f.rotation.start(Qt::LandscapeOrientation);
    f.sample(QOrientationReading::TopUp);
    f.rotation.stop(true);
    f.turns.clear();
    f.rotation.start(Qt::PortraitOrientation);
    settle();
    f.rotation.stop();
    require(f.turns
                    == QList<Qt::ScreenOrientation>{ Qt::PortraitOrientation,
                                                     Qt::LandscapeOrientation },
            "Quarter-turn wake restores reading orientation; close restores native orientation");
    QList<Qt::ScreenOrientation> turns;
    {
        QOrientationSensor sensor;
        sensor.setIdentifier("kobalt-lock-test");
        ReaderOrientation rotation(&sensor,
                                   [&](Qt::ScreenOrientation value) { turns.append(value); });
        rotation.start(Qt::PortraitOrientation);
        factory.backend->sample(QOrientationReading::TopDown);
        settle();
        rotation.stop(true);
        turns.clear();
    }
    require(turns == QList<Qt::ScreenOrientation>{ Qt::PortraitOrientation },
            "Destruction while asleep restores opening");
}

void checkQuarterTurnWake(Factory &factory)
{
    for (const bool borrowed : { false, true }) {
        for (const bool automatic : { false, true }) {
            for (const bool inverted : { false, true }) {
                Fixture f(factory, borrowed);
                const auto reading =
                        inverted ? Qt::InvertedLandscapeOrientation : Qt::LandscapeOrientation;
                const auto sample =
                        inverted ? QOrientationReading::RightUp : QOrientationReading::LeftUp;
                f.rotation.configure({ { "orientation", int(Qt::LandscapeOrientation) },
                                       { "auto_rotate", automatic } });
                f.rotation.start(Qt::PortraitOrientation);
                f.sample(sample);
                for (int cycle = 0; cycle < 2; ++cycle) {
                    f.rotation.freeze();
                    f.rotation.stop(true);
                    require(f.turns.last() == Qt::PortraitOrientation,
                            "Sleep still restores native geometry");
                    f.sample(QOrientationReading::TopDown);
                    f.turns.clear();
                    f.rotation.start(Qt::PortraitOrientation);
                    settle();
                    require(f.turns == QList<Qt::ScreenOrientation>{ reading },
                            "Quarter-turn wake restores reading orientation without a new sample");
                    require(f.sensor.isActive(), "Wake resumes sensor delivery");
                }
                f.rotation.setBlocked(true);
                f.sample(inverted ? QOrientationReading::LeftUp : QOrientationReading::RightUp);
                require(f.turns.last() == reading, "Fresh wake sample waits for layout to finish");
                f.rotation.setBlocked(false);
                settle();
                require(f.turns.last()
                                == (inverted ? Qt::LandscapeOrientation
                                             : Qt::InvertedLandscapeOrientation),
                        "Fresh wake sample corrects restored orientation after layout");
                f.rotation.stop();
                require(f.turns.last() == Qt::PortraitOrientation,
                        "Close retains original native orientation");
            }
        }
    }
}

void checkPendingReadingAtSleep(Factory &factory)
{
    Fixture f(factory);
    f.backend->sample(QOrientationReading::TopUp);
    f.rotation.start(Qt::PortraitOrientation);
    f.rotation.setBlocked(true);
    f.backend->sample(QOrientationReading::TopDown);
    f.rotation.stop(true);
    f.turns.clear();
    f.rotation.start(Qt::PortraitOrientation);
    f.rotation.setBlocked(false);
    settle();
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::PortraitOrientation },
            "Wake does not deliver a turn cancelled by sleep");
    f.turns.clear();
    f.rotation.setAutomatic(true);
    settle();
    require(f.turns.isEmpty(),
            "Enabling Auto after wake does not replay a turn cancelled by sleep");
}

void checkLatestReading(Factory &factory)
{
    Fixture f(factory);
    f.backend->sample(QOrientationReading::TopUp);
    f.rotation.start(Qt::PortraitOrientation);
    f.sample(QOrientationReading::TopDown);
    require(f.sensor.reading()->orientation() == QOrientationReading::TopUp,
            "Rejected samples leave sensor cache stale");
    f.rotation.stop(true);
    f.backend->sample(QOrientationReading::TopUp);
    f.turns.clear();
    f.rotation.start(Qt::PortraitOrientation);
    settle();
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::InvertedPortraitOrientation },
            "Wake ignores sleep samples");
    f.sample(QOrientationReading::TopUp);
    require(f.turns.last() == Qt::PortraitOrientation, "Wake follows a fresh awake sample");
    f.sample(QOrientationReading::TopDown);
    f.rotation.stop(true);
    f.backend->sample(QOrientationReading::LeftUp);
    f.turns.clear();
    f.rotation.start(Qt::PortraitOrientation);
    settle();
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::InvertedPortraitOrientation },
            "Wake rejects a fresh sample outside lock");
    f.sample(QOrientationReading::LeftUp);
    f.turns.clear();
    f.rotation.setAutomatic(true);
    settle();
    require(f.turns == QList<Qt::ScreenOrientation>{ Qt::LandscapeOrientation },
            "Auto follows the latest rejected sample");
    f.rotation.setAutomatic(false);
    f.sample(QOrientationReading::RightUp);
    f.sample(QOrientationReading::FaceUp);
    f.turns.clear();
    f.rotation.setAutomatic(true);
    settle();
    require(f.turns.isEmpty(),
            "Flat latest sample cancels an older direction when Auto is enabled");
}

} // namespace

// Checks sensor orientation: locks, opening, sleep and wake, and handoff to Nickel and the PIN screen. Without sensors, the main below checks manual rotation.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Factory factory;
    QSensorManager::registerBackend(QOrientationSensor::sensorType, "kobalt-lock-test", &factory);
    QOrientationSensor sensor;
    sensor.setIdentifier("kobalt-lock-test");
    QList<Qt::ScreenOrientation> turns;
    ReaderOrientation rotation(&sensor, [&](Qt::ScreenOrientation value) { turns.append(value); });
    rotation.configure({});
    require(!rotation.automatic() && rotation.manual() == Qt::PortraitOrientation,
            "Default portrait lock");
    rotation.start(Qt::PortraitOrientation);
    require(factory.backend && sensor.isActive(), "Locks start the sensor");
    auto sample = [&](QOrientationReading::Orientation value) {
        factory.backend->sample(value);
        settle();
    };
    sample(QOrientationReading::LeftUp);
    require(turns.size() == 1, "Portrait lock rejects landscape before screen delivery");
    sample(QOrientationReading::TopDown);
    require(turns.last() == Qt::InvertedPortraitOrientation,
            "Portrait lock permits inverted portrait");
    sample(QOrientationReading::TopDown);
    require(turns.size() == 2, "Repeated readings do not turn again");
    factory.backend->sample(QOrientationReading::TopUp);
    sample(QOrientationReading::FaceUp);
    require(turns.size() == 2, "Flat reading cancels pending turn");
    rotation.configure({ { "orientation", int(Qt::InvertedLandscapeOrientation) } });
    require(rotation.manual() == Qt::LandscapeOrientation, "Legacy landscape migration");
    sample(QOrientationReading::RightUp);
    require(turns.last() == Qt::InvertedLandscapeOrientation,
            "Landscape lock permits inverted landscape");
    const auto count = turns.size();
    sample(QOrientationReading::TopUp);
    require(turns.size() == count, "Landscape lock rejects portrait");
    rotation.configure(
            { { "orientation", int(Qt::PortraitOrientation) }, { "auto_rotate", true } });
    require(turns.size() == count, "Restoring Auto does not force a manual turn");
    rotation.setAutomatic(true);
    sample(QOrientationReading::TopUp);
    require(turns.last() == Qt::PortraitOrientation, "Auto permits portrait");
    rotation.setBlocked(true);
    sample(QOrientationReading::LeftUp);
    require(turns.last() == Qt::PortraitOrientation, "Blocked delivery waits");
    sample(QOrientationReading::FaceDown);
    rotation.setBlocked(false);
    settle();
    require(turns.last() == Qt::PortraitOrientation, "Flat reading clears blocked pending turn");
    rotation.configure(
            { { "orientation", int(Qt::InvertedPortraitOrientation) }, { "auto_rotate", true } });
    require(rotation.automatic() && rotation.manual() == Qt::PortraitOrientation,
            "Saved Auto migration");
    rotation.stop();
    ReaderOrientation manual(nullptr, [](Qt::ScreenOrientation) { });
    manual.configure({ { "orientation", int(Qt::InvertedLandscapeOrientation) } });
    require(manual.manual() == Qt::InvertedLandscapeOrientation,
            "Sensorless manual preference preserved");
    checkSleepRequest(factory);
    checkNativeHandoff(factory);
    checkPinScreenHandoff(factory);
    checkOpening(factory);
    checkSleepLifecycle(factory);
    checkQuarterTurnWake(factory);
    checkLatestReading(factory);
    checkPendingReadingAtSleep(factory);
    std::puts(
            "PASS: sensor locks, opening, sleep/wake/close lifecycle, latest readings and sensor ownership");
}

#else
#  include "platform/api/reader_orientation.h"
#  include <QCoreApplication>
#  include <cstdio>
#  include <cstdlib>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Qt::ScreenOrientation shown = Qt::PrimaryOrientation;
    int ownership = 0;
    kobalt::ReaderOrientation rotation(
            nullptr, [&](Qt::ScreenOrientation value) { shown = value; },
            [&](bool owns) { ownership += owns ? 1 : -1; });
    auto require = [](bool valid, const char *message) {
        if (!valid) {
            std::fprintf(stderr, "%s\n", message);
            std::exit(1);
        }
    };
    require(!rotation.automaticAvailable() && !rotation.setAutomatic(true),
            "Sensorless rotation rejects Auto");
    rotation.configure(
            { { "orientation", int(Qt::InvertedLandscapeOrientation) }, { "auto_rotate", true } });
    require(rotation.manual() == Qt::InvertedLandscapeOrientation && !rotation.automatic(),
            "Sensorless manual preference preserved");
    rotation.start(Qt::PortraitOrientation);
    require(shown == Qt::InvertedLandscapeOrientation && ownership == 1,
            "Opening applies manual orientation and acquires ownership");
    rotation.rotate();
    require(shown == Qt::PortraitOrientation, "Manual rotation wraps to portrait");
    rotation.rotate();
    require(shown == Qt::LandscapeOrientation, "Manual rotation advances to landscape");
    rotation.freeze();
    rotation.rotate();
    require(shown == Qt::LandscapeOrientation, "Frozen rotation keeps the shown orientation");
    require(rotation.manual() == Qt::LandscapeOrientation,
            "Frozen rotation keeps the manual preference");
    rotation.stop(true);
    require(shown == Qt::PortraitOrientation && ownership == 0,
            "Quarter-turn sleep restores native geometry and releases ownership");
    rotation.start(Qt::PortraitOrientation);
    require(shown == Qt::LandscapeOrientation, "Wake restores the reading orientation");
    rotation.configure({ { "orientation", int(Qt::InvertedPortraitOrientation) } });
    require(shown == Qt::InvertedPortraitOrientation,
            "Awake configuration applies the new orientation");
    require(rotation.settings().value("orientation").toInt()
                    == int(Qt::InvertedPortraitOrientation),
            "Settings retain the manual preference");
    rotation.stop();
    require(shown == Qt::PortraitOrientation && ownership == 0,
            "Closing restores native geometry and releases ownership");
}
#endif
