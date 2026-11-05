#include "support/qt_compat.h"
#include "native_lifecycle.h"
#include "startup_recovery.h"
#include "screensaver_compatibility.h"
#include "reader_environment.h"
#include "engine/crengine/engine_activity.h"
#include "storage/profile_database.h"
#include "support/diagnostics.h"
#include "standby_input.h"
#include <QPointer>
#include <QCoreApplication>
#include <QEvent>
#include <QTimer>
#include <dlfcn.h>
#include <memory>
#include <atomic>
#include <QFile>
#include "support/strings.h"
#include <cerrno>
#include <sys/stat.h>
#ifndef KOBALT_READER_HARDWARE
#  include <QApplication>
#  include <QWidget>
#  include <QWindow>
#  include <QKeyEvent>
#  include "lifecycle_test.h"
#  include <QFile>
#  include <QJsonDocument>
#  include <QSaveFile>
#endif

namespace kobalt {
namespace {
QPointer<NativeLifecycle> active;
std::atomic_bool sessionStorageAvailable{ true };

template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

struct PowerApi
{
    const void *(*manager)() =
            symbol<decltype(manager)>("_ZN22N3PowerWorkflowManager14sharedInstanceEv");
    bool (*sleeping)(const void *) =
            symbol<decltype(sleeping)>("_ZNK22N3PowerWorkflowManager8sleepingEv");
    bool (*visible)(const void *) =
            symbol<decltype(visible)>("_ZNK22N3PowerWorkflowManager16sleepViewVisibleEv");
    bool (*pin)(const void *) =
            symbol<decltype(pin)>("_ZNK22N3PowerWorkflowManager25pinCodeInputDialogVisibleEv");

