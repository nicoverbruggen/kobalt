#include "platform/nickel/common/native_lifecycle.h"
#include "engine/crengine/engine_activity.h"
#include <QCoreApplication>
#include <QTest>

// These host objects expose the symbol names and Qt signals used by the
// adapter. They test Kobalt's lifecycle decisions, not Nickel's private ABI.
class N3PowerWorkflowManager : public QObject {
    Q_OBJECT
public:
    static N3PowerWorkflowManager *sharedInstance();
    bool sleeping() const;
    bool sleepViewVisible() const;
    bool pinCodeInputDialogVisible() const;
    bool asleep = false, sleepVisible = false, pinVisible = false;
signals:
    void aboutToSleep();
    void resumingFromSleep();
    void resumedFromSleep();
};

class PlugManager : public QObject {
    Q_OBJECT
public:
    static PlugManager *sharedInstance();
signals:
    void plugged();
};
PlugManager *PlugManager::sharedInstance() {
    static PlugManager manager;
    return &manager;
}

class PlugWorkflowManager : public QObject {
    Q_OBJECT
public:
    static PlugWorkflowManager *sharedInstance();
    bool pluggedInViewVisible();
    bool visible = false;
signals:
    void doneProcessing();
    void aboutToUnplug();
};

N3PowerWorkflowManager *N3PowerWorkflowManager::sharedInstance() {
    static N3PowerWorkflowManager manager;
    return &manager;
}
bool N3PowerWorkflowManager::sleeping() const {
    return asleep;
}
bool N3PowerWorkflowManager::sleepViewVisible() const {
    return sleepVisible;
}
bool N3PowerWorkflowManager::pinCodeInputDialogVisible() const {
    return pinVisible;
}
PlugWorkflowManager *PlugWorkflowManager::sharedInstance() {
    static PlugWorkflowManager manager;
    return &manager;
}
bool PlugWorkflowManager::pluggedInViewVisible() {
    return visible;
}

extern "C" void kobalt_hide_sleep_view(void *);
extern "C" void kobalt_show_sleep_view(void *);
extern "C" void kobalt_reboot(void *);
extern "C" void kobalt_power_off(void *, bool);
extern "C" void kobalt_usb_workflow(void *);
extern "C" bool kobalt_can_suspend(void *);

namespace {
int reboots = 0, powerOffs = 0, usbWorkflows = 0;
bool lastBatteryLow = false, reenterReboot = false, acceptWake = true, nativeSuspendAllowed = true;
void reboot(void *manager) {
    ++reboots;
    if (reenterReboot)
        kobalt_reboot(manager);
}
void powerOff(void *, bool batteryLow) {
    ++powerOffs;
    lastBatteryLow = batteryLow;
}
void hideSleep(void *) {
    if (acceptWake)
        N3PowerWorkflowManager::sharedInstance()->sleepVisible = false;
}
void showSleep(void *) {
    N3PowerWorkflowManager::sharedInstance()->sleepVisible = true;
}
void noOp(void *) {}
void usbWorkflow(void *) {
    ++usbWorkflows;
}
bool canSuspend(void *) {
    return nativeSuspendAllowed;
}
}

class NativeLifecycleTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        auto *power = N3PowerWorkflowManager::sharedInstance();
        power->asleep = power->sleepVisible = power->pinVisible = false;
        PlugWorkflowManager::sharedInstance()->visible = false;
        reboots = powerOffs = usbWorkflows = 0;
        lastBatteryLow = reenterReboot = false;
        acceptWake = nativeSuspendAllowed = true;
        kobalt::engineActivity().pause(false);
        kobalt::nativeShowSleepView = showSleep;
        kobalt::nativeHideSleepView = hideSleep;
        kobalt::nativeCanSuspend = canSuspend;
        kobalt::nativeUsbConnect = noOp;
        kobalt::nativeUsbWorkflow = usbWorkflow;
        kobalt::nativeAboutUsbConnect = noOp;
        kobalt::nativePowerOff = powerOff;
        kobalt::nativeReboot = reboot;
    }

    void cleanup() {
        kobalt::engineActivity().pause(false);
    }

    void usbPromptObservesTheNativeSignalAfterItsSlotsFinish() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        bool prompt = false;
        lifecycle.usbPrompt = [&] {
            prompt = true;
        };
        emit PlugManager::sharedInstance() -> plugged();
        QVERIFY(lifecycle.usbPromptExpected());
        QVERIFY(!prompt);
        QTRY_VERIFY(prompt);
    }

    void usbWorkflowWaitsForSaveAndRelease() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        auto *manager = PlugWorkflowManager::sharedInstance();
        manager->visible = true;
        std::function<void()> finish;
        int preparations = 0;
        lifecycle.prepareUsb = [&](std::function<void()> ready) {
            ++preparations;
            finish = std::move(ready);
            QVERIFY(kobalt::NativeLifecycle::usbSaveAllowed());
            QVERIFY(kobalt::NativeLifecycle::suspended());
        };
        kobalt_usb_workflow(manager);
        kobalt_usb_workflow(manager);
        QCOMPARE(preparations, 1);
        QCOMPARE(usbWorkflows, 0);
        QVERIFY(!kobalt::NativeLifecycle::canSuspend());
        N3PowerWorkflowManager::sharedInstance()->pinVisible = true;
        QVERIFY(!kobalt::NativeLifecycle::usbSaveAllowed());
        N3PowerWorkflowManager::sharedInstance()->pinVisible = false;
        finish();
        QCOMPARE(usbWorkflows, 1);
        QVERIFY(!kobalt::NativeLifecycle::usbSaveAllowed());
        finish();
        QCOMPARE(usbWorkflows, 1);
    }
    void cancelledUsbDoesNotRunLateWorkflow() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        std::function<void()> finish;
        lifecycle.prepareUsb = [&](std::function<void()> ready) {
            finish = std::move(ready);
        };
        kobalt_usb_workflow(PlugWorkflowManager::sharedInstance());
        kobalt::NativeLifecycle::disconnected();
        finish();
        QCOMPARE(usbWorkflows, 0);
        QVERIFY(!kobalt::NativeLifecycle::usbSaveAllowed());
    }
    void shutdownCancelsHeldUsbWorkflow() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        std::function<void()> finish;
        lifecycle.prepareUsb = [&](std::function<void()> ready) {
            finish = std::move(ready);
        };
        lifecycle.prepareShutdown = [](std::function<void()> ready) {
            ready();
        };
        kobalt_usb_workflow(PlugWorkflowManager::sharedInstance());
        kobalt_reboot(N3PowerWorkflowManager::sharedInstance());
        finish();
        QCOMPARE(reboots, 1);
        QCOMPARE(usbWorkflows, 0);
        QVERIFY(!kobalt::NativeLifecycle::usbSaveAllowed());
    }
    void missingTransitionHookLeavesLifecycleDisconnected_data() {
        QTest::addColumn<int>("hook");
        QTest::newRow("show-sleep") << 0;
        QTest::newRow("hide-sleep") << 1;
        QTest::newRow("can-suspend") << 2;
        QTest::newRow("usb-connect") << 3;
        QTest::newRow("about-usb-connect") << 4;
        QTest::newRow("power-off") << 5;
        QTest::newRow("reboot") << 6;
        QTest::newRow("usb-workflow") << 7;
    }

    void missingTransitionHookLeavesLifecycleDisconnected() {
        QFETCH(int, hook);
        switch (hook) {
        case 7:
            kobalt::nativeUsbWorkflow = nullptr;
            break;
        case 0:
            kobalt::nativeShowSleepView = nullptr;
            break;
        case 1:
            kobalt::nativeHideSleepView = nullptr;
            break;
        case 2:
            kobalt::nativeCanSuspend = nullptr;
            break;
        case 3:
            kobalt::nativeUsbConnect = nullptr;
            break;
        case 4:
            kobalt::nativeAboutUsbConnect = nullptr;
            break;
        case 5:
            kobalt::nativePowerOff = nullptr;
            break;
        case 6:
            kobalt::nativeReboot = nullptr;
            break;
        }
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(!lifecycle.connected());
    }

    void delayedUnlockResumesWithoutAnotherPowerSignal_data() {
        QTest::addColumn<bool>("usbConnected");
        QTest::addColumn<bool>("earlyResume");
        QTest::newRow("sleep-overlay") << false << false;
        QTest::newRow("sleep-overlay-before-frontlight") << false << true;
        QTest::newRow("usb-remount-after-sleep") << true << false;
        QTest::newRow("usb-remount-before-frontlight") << true << true;
    }
    void delayedUnlockResumesWithoutAnotherPowerSignal() {
        QFETCH(bool, usbConnected);
        QFETCH(bool, earlyResume);
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        auto *usb = PlugWorkflowManager::sharedInstance();
        int wakes = 0;
        lifecycle.wake = [&] {
            ++wakes;
        };
        if (usbConnected) {
            lifecycle.prepareUsb = [](std::function<void()> ready) {
                ready();
            };
            QVERIFY(kobalt::NativeLifecycle::interceptUsb(usb));
            QVERIFY(lifecycle.storageBlocked());
        }
        power->asleep = power->sleepVisible = true;
        emit power->aboutToSleep();
        power->asleep = false;
        if (earlyResume) {
            emit power->resumingFromSleep();
        } else {
            emit power->resumedFromSleep();
        }
        QTest::qWait(20);
        QCOMPARE(wakes, 0);
        QVERIFY(lifecycle.blocked());
        if (usbConnected) {
            emit usb->aboutToUnplug();
            emit usb->doneProcessing();
            QVERIFY(!lifecycle.storageBlocked());
        }
        // The native overlay closes after the resume signal has returned.
        power->sleepVisible = false;
        QTRY_COMPARE_WITH_TIMEOUT(wakes, 1, 300);
        QVERIFY(!lifecycle.blocked());
        QVERIFY(!kobalt::NativeLifecycle::suspended());
        emit power->resumedFromSleep();
        QTest::qWait(50);
        QCOMPARE(wakes, 1);
    }

    void wakeWaitsForUsbViewToCloseAfterRemount() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        auto *usb = PlugWorkflowManager::sharedInstance();
        int wakes = 0;
        lifecycle.wake = [&] {
            ++wakes;
        };
        lifecycle.prepareUsb = [](std::function<void()> ready) {
            ready();
        };
        usb->visible = true;
        QVERIFY(kobalt::NativeLifecycle::interceptUsb(usb));
        power->asleep = true;
        emit power->aboutToSleep();
        power->asleep = false;
        emit power->resumingFromSleep();
        QTest::qWait(20);
        emit usb->aboutToUnplug();
        emit usb->doneProcessing();
        QTest::qWait(20);
        QVERIFY(kobalt::NativeLifecycle::suspended());
        QCOMPARE(wakes, 0);
        usb->visible = false;
        QTRY_COMPARE_WITH_TIMEOUT(wakes, 1, 300);
        QVERIFY(!lifecycle.blocked());
    }

    void sleepAndPinPreventPrematureResume() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        int sleeps = 0, wakes = 0;
        lifecycle.sleep = [&] {
            ++sleeps;
            QVERIFY(kobalt::NativeLifecycle::sleepSaveAllowed());
        };
        lifecycle.wake = [&] {
            ++wakes;
        };
        power->asleep = true;
        emit power->aboutToSleep();
        QCOMPARE(sleeps, 1);
        QVERIFY(!kobalt::NativeLifecycle::sleepSaveAllowed());
        QVERIFY(lifecycle.blocked());
        power->asleep = false;
        power->pinVisible = true;
        emit power->resumingFromSleep();
        QCoreApplication::processEvents();
        QCOMPARE(wakes, 0);
        QVERIFY(lifecycle.blocked());
        QVERIFY(kobalt::NativeLifecycle::suspended());
        power->pinVisible = false;
        QTRY_COMPARE(wakes, 1);
        QVERIFY(!lifecycle.blocked());
        QVERIFY(!kobalt::NativeLifecycle::suspended());
        emit power->resumedFromSleep();
        QTest::qWait(50);
        QCOMPARE(wakes, 1);
    }

    void anotherSleepCancelsPendingWake() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        int wakes = 0;
        lifecycle.wake = [&] {
            ++wakes;
        };
        power->asleep = power->sleepVisible = true;
        emit power->aboutToSleep();
        power->asleep = false;
        emit power->resumingFromSleep();
        QTest::qWait(20);
        power->asleep = true;
        emit power->aboutToSleep();
        power->sleepVisible = false;
        // The earlier frontlight animation can finish during the next sleep.
        emit power->resumedFromSleep();
        QTest::qWait(80);
        QCOMPARE(wakes, 0);
        QVERIFY(lifecycle.blocked());
        power->asleep = false;
        emit power->resumingFromSleep();
        QTRY_COMPARE(wakes, 1);
        emit power->resumingFromSleep();
        emit power->resumedFromSleep();
        QTest::qWait(50);
        QCOMPARE(wakes, 1);
    }

    void usbCompletionRestoresAwakeReader_data() {
        QTest::addColumn<bool>("cancelled");
        QTest::newRow("cancelled") << true;
        QTest::newRow("remounted") << false;
    }

    void usbCompletionRestoresAwakeReader() {
        QFETCH(bool, cancelled);
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *usb = PlugWorkflowManager::sharedInstance();
        int wakes = 0;
        lifecycle.wake = [&] {
            ++wakes;
        };
        lifecycle.prepareUsb = [](std::function<void()> ready) {
            ready();
        };
        if (cancelled) {
            kobalt_usb_workflow(usb);
            QVERIFY(lifecycle.storageBlocked());
            kobalt::NativeLifecycle::disconnected();
        } else {
            QVERIFY(kobalt::NativeLifecycle::interceptUsb(usb));
            emit usb->aboutToUnplug();
            emit usb->doneProcessing();
        }
        QTRY_COMPARE(wakes, 1);
        QVERIFY(!lifecycle.blocked());
        emit N3PowerWorkflowManager::sharedInstance() -> resumedFromSleep();
        QTest::qWait(50);
        QCOMPARE(wakes, 1);
    }

    void refusedSleepViewRemovalCancelsPreparedWake() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        emit power->aboutToSleep();
        power->sleepVisible = true;
        int prepared = 0, cancelled = 0;
        lifecycle.beforeWake = [&] {
            ++prepared;
        };
        lifecycle.cancelWake = [&] {
            ++cancelled;
        };
        acceptWake = false;
        kobalt_hide_sleep_view(power);
        QCOMPARE(prepared, 1);
        QCOMPARE(cancelled, 1);
        acceptWake = true;
        kobalt_hide_sleep_view(power);
        QCOMPARE(prepared, 2);
        QCOMPARE(cancelled, 1);
        power->pinVisible = true;
        kobalt_hide_sleep_view(power);
        QCOMPARE(prepared, 3);
        QCOMPARE(cancelled, 2);
    }

    void drainingDoesNotPrepareWakeAndSleepCoverWaitsForNativeView() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        int prepared = 0, covered = 0;
        lifecycle.beforeWake = [&] {
            ++prepared;
        };
        lifecycle.sleepViewReady = [&] {
            QVERIFY(power->sleepVisible);
            ++covered;
        };
        kobalt::NativeLifecycle::finishSleepView();
        QCOMPARE(covered, 0);
        emit power->aboutToSleep();
        kobalt_show_sleep_view(power);
        QCOMPARE(covered, 1);
        lifecycle.draining = true;
        kobalt_hide_sleep_view(power);
        QCOMPARE(prepared, 0);
    }

    void preparingWakeCanDestroyLifecycleBeforeNativeHideReturns() {
        auto *lifecycle = new kobalt::NativeLifecycle(nullptr);
        QPointer<kobalt::NativeLifecycle> guard(lifecycle);
        QVERIFY(lifecycle->connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        emit power->aboutToSleep();
        lifecycle->beforeWake = [lifecycle] {
            delete lifecycle;
        };
        kobalt_hide_sleep_view(power);
        QVERIFY(guard.isNull());
        QVERIFY(kobalt::NativeLifecycle::canSuspend());
    }

    void suspendWaitsForActiveEngineWorkAndDraining() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        QVERIFY(kobalt_can_suspend(nullptr));
        {
            kobalt::EngineJob job;
            QVERIFY(!kobalt_can_suspend(nullptr));
        }
        QVERIFY(kobalt_can_suspend(nullptr));
        lifecycle.draining = true;
        QVERIFY(!kobalt_can_suspend(nullptr));
        lifecycle.draining = false;
        nativeSuspendAllowed = false;
        QVERIFY(!kobalt_can_suspend(nullptr));
    }

    void shutdownCompletionIsConsumedBeforeReentry() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        auto *power = N3PowerWorkflowManager::sharedInstance();
        std::function<void()> complete;
        int preparations = 0;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) {
            ++preparations;
            complete = std::move(ready);
        };
        kobalt_reboot(power);
        QCOMPARE(preparations, 1);
        QCOMPARE(reboots, 0);
        QVERIFY(lifecycle.shuttingDown());
        QVERIFY(!kobalt_can_suspend(power));
        kobalt_reboot(power);
        QCOMPARE(preparations, 1);
        auto copied = complete;
        reenterReboot = true;
        complete();
        copied();
        QCOMPARE(reboots, 1);
        QCOMPARE(preparations, 1);
    }

    void powerOffPreservesBatteryLowReason_data() {
        QTest::addColumn<bool>("batteryLow");
        QTest::newRow("normal") << false;
        QTest::newRow("battery-low") << true;
    }

    void powerOffPreservesBatteryLowReason() {
        QFETCH(bool, batteryLow);
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        std::function<void()> complete;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) {
            complete = std::move(ready);
        };
        kobalt_power_off(N3PowerWorkflowManager::sharedInstance(), batteryLow);
        QCOMPARE(powerOffs, 0);
        complete();
        QCOMPARE(powerOffs, 1);
        QCOMPARE(lastBatteryLow, batteryLow);
        complete();
        QCOMPARE(powerOffs, 1);
    }

    void destroyedShutdownManagerDoesNotReceiveCompletion() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        std::function<void()> complete;
        lifecycle.prepareShutdown = [&](std::function<void()> ready) {
            complete = std::move(ready);
        };
        auto *manager = new QObject;
        QVERIFY(kobalt::NativeLifecycle::interceptShutdown(manager, kobalt::NativeLifecycle::Shutdown::Reboot));
        delete manager;
        complete();
        QCOMPARE(reboots, 0);
    }

    void shutdownWithoutPreparationKeepsNativeBehavior() {
        kobalt::NativeLifecycle lifecycle(nullptr);
        QVERIFY(lifecycle.connected());
        kobalt_power_off(N3PowerWorkflowManager::sharedInstance(), true);
        QCOMPARE(powerOffs, 1);
        QVERIFY(lastBatteryLow);
        QVERIFY(!lifecycle.shuttingDown());
    }
};

QTEST_GUILESS_MAIN(NativeLifecycleTest)
#include "native_lifecycle_test.moc"
