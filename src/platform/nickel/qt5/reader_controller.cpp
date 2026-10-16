#include "support/qt_compat.h"
#include "hooks.h"
#include "platform/nickel/common/book_menu.h"
#include "opening_context.h"
#include "touch_input.h"
#include "native_settings.h"
#include "storage/reader_storage.h"
#include "platform/nickel/common/native_lifecycle.h"
#include "platform/nickel/common/native_publication.h"
#include "platform/nickel/common/native_progress.h"
#include "platform/nickel/common/native_ui.h"
#include "support/diagnostics.h"
#include "platform/nickel/common/native_startup.h"
#include "platform/nickel/common/reader_environment.h"
#include "platform/nickel/common/reader_coordinator.h"
#include <NickelHook.h>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QScreen>
#include <QTimer>
#include <dlfcn.h>
#include <utility>
#ifdef KOBALT_READER_HARDWARE
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace kobalt::nickel4;
using kobalt::storage::readJson;
using kobalt::storage::writeJson;
using namespace kobalt::nickel;
ReaderEnvironment &environment() {
    static ReaderEnvironment value;
    return value;
}
extern "C" __attribute__((visibility("default"))) bool kobalt_home_key_filter(void *filter, QObject *watched,
                                                                              QKeyEvent *event) {
    if (kobalt::nickel::interceptHomeKey(event)) {
        return true;
    }
    return nativeHomeKeyFilter(filter, watched, event);
}

std::function<bool(const void *, const kobalt::nickel4::OpeningContext &)> beginOpening;
bool alternativeEnabled();
bool nativeInitialized = false;
bool nativeFinishing = false;
std::function<void()> nativeInitializationFinished;
extern "C" __attribute__((visibility("default"))) void kobalt_finish_initialization(void *application) {
    kobalt::diagnostic("native-initialization-enter");
    nativeFinishing = true;
    nativeFinishInitialization(application);
    nativeFinishing = false;
    kobalt::diagnostic("native-initialization-return");
    // Wait until Nickel has finished wiring its native workflows. A lazy
    // singleton getter cannot prove that initialization is complete.
    nativeInitialized = true;
    if (nativeInitializationFinished)
        nativeInitializationFinished();
}
extern "C" __attribute__((visibility("default"))) void
kobalt_show_reader(void *mixin, const void *volume, const void *keys, void *library, const void *bookmark) {
    // Firmware 4 passes the accepted opening as separate arguments. They are
    // valid only during this call, so the controller retains its own copies.
    if (kobalt::readerChoice(volume, alternativeEnabled()) && beginOpening &&
        beginOpening(volume, {keys, library, bookmark}))
        return;
    nativeShowReader(mixin, volume, keys, library, bookmark);
}
extern "C" __attribute__((visibility("default"))) void kobalt_continue_using(void *manager) {
    kobalt::diagnostic("usb-declined");
    nativeContinueUsing(manager);
    kobalt::NativeProgress::disconnected();
    kobalt::NativeLifecycle::disconnected();
}
extern "C" __attribute__((visibility("default"))) void kobalt_unplugged(void *manager) {
    kobalt::diagnostic("usb-unplugged");
    nativeUnplugged(manager);
    kobalt::NativeProgress::disconnected();
    kobalt::NativeLifecycle::disconnected();
}