    bool available() const { return manager && sleeping && visible && pin; }
};

const PowerApi &powerApi()
{
    static const PowerApi value;
    return value;
}

bool nativeSleeping()
{
    const auto &api = powerApi();
    if (!api.available())
        return true;
    const auto *power = api.manager();
    return power && (api.sleeping(power) || api.visible(power) || api.pin(power));
}
} // namespace

bool NativeLifecycle::powerAvailable()
{
    return powerApi().available();
}

bool NativeLifecycle::pinScreenActive()
{
    const auto &api = powerApi();
    const auto *power = api.available() ? api.manager() : nullptr;
    return power && !api.sleeping(power) && api.pin(power);
}

bool NativeLifecycle::suspended()
{
    static const auto manager =
            symbol<const void *(*)()>("_ZN19PlugWorkflowManager14sharedInstanceEv");
    static const auto visible =
            symbol<bool (*)(const void *)>("_ZN19PlugWorkflowManager20pluggedInViewVisibleEv");
    return (manager && visible && visible(manager())) || nativeSleeping();
}

bool NativeLifecycle::usbSaveAllowed()
{
    // The USB prompt can cover the reader before storage is exported. Only
    // the held workflow permits its final write; sleep and PIN still block it.
    return active && active->pendingWorkflow && !active->exported && !active->poweringOff
            && !nativeSleeping();
}

bool NativeLifecycle::nativeSessionSaveAllowed()
{
    return sessionStorageAvailable.load();
}

bool NativeLifecycle::nativeProgressSaveAllowed()
{
    if (!nativeSessionSaveAllowed())
        return false;
    // The shutdown hook owns storage until its save continuation returns.
    // A visible sleep screen or PIN must not prevent that final write.
    if (active && active->shuttingDown())
        return !active->storageBlocked();
    return !suspended() || usbSaveAllowed() || sleepSaveAllowed();
}

bool NativeLifecycle::sleepSaveAllowed()
{
    return active && active->savingBeforeSleep && !active->storageBlocked()
            && !active->shuttingDown();
}

void (*nativeHideSleepView)(void *);

extern "C" __attribute__((visibility("default"))) void kobalt_hide_sleep_view(void *manager)
{
    const auto lifecycle = active;
    const bool holding = lifecycle && lifecycle->blocked() && !lifecycle->storageBlocked()
            && !lifecycle->shuttingDown() && !lifecycle->draining && lifecycle->beforeWake;
    if (holding)
        lifecycle->beforeWake();
    nativeHideSleepView(manager);
    // Nickel can refuse to remove the sleep view. Do not leave its window
    // frozen while a lock screen still needs to accept input or repaint.
    const auto visible =
            symbol<bool (*)(const void *)>("_ZNK22N3PowerWorkflowManager16sleepViewVisibleEv");
    const auto pin = symbol<bool (*)(const void *)>(
            "_ZNK22N3PowerWorkflowManager25pinCodeInputDialogVisibleEv");
    if (holding && lifecycle && lifecycle == active
        && (!visible || !pin || visible(manager) || pin(manager)) && lifecycle->cancelWake)
        lifecycle->cancelWake();
}

void (*nativeShowSleepView)(void *);

extern "C" __attribute__((visibility("default"))) void kobalt_show_sleep_view(void *manager)
{
#ifndef KOBALT_READER_HARDWARE
    if (active) {
        auto *modal = QApplication::activeModalWidget();
        active->testSleepCovered = modal && modal->objectName() == "rrReader" && modal->isVisible();
    }
#endif
    nativeShowSleepView(manager);
#ifndef KOBALT_READER_HARDWARE
    if (active) {
        const auto visible =
                symbol<bool (*)(const void *)>("_ZNK22N3PowerWorkflowManager16sleepViewVisibleEv");
        active->testSleepViewReady = visible && visible(manager);
    }
#endif
    NativeLifecycle::finishSleepView();
}

void NativeLifecycle::finishSleepView()
{
    if (active)
        active->checkSleepView("hook");
}

void NativeLifecycle::checkSleepView(const char *source)
{
    if (!sleepHandoffPending || !asleep || storageBlocked() || shuttingDown() || removalPending)
        return;
    const auto &api = powerApi();
    const auto *power = api.available() ? api.manager() : nullptr;
    const bool visible = power && api.visible(power);
    const bool pin = power && api.pin(power);
    const bool ready = visible && !pin && bool(sleepViewReady);
    if (ready) {
        // Claim this transition before painting. Child show events and a
        // second hook call must not repeat the handoff.
        sleepHandoffPending = false;
        sleepViewCheck.stop();
        sleepViewReady();
    }
    diagnostic(ready ? "sleep-view-handoff" : "sleep-view-unavailable",
               qt::object({ { "source", QString::fromLatin1(source) },
                            { "visible", visible },
                            { "pin", pin },
                            { "sleeping", power && api.sleeping(power) } }),
               false);
}

void NativeLifecycle::queueSleepViewCheck()
{
    if (sleepHandoffPending && asleep && !sleepViewCheck.isActive())
        sleepViewCheck.start(0);
}

bool NativeLifecycle::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Show && object->inherits("AbstractPowerView"))
        queueSleepViewCheck();
    return false;
}

bool (*nativeCanSuspend)(void *);
void (*nativeSuspendDevice)(void *, const QString &);
void (*nativeSuspendDeviceWithAlarm)(void *, const QString &);

extern "C" __attribute__((visibility("default"))) void
kobalt_suspend_device(void *manager, const QString &wakeupCount)
{
    StandbyInputScope input(NativeLifecycle::protectStandbyInput(), wakeupCount);
    DiagnosticOperation operation("nickel-suspend-device");
    nativeSuspendDevice(manager, wakeupCount);
}

extern "C" __attribute__((visibility("default"))) void
kobalt_suspend_device_with_alarm(void *manager, const QString &wakeupCount)
{
    StandbyInputScope input(NativeLifecycle::protectStandbyInput(), wakeupCount);
    DiagnosticOperation operation("nickel-suspend-device-with-alarm");
    nativeSuspendDeviceWithAlarm(manager, wakeupCount);
}

void (*nativeUsbConnect)(void *);
void (*nativeUsbWorkflow)(void *);
void (*nativeAboutUsbConnect)(void *);
void (*nativePowerOff)(void *, bool);
void (*nativeReboot)(void *);

