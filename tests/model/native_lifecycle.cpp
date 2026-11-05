#include "platforms/kobo/common/native_lifecycle.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "check.h"
#include "support/diagnostics.h"
#include <QCoreApplication>
#include <QEvent>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QVector>
#include <atomic>

// Nickel's exported managers and signals, resolved by the production adapter.
class N3PowerWorkflowManager : public QObject
{
    Q_OBJECT
public:
    bool asleep = false, visible = false, pin = false;
    static N3PowerWorkflowManager *sharedInstance();
    bool sleeping() const;
    bool sleepViewVisible() const;
    bool pinCodeInputDialogVisible() const;
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
    return visible;
}

bool N3PowerWorkflowManager::pinCodeInputDialogVisible() const
{
    return pin;
}

class PermissionRequest;

class PowerManager : public QObject
{
    Q_OBJECT
public:
    static PowerManager *sharedInstance();
signals:
    void aboutToSuspend(PermissionRequest *);
    void resumed();
};

PowerManager *PowerManager::sharedInstance()
{
    static PowerManager manager;
    return &manager;
}

extern "C" void kobalt_suspend_device(void *, const QString &);
extern "C" void kobalt_suspend_device_with_alarm(void *, const QString &);

class AbstractPowerView : public QObject
{
    Q_OBJECT
};

extern "C" void kobalt_show_sleep_view(void *);

class PlugWorkflowManager : public QObject
{
    Q_OBJECT
public:
    static PlugWorkflowManager *sharedInstance();
    bool pluggedInViewVisible() const;
signals:
    void aboutToConnect();
    void aboutToUnplug();
    void doneProcessing();
};

PlugWorkflowManager *PlugWorkflowManager::sharedInstance()
{
    static PlugWorkflowManager manager;
    return &manager;
}

bool PlugWorkflowManager::pluggedInViewVisible() const
{
    return false;
}

namespace {
QString profile;
int handoffs = 0, workflows = 0, shutdowns = 0;

bool available()
{
    return kobalt::storage::profileTransaction(profile, [](QSqlDatabase &) { return true; });
}

void unused(void *) { }

void handoff(void *)
{
    require(!available(), "USB handoff left the profile writable");
    require(!kobalt::NativeLifecycle::nativeSessionSaveAllowed(),
            "USB handoff allowed a late native session write");
    ++handoffs;
}

void workflow(void *manager)
{
    require(!available(), "USB workflow resumed before the profile was blocked");
    ++workflows;
    kobalt::NativeLifecycle::interceptUsb(static_cast<QObject *>(manager));
}

void poweredOff(void *, bool)
{
    ++shutdowns;
}
} // namespace