bool enabled() {
#ifdef KOBALT_READER_HARDWARE
    return QFile::exists(KOBALT_CONFIG_DIR "/uninstall");
#else
    return QFile::exists("/instance/kobalt-enabled");
#endif
}
// A reader preference is independent of the installation and recovery markers.
bool alternativeEnabled() {
    if (!QFile::exists(selectionPath))
        return true;
    return readJson(selectionPath).value("enabled").toBool(false);
}
bool setAlternativeEnabled(bool on) {
    auto config = readJson(selectionPath);
    config["enabled"] = on;
    return writeJson(selectionPath, config);
}
#ifdef KOBALT_READER_HARDWARE
bool armFlag(const char *path, const QByteArray &message) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;
    bool ok = ::write(fd, message.constData(), message.size()) == message.size();
    if (::fsync(fd) != 0)
        ok = false;
    if (::close(fd) != 0)
        ok = false;
    const int directory = ::open(KOBALT_CONFIG_DIR, O_RDONLY | O_DIRECTORY);
    if (directory < 0)
        return false;
    if (::fsync(directory) != 0)
        ok = false;
    if (::close(directory) != 0)
        ok = false;
    return ok;
}
bool supportedFirmware() {
    QFile version("/mnt/onboard/.kobo/version");
    if (!version.open(QIODevice::ReadOnly))
        return false;
    const auto fields = version.readAll().trimmed().split(',');
    if (fields.size() != 6)
        return false;
    const auto release = fields[2].split('.');
    bool ok = false;
    return release.size() >= 2 && release[0] == "4" && release[1].toInt(&ok) >= 38 && ok;
}
bool uninstall() {
    // The absent uninstall flag is NickelHook's normal removal trigger.
    // unlink also preserves real errors without resolving an absent path.
    const auto remove = [](const char *path) {
        return ::unlink(path) == 0 || errno == ENOENT;
    };
    const bool active = remove(activePath);
    const bool startup = remove(startupPath);
    return remove(KOBALT_CONFIG_DIR "/uninstall") && active && startup;
}
#endif
// Startup owns no book request. An opener retries on its original stack, so
// the native opening arguments never survive the call that supplied them.
class Startup : public QObject {
public:
    explicit Startup(QObject *parent) : QObject(parent) {
        elapsed.start();
#ifndef KOBALT_READER_HARDWARE
        test = readJson("/instance/kobalt-startup-test.json");
#endif
        connect(&retry, &QTimer::timeout, this, [this] {
            if (++ticks >= 120 && nativeInitialized)
                retry.stop();
            attempt(false);
            if (!controller && !rejected && ticks % 20 == 0) {
                kobalt::diagnostic(
                    "startup-state",
                    kobalt::qt::object(
                        {{"native_initialized", nativeInitialized},
                         {"native_finishing", nativeFinishing},
                         {"power_guard_known", kobalt::nickel::startup_detail::constructionGuard() != nullptr},
                         {"power_constructed", kobalt::nickel::startup_detail::existingPower() != nullptr},
                         {"native_state_ready", kobalt::nickel::nativeStartupReady()},
                         {"reason", lastWait},
                         {"ticks", ticks},
                         {"elapsed_ms", double(elapsed.elapsed())}}),
                    false);
            }
            if (ticks >= 120 && nativeInitialized && !controller && !rejected)
                kobalt::diagnostic("startup-deferred", kobalt::qt::object({{"reason", lastWait}}));
        });
        retry.setInterval(250);
    }
    void start() {
        retry.start();
        kobalt::qt::later(0, this, [this] {
            attempt(false);
        });
    }
    bool open(const void *volume, const kobalt::nickel4::OpeningContext &context) {
        kobalt::diagnostic("startup-opener",
                           kobalt::qt::object({{"native_initialized", nativeInitialized},
                                               {"controller_ready", bool(controller)},
                                               {"rejected", rejected}}),
                           false);
        if (initializing) {
            reentered = true;
            return false;
        }
        attempt(true);
        return controller && controller->begin([&] {
            auto request = std::make_unique<kobalt::NativeOpening>();
            if (!request->retain(volume, &context))
                return std::unique_ptr<kobalt::NativeOpening>();
            return request;
        });
    }
    void attempt(bool fromOpener) {
        if (controller || rejected || initializing)
            return;
        if (!nativeInitialized && !nativeFinishing && kobalt::nickel::nativeStartupReady()) {
            nativeInitialized = true;
            kobalt::diagnostic("startup-recovered", kobalt::qt::object({{"reason", "native-state"},
                                                                        {"elapsed_ms", double(elapsed.elapsed())}}));
        }
        if (!nativeInitialized || nativeFinishing) {
            if (elapsed.elapsed() >= 30000) {
                rejected = true;
                retry.stop();
                writeJson(sessionReport,
                          kobalt::qt::object({{"status", "stock"}, {"reason", "Native initialization not observed"}}));
                kobalt::diagnostic("startup-failed", kobalt::qt::object({{"reason", "native-initialization"}}));
                return;
            }
            waiting("native-initialization");
            return;
        }
        auto *screen = QGuiApplication::primaryScreen();
        if (!screen || screen->size().isEmpty()) {
            waiting("screen");
            return;
        }
#ifdef KOBALT_READER_HARDWARE
        if (screen->size().width() >= screen->size().height() || screen->devicePixelRatio() != 1) {
            waiting("portrait-screen");
            return;
        }
#else
        if (test.value("reject").toBool()) {
            rejected = true;
            retry.stop();
            waiting("test-rejected");
            return;
        }
        if (test.value("defer_until_opener").toBool() && !fromOpener) {
            waiting("test-opener");
            return;
        }
        if (test.value("wait_attempts").toInt() > 0) {
            test["wait_attempts"] = test.value("wait_attempts").toInt() - 1;
            waiting("test-screen");
            return;
        }
#endif
#ifdef KOBALT_READER_HARDWARE
        // Waiting for Nickel or a usable screen runs no Kobalt initialization.
        // Arm only the synchronous attempt, so deferral cannot look like a crash.
        if (!armFlag(startupPath, "Kobalt startup did not complete.\n")) {
            rejected = true;
            retry.stop();
            kobalt::diagnostic("startup-arm-failed");
            return;
        }
#endif
        initializing = true;
        reentered = false;
        const auto finish = [this]([[maybe_unused]] bool deferred = true) {
#ifdef KOBALT_READER_HARDWARE
            if (deferred && !QFile::remove(startupPath)) {
                rejected = true;
                retry.stop();
                kobalt::diagnostic("startup-disarm-failed");
            }
#endif
            initializing = false;
        };
        if (!QDir().mkpath(QString::fromLatin1(configDir)) ||
            (!QFile::exists(selectionPath) && !writeJson(selectionPath, kobalt::qt::object({{"enabled", true}})))) {
            waiting("preferences");
            finish();
            return;
        }
        CoordinatorBindings bindings{enabled};
        bindings.automaticBookOpening = alternativeEnabled;
        bindings.setAutomaticBookOpening = setAlternativeEnabled;
        bindings.installInput = kobalt::nickel4::installTouchInput;
        if (nativeHomeKeyFilter && dlsym(RTLD_DEFAULT, "_ZN20MainWindowController14sharedInstanceEv") &&
            dlsym(RTLD_DEFAULT, "_ZN20MainWindowController5clearEP18AbstractControllerb")) {
            bindings.goHome = [] {
                const auto instance =
                    reinterpret_cast<void *(*)()>(dlsym(RTLD_DEFAULT, "_ZN20MainWindowController14sharedInstanceEv"));
                const auto clear = reinterpret_cast<void (*)(void *, void *, bool)>(
                    dlsym(RTLD_DEFAULT, "_ZN20MainWindowController5clearEP18AbstractControllerb"));
                // This is the navigation used by Nickel's HomePageKeyFilter.
                clear(instance(), nullptr, false);
            };
        }
        bindings.publicationKeys = [](const kobalt::NativeOpening &opening) {
            return opening.keys();
        };
        bindings.publicationMismatch = [](const kobalt::NativePublication &publication, const QString &identity,
                                          const QString &path) {
            kobalt::diagnostic("native-publication-failed",
                               kobalt::qt::object({{"stage", "opening-mismatch"},
                                                   {"identity_matches", publication.identity() == identity},
                                                   {"path_matches", publication.path() == path}}));
        };
        auto *candidate = new ReaderCoordinator(environment(), bindings, qApp);
        if (!candidate->available() || reentered) {
            delete candidate;
            waiting(reentered ? "reentrant-opening" : "lifecycle");
            finish();
            return;
        }
        if (reentered) {
            delete candidate;
            waiting("reentrant-opening");
            finish();
            return;
        }
#ifdef KOBALT_READER_HARDWARE
        if (!QFile::remove(startupPath)) {
            delete candidate;
            waiting("startup-guard");
            finish();
            return;
        }
#endif
        watchReadingSettings(candidate, [](QWidget *view) {
            kobalt::installNativeReaderSetting(view, alternativeEnabled, setAlternativeEnabled);
        });
        controller = candidate;
        retry.stop();
        finish(false);
        writeJson(sessionReport, kobalt::qt::object({{"status", "ready"}}));
        kobalt::diagnostic("ready",
                           kobalt::qt::object({{"startup_ms", double(elapsed.elapsed())}, {"opener", fromOpener}}));
        nh_log("reader: native controller ready");
    }

private:
    QPointer<ReaderCoordinator> controller;
    QTimer retry;
    QElapsedTimer elapsed;
    QString lastWait;
    bool initializing = false, reentered = false, rejected = false;
    int ticks = 0;
#ifndef KOBALT_READER_HARDWARE
    QJsonObject test;
#endif
    void waiting(const QString &reason) {
        if (lastWait == reason)
            return;
        lastWait = reason;
        kobalt::diagnostic("startup-wait",
                           kobalt::qt::object({{"reason", reason}, {"elapsed_ms", double(elapsed.elapsed())}}));
    }
};