extern "C" __attribute__((visibility("default"))) void kobalt_power_off(void *manager,
                                                                        bool batteryLow)
{
#ifdef KOBALT_READER_HARDWARE
    kobalt_startup_complete(nickel::startupPath);
#endif
    const auto mode = batteryLow ? NativeLifecycle::Shutdown::BatteryLow
                                 : NativeLifecycle::Shutdown::PowerOff;
    if (!NativeLifecycle::interceptShutdown(static_cast<QObject *>(manager), mode))
        nativePowerOff(manager, batteryLow);
}

extern "C" __attribute__((visibility("default"))) void kobalt_reboot(void *manager)
{
#ifdef KOBALT_READER_HARDWARE
    kobalt_startup_complete(nickel::startupPath);
#endif
    if (!NativeLifecycle::interceptShutdown(static_cast<QObject *>(manager),
                                            NativeLifecycle::Shutdown::Reboot))
        nativeReboot(manager);
}

extern "C" __attribute__((visibility("default"))) void kobalt_usb_workflow(void *manager)
{
    if (!NativeLifecycle::interceptUsbWorkflow(static_cast<QObject *>(manager))) {
        nativeUsbWorkflow(manager);
    }
}

extern "C" __attribute__((visibility("default"))) void kobalt_usb_about_to_connect(void *manager)
{
    NativeLifecycle::beginUsb();
    nativeAboutUsbConnect(manager);
}

extern "C" __attribute__((visibility("default"))) bool kobalt_can_suspend(void *manager)
{
    return nativeCanSuspend(manager) && NativeLifecycle::canSuspend();
}

extern "C" __attribute__((visibility("default"))) void kobalt_usb_connect(void *manager)
{
    if (!NativeLifecycle::interceptUsb(static_cast<QObject *>(manager)))
        nativeUsbConnect(manager);
}

NativeLifecycle::NativeLifecycle(QObject *parent) : QObject(parent)
{
    sleepViewCheck.setSingleShot(true);
    connect(&sleepViewCheck, &QTimer::timeout, this, [this] { checkSleepView("fallback"); });
    wakeRetry.setSingleShot(true);
    connect(&wakeRetry, &QTimer::timeout, this, &NativeLifecycle::finishWake);
    const auto power = symbol<QObject *(*)()>("_ZN22N3PowerWorkflowManager14sharedInstanceEv");
    const auto usb = symbol<QObject *(*)()>("_ZN19PlugWorkflowManager14sharedInstanceEv");
    if (!power || !usb || !nativeUsbWorkflow || !nativeCanSuspend || !nativeUsbConnect
        || !nativeAboutUsbConnect || !nativeShowSleepView || !nativeHideSleepView || !nativePowerOff
        || !nativeReboot)
        return;
    if (!connectScreensaverPowerOff())
        return;
    const auto a = connect(power(), SIGNAL(aboutToSleep()), this, SLOT(aboutToSleep()),
                           Qt::DirectConnection);
    // Nickel can defer resumedFromSleep until the frontlight fade finishes.
    // Start earlier; finishWake still waits for the sleep, PIN and USB views.
    const auto resuming = connect(power(), SIGNAL(resumingFromSleep()), this, SLOT(resumed()),
                                  Qt::DirectConnection);
    const auto b = connect(power(), SIGNAL(resumedFromSleep()), this, SLOT(resumed()),
                           Qt::DirectConnection);
    const auto c = connect(usb(), SIGNAL(doneProcessing()), this, SLOT(processingDone()),
                           Qt::DirectConnection);
    const auto d =
            connect(usb(), SIGNAL(aboutToUnplug()), this, SLOT(unplugging()), Qt::DirectConnection);
    const auto plugManager = symbol<QObject *(*)()>("_ZN11PlugManager14sharedInstanceEv");
    if (plugManager) {
        connect(plugManager(), SIGNAL(plugged()), this, SLOT(usbPlugged()), Qt::DirectConnection);
    }
    // Standby while a reading page stays visible is separate from the sleep
    // screen workflow. Observe the native signals without requesting a wakeup.
    const auto standby = symbol<QObject *(*)()>("_ZN12PowerManager14sharedInstanceEv");
    if (standby && standby()) {
        const auto entering = connect(standby(), SIGNAL(aboutToSuspend(PermissionRequest *)), this,
                                      SLOT(standbyRequested()), Qt::DirectConnection);
        const auto leaving = connect(standby(), SIGNAL(resumed()), this, SLOT(standbyResumed()),
                                     Qt::DirectConnection);
        if (diagnosticsActive())
            diagnostic("power-observer",
                       { { "standby_request", bool(entering) },
                         { "standby_resume", bool(leaving) },
                         { "suspend_call", bool(nativeSuspendDevice) },
                         { "alarm_suspend_call", bool(nativeSuspendDeviceWithAlarm) },
                         { "kernel_input_guard",
                           bool(nativePowerStringToFile
                                && (nativeSuspendDevice || nativeSuspendDeviceWithAlarm)) } },
                       false);
    }
    connected_ = a && resuming && b && c && d;
    if (connected_) {
        active = this;
        sessionStorageAvailable.store(true);
        QCoreApplication::instance()->installEventFilter(this);
        asleep = nativeSleeping();
        engineActivity().pause(asleep);
    }
#ifndef KOBALT_READER_HARDWARE
    auto *testTimer = new QTimer(this);
    connect(testTimer, &QTimer::timeout, this, &NativeLifecycle::testCommand);
    testTimer->start(100);
#endif
}

