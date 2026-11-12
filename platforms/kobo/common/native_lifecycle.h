#pragma once
#include "usb_removal.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

namespace kobalt {
extern void (*nativeShowSleepView)(void *);
extern void (*nativeHideSleepView)(void *);
extern bool (*nativeCanSuspend)(void *);
extern void (*nativeSuspendDevice)(void *, const QString &);
extern void (*nativeSuspendDeviceWithAlarm)(void *, const QString &);
extern void (*nativeUsbConnect)(void *);
extern void (*nativeUsbWorkflow)(void *);
extern void (*nativeAboutUsbConnect)(void *);
extern void (*nativePowerOff)(void *, bool);
extern void (*nativeReboot)(void *);

// Construct only after firmwareSupported() accepts Nickel. Nickel owns power and storage;
// this adapter prepares Kobalt before those native transitions continue.
class NativeLifecycle : public QObject
{
    Q_OBJECT
public:
    enum class Shutdown { PowerOff, BatteryLow, Reboot };
    explicit NativeLifecycle(QObject *parent);
    static bool powerAvailable();
    static bool suspended();
    // PIN entry can own native rotation while the reader remains suspended.
    static bool pinScreenActive();
    static bool sleepSaveAllowed();
    static bool usbSaveAllowed();
    // Thread-safe gate for queued session writes. Closed before storage is
    // handed to USB or Nickel begins its final shutdown.
    static bool nativeSessionSaveAllowed();
    // GUI-thread permission for progress writes, including a held shutdown
    // while Nickel's sleep or PIN screen is still visible.
    static bool nativeProgressSaveAllowed();

    bool connected() const { return connected_; }

    bool blocked() const
    {
        return asleep || usbPending || exported || poweringOff || removalPending;
    }

    bool batteryLowShutdown() const { return poweringOff && batteryLow_; }

    bool shuttingDown() const { return poweringOff; }

    bool storageBlocked() const { return usbPending || exported; }

    bool draining = false;
    std::function<void(Shutdown)> preparePowerScreen;
    std::function<void()> sleep, sleepViewReady, wake, beforeWake, cancelWake, usbPrompt;
    std::function<bool()> sleepWorkPending;
    // True while a book opens, from the request until the reader is shown.
    // canSuspend then answers false. A dialog that waits for the user is not
    // opening work.
    std::function<bool()> openingWorkPending;
    static void finishSleepView();
    std::function<void(std::function<void()>)> prepareUsb;
    std::function<void(std::function<void()>)> prepareShutdown;
    // False keeps the device awake while pending reader saves are started.
    std::function<bool()> beforeStandby;
    std::function<bool()> readerInputActive;
    static bool protectStandbyInput();
    static void disconnected();
    static bool canSuspend();
    // For the scenario player: true makes canSuspend answer false, so Nickel's
    // standby leaves the device awake while a run lasts.
    static void holdAwake(bool hold);
    static void beginUsb();
    static bool interceptUsb(QObject *manager);
    static bool interceptUsbWorkflow(QObject *manager);
    static bool interceptShutdown(QObject *manager, Shutdown mode);
#ifndef KOBALT_READER_HARDWARE
    void testCommand();
    bool testUsb = false;
    int testHandoffs = 0;
    bool testSleepCovered = false, testSleepViewReady = false;
#endif
private slots:
    void standbyRequested();
    void standbyResumed();
    void usbPlugged();
    void aboutToSleep();
    void resumed();
    void processingDone();
    void unplugging();

private:
    bool eventFilter(QObject *object, QEvent *event) override;
    void checkSleepView(const char *source);
    void queueSleepViewCheck();
    bool sleepHandoffPending = false;
    QTimer sleepViewCheck;
    bool savingBeforeSleep = false;
    bool batteryLow_ = false;
    bool connected_ = false, asleep = false, usbPending = false, exported = false,
         poweringOff = false;
    bool storageReady = false, remounting = false;
    QPointer<QObject> pendingGadget, pendingWorkflow;
    void finishUsb();
    void finishWake();
    QTimer wakeRetry;
    quint64 generation = 0;
    UsbRemoval usbRemoval;
    bool removalPending = false;
    bool restartAfterRemoval();
};
} // namespace kobalt
