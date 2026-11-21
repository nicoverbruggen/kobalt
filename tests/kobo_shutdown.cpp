#include "platforms/kobo/common/native_lifecycle.h"
#include "platforms/kobo/common/reader_environment.h"
#include "support/font_fixtures.h"
#include "platforms/kobo/common/screensaver_compatibility.h"
#include "storage/profile_database.h"
#include "storage/record_writer.h"
#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QLibrary>
#include <chrono>
#include <dlfcn.h>

extern "C" void kobalt_power_off(void *, bool);
extern "C" void kobalt_reboot(void *);

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
    QVERIFY(!kobalt::NativeLifecycle::nativeSessionSaveAllowed());
    QVERIFY(!kobalt::NativeLifecycle::nativeProgressSaveAllowed());
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

// Checks direct Nickel handoff, the storage gate and the Screensaver chain.
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

    void stockRemountAfterBootDelivery()
    {
        using namespace kobalt;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto faces =
                test::copyOpeningFontFixture(QStringLiteral(KOBALT_TEST_ASSET_ROOT), root.path());
        QVERIFY(!faces.isEmpty());
        nickel::ReaderEnvironment environment(root.path());
        QList<bool> results;
        const auto start = [&] {
            environment.verifyAssets(
                    this, "boot", [&](bool valid) { results.append(valid); },
                    NativeLifecycle::stockStorageGeneration);
        };
        start();
        QTRY_COMPARE(results, QList<bool>{ true });
        QVERIFY(environment.assetsAvailable());
        const auto boot = NativeLifecycle::stockStorageGeneration();
        QVERIFY(environment.verificationForStorage(boot));
        auto *usb = PlugWorkflowManager::sharedInstance();
        QVERIFY(!NativeLifecycle::interceptUsb(usb));
        const bool availableOnUsb = environment.assetsAvailable();
        const bool removed = QFile::remove(root.filePath("fonts/" + faces.first()));
        emit usb->aboutToUnplug();
        emit usb->doneProcessing();
        QVERIFY(removed);
        QVERIFY(!availableOnUsb);
        const auto mounted = NativeLifecycle::stockStorageGeneration();
        QVERIFY(mounted && mounted != boot);
        QVERIFY(!environment.verificationForStorage(mounted));
        QVERIFY(!environment.assetsAvailable());
        start();
        QTRY_COMPARE(results.size(), 2);
        QVERIFY(!results.last());
        QVERIFY(!environment.assetsAvailable());
    }

    void stockUsbWaitsForAssetVerification()
    {
        using namespace kobalt;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto faces =
                test::copyOpeningFontFixture(QStringLiteral(KOBALT_TEST_ASSET_ROOT), root.path());
        QVERIFY(!faces.isEmpty());
        const auto face = root.filePath("fonts/" + faces.first());
        QVERIFY(QFile::rename(face, root.filePath("fonts/FSCK0001.000")));
        nickel::ReaderEnvironment environment(root.path());
        QSemaphore insideVerification, returned;
        bool handedOffDuringVerification = false;
        int checks = 0;
        const auto permit = [&] {
            if (QThread::currentThread() != thread() && ++checks == 2) {
                // verifyFiles is at its first file. Stay inside verify until the
                // stock hook revokes storage, then see whether it returns early.
                insideVerification.release();
                while (NativeLifecycle::stockStorageGeneration())
                    QThread::yieldCurrentThread();
                handedOffDuringVerification = returned.tryAcquire(1, 100);
            }
            return NativeLifecycle::stockStorageGeneration();
        };
        QList<bool> results;
        environment.verifyAssets(this, "boot", [&](bool valid) { results.append(valid); }, permit);
        QVERIFY(insideVerification.tryAcquire(1, 5000));
        auto *usb = PlugWorkflowManager::sharedInstance();
        const bool stock = !NativeLifecycle::interceptUsb(usb);
        returned.release();
        QTRY_COMPARE(results.size(), 1);
        const bool blocked = !environment.assetsAvailable();
        emit usb->aboutToUnplug();
        emit usb->doneProcessing();
        QVERIFY(stock);
        QVERIFY(!handedOffDuringVerification);
        // The check stopped at the next file, before it restored the recovered
        // face. The verification after remount restores it.
        QVERIFY(!QFile::exists(face));
        QVERIFY(QFile::exists(root.filePath("fonts/FSCK0001.000")));
        QVERIFY(!results.first());
        QVERIFY(blocked);
        environment.verifyAssets(this, "usb", [&](bool valid) { results.append(valid); }, permit);
        QTRY_COMPARE(results.size(), 2);
        QVERIFY(results.last());
        QVERIFY(QFile::exists(face));
        QVERIFY(!QFile::exists(root.filePath("fonts/FSCK0001.000")));
        QVERIFY(environment.assetsAvailable());
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

    void rebootPassesThroughWithoutWaitingForReader()
    {
        QTemporaryDir profile;
        profilePath = profile.path();
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        QSemaphore started, release;
        std::atomic_bool timedOut{ false };
        kobalt::storage::RecordWriter writer;
        QVERIFY(writer.tryPost([&] {
            started.release();
            timedOut = !release.tryAcquire(1, 5000);
            return true;
        }));
        QVERIFY(started.tryAcquire(1, 5000));
        kobalt_reboot(N3PowerWorkflowManager::sharedInstance());
        release.release();
        QVERIFY(writer.flush(5000));
        QVERIFY(!timedOut);
        QCOMPARE(reboots, 1);
        QVERIFY(!kobalt::storage::profileTransaction(profilePath,
                                                     [](QSqlDatabase &) { return true; }));
    }

    void powerOffDoesNotWaitForWriter_data()
    {
        QTest::addColumn<bool>("low");
        QTest::newRow("manual") << false;
        QTest::newRow("battery-low") << true;
    }

    void powerOffDoesNotWaitForWriter()
    {
        QFETCH(bool, low);
        QTemporaryDir profile;
        profilePath = profile.path();
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        int screens = 0;
        lifecycle.preparePowerScreen = [&](kobalt::NativeLifecycle::Shutdown mode) {
            QCOMPARE(mode == kobalt::NativeLifecycle::Shutdown::BatteryLow, low);
            ++screens;
        };
        QSemaphore started, release;
        std::atomic_bool timedOut{ false };
        kobalt::storage::RecordWriter writer;
        QVERIFY(writer.tryPost([&] {
            started.release();
            timedOut = !release.tryAcquire(1, 5000);
            return true;
        }));
        QVERIFY(started.tryAcquire(1, 5000));
        // The native double must not submit its own profile transaction while
        // that worker is held. Check the local profile gate after releasing it.
        kobalt::nativePowerOff = [](void *, bool critical) {
            QVERIFY(!kobalt::NativeLifecycle::nativeSessionSaveAllowed());
            ++powerOffs;
            batteryLow = critical;
        };
        auto *power = N3PowerWorkflowManager::sharedInstance();
        kobalt_power_off(power, low);
        const int immediateCalls = powerOffs;
        const bool workStillActive = !writer.idle();
        release.release();
        QVERIFY(writer.flush(5000));
        QVERIFY(!timedOut);
        QCOMPARE(immediateCalls, 1);
        QVERIFY(workStillActive);
        QCOMPARE(screens, 1);
        QCOMPARE(batteryLow, low);
        kobalt_power_off(power, !low);
        kobalt::NativeLifecycle::disconnected();
        QCOMPARE(powerOffs, 1);
        QVERIFY(!kobalt::storage::profileTransaction(profilePath,
                                                      [](QSqlDatabase &) { return true; }));
    }

    void sleepingShutdown_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("screen");
        for (int mode = 0; mode < 2; ++mode)
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
        int prepared = 0;
        lifecycle.preparePowerScreen = [&](kobalt::NativeLifecycle::Shutdown action) {
            QCOMPARE(action, kobalt::NativeLifecycle::Shutdown(mode));
            ++prepared;
        };
        kobalt_power_off(power, mode == 1);
        QCOMPARE(prepared, 1);
        QCOMPARE(powerOffs, 1);
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
        QObject manager;
        QVERIFY(kobalt::NativeLifecycle::interceptUsb(&manager));
        QCOMPARE(usbHandoffs, 1);
        QVERIFY(!kobalt::NativeLifecycle::nativeProgressSaveAllowed());
        QVERIFY(!kobalt::NativeLifecycle::interceptShutdown(
                &manager, kobalt::NativeLifecycle::Shutdown::PowerOff));
        QVERIFY(!kobalt::NativeLifecycle::nativeProgressSaveAllowed());
    }

    void screensaverChain()
    {
        // Load once for all four scenarios so ASan registers the fixture globals once.
        QLibrary screensaver(QStringLiteral(KOBALT_SCREENSAVER_FIXTURE));
        QVERIFY2(screensaver.load(), qPrintable(screensaver.errorString()));
        for (bool screensaverLast : { false, true }) {
            for (bool low : { false, true }) {
                init();
                checkScreensaverChain(screensaver, screensaverLast, low);
                cleanup();
            }
        }
        kobalt::nativePowerOff = powerOff;
        QVERIFY(screensaver.unload());
    }

