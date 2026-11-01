#include "platforms/kobo/common/standby_input.h"
#include "support/diagnostics.h"
#include "check.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <qpa/qwindowsysteminterface_p.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>

namespace {
int writes = 0;
bool nativeResult = true;
bool write(const QString &, const QString &) {
    ++writes;
    if (!nativeResult) errno = EIO;
    return nativeResult;
}
bool suspend() { return kobalt_power_string_to_file(QStringLiteral("/sys/power/state"), QStringLiteral("mem")); }
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QTemporaryDir logs;
    kobalt::initializeDiagnostics(logs.path());
    std::atomic<int> acquired{0}, deferred{0}, completed{0}, wakeupChecks{0};
    std::atomic<bool> acquiredOnWorker{false};
    std::atomic<bool> savedCountSeen{false};
    kobalt::setDiagnosticObserver([&](const QJsonObject &event) {
        if (event.value("event") == "input-acquired") {
            ++acquired;
            if (!event.value("main_thread").toBool()) acquiredOnWorker = true;
        }
        if (event.value("event") == "standby-deferred") ++deferred;
        if (event.value("event") == "kernel-suspend-result") ++completed;
        if (event.value("event") == "standby-wakeup-check") {
            ++wakeupChecks;
            if (event.value("saved_wakeup_count").toString() == "123") savedCountSeen = true;
        }
    });
    kobalt::nativePowerStringToFile = write;
    require(suspend() && writes == 1, "Unscoped native writes changed");
    {
        kobalt::StandbyInputScope scope(true, QStringLiteral("123"));
        require(suspend() && writes == 2, "Quiet reader cannot enter standby");
        require(kobalt_power_string_to_file("/sys/power/wakeup_count", "123"), "Native wakeup handshake changed");
        require(kobalt_power_string_to_file("/sys/power/state-extended", "0"), "Native cleanup changed");
        require(kobalt_power_string_to_file("/sys/power/state", "freeze"), "Unrelated sleep mode changed");
    }
    const int before = writes;
    {
        kobalt::StandbyInputScope outer(true);
        std::thread input([] { kobalt::standbyTouchAcquired(); });
        input.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        // The alarm wrapper nests suspendDevice after preparation. Taking a
        // fresh baseline here would forget input acquired during alarm setup.
        kobalt::StandbyInputScope inner(true);
        require(!suspend() && errno == EAGAIN && writes == before, "Input acquired during preparation was slept over");
    }
    kobalt::standbyTouchAcquired();
    {
        kobalt::StandbyInputScope scope(true);
        require(!suspend() && writes == before, "Acquisition just before entry was slept over before Qt enqueue");
    }
    // This is the guard's acquisition-to-enqueue quiet window, not a new
    // polling timer in the reader. Standby must resume after input settles.
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    {
        kobalt::StandbyInputScope scope(true);
        require(suspend() && writes == before + 1, "Completed input prevented later standby");
    }
    using Events = QWindowSystemInterfacePrivate;
    for (auto type : {Events::Touch, Events::Mouse, Events::Key, Events::Wheel}) {
        // Peek only: the guard must neither dispatch nor remove pending input.
        auto *event = new Events::WindowSystemEvent(type);
        Events::windowSystemEventQueue.append(event);
        {
            kobalt::StandbyInputScope scope(true);
            require(!suspend(), "Queued platform input was slept over");
            require(Events::peekWindowSystemEvent(type) == event, "Guard consumed queued input");
        }
        Events::removeWindowSystemEvent(event);
    }
    {
        auto *event = new Events::WindowSystemEvent(Events::Expose);
        Events::windowSystemEventQueue.append(event);
        kobalt::StandbyInputScope scope(true);
        require(suspend(), "Non-input platform events prevented standby");
        Events::removeWindowSystemEvent(event);
    }
    {
        kobalt::StandbyInputScope scope(false);
        kobalt::standbyTouchAcquired();
        require(suspend(), "Guard changed stock or sleep-screen suspension");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    {
        kobalt::StandbyInputScope scope(true);
        nativeResult = false;
        require(!suspend() && errno == EIO, "Native suspend failure was not preserved");
        require(!kobalt_power_string_to_file("/sys/power/wakeup_count", "124") && errno == EIO,
                "Native wakeup-count failure was not preserved");
    }
    require(acquired == 3 && acquiredOnWorker && deferred == 6 && completed == 5 && wakeupChecks == 2 && savedCountSeen,
            "Acquisition, deferral or kernel result tracing missing");
    kobalt::setDiagnosticObserver({});
    kobalt::pauseDiagnostics(true);
}