void NativeLifecycle::standbyRequested()
{
    if (diagnosticsActive())
        diagnostic("standby-request", {}, false);
}

void NativeLifecycle::standbyResumed()
{
    if (diagnosticsActive())
        diagnostic("standby-resumed", {}, false);
}

void NativeLifecycle::usbPlugged()
{
    // Nickel's signal can be called directly inside its library, bypassing a
    // PLT hook. Wait until its slots have created the confirmation dialog.
    qt::later(0, this, [this] {
        if (usbPrompt && !storageBlocked() && !poweringOff) {
            usbPrompt();
        }
    });
}

void NativeLifecycle::aboutToSleep()
{
    wakeRetry.stop();
    asleep = true;
    sleepHandoffPending = true;
    engineActivity().pause(!draining && !storageBlocked());
    diagnostic("sleep-start");
    savingBeforeSleep = true;
    if (sleep) {
        sleep();
    }
    savingBeforeSleep = false;
    // Older NickelHook users can replace our showSleepView hook without
    // chaining it. Check once after Nickel and the add-on finish their stack.
    // A later power-view Show event schedules another check, without polling.
    queueSleepViewCheck();
}

void NativeLifecycle::resumed()
{
    sleepHandoffPending = false;
    sleepViewCheck.stop();
    // The completed signal can arrive after the reader is already awake.
    if (!asleep) {
        return;
    }
    // Leave Nickel's resume stack before restoring the reader.
    wakeRetry.start(0);
}

void NativeLifecycle::finishWake()
{
    if (poweringOff || removalPending) {
        return;
    }
    const auto &api = powerApi();
    const auto *power = api.available() ? api.manager() : nullptr;
    if (!power || api.sleeping(power)) {
        return;
    }
    if (suspended()) {
        // Sleep, PIN and USB views can outlast their native transition signal.
        // Retry only while awake; another sleep signal cancels the timer.
        wakeRetry.start(25);
        return;
    }
    asleep = false;
    if (storageBlocked()) {
        return;
    }
    engineActivity().pause(false);
    diagnostic("sleep-end");
    if (wake) {
        wake();
    }
}

bool NativeLifecycle::protectStandbyInput()
{
    return active && !active->blocked() && active->readerInputActive && active->readerInputActive();
}

namespace {
bool awakeHeld = false;
}

void NativeLifecycle::holdAwake(bool hold)
{
    awakeHeld = hold;
}

