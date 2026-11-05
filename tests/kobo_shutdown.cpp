#include "platforms/kobo/common/native_lifecycle.h"
#include "platforms/kobo/common/screensaver_compatibility.h"
#include "storage/profile_database.h"
#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QLibrary>
#include <chrono>
#include <dlfcn.h>

extern "C" void kobalt_power_off(void *, bool);

// Export the native entry points and signals used by the real lifecycle adapter.
class N3PowerWorkflowManager : public QObject
{
    Q_OBJECT
public:
    static N3PowerWorkflowManager *sharedInstance();
    bool sleeping() const;
    bool sleepViewVisible() const;
    bool pinCodeInputDialogVisible() const;
    void powerOff(bool low);
    bool asleep = false, sleepVisible = false, pinVisible = false;
signals:
    void aboutToSleep();
    void resumingFromSleep();
    void resumedFromSleep();
};

N3PowerWorkflowManager *N3PowerWorkflowManager::sharedInstance()
{
    static N3PowerWorkflowManager manager;
    return &manager;
}

bool N3PowerWorkflowManager::sleeping() const
{
    return asleep;
}

bool N3PowerWorkflowManager::sleepViewVisible() const
{
    return sleepVisible;
}

bool N3PowerWorkflowManager::pinCodeInputDialogVisible() const
{
    return pinVisible;
}

class PlugWorkflowManager : public QObject
{
    Q_OBJECT
public:
    static PlugWorkflowManager *sharedInstance();
signals:
    void doneProcessing();
    void aboutToUnplug();
};

PlugWorkflowManager *PlugWorkflowManager::sharedInstance()
{
    static PlugWorkflowManager manager;
    return &manager;
}

namespace {
int powerOffs = 0, reboots = 0, usbHandoffs = 0;
bool batteryLow = false;
QString profilePath;
bool profileClosedAtHandoff = false;

void ignore(void *) { }

void usbConnect(void *)
{
    ++usbHandoffs;
}

bool canSuspend(void *)
{
    return true;
}

void recordHandoff()
{
    profileClosedAtHandoff =
            !kobalt::storage::profileTransaction(profilePath, [](QSqlDatabase &) { return true; });
    QVERIFY2(!kobalt::NativeLifecycle::nativeSessionSaveAllowed(),
             "Shutdown allowed late native session writes");
    QVERIFY2(!kobalt::NativeLifecycle::nativeProgressSaveAllowed(),
             "Shutdown allowed late native progress writes");
}

void powerOff(void *, bool low)
{
    recordHandoff();
    ++powerOffs;
    batteryLow = low;
}

void reboot(void *)
{
    recordHandoff();
    ++reboots;
}

// The single-shot timers that can still fire in this thread: active
// single-shot QTimers under the lifecycle, the lifecycle's own timers, and
// QTimer::singleShot calls, which Qt keeps under the event dispatcher. A
// shutdown deadline would be one of them. The lifecycle's repeating test
// command poll is not a deadline.
QStringList pendingSingleShots(QObject &lifecycle)
{
    auto *dispatcher = QAbstractEventDispatcher::instance();
    QStringList pending;
    const auto add = [&](QObject *object) {
        for (const auto &timer : dispatcher->timersForObject(object))
            pending.append(QString("%1 (%2 ms)")
                                   .arg(QString::fromLatin1(object->metaObject()->className()))
                                   .arg(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                timer.interval)
                                                .count()));
    };
    add(&lifecycle);
    for (auto *timer : lifecycle.findChildren<QTimer *>())
        if (timer->isSingleShot())
            add(timer);
    for (auto *object : dispatcher->children())
        add(object);
    return pending;
}

// Runs the events that are already queued, and the deferred deletes they
// cause, without waiting for any timer.
void processQueuedEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
} // namespace

void N3PowerWorkflowManager::powerOff(bool low)
{
    ::powerOff(this, low);
}

// Checks that power-off, battery-low power-off and reboot wait for saves without a deadline, close the profile first and chain with Screensaver.
class ShutdownTests : public QObject
{
    Q_OBJECT
private slots:

    void init()
    {
        powerOffs = reboots = usbHandoffs = 0;
        batteryLow = false;
        profileClosedAtHandoff = false;
        auto *power = N3PowerWorkflowManager::sharedInstance();
        power->asleep = power->sleepVisible = power->pinVisible = false;
        kobalt::nativeShowSleepView = kobalt::nativeHideSleepView = ignore;
        kobalt::nativeUsbConnect = kobalt::nativeUsbWorkflow = kobalt::nativeAboutUsbConnect =
                ignore;
        kobalt::nativeCanSuspend = canSuspend;
        kobalt::nativePowerOff = powerOff;
        kobalt::nativeReboot = reboot;
        kobalt::storage::reopenProfile();
    }

    void cleanup()
    {
        kobalt::storage::reopenProfile();
        kobalt::storage::closeProfile();
    }

    void timerCheckSeesDeadlines()
    {
        // The held-shutdown checks rely on this: a deadline started as a
        // single-shot call or as a lifecycle timer is listed.
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        processQueuedEvents();
        QVERIFY2(pendingSingleShots(lifecycle).isEmpty(),
                 qPrintable(pendingSingleShots(lifecycle).join(", ")));
        // A single-shot call stays registered until it fires, even when its
        // context is gone. Use a short one so that it cannot outlive this case.
        bool fired = false;
        QTimer::singleShot(1, &lifecycle, [&] { fired = true; });
        QCOMPARE(pendingSingleShots(lifecycle).size(), 1);
        QTRY_VERIFY(fired);
        processQueuedEvents();
        QVERIFY(pendingSingleShots(lifecycle).isEmpty());
        QTimer deadline(&lifecycle);
        deadline.setSingleShot(true);
        deadline.start(3000);
        QCOMPARE(pendingSingleShots(lifecycle).size(), 1);
    }