int initialize() {
    kobalt::initializeDiagnostics(QString::fromLatin1(configDir));
    kobalt::diagnostic("startup-hooks",
                       kobalt::qt::object({{"finish_hook_available", bool(nativeFinishInitialization)},
                                           {"finish_next_is_stock",
                                            reinterpret_cast<void *>(nativeFinishInitialization) ==
                                                dlsym(RTLD_DEFAULT, "_ZN18Nickel3Application20finishInitializationEv")},
                                           {"native_initialized", nativeInitialized},
                                           {"application_available", bool(qApp)}}));
#ifdef KOBALT_READER_TEMPORARY
    if (!armFlag(KOBALT_CONFIG_DIR "/uninstall-now", "Temporary reader: remove on the next Nickel start.\n")) {
        kobalt::diagnostic("removal-arm-failed");
        nh_log("reader: cannot arm removal; disabling library");
        nh_delete_file("/usr/local/Kobo/libkobalt.so.failsafe");
        return 0;
    }
#endif
    if (!enabled()) {
        kobalt::diagnostic("disabled");
        return 0;
    }
#ifdef KOBALT_READER_HARDWARE
    if (QFile::exists(startupPath)) {
        kobalt::diagnostic("interrupted-startup");
        nh_log("reader: interrupted startup; leaving stock Nickel active");
        return 0;
    }
#endif
    if (!firmwareSupported()) {
        kobalt::diagnostic("firmware-rejected");
        return 0;
    }
    if (!kobalt::NativeOpening::available() || !kobalt::NativePublication::available() ||
        !kobalt::NativeProgress::available() || !kobalt::NativeLifecycle::powerAvailable()) {
        kobalt::diagnostic("native-api-unavailable");
        return 0;
    }
#ifdef KOBALT_READER_HARDWARE
    if (!supportedFirmware()) {
        kobalt::diagnostic("hardware-rejected");
        writeJson(sessionReport,
                  kobalt::qt::object({{"status", "stock"}, {"reason", "Requires firmware 4.38 or later"}}));
        return 0;
    }
#ifdef KOBALT_READER_TEMPORARY
    kobalt::diagnostic("installation", kobalt::qt::object({{"temporary", true}}));
#else
    kobalt::diagnostic("installation", kobalt::qt::object({{"temporary", false}}));
#endif
#endif
    auto *startup = new Startup(qApp);
    beginOpening = [startup](const void *volume, const kobalt::nickel4::OpeningContext &context) {
        return startup->open(volume, context);
    };
    nativeInitializationFinished = [startup] {
        kobalt::qt::later(0, startup, [startup] {
            startup->attempt(false);
        });
    };
    kobalt::configureBookMenu(
        [] {
            return enabled() && bool(beginOpening);
        },
        alternativeEnabled);
    startup->start();
    kobalt::diagnostic("startup-listening");
    return 0;
}
}

namespace kobalt::nickel4 {
int initializeReader() {
    return initialize();
}
#ifdef KOBALT_READER_HARDWARE
bool uninstallReader() {
    return uninstall();
}
#endif
}