bool NativeLifecycle::canSuspend()
{
    if (awakeHeld)
        return false;
    // Active storage work must finish before standby. Only low battery
    // requests a new reading save here; ordinary activity can stay in memory.
    return !active
            || (!active->sleepViewCheck.isActive() && !active->draining && !active->usbPending
                && !active->poweringOff && engineActivity().quiescent()
                && (!active->sleepWorkPending || !active->sleepWorkPending())
                && (!active->beforeStandby || active->beforeStandby()));
}

bool NativeLifecycle::interceptShutdown(QObject *manager, Shutdown mode)
{
    if (!active || !active->prepareShutdown)
        return false;
    if (active->poweringOff)
        return true;
    if (!active->storageBlocked() && active->preparePowerScreen)
        active->preparePowerScreen(mode);
    active->poweringOff = true;
    active->batteryLow_ = mode == Shutdown::BatteryLow;
    // USB export has already released the book. Do not touch exported storage.
    if (active->exported)
        return false;
    ++active->generation;
    active->usbPending = false;
    active->pendingGadget.clear();
    active->pendingWorkflow.clear();
    engineActivity().pause(false);
    diagnostic("shutdown-release-start",
               kobalt::qt::object({ { "reboot", mode == Shutdown::Reboot } }));
    QPointer<QObject> guard(manager);
    // Callback copies share one completion. Consume it before calling Nickel,
    // whose shutdown path can reenter this hook while the manager still lives.
    const auto completed = std::make_shared<bool>(false);
    const auto lifecycle = active;
    const auto finish = [guard, lifecycle, mode, completed] {
        if (!guard || !lifecycle || lifecycle != active || *completed)
            return;
        *completed = true;
        // Nickel stops EventEngine and closes its databases in powerOff().
        // Every software shutdown, including battery-low shutdown, waits for
        // save outcome and the worker drain. Battery-low may waive completed
        // failures, never active storage calls. Physical power loss is separate.
        diagnostic("shutdown-release-complete");
        // Close and block the profile before Nickel freezes its threads.
        // Late saves must keep failing afterward.
        sessionStorageAvailable.store(false);
        storage::closeProfile(true);
        // The event log is written on its own thread. Let it finish the
        // last events, briefly, before Nickel freezes its threads.
        flushDiagnostics(1000);
        if (mode == Shutdown::Reboot)
            nativeReboot(guard.data());
        else
            nativePowerOff(guard.data(), mode == Shutdown::BatteryLow);
    };
    active->prepareShutdown(finish);
    return true;
}

bool NativeLifecycle::interceptUsbWorkflow(QObject *manager)
{
    if (!active || !active->prepareUsb) {
        return false;
    }
    if (active->storageBlocked() || active->poweringOff || active->removalPending
        || active->pendingWorkflow) {
        return true;
    }
    const auto directory = QString::fromLatin1(nickel::configDir);
    active->usbRemoval.begin(QFile::exists(directory + QStringLiteral("/.initialized")),
                             QFile::exists(directory + QStringLiteral("/uninstall")));
    auto guard = active;
    const auto request = ++active->generation;
    active->pendingWorkflow = manager;
    active->usbPending = true;
    active->storageReady = false;
    engineActivity().pause(false);
    diagnostic("usb-save-start");
    active->prepareUsb([guard, request] {
        if (!guard || request != guard->generation || !guard->usbPending
            || !guard->pendingWorkflow) {
            return;
        }
        // Only a current USB request may block storage. The reader drain can
        // finish after disconnect has cancelled it and reopened the profile.
        sessionStorageAvailable.store(false);
        storage::closeProfile(true);
        auto *manager = guard->pendingWorkflow.data();
        guard->pendingWorkflow.clear();
        guard->storageReady = true;
        diagnostic("usb-release-complete");
        // Resume the whole caller, not just its aboutToConnect signal. Nickel
        // can now clear its views and databases without racing our save.
        nativeUsbWorkflow(manager);
    });
    return true;
}