private:
    void checkScreensaverChain(QLibrary &screensaver, bool screensaverLast, bool low)
    {
        using PowerOff = void (*)(void *, bool);
        const auto hook = reinterpret_cast<PowerOff>(
                screensaver.resolve("hook_N3PowerWorkflowManager_powerOff"));
        const auto reset =
                reinterpret_cast<void (*)(PowerOff)>(screensaver.resolve("fixtureReset"));
        const auto calls = reinterpret_cast<int (*)()>(screensaver.resolve("fixtureCalls"));
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
            const auto entry = screensaverLast ? hook : kobalt_power_off;
            entry(N3PowerWorkflowManager::sharedInstance(), low);
            QCOMPARE(powerOffs, 1);
            QCOMPARE(calls(), 1);
            QCOMPARE(batteryLow, low);
            QVERIFY(profileClosedAtHandoff);
        }
        // An unknown third-party continuation must not be overwritten.
        reset(powerOff);
        kobalt::nativePowerOff = stock;
        QVERIFY(!kobalt::connectScreensaverPowerOff());
        {
            kobalt::NativeLifecycle unsupported(nullptr);
            QVERIFY(!unsupported.connected());
        }
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ShutdownTests tests;
    QTimer::singleShot(0, &app, [&] { app.exit(QTest::qExec(&tests, argc, argv)); });
    return app.exec();
}

#include "kobo_shutdown.moc"