    void completion_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("stalled");
        for (int mode = 0; mode < 3; ++mode) {
            const auto name = QByteArray::number(mode);
            QTest::newRow((name + "-drained").constData()) << mode << false;
            QTest::newRow((name + "-stalled").constData()) << mode << true;
        }
    }

    void completion()
    {
        QFETCH(int, mode);
        QFETCH(bool, stalled);
        QTemporaryDir profile;
        profilePath = profile.path();
        const auto available = [&] {
            return kobalt::storage::profileTransaction(profile.path(),
                                                       [](QSqlDatabase &) { return true; });
        };
        QVERIFY(available());
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        QVERIFY(kobalt::NativeLifecycle::nativeProgressSaveAllowed());
        std::function<void()> drained;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) {
            drained = std::move(ready);
        };
        QObject manager;
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(
                &manager, kobalt::NativeLifecycle::Shutdown(mode)));
        QVERIFY(drained);
        QCOMPARE(powerOffs + reboots, 0);
        QVERIFY(kobalt::NativeLifecycle::nativeSessionSaveAllowed());
        if (stalled) {
            // No deadline forces the shutdown: required saves must retain
            // storage for every shutdown mode. Check that no timer is left
            // that could end the wait, instead of waiting past a deadline.
            processQueuedEvents();
            QVERIFY2(pendingSingleShots(lifecycle).isEmpty(),
                     qPrintable("A timer could end the held shutdown: "
                                + pendingSingleShots(lifecycle).join(", ")));
            QCOMPARE(powerOffs + reboots, 0);
            QVERIFY2(available(), "Pending shutdown closed the profile before saves completed");
            QVERIFY(kobalt::NativeLifecycle::nativeSessionSaveAllowed());
            QVERIFY2(!kobalt::NativeLifecycle::canSuspend(),
                     "Pending shutdown allowed CPU suspension");
            QVERIFY(kobalt::NativeLifecycle::interceptShutdown(
                    &manager, kobalt::NativeLifecycle::Shutdown(mode)));
            QCOMPARE(powerOffs + reboots, 0);
        }
        drained();
        QCOMPARE(reboots, mode == 2 ? 1 : 0);
        QCOMPARE(powerOffs, mode == 2 ? 0 : 1);
        QCOMPARE(batteryLow, mode == 1);
        QVERIFY2(profileClosedAtHandoff, "The native action ran before the profile was blocked");
        QVERIFY2(!available(), "Power-off left the shared profile open or writable");
        kobalt::NativeLifecycle::disconnected();
        QVERIFY2(!available(), "USB disconnect reopened the profile during power-off");
        drained();
        drained();
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(
                &manager, kobalt::NativeLifecycle::Shutdown(mode)));
        QCOMPARE(powerOffs + reboots, 1);
    }

    void sleepingShutdown_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("screen");
        for (int mode = 0; mode < 3; ++mode)
            for (int screen = 0; screen < 3; ++screen)
                QTest::newRow(
                        (QByteArray::number(mode) + "-" + QByteArray::number(screen)).constData())
                        << mode << screen;
    }

    void sleepingShutdown()
    {
        QFETCH(int, mode);
        QFETCH(int, screen);
        QTemporaryDir profile;
        profilePath = profile.path();
        QVERIFY(kobalt::storage::profileTransaction(profilePath,
                                                    [](QSqlDatabase &) { return true; }));
        auto *power = N3PowerWorkflowManager::sharedInstance();
        power->asleep = screen == 0;
        power->sleepVisible = screen == 1;
        power->pinVisible = screen == 2;
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        QVERIFY(kobalt::NativeLifecycle::suspended());
        QVERIFY(!kobalt::NativeLifecycle::nativeProgressSaveAllowed());
        std::function<void()> drained;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) {
            QVERIFY2(
                    kobalt::NativeLifecycle::nativeProgressSaveAllowed(),
                    "A held shutdown could not write progress behind the native sleep or PIN screen");
            drained = std::move(ready);
        };
        QObject manager;
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(
                &manager, kobalt::NativeLifecycle::Shutdown(mode)));
        QVERIFY(drained);
        QVERIFY(kobalt::NativeLifecycle::nativeProgressSaveAllowed());
        QCOMPARE(powerOffs + reboots, 0);
        drained();
        QCOMPARE(powerOffs + reboots, 1);
        QVERIFY(profileClosedAtHandoff);
        QVERIFY(!kobalt::NativeLifecycle::nativeProgressSaveAllowed());
        power->asleep = power->sleepVisible = power->pinVisible = false;
        QVERIFY2(!kobalt::NativeLifecycle::nativeProgressSaveAllowed(),
                 "An awake screen bypassed the final storage gate");
    }

    void exportedStorageStaysBlocked()
    {
        QTemporaryDir profile;
        profilePath = profile.path();
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        kobalt::nativeUsbConnect = usbConnect;
        lifecycle.prepareUsb = [](std::function<void()> ready) { ready(); };
        lifecycle.prepareShutdown = [](std::function<void()>) {
            QFAIL("Shutdown tried to reclaim exported storage");
        };
        QObject manager;
        QVERIFY(kobalt::NativeLifecycle::interceptUsb(&manager));
        QCOMPARE(usbHandoffs, 1);
        QVERIFY(!kobalt::NativeLifecycle::nativeProgressSaveAllowed());
        QVERIFY(!kobalt::NativeLifecycle::interceptShutdown(
                &manager, kobalt::NativeLifecycle::Shutdown::PowerOff));
        QVERIFY(!kobalt::NativeLifecycle::nativeProgressSaveAllowed());
    }

    void destroyedLifecycleIgnoresCompletion()
    {
        std::function<void()> drained;
        QObject manager;
        {
            kobalt::NativeLifecycle lifecycle(nullptr);
            lifecycle.prepareShutdown = [&](std::function<void()> ready) {
                drained = std::move(ready);
            };
            QVERIFY(kobalt::NativeLifecycle::interceptShutdown(
                    &manager, kobalt::NativeLifecycle::Shutdown::PowerOff));
        }
        drained();
        QCOMPARE(powerOffs + reboots, 0);
    }

    void destroyedManagerIgnoresCompletion()
    {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        std::function<void()> drained;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) {
            drained = std::move(ready);
        };
        auto *manager = new QObject;
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(
                manager, kobalt::NativeLifecycle::Shutdown::PowerOff));
        delete manager;
        drained();
        QCOMPARE(powerOffs + reboots, 0);
    }

    void screensaverChain_data()
    {
        QTest::addColumn<bool>("screensaverLast");
        QTest::addColumn<bool>("low");
        for (bool last : { false, true })
            for (bool low : { false, true })
                QTest::newRow(
                        (QByteArray::number(last) + "-" + QByteArray::number(low)).constData())
                        << last << low;
    }

    void screensaverChain()
    {
        QFETCH(bool, screensaverLast);
        QFETCH(bool, low);
        using PowerOff = void (*)(void *, bool);
        QLibrary addon(QStringLiteral(KOBALT_SCREENSAVER_FIXTURE));
        QVERIFY2(addon.load(), qPrintable(addon.errorString()));
        const auto hook =
                reinterpret_cast<PowerOff>(addon.resolve("hook_N3PowerWorkflowManager_powerOff"));
        const auto reset = reinterpret_cast<void (*)(PowerOff)>(addon.resolve("fixtureReset"));
        const auto calls = reinterpret_cast<int (*)()>(addon.resolve("fixtureCalls"));
        const auto stock = reinterpret_cast<PowerOff>(
                dlsym(RTLD_DEFAULT, "_ZN22N3PowerWorkflowManager8powerOffEb"));
        QVERIFY(hook && reset && calls && stock);
        reset(stock);
        kobalt::nativePowerOff = screensaverLast ? stock : hook;
        QTemporaryDir profile;
        profilePath = profile.path();
        QVERIFY(kobalt::storage::profileTransaction(profilePath,
                                                    [](QSqlDatabase &) { return true; }));
        {
            kobalt::NativeLifecycle lifecycle(nullptr);
            QVERIFY(lifecycle.connected());
            // Repeated discovery must not turn either load order into a cycle.
            QVERIFY(kobalt::connectScreensaverPowerOff());
            std::function<void()> drained;
            lifecycle.prepareShutdown = [&](std::function<void()> ready) {
                drained = std::move(ready);
            };
            const auto entry = screensaverLast ? hook : kobalt_power_off;
            entry(N3PowerWorkflowManager::sharedInstance(), low);
            QVERIFY(drained);
            QCOMPARE(powerOffs, 0);
            QVERIFY(kobalt::NativeLifecycle::nativeSessionSaveAllowed());
            // A slow or failed save must not be bypassed by the other mod,
            // and no timer may end the held shutdown.
            processQueuedEvents();
            QVERIFY2(pendingSingleShots(lifecycle).isEmpty(),
                     qPrintable("A timer could end the held shutdown: "
                                + pendingSingleShots(lifecycle).join(", ")));
            QCOMPARE(powerOffs, 0);
            QVERIFY(kobalt::storage::profileTransaction(profilePath,
                                                        [](QSqlDatabase &) { return true; }));
            QVERIFY(kobalt::NativeLifecycle::nativeProgressSaveAllowed());
            drained();
            QCOMPARE(powerOffs, 1);
            QCOMPARE(calls(), 1);
            QCOMPARE(batteryLow, low);
            QVERIFY(profileClosedAtHandoff);
            drained();
            QCOMPARE(powerOffs, 1);
            QCOMPARE(calls(), 1);
        }
        // An unknown third-party continuation must not be overwritten.
        reset(powerOff);
        kobalt::nativePowerOff = stock;
        QVERIFY(!kobalt::connectScreensaverPowerOff());
        {
            kobalt::NativeLifecycle unsupported(nullptr);
            QVERIFY(!unsupported.connected());
        }
        kobalt::nativePowerOff = powerOff;
        QVERIFY(addon.unload());
    }
};
QTEST_GUILESS_MAIN(ShutdownTests)
#include "kobo_shutdown.moc"