void NativeLifecycle::beginUsb()
{
    if (!active || !active->prepareUsb || active->storageBlocked() || active->poweringOff)
        return;
    const auto directory = QString::fromLatin1(nickel::configDir);
    active->usbRemoval.begin(QFile::exists(directory + QStringLiteral("/.initialized")),
                             QFile::exists(directory + QStringLiteral("/uninstall")));
    auto guard = active;
    const auto request = ++active->generation;
    active->usbPending = true;
    active->storageReady = false;
    engineActivity().pause(false);
    diagnostic("usb-release-start");
    active->prepareUsb([guard, request] {
        if (!guard || request != guard->generation || !guard->usbPending)
            return;
        sessionStorageAvailable.store(false);
        storage::closeProfile(true);
        diagnostic("usb-release-complete");
        guard->storageReady = true;
        guard->finishUsb();
    });
}

bool NativeLifecycle::interceptUsb(QObject *manager)
{
    if (!active || !active->prepareUsb)
        return false;
    if (active->exported || active->poweringOff)
        return true;
    beginUsb();
    active->pendingGadget = manager;
    active->finishUsb();
    return true;
}

void NativeLifecycle::finishUsb()
{
    if (!usbPending || !storageReady || !pendingGadget)
        return;
    auto *manager = pendingGadget.data();
    pendingGadget.clear();
    usbPending = false;
    exported = true;
    diagnostic("usb-handoff");
    pauseDiagnostics(true);
#ifndef KOBALT_READER_HARDWARE
    if (testUsb) {
        ++testHandoffs;
        return;
    }
#endif
    nativeUsbConnect(manager);
}

void NativeLifecycle::unplugging()
{
    if (exported)
        remounting = true;
    else
        disconnected();
}

void NativeLifecycle::disconnected()
{
    if (!active || active->exported || active->poweringOff)
        return; // Wait for remount, or leave storage closed for shutdown.
    // USB ended before the export; the partition was never released.
    storage::reopenProfile();
    sessionStorageAvailable.store(true);
    ++active->generation;
    active->usbPending = false;
    active->pendingGadget.clear();
    active->storageReady = false;
    active->pendingWorkflow.clear();
    active->wakeRetry.start(0);
}

void NativeLifecycle::processingDone()
{
    // Ordinary library scans also emit this signal. Only a remount ends USB.
    if (!exported || !remounting)
        return;
    remounting = false;
    ++generation;
    usbPending = exported = storageReady = false;
    pendingGadget.clear();
    storage::reopenProfile();
    sessionStorageAvailable.store(true);
    pauseDiagnostics(false);
    diagnostic("usb-storage-ready");
    if (restartAfterRemoval())
        return;
    if (nativeSleeping()) {
        engineActivity().pause(true);
    }
    wakeRetry.start(0);
}