// Checks the Kobo lifecycle adapter with controlled Nickel signals: sleep-view handoff, PIN rotation, USB entry and power-off.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    profile = root.path();
    require(available(), "Cannot initialize lifecycle profile");
    kobalt::nativeShowSleepView = kobalt::nativeHideSleepView = unused;
    kobalt::nativeCanSuspend = [](void *) { return true; };
    kobalt::nativeUsbConnect = handoff;
    kobalt::nativeUsbWorkflow = workflow;
    kobalt::nativeAboutUsbConnect = kobalt::nativeReboot = unused;
    kobalt::nativePowerOff = poweredOff;
    auto *usb = PlugWorkflowManager::sharedInstance();
    N3PowerWorkflowManager::sharedInstance();
    QTemporaryDir logs;
    kobalt::initializeDiagnostics(logs.path());
    QJsonObject observedPower;
    int requested = 0, resumed = 0, suspendStarts = 0, suspendEnds = 0;
    kobalt::setDiagnosticObserver([&](const QJsonObject &event) {
        const auto name = event.value("event").toString();
        if (name == "power-observer")
            observedPower = event;
        if (name == "standby-request")
            ++requested;
        if (name == "standby-resumed")
            ++resumed;
        if (event.value("operation").toString().startsWith("nickel-suspend-device")) {
            if (name == "operation-start")
                ++suspendStarts;
            if (name == "operation-end")
                ++suspendEnds;
        }
    });
    auto *standby = PowerManager::sharedInstance();
    kobalt::nativeSuspendDevice =
            kobalt::nativeSuspendDeviceWithAlarm = [](void *manager, const QString &wakeupCount) {
                require(manager == PowerManager::sharedInstance() && wakeupCount == "123",
                        "Suspend hook changed its native arguments");
            };
    kobalt::NativeLifecycle lifecycle(nullptr);
    require(lifecycle.connected(), "Mock Nickel managers did not connect");
    require(!kobalt::NativeLifecycle::protectStandbyInput(),
            "Reader input guard activated without a reader");
    lifecycle.readerInputActive = [] { return true; };
    require(kobalt::NativeLifecycle::protectStandbyInput(),
            "Active reader did not enable the input guard");
    require(observedPower.value("standby_request").toBool()
                    && observedPower.value("standby_resume").toBool(),
            "Standby signal capabilities were not recorded");
    emit standby->aboutToSuspend(nullptr);
    kobalt_suspend_device(standby, QStringLiteral("123"));
    kobalt_suspend_device_with_alarm(standby, QStringLiteral("123"));
    emit standby->resumed();
    require(requested == 1 && resumed == 1 && suspendStarts == 2 && suspendEnds == 2,
            "Standby transitions or paired suspend operation records were lost");
    kobalt::setDiagnosticObserver({});

    auto *power = N3PowerWorkflowManager::sharedInstance();
    int sleepHandoffs = 0;
    lifecycle.sleepViewReady = [&] { ++sleepHandoffs; };
    const auto wake = [&] {
        power->asleep = power->visible = power->pin = false;
        emit power->resumingFromSleep();
        QCoreApplication::processEvents();
        require(!lifecycle.blocked(), "Wake did not release sleep state");
    };
    for (int cycle = 0; cycle < 2; ++cycle) {
        power->asleep = power->visible = true;
        power->pin = false;
        emit power->aboutToSleep();
        require(!kobalt::NativeLifecycle::pinScreenActive(),
                "Sleep screen permits native PIN rotation");
        power->pin = true;
        require(!kobalt::NativeLifecycle::pinScreenActive(),
                "Sleeping device permits native PIN rotation");
        power->asleep = false;
        emit power->resumingFromSleep();
        QCoreApplication::processEvents();
        require(kobalt::NativeLifecycle::pinScreenActive(),
                "Awake PIN did not permit native rotation");
        require(lifecycle.blocked() && kobalt::NativeLifecycle::suspended(),
                "PIN entry resumed the reader");
        power->visible = false;
        require(kobalt::NativeLifecycle::pinScreenActive(),
                "PIN rotation depends on the underlying sleep view");
        power->asleep = true;
        require(!kobalt::NativeLifecycle::pinScreenActive(),
                "Returning to sleep retained PIN rotation");
        wake();
        require(!kobalt::NativeLifecycle::pinScreenActive(), "Unlock retained PIN rotation");
    }
    sleepHandoffs = 0;
    // An old NickelHook user can overwrite our showSleepView hook. Both
    // paths must hand off exactly once after the native sleep view is ready.
    for (bool bypassHook : { true, false }) {
        const int before = sleepHandoffs;
        power->asleep = true;
        emit power->aboutToSleep();
        require(!kobalt::NativeLifecycle::protectStandbyInput(),
                "Sleep-screen suspension kept the reading input guard");
        power->visible = true;
        if (!bypassHook)
            kobalt_show_sleep_view(power);
        QCoreApplication::processEvents();
        require(sleepHandoffs == before + 1, "Missing or duplicate sleep handoff");
        kobalt_show_sleep_view(power);
        require(sleepHandoffs == before + 1, "Repeated hook duplicated sleep handoff");
        wake();
    }
    // A late view can appear after the first queued check. Do not expose the
    // library or a PIN prompt while waiting, or keep suspend blocked forever.
    power->asleep = true;
    emit power->aboutToSleep();
    require(!kobalt::NativeLifecycle::canSuspend(), "Suspend raced the queued handoff check");
    QCoreApplication::processEvents();
    require(sleepHandoffs == 2, "Handed off before a native sleep view existed");
    require(kobalt::NativeLifecycle::canSuspend(),
            "Missing sleep view blocked suspend indefinitely");
    power->visible = power->pin = true;
    kobalt_show_sleep_view(power);
    require(sleepHandoffs == 2, "Handed off over the PIN prompt");
    power->pin = false;
    AbstractPowerView view;
    QEvent shown(QEvent::Show);
    QCoreApplication::sendEvent(&view, &shown);
    QCoreApplication::processEvents();
    require(sleepHandoffs == 3, "Late native view did not recover the bypassed hook");
    wake();
    power->asleep = true;
    emit power->aboutToSleep();
    power->visible = true;
    wake();
    require(sleepHandoffs == 3, "Cancelled sleep ran a stale handoff");
    QVector<std::function<void()>> drains;
    lifecycle.prepareUsb = [&](std::function<void()> ready) { drains.append(std::move(ready)); };
    power->asleep = true;
    emit power->aboutToSleep();
    power->visible = true;
    kobalt::NativeLifecycle::interceptUsbWorkflow(usb);
    QCoreApplication::processEvents();
    require(sleepHandoffs == 3, "A queued sleep handoff crossed USB preparation");
    const auto cancelledSleepUsb = drains.takeLast();
    kobalt::NativeLifecycle::disconnected();
    cancelledSleepUsb();
    wake();
    const auto remount = [&] {
        kobalt::NativeLifecycle::disconnected();
        require(!available(), "Disconnect reopened exported storage");
        emit usb->doneProcessing();
        require(!available(), "An ordinary scan reopened exported storage");
        emit usb->aboutToUnplug();
        emit usb->doneProcessing();
        require(available() && !lifecycle.blocked(), "Remount did not reopen the profile");
        require(kobalt::NativeLifecycle::nativeSessionSaveAllowed(),
                "Remount left native session saves blocked");
    };
    // Both native USB entry points must release the connection before handoff.
    for (bool fullWorkflow : { false, true }) {
        if (fullWorkflow)
            kobalt::NativeLifecycle::interceptUsbWorkflow(usb);
        else
            kobalt::NativeLifecycle::interceptUsb(usb);
        require(available(), "Pending USB preparation blocked storage before its final save");
        require(kobalt::NativeLifecycle::nativeSessionSaveAllowed(),
                "Pending USB blocked its native final save");
        const auto finish = drains.takeLast();
        finish();
        require(!available(), "USB preparation did not keep the profile closed");
        const int count = handoffs;
        finish();
        require(handoffs == count, "A duplicate completion repeated USB handoff");
        remount();
    }
    require(handoffs == 2 && workflows == 1, "USB entry points did not complete");
    // Cancel while the asynchronous reader drain is pending. Its late result
    // must not block storage again, including when a newer USB request exists.
    for (bool fullWorkflow : { false, true }) {
        const int before = handoffs;
        if (fullWorkflow)
            kobalt::NativeLifecycle::interceptUsbWorkflow(usb);
        else
            kobalt::NativeLifecycle::interceptUsb(usb);
        const auto cancelled = drains.takeLast();
        kobalt::NativeLifecycle::disconnected();
        cancelled();
        require(available() && !lifecycle.blocked() && handoffs == before,
                "Cancelled USB cleanup blocked the profile or exported storage");
        require(kobalt::NativeLifecycle::nativeSessionSaveAllowed(),
                "Cancelled USB blocked native session retry");
        kobalt::NativeLifecycle::interceptUsbWorkflow(usb);
        const auto current = drains.takeLast();
        cancelled();
        require(available() && lifecycle.storageBlocked(),
                "A stale completion affected the newer USB request");
        current();
        remount();
    }
    // A failed queued write still drains and permits USB handoff. Nickel must
    // not wait forever because a save failed on a full or failing volume.
    require(kobalt::storage::profileTransaction(
                    profile,
                    [](QSqlDatabase &db) {
                        QSqlQuery query(db);
                        return query.exec(
                                "CREATE TRIGGER refuse_save BEFORE INSERT ON book_state BEGIN SELECT RAISE(ABORT,'test'); END");
                    }),
            "Cannot obstruct the final save");
    std::atomic_int saved{ -1 };
    kobalt::storage::RecordWriter writer;
    writer.post(
            [] {
                QJsonObject state;
                state["book"] = "failed-save";
                return kobalt::storage::writeBookState(kobalt::storage::Paths{ profile },
                                                       "failed-save", state);
            },
            [&](bool ok) { saved = ok ? 1 : 0; });
    const int beforeFailure = handoffs;
    kobalt::NativeLifecycle::interceptUsb(usb);
    drains.takeLast()();
    require(saved == 0 && handoffs == beforeFailure + 1, "A failed save prevented USB handoff");
    remount();
    // Power-off supersedes a pending USB drain. A cable disconnect cannot
    // release the shutdown's storage barrier.
    kobalt::NativeLifecycle::interceptUsb(usb);
    const auto abandoned = drains.takeLast();
    lifecycle.prepareShutdown = [](std::function<void()> ready) {
        kobalt::storage::closeProfile(true);
        ready();
    };
    power->asleep = true;
    emit power->aboutToSleep();
    power->visible = true;
    require(kobalt::NativeLifecycle::interceptShutdown(usb,
                                                       kobalt::NativeLifecycle::Shutdown::PowerOff),
            "Power-off was not intercepted");
    kobalt::NativeLifecycle::disconnected();
    abandoned();
    QCoreApplication::processEvents();
    require(sleepHandoffs == 3, "A queued sleep handoff crossed power-off");
    require(shutdowns == 1 && !available(), "USB cancellation reopened storage during shutdown");
    require(!kobalt::NativeLifecycle::nativeSessionSaveAllowed(),
            "USB cancellation reopened native saves during shutdown");
    kobalt::storage::reopenProfile();
    kobalt::storage::closeProfile();
    kobalt::pauseDiagnostics(true);
}

#include "native_lifecycle.moc"