bool NativeLifecycle::restartAfterRemoval()
{
    // A read error must not look like an uninstall request. Nickel has already
    // remounted user storage and completed its USB processing at this point.
    struct stat marker;
    const int result = ::lstat(
            QFile::encodeName(QString::fromLatin1(nickel::configDir) + QStringLiteral("/uninstall"))
                    .constData(),
            &marker);
    const bool missing = result != 0 && errno == ENOENT;
    if (!usbRemoval.finish(!missing) || removalPending)
        return false;
    const auto manager = symbol<QObject *(*)()>("_ZN22N3PowerWorkflowManager14sharedInstanceEv");
    auto *power = manager ? manager() : nullptr;
    if (!power || !nativeReboot)
        return false;
    removalPending = true;
    // Reboot does not depend on whether Nickel can display the message.
    // Leave the current processing stack and give the overlay time to paint.
    // The normal shutdown hook drains Kobalt before Nickel closes its databases.
    QPointer<QObject> guard(power);
    qt::later(3000, this, [this, guard] {
        if (guard)
            kobalt_reboot(guard.data());
        else {
            removalPending = false;
            resumed();
        }
    });
    const auto window = symbol<void *(*)()>("_ZN20MainWindowController14sharedInstanceEv");
    const auto toast = symbol<void (*)(void *, const QString &, const QString &, int)>(
            "_ZN20MainWindowController5toastERK7QStringS2_i");
    if (window && toast) {
        // Nickel always starts a dismissal timer. Keep its native toast alive
        // through shutdown rather than letting it disappear at the reboot delay.
        if (auto *controller = window())
            toast(controller, strings::get("native_ui.removing_kobalt"),
                  strings::get("native_ui.restarting_device"), 10000);
    }
    return true;
}
#ifndef KOBALT_READER_HARDWARE
void NativeLifecycle::testCommand()
{
    QFile input("/instance/lifecycle-test.json");
    if (!input.open(QIODevice::ReadOnly))
        return;
    const auto command = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    input.remove();
    const auto action = command.value("action").toString();
    if (action == "native-power") {
        // The simulator's evdev power action releases any held touch first.
        // Enter Nickel directly to test sleeping during an active gesture.
        const auto manager = symbol<void *(*)()>("_ZN22N3PowerWorkflowManager14sharedInstanceEv");
        const auto toggle = symbol<void (*)(void *)>("_ZN22N3PowerWorkflowManager11toggleSleepEv");
        if (manager && toggle)
            toggle(manager());
    } else if (action == "native-power-off") {
        const auto manager =
                symbol<QObject *(*)()>("_ZN22N3PowerWorkflowManager14sharedInstanceEv");
        if (manager)
            QMetaObject::invokeMethod(manager(), "powerOff", Qt::DirectConnection,
                                      Q_ARG(bool, false));
    } else if (action == "native-reboot") {
        const auto manager =
                symbol<QObject *(*)()>("_ZN22N3PowerWorkflowManager14sharedInstanceEv");
        // reboot() is not a Qt slot. Enter the installed wrapper with the real
        // manager, then continue into Nickel's original implementation.
        if (manager)
            kobalt_reboot(manager());
    } else if (action == "page-button") {
        nickel::sendTestPageButton(command, blocked());
    } else if (action == "key") {
        // Any Qt key, as a Bluetooth keyboard or remote delivers it through
        // Qt's evdev keyboard handler and Nickel's keymaps.
        if (auto *target = QGuiApplication::focusWindow()) {
            const int key = command.value("key").toInt();
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &press);
            QCoreApplication::sendEvent(target, &release);
        }
    } else if (action == "usb" || action == "usb-unplug") {
        testUsb = true;
        const auto manager = symbol<QObject *(*)()>("_ZN11PlugManager14sharedInstanceEv");
        interceptUsb(manager());
        if (action == "usb-unplug")
            disconnected();
    } else if (action == "native-usb") {
        testUsb = true;
        const auto manager = symbol<QObject *(*)()>("_ZN19PlugWorkflowManager14sharedInstanceEv");
        QMetaObject::invokeMethod(manager(), "onCancelAndConnect", Qt::DirectConnection);
    } else if (action == "native-unplug") {
        const auto manager = symbol<QObject *(*)()>("_ZN19PlugWorkflowManager14sharedInstanceEv");
        QMetaObject::invokeMethod(manager(), "unplugged", Qt::DirectConnection);
    } else if (action == "unplug")
        disconnected();
    else if (action == "mounted") {
        unplugging();
        processingDone();
    } else if (action == "processing-done")
        processingDone();
    QSaveFile output("/instance/artifacts/lifecycle-test.json");
    if (!output.open(QIODevice::WriteOnly))
        return;
    output.write(QJsonDocument(kobalt::qt::object({ { "id", command.value("id") },
                                                    { "asleep", asleep },
                                                    { "usb_pending", usbPending },
                                                    { "exported", exported },
                                                    { "draining", draining },
                                                    { "powering_off", poweringOff },
                                                    { "quiescent", engineActivity().quiescent() },
                                                    { "suspend_allowed", canSuspend() },
                                                    { "sleep_covered", testSleepCovered },
                                                    { "sleep_view_ready", testSleepViewReady },
                                                    { "handoffs", testHandoffs } }))
                         .toJson());
    output.commit();
}
#endif
} // namespace kobalt
