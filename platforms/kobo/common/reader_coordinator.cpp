#include "diagnostics.h"
#include "clocks.h"
#include "native_tile_cover.h"
#include "reading/appearance_settings.h"
#include "reader_coordinator.h"
#include "home_sleep_screen.h"
#include "window_presentation.h"
#include "usb_transition.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "book_menu.h"
#include "browser_handoff.h"
#include "app/reader_window.h"
#include "reading/reading_book.h"
#include "reading/rendering_mode.h"
#include "storage/profile_database.h"
#include "storage/annotation_database.h"
#include "storage/reader_storage.h"
#include "storage/preview_store.h"
#include "storage/sleep_snapshot.h"
#include "reading/reader_session.h"
#include "engine/engine_activity.h"
#include "native_lifecycle.h"
#include "native_authorization.h"
#include "native_publication.h"
#include "native_progress.h"
#include "native_reading_session.h"
#include "native_dictionary.h"
#include "publication/native_resources.h"
#include "publication/local_resources.h"
#include "reading_bridge.h"
#include "engine_positions.h"
#include "native_ui.h"
#include "native_fallback.h"
#include "native_font_scale.h"
#include "platform/api/reader_orientation.h"
#include "orientation.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "reader_environment.h"
#include "reader_host_services.h"
#include "ui/style/fonts.h"
#include "app/reader_interface.h"
#include "app/messages/opening_message.h"
#include "app/messages/recovery_message.h"
#ifdef KOBALT_SCENARIO_PLAYER
#  include "app/messages/benchmark_message.h"
#endif
#include "platforms/kobo/display/screen_density.h"
#include <NickelHook.h>
#include <QDir>
#include <QApplication>
#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QKeyEvent>
#include <QScreen>
#ifdef KOBALT_SCENARIO_PLAYER
#  include "sync/kosync.h"
#  include "scenario/player.h"
#  include "native_sync_network.h"
#  include "scenario/sleep_persistence.h"
#  include <QFileInfo>
#  include <QJsonArray>
#  include <QJsonDocument>
#  include <QMouseEvent>
#  include <QSaveFile>
#  include <QWindow>
#  include <dlfcn.h>
#endif
#include <QThread>
#include <QJsonDocument>
#include <cstring>
#include <utility>

namespace kobalt::nickel {
using kobalt::storage::readJson;
using kobalt::storage::writeJson;

namespace {
// A profile path as the user finds it on the Kobo's storage over USB.
QString usbPath(QString path)
{
    if (path.startsWith(QLatin1String("/mnt/onboard/")))
        path.remove(0, int(qstrlen("/mnt/onboard/")));
    return path;
}

QPointer<ReaderCoordinator> activeCoordinator;
#if defined(KOBALT_BENCHMARK_API) || defined(KOBALT_SCENARIO_PLAYER)
bool benchmarkTypographyEnabled = false;

bool benchmarkPublication(const QString &identity)
{
    return identity == QStringLiteral("file:///mnt/onboard/kobalt-benchmark.kepub.epub");
}

#endif
#ifdef KOBALT_SCENARIO_PLAYER
// Nickel's own store sync, for the evaluation player. On firmware 4.46.23836
// and 6.0.279225 the status-bar sync button calls N3SyncManager::sync(). The
// call returns once the sync has started, and Nickel's workers do the rest.
struct NativeSync
{
    QObject *(*instance)() = reinterpret_cast<decltype(instance)>(
            dlsym(RTLD_DEFAULT, "_ZN13N3SyncManager14sharedInstanceEv"));
    void (*sync)(QObject *) = reinterpret_cast<decltype(sync)>(dlsym(RTLD_DEFAULT,
                                                                     "_ZN13N3SyncManager4syncEv"));
    bool (*syncing)(const QObject *) = reinterpret_cast<decltype(syncing)>(
            dlsym(RTLD_DEFAULT, "_ZNK15SyncManagerBase9isSyncingEv"));

    QObject *manager() const { return instance && sync && syncing ? instance() : nullptr; }
};

// The radio calls a scenario uses to start from Wi-Fi off. off() is the call
// that KOSync's release makes after a sync it connected for.
struct NativeRadio
{
    QObject *(*instance)() = reinterpret_cast<decltype(instance)>(
            dlsym(RTLD_DEFAULT, "_ZN15WirelessManager14sharedInstanceEv"));
    void (*off)(QObject *) = reinterpret_cast<decltype(off)>(dlsym(RTLD_DEFAULT,
                                                                   "_ZN15WirelessManager3offEv"));
    bool (*up)(QObject *) = reinterpret_cast<decltype(up)>(dlsym(RTLD_DEFAULT,
                                                                 "_ZNK15WirelessManager2upEv"));

    QObject *manager() const { return instance && off && up ? instance() : nullptr; }
};
#endif
} // namespace

#if defined(KOBALT_BENCHMARK_API) || defined(KOBALT_SCENARIO_PLAYER)
extern "C" __attribute__((visibility("default"))) int
kobalt_benchmark_typography_v1(const char *contentId, int enable)
{
    if (!qApp || QThread::currentThread() != qApp->thread() || !contentId
        || (enable != 0 && enable != 1) || !benchmarkPublication(QString::fromUtf8(contentId))
        || !activeCoordinator || (enable && !activeCoordinator->idleForBenchmark()))
        return 0;
    if (!enable && !activeCoordinator->clearBenchmarkReader())
        return 0;
    benchmarkTypographyEnabled = enable != 0;
    return 1;
}

extern "C" __attribute__((visibility("default"))) int
kobalt_benchmark_state_v1(const char *contentId, char *buffer, int capacity)
{
    if (!qApp || QThread::currentThread() != qApp->thread() || !contentId || !buffer
        || capacity <= 0 || !activeCoordinator)
        return 0;
    const auto state = activeCoordinator->benchmarkState(QString::fromUtf8(contentId));
    if (state.isEmpty())
        return 0;
    const auto json = QJsonDocument(state).toJson(QJsonDocument::Compact);
    if (json.size() >= capacity)
        return 0;
    std::memcpy(buffer, json.constData(), json.size());
    buffer[json.size()] = '\0';
    return json.size();
}

extern "C" __attribute__((visibility("default"))) int
kobalt_benchmark_command_v1(const char *contentId, int command, int argument)
{
    if (!qApp || QThread::currentThread() != qApp->thread() || !contentId || !activeCoordinator)
        return 0;
    return activeCoordinator->benchmarkCommand(QString::fromUtf8(contentId), command, argument);
}

extern "C" __attribute__((visibility("default"))) int
kobalt_benchmark_reader_v1(const char *contentId, int enable)
{
    if (!qApp || QThread::currentThread() != qApp->thread() || !contentId
        || (enable != 0 && enable != 1) || !benchmarkPublication(QString::fromUtf8(contentId))
        || !benchmarkTypographyEnabled || !activeCoordinator)
        return 0;
    return activeCoordinator->selectBenchmarkReader(enable != 0);
}

bool ReaderCoordinator::selectBenchmarkReader(bool enabled)
{
    return idleForBenchmark() && bindings_.setBenchmarkReader
            && bindings_.setBenchmarkReader(enabled ? 1 : 0);
}

bool ReaderCoordinator::clearBenchmarkReader()
{
    // Cleanup may run while the reader finishes its closing jobs.
    return !bindings_.setBenchmarkReader || bindings_.setBenchmarkReader(-1);
}

QJsonObject ReaderCoordinator::benchmarkState(const QString &identity) const
{
    if (!benchmarkTypographyEnabled || !benchmarkPublication(identity) || identity != run.identity
        || !run.reader)
        return {};
    auto state = run.reader->benchmarkState();
    if (!state.isEmpty())
        state["book"] = identity;
    return state;
}

bool ReaderCoordinator::benchmarkCommand(const QString &identity, int command, int argument)
{
    return benchmarkTypographyEnabled && benchmarkPublication(identity) && identity == run.identity
            && run.reader && run.reader->benchmarkCommand(command, argument);
}

#endif

ReaderCoordinator::ReadingRun::ReadingRun() = default;
ReaderCoordinator::ReadingRun::~ReadingRun() = default;

ReaderCoordinator::ReaderCoordinator(ReaderEnvironment &environment, CoordinatorBindings bindings,
                                     QObject *parent)
    : QObject(parent),
      environment_(environment),
      bindings_(bindings),
      lifecycle(new kobalt::NativeLifecycle(this)),
      authorization(new kobalt::NativeAuthorization(this))
{
    activeCoordinator = this;
    homeSleep = std::make_unique<HomeSleepScreen>(environment_.paths, this);
    authorization->revoked = [this](kobalt::NativeAuthorization::Reason) {
        const QPointer<ReaderWindow> reader = run.reader;
        if (reader)
            reader->revokeNativeReading();
        homeAfterClose = false;
        homeSleep->forget();
        if (run.sourceLease)
            run.sourceLease->revoke();
        timer.stop();
        if (reader)
            reader->hide();
        // Save the local checkpoint and release all content without native
        // progress writeback or retrying the retained opening request.
        disposeForTransition([this] { status("authorization-lost"); }, false);
    };
    lifecycle->readerInputActive = [this] { return run.reader && run.reader->isVisible(); };
    lifecycle->beforeStandby = [this] {
        return busy || !run.reader || run.reader->flushBeforeStandby();
    };
    // busy covers the whole opening, also before and after its engine jobs.
    // A fallback or error dialog also sets busy, but waits for the user.
    lifecycle->openingWorkPending = [this] { return busy && !run.fallbackAlert; };
    lifecycle->sleepWorkPending = [this] {
        return run.reader && (run.reader->pendingNetworkSync() || run.reader->pendingStorageSave());
    };
    // Outside the reader, Kobalt's screen still uses its own type. The fonts
    // load with the first book otherwise. Load them once, only when the home
    // screen draws, and never while a reader owns them. Input cannot arrive
    // while the device sleeps or powers off.
    homeSleep->prepareFonts = [this] {
        if (interfaceFontsLoaded || busy || run.reader || !environment_.assets.available())
            return;
        interfaceFontsLoaded = kobalt::ui::initializeInterfaceFonts(environment_.assets);
    };
    lifecycle->sleep = [this] {
        if (lifecycle->shuttingDown())
            return;
        homeSleep->clear();
        homeAfterClose = false;
        authorization->setBlocked(true);
        timer.stop();
        const bool browser = run.reader && BrowserHandoff::activeFor(run.reader);
        const bool homeOwnsSleep = busy || !run.reader || browser;
        // A preparing reader can outlive asynchronous disposal. Prepare its
        // private backdrop before disposal changes the run or its busy state.
        // Once closed, identity is only retained for reuse: consult Nickel then.
        if (bindings_.enabled() && homeOwnsSleep)
            homeSleep->prepareHomeScreen(false, busy || run.reader ? run.identity : QString());
        if (busy) {
            disposeForTransition([] { });
        } else if (run.reader) {
            // The browser owns the screen while this reader is already
            // hidden. Keep its saved position without creating a sleep cover.
            if (BrowserHandoff::activeFor(run.reader))
                run.reader->checkpoint();
            else
                run.reader->sleepReader();
        } else if (run.fallbackAlert) {
            disposeForTransition([] { });
        }
    };
    lifecycle->sleepViewReady = [this] {
        if (run.reader && !busy && !BrowserHandoff::activeFor(run.reader))
            run.reader->sleepViewReady();
        // Preparation may finish disposing between the sleep and view signals.
        // Present a prepared Home frame independently of the remaining reader.
        homeSleep->present();
    };
    lifecycle->beforeWake = [this] {
        homeSleep->clear();
        if (run.reader && !busy) {
            run.reader->prepareWake();
        }
    };
    lifecycle->cancelWake = [this] {
        if (run.reader && !busy) {
            run.reader->cancelWake();
        }
    };
    lifecycle->wake = [this] {
        homeSleep->clear();
        if (lifecycle->draining || busy) {
            return;
        }
        authorization->setBlocked(false);
        timer.start();
        if (run.reader || run.session) {
            const bool allowed = authorization->checkNow();
            if (allowed && run.reader && !BrowserHandoff::activeFor(run.reader)) {
                run.reader->wakeReader();
                // A sync can finish while the device sleeps. Check after the wake paint.
                checkProgressSoon();
            } else if (!allowed && (run.reader || run.session) && !lifecycle->draining) {
                // A deferred or unavailable check must not leave an invisible
                // reader holding Nickel's backing store indefinitely.
                kobalt::diagnostic("resume-authorization-unavailable", {},
                                   kobalt::DiagnosticSeverity::Warning);
                // An unverified account must not receive native progress or time writes.
                disposeForTransition([this] { status("authorization-unavailable"); }, false);
            }
        }
    };
    lifecycle->prepareUsb = [this](std::function<void()> ready) {
        homeSleep->clear();
        homeAfterClose = false;
        authorization->setBlocked(true);
        timer.stop();
        prepareUsbTransition(
                busy ? nullptr : run.reader.data(), cleanupCallbacks,
                [this] {
                    kobalt::diagnostic("usb-release-start");
                    disposeForTransition([] { });
                },
                std::move(ready));
    };
    lifecycle->storageReleased = [this] {
        environment_.invalidateAssets();
        // The files can change while the computer has the storage.
        interfaceFontsLoaded = false;
    };
    lifecycle->storageRestored = [this] {
        environment_.verifyAssets(this, "usb", [this](bool valid) {
            if (valid)
                kobalt::ui::prepareOpeningFonts(environment_.assets);
        });
    };
    lifecycle->preparePowerScreen = [this](NativeLifecycle::Shutdown mode) {
        homeSleep->clear();
        if (mode == NativeLifecycle::Shutdown::Reboot)
            return;
        // The reader hides after handing its sleep frame to Nickel, but still
        // owns this book and the clean backdrop needed for power-off.
        if (run.reader && !busy && !BrowserHandoff::activeFor(run.reader))
            homeSleep->preparePowerOff(run.reader->powerScreenSnapshot());
        else if (bindings_.enabled())
            homeSleep->prepareHomeScreen(true, busy || run.reader ? run.identity : QString());
    };
    lifecycle->prepareShutdown = [this](std::function<void()> ready) {
        homeAfterClose = false;
        authorization->setBlocked(true);
        timer.stop();
        // Revocation can dispose the reader while its bookmark is being
        // exported. Keep the shutdown continuation with the shared drain.
        // Nickel only freezes Kobalt at power-off, so close the profile first.
        cleanupCallbacks.append([ready = std::move(ready)] {
            kobalt::storage::closeProfile(true);
            ready();
        });
        if (run.reader && !busy)
            run.reader->prepareShutdown([this] { disposeForTransition([] { }); },
                                        lifecycle->batteryLowShutdown());
        else
            disposeForTransition([] { });
    };
    if (bindings_.installInput)
        bindings_.installInput(this);
    connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    // Nickel's sleep, wake, USB and sync signals drive the reader. This check
    // repeats the visibility checks in case a transition left the reader in the
    // wrong state. It reads Nickel's saved position only after a signal or a
    // failed read, or once a minute when Nickel's signals are unavailable.
    // A visible reader runs it on its clock tick (minuteTicked); this timer is
    // the fallback for a hidden or sleeping reader, and slightly longer than a
    // minute so a ticking clock always comes first.
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(65000);
    if (!lifecycle->blocked())
        timer.start();
    // Read and check the interface faces on a worker thread before any tap,
    // so the first opening view does not wait for storage.
    if (available()) {
        if (environment_.assets.available())
            kobalt::ui::prepareOpeningFonts(environment_.assets);
        openingDark = storage::readPreferences(environment_.paths.preferences())
                              .object.value("dark_mode")
                              .toBool(false);
    }
#ifdef KOBALT_SCENARIO_PLAYER
    // A device set up for scenarios must not sync the benchmark book, also
    // not when a person opens it to start the run.
    if (QFile::exists(environment_.paths.directory + "/scenarios/scenarios.json"))
        kobalt::kosync::pauseAutomatic(true);
#endif
}

void forgetHomeSleepScreen()
{
    if (activeCoordinator)
        activeCoordinator->forgetSleepScreen();
    else if (!NativeLifecycle::suspended())
        storage::removeSleepSnapshot(
                storage::Paths{ QString::fromLatin1(configDir) }.sleepScreen());
}

void ReaderCoordinator::forgetSleepScreen()
{
    if (!lifecycle->storageBlocked() && !lifecycle->shuttingDown())
        homeSleep->forget();
}

bool interceptHomeKey(QKeyEvent *event)
{
    return activeCoordinator && activeCoordinator->handleHomeKey(event);
}

bool ReaderCoordinator::handleHomeKey(QKeyEvent *event)
{
    if (!bindings_.goHome
        || (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)) {
        return false;
    }
    if (event->key() != Qt::Key_Home || event->modifiers() != Qt::NoModifier || lifecycle->blocked()
        || kobalt::NativeLifecycle::suspended()) {
        return false;
    }
    const bool readerVisible =
            run.reader && run.reader->isVisible() && !BrowserHandoff::activeFor(run.reader);
    const bool openingVisible =
            (run.loading && run.loading->window() && run.loading->window()->isVisible())
            || (run.fallbackAlert && run.fallbackAlert->isVisible());
    if (!homeAfterClose && !readerVisible && !openingVisible) {
        return false;
    }
    // Consume both edges. Nickel must navigate only after the normal close
    // has saved progress and released the book, including any exit choices.
    if (event->type() == QEvent::KeyPress && !event->isAutoRepeat() && !homeAfterClose) {
        homeAfterClose = true;
        if (readerVisible) {
            run.reader->close();
        } else {
            disposeForTransition([this] { finishHomeNavigation(); });
        }
    }
    event->accept();
    return true;
}

void ReaderCoordinator::finishHomeNavigation()
{
    if (std::exchange(homeAfterClose, false) && !lifecycle->blocked()
        && !kobalt::NativeLifecycle::suspended()) {
        bindings_.goHome();
    }
}

bool ReaderCoordinator::idleForBenchmark() const
{
    return !busy && !run.reader && !run.fallbackAlert && !lifecycle->draining;
}

void ReaderCoordinator::finishPreviewMigration()
{
    const auto directory = environment_.paths.previews();
    if (storage::previewMigrationNeedsScan(directory))
        lifecycle->scanLibrary([directory] { storage::completePreviewMigration(directory); });
}

bool ReaderCoordinator::available() const
{
    return lifecycle->connected() && authorization->available()
            && kobalt::NativeReadingSession::available();
}

bool ReaderCoordinator::begin(const RetainOpening &retainOpening)
{
    QElapsedTimer openingStarted;
    openingStarted.start();
    if (!available() || !environment_.assets.available())
        return false;
    if (lifecycle->blocked() || lifecycle->draining)
        return true;
    if (!bindings_.enabled() || kobalt::NativeLifecycle::suspended())
        return false;
    // Ignore another opening while this request owns the modal loading UI.
    if (busy || run.reader || run.fallbackAlert)
        return true;
    auto request = retainOpening();
    if (!request)
        return false;
    if (!readerBookFormatSupported(request->volume()))
        return false;
    // A preload that is still running gives way to the book that was tapped.
    // One that finished is the kept book in run.session.
    cancelPreload();
    run.opening = std::move(request);
    busy = true;
    watch();
    run.openingTimer = openingStarted;
    run.openingTimings = qt::object({ { "handoff_ms", double(openingStarted.elapsed()) } });
    QWidget *background = QApplication::activeWindow();
    for (auto *window : QApplication::topLevelWidgets())
        if (window->isVisible() && window->inherits("MainWindowView")) {
            background = window;
            break;
        }
    run.interface = &kobalt::readerInterface();
    run.loading = run.interface->createOpeningSurface(background, openingDark,
                                                      readerHostServices().presentation);
    ignoreNativeFontScale(run.loading->window());
    connect(run.loading->window(), &QDialog::rejected, this, [this] { cancelOpening(); });
    const auto epoch = generation;
    run.loading->afterFirstPaint([this, epoch] {
        if (epoch != generation || !busy || !run.opening || !run.loading)
            return;
        const auto elapsed = run.openingTimer.elapsed();
        run.openingTimings["first_frame_ms"] = double(elapsed);
        diagnostic("opening-first-frame", { { "elapsed_ms", double(elapsed) } });
        prepareOpeningControls();
    });
    run.loading->window()->show();
    run.loading->window()->raise();
    return true;
}

// How long a book with its hidden window shows only the logo before the
// loading controls and cover appear.
static constexpr int preparedLoadingDelay = 600;
// How long after a close the book is prepared again: just long enough for
// Nickel to show Home first.
static constexpr int reopenPreparationDelay = 300;

void ReaderCoordinator::prepareOpeningControls()
{
    QString nextIdentity, nextPath;
    const auto route = readerBookCandidateRoute(run.opening->volume(), &nextIdentity, &nextPath);
    run.openingTimings["candidate_ms"] = double(run.openingTimer.elapsed());
    if (route == ReaderBookRoute::Unsupported) {
        auto fallback = std::move(run.opening);
        release();
        if (fallback && !lifecycle->blocked() && !lifecycle->draining
            && !NativeLifecycle::suspended())
            fallback->openStock();
        return;
    }
    const bool discardRetained = run.session && nextIdentity != run.identity;
    run.identity = nextIdentity;
    run.filePath = nextPath;
    // The logo splash has reached Qt's backing store. No font registration,
    // storage commit or live widget layout can hold up that first feedback.
    kobalt::markMemoryBase();
    if (!kobalt::ui::initializeOpeningFonts(environment_.assets)) {
        const bool assetsReady = environment_.assets.available();
        if (!assetsReady || !kobalt::ui::initializeInterfaceFonts(environment_.assets)) {
            missingAssets(assetsReady);
            return;
        }
    }
    // Each step until the book work starts, to find time spent outside Kobalt.
    const auto step = [this](const char *name) {
        diagnostic("opening-step", { { "step", QString::fromLatin1(name) },
                                     { "elapsed_ms", double(run.openingTimer.elapsed()) } });
    };
    step("fonts");
    const bool armed = writeJson(activePath, kobalt::qt::object({ { "book", run.identity } }));
    step("armed");
    if (!armed || !authorization->begin(run.identity)) {
        if (!armed)
            diagnostic("session-arm-failed", {}, DiagnosticSeverity::Error);
        auto fallback = std::move(run.opening);
        release();
        if (fallback && !lifecycle->blocked() && !lifecycle->draining
            && !NativeLifecycle::suspended())
            fallback->openStock();
        return;
    }
    step("authorized");
    const auto preferences = storage::readPreferences(environment_.paths.preferences()).object;
    openingDark = preferences.value("dark_mode").toBool(false);
    run.interface->setDensity(currentScreenDensity());
    run.interface->setLargeText(nativeFontScale());
    step("screen");
    // A book with its hidden window opens in well under a second. Keep the
    // logo and start at once; the loading controls and cover follow only
    // when the opening takes longer than that.
    const bool prepared = quiet.window && quiet.identity == run.identity;
    if (prepared) {
        const auto epoch = generation;
        const auto coverPath = nativeReaderCoverPath(run.opening->volume());
        kobalt::qt::later(preparedLoadingDelay, this, [this, epoch, preferences, coverPath] {
            if (epoch != generation || !busy || !run.loading || run.reader)
                return;
            diagnostic("opening-controls-late");
            {
                NativeFontScaleScope building;
                run.loading->prepareControls(preferences);
            }
            run.loading->loadCover(coverPath);
            run.loading->setProgress(kobalt::openingProgressPercent(run.openingStage,
                                                                    run.openingPercent));
        });
        ui::setInterfaceWeight(ui::heavierTextSteps);
    } else {
        NativeFontScaleScope building;
        ui::setInterfaceWeight(ui::heavierTextSteps);
        run.loading->prepareControls(preferences);
    }
    if (!prepared && nativeTileCoverProbeEnabled()) {
        const auto image = nativeTileCoverProbeImage(run.opening->volume());
        run.loading->window()->setProperty("kobalt_tile_cover_probe", true);
        run.loading->setCover(image);
        diagnostic("tile-cover-probe-seeded",
                   qt::object({ { "generation", double(generation) },
                                { "width", image.width() },
                                { "height", image.height() },
                                { "image_key", QString::number(image.cacheKey()) } }));
    }
    if (!prepared)
        run.loading->loadCover(nativeReaderCoverPath(run.opening->volume()));
    run.openingStage = OpeningStage::Content;
    run.openingPercent = -1;
    run.progressPainted = -1000;
    updateOpening(OpeningStage::Content, 0);
    const auto epoch = generation;
    step("controls");
    const auto continueOpening = [this, epoch, route, discardRetained] {
        if (epoch != generation || !busy || !run.opening || !run.loading)
            return;
        run.openingTimings["loading_frame_ms"] = double(run.openingTimer.elapsed());
        diagnostic("opening-step",
                   { { "step", QStringLiteral("after-paint") },
                     { "elapsed_ms", run.openingTimings.value("loading_frame_ms").toDouble() } });
        if (discardRetained) {
            if (run.sourceLease)
                run.sourceLease->cancel();
            run.session.reset();
            run.sourceLease.reset();
        }
        if (quiet.window && quiet.identity != run.identity)
            dropQuietReader(false);
        prepareOpening(route);
    };
    // The wait lets the loading controls paint before the work starts. A
    // prepared book shows only the logo, which has painted already, and on a
    // Kobo the wait for the next event pass took about 0.4 s.
    if (prepared)
        continueOpening();
    else
        run.loading->afterFirstPaint(continueOpening);
}

void ReaderCoordinator::missingAssets(bool assetsReady)
{
    kobalt::diagnostic(assetsReady ? "interface-fonts-unavailable" : "assets-unavailable");
    const bool filesystemRecovery =
            !assetsReady && environment_.assets.hasFilesystemRecoveryFiles();
    if (filesystemRecovery) {
        kobalt::diagnostic("asset-filesystem-recovery");
    }
    auto fallback = std::move(run.opening);
    release();
    run.fallbackAlert =
            kobalt::showMissingAssetsAlert(QStringLiteral(KOBALT_VERSION), filesystemRecovery);
    if (!run.fallbackAlert) {
        kobalt::diagnostic("assets-alert-unavailable", {}, kobalt::DiagnosticSeverity::Warning);
        status("stock");
        if (fallback && !lifecycle->blocked() && !lifecycle->draining
            && !kobalt::NativeLifecycle::suspended()) {
            fallback->openStock();
        }
        return;
    }
    busy = true;
    watch();
    const auto epoch = generation;
    connect(run.fallbackAlert.data(), &QDialog::finished, this, [this, epoch] {
        if (epoch == generation) {
            busy = false;
            run.fallbackAlert.clear();
            status("ready");
        }
    });
    status("assets-unavailable");
}

void ReaderCoordinator::prepareOpening(kobalt::ReaderBookRoute candidate)
{
    // Nickel initializes its own database and imports books during startup.
    // Wait until a reading request, with the loading view already painted,
    // before opening our database or importing the profile's saved files.
    if (!profilePrepared) {
        kobalt::storage::ProfileRecovery recovery;
        profilePrepared = kobalt::storage::prepareProfile(environment_.paths, &recovery);
        if (recovery.storageFailed) {
            fail(strings::get("opening.storage_failed_detail"), kobalt::OpeningFallback::Storage);
            return;
        }
        if (recovery.recovered())
            pendingNotice = kobalt::profileRecoveryNotice(
                    recovery, usbPath(environment_.paths.directory + "/backups"));
    }
    if (!historyProfile && kobalt::annotationDatabaseEnabled()) {
        QString error;
        if (profilePrepared)
            historyProfile =
                    kobalt::ReadingHistory::openProfile(environment_.paths.directory, &error);
        if (!historyProfile) {
            fail(error == "profile-in-use" ? strings::get("error.profile_already_in_use")
                                           : strings::get("error.saved_data_unavailable"));
            return;
        }
    }
    status("loading");
    const bool assetsReady = environment_.assets.available();
    bool fontsChanged = false;
    // Loaded faces stay valid until USB releases the assets. Loading them again
    // would read and hash the same files and reset the private text caches.
    if (!interfaceFontsLoaded || !assetsReady)
        interfaceFontsLoaded = assetsReady
                && kobalt::ui::initializeInterfaceFonts(environment_.assets, &fontsChanged);
    if (!interfaceFontsLoaded) {
        missingAssets(assetsReady);
        return;
    }
    if (fontsChanged) {
        run.loading->refreshFonts();
    }
    QString identity, path;
    const auto route = candidate == kobalt::ReaderBookRoute::Direct
            ? kobalt::readerBookRoute(run.opening->volume(), &identity, &path)
            : candidate;
    if (route == kobalt::ReaderBookRoute::Protected) {
        fail(QString(), kobalt::OpeningFallback::Protected);
    } else if (route == kobalt::ReaderBookRoute::Remembered) {
        fail(QString(), kobalt::OpeningFallback::Remembered);
    } else if (route == kobalt::ReaderBookRoute::Failed) {
        fail(strings::get("opening.failed"));
    } else if (route != kobalt::ReaderBookRoute::Direct || identity != run.identity
               || path != run.filePath) {
        fail(strings::get(
                "reader_controller.native_publication_does_not_match_the_opening_request"));
    } else {
        run.orientation = std::make_unique<kobalt::ReaderOrientation>(
                kobalt::nativeOrientationSensor(), kobalt::turnReaderScreen,
                kobalt::ownReaderRotation, kobalt::NativeLifecycle::pinScreenActive);
        run.orientation->configure(
                kobalt::storage::readPreferences(environment_.paths.preferences()).object);
        run.statePath = environment_.paths.bookState(run.identity);
        load();
    }
}

void ReaderCoordinator::updateOpening(kobalt::OpeningStage stage, int percent)
{
    if (!run.loading || int(stage) < int(run.openingStage))
        return;
    percent = qBound(0, percent, 100);
    const bool changedStage = stage != run.openingStage;
    if (!changedStage && percent <= run.openingPercent)
        return;
    // Limit display work on e-ink. Stage changes and completion are immediate.
    const auto now = run.openingTimer.elapsed();
    if (!changedStage && percent != 100 && now - run.progressPainted < 200)
        return;
    if (changedStage || percent == 100)
        kobalt::diagnostic("opening-stage",
                           kobalt::qt::object({ { "stage", int(stage) },
                                                { "percent", percent },
                                                { "elapsed_ms", double(now) } }));
    run.openingStage = stage;
    run.openingPercent = percent;
    run.progressPainted = now;
    run.loading->setProgress(kobalt::openingProgressPercent(stage, percent));
}

void ReaderCoordinator::status(const QString &value, const QString &reason)
{
    int nativeViews = 0, nativeReaders = 0;
    for (auto *widget : QApplication::allWidgets()) {
        const QByteArray type = widget->metaObject()->className();
        nativeViews += type == "ReadingView";
        nativeReaders += type == "PageImageReader";
    }
    kobalt::diagnostic("session",
                       kobalt::qt::object({ { "status", value },
                                            { "failed", !reason.isEmpty() },
                                            { "native_views", nativeViews },
                                            { "native_readers", nativeReaders },
                                            { "opening_timings", run.openingTimings } }));
    environment_.reportSession(kobalt::qt::object({ { "status", value },
                                                    { "reason", reason },
                                                    { "book", run.identity },
                                                    { "native_views", nativeViews },
                                                    { "native_readers", nativeReaders },
                                                    { "retained_book", bool(run.session) && !busy },
                                                    { "opening_timings", run.openingTimings } }));
    nh_log("reader: %s %s", qPrintable(value), qPrintable(reason));
}

void ReaderCoordinator::hideLoading()
{
    if (run.loading) {
        if (auto *window = run.loading->window())
            window->disconnect(this);
        run.loading.reset();
    }
}

void ReaderCoordinator::drainReadingJobs(std::function<void()> finished)
{
    kobalt::ReaderSession::drain(std::move(finished));
}

void ReaderCoordinator::disposeForTransition(std::function<void()> finished, bool nativeAllowed)
{
    cancelPreload();
    dropQuietReader(false);
    authorization->end();
    if (run.fallbackAlert)
        run.fallbackAlert->reject();
    cleanupCallbacks.append(std::move(finished));
    if (lifecycle->draining)
        return;
    lifecycle->draining = true;
    const QString finalStatus = busy ? "cancelled" : "closed";
    ++generation;
    if (run.sourceLease)
        run.sourceLease->cancel();
    kobalt::engineActivity().pause(false);
    hideLoading();
    finishDisposal(finalStatus, nativeAllowed);
}

void ReaderCoordinator::finishDisposal(const QString &finalStatus, bool nativeAllowed)
{
    if (run.reader) {
        const QPointer<ReaderCoordinator> guard(this);
        run.reader->finishStorageSave(
                [guard, finalStatus, nativeAllowed](bool saved) {
                    if (!guard || !NativeLifecycle::nativeSessionSaveAllowed())
                        return;
                    // Reader completion means every in-flight write has ended, even
                    // when the bounded save job failed. Do not renew its budget here.
                    if (!saved)
                        kobalt::diagnostic("reader-disposed-unsaved", {},
                                           kobalt::DiagnosticSeverity::Error);
                    delete guard->run.reader.data();
                    guard->run.reader.clear();
                    guard->finishDisposal(finalStatus, nativeAllowed);
                },
                nativeAllowed);
        return;
    }
    run.session.reset();
    run.nativeProgress.reset();
    run.opening.reset();
    run.orientation.reset();
    busy = false;
    // The publication must outlive requests already using its native server.
    // Deleting sessions invalidates their GUI callbacks before draining.
    drainReadingJobs([this, finalStatus] {
        run.nativePublication.reset();
        run.sourceLease.reset();
        QFile::remove(activePath);
        status(finalStatus);
        kobalt::releaseResourceConnections([this] {
            lifecycle->draining = false;
            if (lifecycle->blocked() && !lifecycle->storageBlocked())
                kobalt::engineActivity().pause(true);
            // The reader and its savers are gone; leave one closed file.
            kobalt::storage::closeProfile();
            // USB must not export storage with the history ownership file open.
            // Reacquire and recover on the next opening after this drain.
            historyProfile.reset();
            const auto callbacks = std::exchange(cleanupCallbacks, {});
            for (const auto &callback : callbacks)
                callback();
            if (!lifecycle->blocked() && !lifecycle->draining) {
                kobalt::engineActivity().pause(false);
                authorization->setBlocked(false);
                timer.start();
            }
        });
    });
}

void ReaderCoordinator::release(bool endAuthorization)
{
    cancelPreload();
    dropQuietReader(false);
    if (endAuthorization)
        authorization->end();
    ++generation;
    if (run.fallbackAlert) {
        run.fallbackAlert->reject();
        run.fallbackAlert.clear();
    }
    if (run.sourceLease)
        run.sourceLease->cancel();
    hideLoading();
    run.session.reset();
    run.nativeProgress.reset();
    run.nativePublication.reset();
    run.opening.reset();
    run.orientation.reset();
    run.sourceLease.reset();
    busy = false;
    QFile::remove(activePath);
}

void ReaderCoordinator::cancelOpening()
{
    if (run.reader && busy) {
        disposeForTransition([this] { status("cancelled"); });
        return;
    }
    release();
    status("cancelled");
}

void ReaderCoordinator::fail(const QString &reason, kobalt::OpeningFallback state,
                             bool publicationRejected)
{
    const bool automatic = state == kobalt::OpeningFallback::Protected
            || state == kobalt::OpeningFallback::Remembered;
    const bool protectedBook = state == kobalt::OpeningFallback::Protected;
    kobalt::diagnostic(protectedBook       ? "opening-protected-handoff"
                               : automatic ? "opening-remembered-handoff"
                                           : "opening-failed",
                       { { "error_code", kobalt::diagnosticErrorCode(reason) } });
    auto fallback = std::move(run.opening);
    // Keep the splash on screen while releasing failed loading resources.
    auto view = std::move(run.loading);
    if (view && view->window())
        view->window()->disconnect(this);
    release(false);
    status("stock", reason);
    run.loading = std::move(view);
    if (fallback && run.loading && !lifecycle->blocked() && !lifecycle->draining
        && !kobalt::NativeLifecycle::suspended()) {
        const auto epoch = generation;
        const std::shared_ptr<kobalt::NativeOpening> retained(std::move(fallback));
        busy = true;
        watch();
        run.loading->showFallback(state);
        run.fallbackAlert = run.loading->window();
        kobalt::bindReaderFallback(
                this, run.fallbackAlert,
                [this, epoch, retained, publicationRejected] {
                    if (epoch != generation)
                        return;
                    busy = false;
                    run.fallbackAlert.clear();
                    hideLoading();
                    const bool allowed = !lifecycle->blocked() && !lifecycle->draining
                            && !kobalt::NativeLifecycle::suspended() && authorization->checkNow();
                    authorization->end();
                    if (allowed) {
                        if (publicationRejected)
                            kobalt::rememberNativeBook(run.identity, run.filePath, false);
                        retained->openStock();
                    }
                },
                automatic ? 3000 : -1);
        if (run.fallbackAlert)
            connect(run.fallbackAlert.data(), &QDialog::finished, this, [this, epoch] {
                if (epoch == generation) {
                    busy = false;
                    run.fallbackAlert.clear();
                    hideLoading();
                    authorization->end();
                }
            });
    } else {
        hideLoading();
        authorization->end();
    }
}

bool ReaderCoordinator::sameBook() const
{
    return busy && run.nativePublication && run.nativePublication->active();
}

void ReaderCoordinator::poll()
{
#ifndef KOBALT_READER_HARDWARE
    kobalt::NativeProgress::testCommand();
    kobalt::NativeReadingSession::testCommand();
    kobalt::NativeAuthorization::testCommand();
    if (!kobalt::NativeLifecycle::suspended())
        kobalt::NativeDictionary::testCommand();
#endif
    if (lifecycle->blocked() || lifecycle->draining)
        return;
    if (run.reader && !busy) {
        const bool hide = kobalt::NativeLifecycle::suspended();
        if (hide && run.reader->isVisible())
            run.reader->sleepReader();
        else if (!hide && !run.reader->isVisible() && !BrowserHandoff::activeFor(run.reader)) {
            authorization->setBlocked(false);
            if (!authorization->checkNow() || !run.reader)
                return;
            run.reader->wakeReader();
        }
        if (run.reader)
            run.reader->progressTick();
    } else if (busy && kobalt::NativeLifecycle::suspended())
        cancelOpening();
#ifdef KOBALT_READER_HARDWARE
    // Nothing needs watching without a book. The next opening starts the timer again.
    if (!run.reader && !busy)
        timer.stop();
#endif
}

void ReaderCoordinator::checkProgressSoon()
{
    // Leave Nickel's signal, or the opening and wake work, before reading
    // Nickel's saved position.
    QPointer<ReaderWindow> reader(run.reader);
    kobalt::qt::later(0, this, [this, reader] {
        if (reader && reader == run.reader && !busy && !lifecycle->blocked()
            && !lifecycle->draining)
            reader->progressTick();
    });
}

void ReaderCoordinator::watch()
{
    if (!lifecycle->blocked() && !timer.isActive())
        timer.start();
}

void ReaderCoordinator::load()
{
    if (kobalt::NativeLifecycle::suspended()) {
        cancelOpening();
        return;
    }
    run.orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
    // A retained document is data, never proof that access is still allowed.
    if ((run.session || quiet.window) && !authorization->checkNow()) {
        if (!lifecycle->draining)
            fail(strings::get("reader_controller.native_authorization_is_unavailable"));
        return;
    }
    QString error;
    run.nativePublication = std::make_unique<kobalt::NativePublication>();
    if (!run.nativePublication->open(
                run.opening->volume(), run.opening->bookmark(), &error,
                bindings_.publicationKeys ? bindings_.publicationKeys(*run.opening) : nullptr)) {
        fail(error);
        return;
    }
    if (run.nativePublication->identity() != run.identity
        || run.nativePublication->path() != run.filePath) {
        if (bindings_.publicationMismatch)
            bindings_.publicationMismatch(*run.nativePublication, run.identity, run.filePath);
        fail(strings::get(
                "reader_controller.native_publication_does_not_match_the_opening_request"));
        return;
    }
    run.openingTimings["prepare_ms"] = run.openingTimer.elapsed();
    run.layoutStarted = run.openingTimer.elapsed();
    const auto stored =
            kobalt::storage::readBookState(environment_.paths, run.identity, run.statePath);
    if (stored.status != kobalt::storage::ReadStatus::Ok
        && stored.status != kobalt::storage::ReadStatus::Missing) {
        fail(strings::get("error.saved_data_unavailable"));
        return;
    }
    auto saved = stored.object;
    if (saved.value("book").toString() != run.identity)
        saved = {};
    run.nativeProgress = std::make_unique<kobalt::NativeProgress>(run.identity);
    run.diskBaseline = run.nativeProgress->read();
    const auto savedBaseline =
            kobalt::ReadingProgress::fromJson(saved.value("nickel_progress").toObject());
    const bool unread = kobalt::resetNativeUnread(saved, run.diskBaseline);
    const bool useNative = unread || kobalt::preferNativeProgress(run.diskBaseline, savedBaseline);
    run.baseline = run.diskBaseline.valid ? run.diskBaseline : savedBaseline;
    if (useNative) {
        saved.remove("position");
        saved.remove("finished_position");
    }
    if (!run.session)
        run.session = std::make_unique<kobalt::ReaderSession>(environment_.fonts(),
                                                              environment_.placeholderFamily());
    run.sourceLease = std::make_shared<kobalt::SourceLease>();
    const auto originalAppearance = saved;
    QJsonObject temporaryAppearance;
#if defined(KOBALT_BENCHMARK_API) || defined(KOBALT_SCENARIO_PLAYER)
    if (benchmarkTypographyEnabled && benchmarkPublication(run.identity))
        temporaryAppearance = kobalt::appearance::benchmarkTypography();
#endif
    const auto preferences = kobalt::appearance::readerPreferences(
            kobalt::storage::readPreferences(environment_.paths.preferences()).object, saved);
    kobalt::renderingMode::apply(preferences);
    run.orientation->configure(preferences);
    saved["dark_mode"] = preferences.value("dark_mode").toBool(false);
    saved["bold_reading"] = preferences.value("bold_reading").toString("off");
    saved["avoid_wasteful_styling"] = preferences.value("avoid_wasteful_styling").toBool(true);
    const auto size =
            run.interface->readingViewport(QGuiApplication::primaryScreen()->size(), preferences);
    const auto nativeLocation = unread
            ? QString()
            : (useNative ? run.diskBaseline.location : run.nativePublication->location());
    const auto bookDefaults = kobalt::appearance::bookDefaults(preferences);
    const bool restart = run.diskBaseline.valid && run.diskBaseline.status == 2;
    if (quiet.window) {
        if (temporaryAppearance.isEmpty()
            && quietMatches(saved, bookDefaults, size, nativeLocation, restart,
                            kobalt::isKoboCloudBook(run.opening->volume()))) {
            // The quiet window shows this book as this opening would. Nickel
            // accepted the opening and authorization is checked above.
            run.session.reset();
            run.nativeProgress.reset();
            run.sourceLease = std::move(quiet.lease);
            run.orientation = std::move(quiet.orientation);
            run.orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
            run.orientation->configure(preferences);
            run.openingTimings["load_and_layout_ms"] =
                    run.openingTimer.elapsed() - run.layoutStarted;
            showReader(true);
            return;
        }
        diagnostic("reader-quiet-mismatch");
        run.session.reset();
        dropQuietReader(true);
        if (!run.session)
            run.session = std::make_unique<kobalt::ReaderSession>(environment_.fonts(),
                                                                  environment_.placeholderFamily());
    }
    run.session->open(
            { run.identity,
              run.filePath,
              size,
              saved,
              nativeLocation,
              environment_.paths.layoutCache(),
              run.sourceLease,
              run.nativePublication->resources(run.sourceLease),
              positionCodec(),
              environment_.paths.documentCache(),
              temporaryAppearance,
              originalAppearance,
              environment_.paths.directory + "/userstyle.css",
              bookDefaults,
              {},
              restart,
              run.nativePublication->coverPath(),
              environment_.paths.customPins(),
              QStringLiteral(KOBALT_CONFIG_DIR "/dictionaries") },
            [this](const kobalt::OpeningResult &outcome) {
                run.openingTimings["load_and_layout_ms"] =
                        run.openingTimer.elapsed() - run.layoutStarted;
                if (!outcome.ready()) {
                    fail(kobalt::openingMessage(outcome), kobalt::OpeningFallback::Failure,
                         outcome.rememberRejection());
                    return;
                }
                if (!sameBook()) {
                    fail(strings::get("reader_controller.native_publication_service_closed"));
                    return;
                }
                run.expectedHash = run.session->sourceHash();
                run.openingTimings["resource_cache"] = run.session->resourceStats();
                showReader();
            },
            [this](kobalt::OpeningStage stage, int percent) { updateOpening(stage, percent); },
            [this](QImage cover) {
                if (run.loading)
                    run.loading->setCover(std::move(cover));
            },
            [this] {
                if (run.loading)
                    run.loading->setReopening();
            });
}

kobalt::ReaderWindow *ReaderCoordinator::createReader(
        std::unique_ptr<kobalt::ReaderSession> session,
        std::unique_ptr<kobalt::NativeProgress> progress, bool koboCloud, const QString &identity,
        const QString &statePath, const QByteArray &hash, const kobalt::ReadingProgress &baseline,
        const kobalt::ReadingProgress &diskBaseline, kobalt::ReaderOrientation *orientation,
        const QString &coverPath, bool quiet)
{
    progress->changeAvailable = [this] { checkProgressSoon(); };
    if (!progress->signalsChanges())
        kobalt::diagnostic("native-progress-signals-unavailable", {},
                           kobalt::DiagnosticSeverity::Warning);
    auto book = std::make_unique<kobalt::ReadingBook>(
            std::move(session), statePath, environment_.paths, identity, hash,
            kobalt::nickel::makeReadingBridge(identity, std::move(progress), koboCloud), baseline,
            diskBaseline, QString(), kobalt::ReadingHistory::UtcClock(), nickel::bootTimeClock());
    auto services = readerHostServices();
    services.presentation = [this, identity](QWidget *reader) {
        return makeWindowPresentation(
                reader, nativeReaderSleepScreenAvailable(),
                [this, identity](const sleep::Snapshot &snapshot) {
                    if ((!lifecycle->blocked() && !NativeLifecycle::suspended())
                        || NativeLifecycle::usbSaveAllowed()) {
                        homeSleep->remember(snapshot, identity);
                        // USB export unmounts this storage after the reader's
                        // final save. Write the file before that continues.
                        if (lifecycle->blocked())
                            homeSleep->finishSave();
                    }
                });
    };
    services.coverImagePath = coverPath;
    if (!quiet)
        services.startupNotice = std::exchange(pendingNotice, kobalt::ReaderNotice{});
    services.quiet = quiet;
    services.automaticBookOpening = bindings_.automaticBookOpening;
    services.setAutomaticBookOpening = bindings_.setAutomaticBookOpening;
    const auto suspended = services.suspended;
    services.suspended = [this, suspended] {
        return NativeDialogOverlay::activeFor(run.reader) || suspended();
    };
    NativeFontScaleScope building;
    auto *reader = kobalt::readerInterface().createWindow(std::move(book), orientation,
                                                          std::move(services));
    ignoreNativeFontScale(reader);
    return reader;
}

void ReaderCoordinator::showReader(bool fromQuiet)
{
    if (kobalt::NativeLifecycle::suspended()) {
        cancelOpening();
        return;
    }
#ifdef KOBALT_SCENARIO_PLAYER
    if (!scenarioOpening && run.opening)
        scenarioOpening = run.opening->copy();
#endif
    const bool koboCloud = run.opening && kobalt::isKoboCloudBook(run.opening->volume());
    run.opening.reset();
    // Read Nickel's page-button setting again for this book.
    forgetNativeInvertedPageButtons();
    const auto coverPath = run.nativePublication ? run.nativePublication->coverPath() : QString();
    QElapsedTimer creating;
    creating.start();
    if (fromQuiet) {
        run.reader = quiet.window;
        quiet.window.clear();
        diagnostic("reader-quiet-used");
        NativeFontScaleScope activating;
        run.reader->activate(std::exchange(pendingNotice, kobalt::ReaderNotice{}), coverPath);
    } else {
        run.reader = createReader(std::move(run.session), std::move(run.nativeProgress), koboCloud,
                                  run.identity, run.statePath, run.expectedHash, run.baseline,
                                  run.diskBaseline, run.orientation.get(), coverPath, false);
    }
    const double created = creating.nsecsElapsed() / 1e6;
    if (run.loading)
        run.loading->setProgress(kobalt::openingBarWindow);
    // While the reader is visible and awake, its clock tick runs this check, so
    // the device wakes once a minute. The timer then only covers a reader that
    // is hidden or asleep, and the tick keeps pushing it back.
    run.reader->minuteTicked = [this] {
        if (!lifecycle->blocked())
            timer.start();
        kobalt::qt::later(0, this, [this] { poll(); });
    };
    run.reader->finished = [this](std::unique_ptr<kobalt::ReaderSession> retained) {
        auto closing = run.reader ? run.reader->closingTiming() : kobalt::ActionTiming{};
        if (run.reader)
            openingDark = run.reader->property("reader_dark").toBool();
        // closeEvent has accepted this close. Do not let the visibility
        // poll show the reader again before Qt processes deleteLater().
        run.reader.clear();
        if (!retained || lifecycle->blocked() || kobalt::NativeLifecycle::suspended()) {
            disposeForTransition([] { });
            return;
        }
        run.session = std::move(retained);
        if (run.sourceLease)
            run.sourceLease->cancel();
        // The old facade dies with the window. Drain its last jobs before
        // closing Nickel's service; idle RAM owns no native file handles.
        lifecycle->draining = true;
        ++generation;
        drainReadingJobs([this, closing] {
            if (run.session)
                run.session->cancelSource();
            run.nativePublication.reset();
            run.opening.reset();
            run.orientation.reset();
            kobalt::releaseResourceConnections([this, closing] {
                lifecycle->draining = false;
                QFile::remove(activePath);
                // The reader and its savers are gone; leave one closed file
                // while Nickel shows Home.
                QElapsedTimer profile;
                profile.start();
                kobalt::storage::closeProfile();
                auto timing = closing;
                timing.add("profile", profile);
                kobalt::reportBookClose(timing);
                status("closed");
                finishHomeNavigation();
                // USB or revocation may have arrived during the barrier.
                if (!cleanupCallbacks.isEmpty()) {
                    disposeForTransition([] { });
                    return;
                }
                if (lifecycle->blocked() && !lifecycle->storageBlocked())
                    kobalt::engineActivity().pause(true);
                else if (!lifecycle->blocked()) {
                    authorization->setBlocked(false);
                    timer.start();
                    // Prepare this book and its hidden window again, so the
                    // next opening is as quick as the first after a restart.
                    // An opening or transition meanwhile changes the epoch.
                    const auto epoch = generation;
                    kobalt::qt::later(reopenPreparationDelay, this, [this, epoch] {
                        if (epoch == generation)
                            preloadLastBook();
                    });
                }
            });
        });
    };
    const auto epoch = generation;
    QPointer<ReaderCoordinator> guard(this);
    QPointer<ReaderWindow> reader(run.reader);
    const auto lease = run.sourceLease;
    QElapsedTimer waiting;
    waiting.start();
    reader->preparePresentation([guard, reader, epoch, lease, created, waiting](bool ready) {
        if (!guard || !reader || guard->generation != epoch || guard->run.reader != reader
            || guard->run.sourceLease != lease) {
            return;
        }
        if (!ready || guard->lifecycle->blocked() || kobalt::NativeLifecycle::suspended() || !lease
            || lease->revoked() || !guard->sameBook()) {
            guard->disposeForTransition([] { });
            return;
        }
        guard->busy = false;
        if (guard->run.loading)
            guard->run.loading->setProgress(100);
        const auto &timings = guard->run.openingTimings;
        reader->setOpeningTiming(
                guard->run.openingTimer,
                kobalt::qt::object({ { "prepare_ms", timings.value("prepare_ms") },
                                     { "engine_ms", timings.value("load_and_layout_ms") },
                                     { "window_ms", created },
                                     { "wait_ms", waiting.nsecsElapsed() / 1e6 } }));
        {
            // The first show polishes every widget of the window with its
            // style sheet, then lays it out. Measured apart from the show.
            DiagnosticOperation polishing("reader-polish");
            reader->ensurePolished();
        }
        {
            DiagnosticOperation showing("reader-show");
            reader->showFullScreen();
        }
        {
            // Finish deferred widget paints before removing the opening view.
            DiagnosticOperation painting("reader-first-paints");
            for (auto *widget : reader->findChildren<QWidget *>()) {
                QCoreApplication::sendPostedEvents(widget, QEvent::UpdateLater);
            }
            QCoreApplication::sendPostedEvents(reader, QEvent::UpdateLater);
            QCoreApplication::sendPostedEvents(reader, QEvent::UpdateRequest);
        }
        guard->hideLoading();
        guard->run.openingTimings["total_ms"] = guard->run.openingTimer.elapsed();
        guard->status("reading");
        guard->rememberLastBook();
        // A sync can finish while the book opens.
        guard->checkProgressSoon();
#ifdef KOBALT_SCENARIO_PLAYER
        guard->startScenarios();
#endif
    });
}

QString ReaderCoordinator::lastBookPath() const
{
    return environment_.paths.directory + "/cache/last-book.json";
}

void ReaderCoordinator::rememberLastBook()
{
    if (run.identity.isEmpty() || run.filePath.isEmpty())
        return;
    if (!lastBook)
        lastBook = std::make_unique<storage::RecordWriter>();
    const auto record = qt::object({ { "book", run.identity }, { "path", run.filePath } });
    lastBook->post(
            [path = lastBookPath(), record] {
                QDir().mkpath(QFileInfo(path).absolutePath());
                return storage::readJson(path) == record || storage::writeJson(path, record);
            },
            {});
}

void ReaderCoordinator::preloadLastBook()
{
    // A book kept after a close may stay; startPreload() checks it is the
    // same book.
    if (!bindings_.enabled() || busy || run.reader || preload.session || quiet.window
        || lifecycle->blocked() || lifecycle->draining || NativeLifecycle::suspended()
        || !environment_.assets.available())
        return;
    if (!lastBook)
        lastBook = std::make_unique<storage::RecordWriter>();
    QPointer<ReaderCoordinator> guard(this);
    const auto paths = environment_.paths;
    // The record, the book's saved state and the preferences are read on the
    // writer thread. The archive check reads the book's ZIP directory.
    const bool queued = lastBook->tryPost(
            [guard, paths, recordPath = lastBookPath()] {
                const auto record = storage::readJson(recordPath);
                const auto identity = record.value("book").toString();
                const auto path = record.value("path").toString();
                // A plain EPUB that is still on the device. Protected books
                // open in Nickel's reader and are never preloaded.
                if (identity.isEmpty() || !QFileInfo(path).isFile()
                    || !native::openLocalPublication(path))
                    return true;
                const auto stored =
                        storage::readBookState(paths, identity, paths.bookState(identity));
                if (stored.status != storage::ReadStatus::Ok
                    && stored.status != storage::ReadStatus::Missing)
                    return true;
                const auto preferences = storage::readPreferences(paths.preferences()).object;
                kobalt::qt::invoke(
                        qApp,
                        [guard, identity, path, saved = stored.object, preferences] {
                            if (guard)
                                guard->startPreload(identity, path, saved, preferences);
                        },
                        Qt::QueuedConnection);
                return true;
            });
    if (!queued)
        diagnostic("reader-preload-worker-unavailable", {}, DiagnosticSeverity::Warning);
}

void ReaderCoordinator::startPreload(const QString &identity, const QString &path,
                                     QJsonObject saved, const QJsonObject &stored)
{
    // A kept book stays in memory until the hidden window replaces it, so a
    // tap meanwhile still reopens it. Another kept book is left alone.
    if (!bindings_.enabled() || busy || run.reader || (run.session && run.identity != identity)
        || preload.session || quiet.window || lifecycle->blocked() || lifecycle->draining
        || NativeLifecycle::suspended())
        return;
    if (saved.value("book").toString() != identity)
        saved = {};
    // As load() prepares an opening. The tap opens the kept book again with
    // Nickel's progress and the current preferences.
    const auto originalAppearance = saved;
    const auto preferences = appearance::readerPreferences(stored, saved);
    renderingMode::apply(preferences);
    saved["dark_mode"] = preferences.value("dark_mode").toBool(false);
    saved["bold_reading"] = preferences.value("bold_reading").toString("off");
    saved["avoid_wasteful_styling"] = preferences.value("avoid_wasteful_styling").toBool(true);
    const auto size = kobalt::readerInterface().readingViewport(
            QGuiApplication::primaryScreen()->size(), preferences);
    // Kobalt's memory share counts from before the prepared book and window.
    markMemoryBase();
    preload.session =
            std::make_unique<ReaderSession>(environment_.fonts(), environment_.placeholderFamily());
    preload.lease = std::make_shared<SourceLease>();
    preload.started.start();
    diagnostic("book-preload-start");
    QPointer<ReaderCoordinator> guard(this);
    auto *session = preload.session.get();
    session->open({ identity,
                    path,
                    size,
                    saved,
                    QString(),
                    environment_.paths.layoutCache(),
                    preload.lease,
                    localResourceFactory(path, preload.lease),
                    positionCodec(),
                    environment_.paths.documentCache(),
                    {},
                    originalAppearance,
                    environment_.paths.directory + "/userstyle.css",
                    appearance::bookDefaults(preferences),
                    {},
                    false,
                    QString(),
                    environment_.paths.customPins(),
                    QStringLiteral(KOBALT_CONFIG_DIR "/dictionaries") },
                  [guard, session, identity, path, saved, preferences,
                   size](const OpeningResult &outcome) {
                      if (!guard || guard->preload.session.get() != session)
                          return;
                      auto &self = *guard;
                      const double elapsed = double(self.preload.started.elapsed());
                      // An opening, USB or another transition took over meanwhile.
                      const bool usable = outcome.ready() && !self.busy && !self.run.reader
                              && (!self.run.session || self.run.identity == identity)
                              && !self.lifecycle->blocked() && !self.lifecycle->draining
                              && !NativeLifecycle::suspended();
                      diagnostic("book-preload-end",
                                 { { "ready", usable }, { "elapsed_ms", elapsed } });
                      if (!usable) {
                          self.preload.session.reset();
                          self.preload.lease.reset();
                          return;
                      }
                      self.quiet.identity = identity;
                      self.quiet.path = path;
                      if (self.buildQuietReader(self.preload.session, saved, preferences, size)) {
                          self.preload.lease.reset();
                          // The hidden window holds this book now.
                          self.run.session.reset();
                          return;
                      }
                      // Without the window, the book is kept as after a close.
                      auto kept = self.preload.session ? self.preload.session->retained() : nullptr;
                      self.preload.session.reset();
                      if (self.preload.lease)
                          self.preload.lease->cancel();
                      self.preload.lease.reset();
                      if (!kept || self.run.session)
                          return;
                      self.run.session = std::move(kept);
                      self.run.identity = identity;
                      self.run.filePath = path;
                  });
}

bool ReaderCoordinator::buildQuietReader(std::unique_ptr<ReaderSession> &session,
                                         const QJsonObject &saved, const QJsonObject &preferences,
                                         QSize size)
{
    QElapsedTimer building;
    building.start();
    const auto skip = [](const char *reason) {
        diagnostic("reader-quiet-skipped", { { "reason", QString::fromLatin1(reason) } });
        return false;
    };
    // The tap would start from Nickel's position instead of Kobalt's.
    auto progress = std::make_unique<NativeProgress>(quiet.identity);
    const auto disk = progress->read();
    auto state = saved;
    const auto savedBaseline = ReadingProgress::fromJson(saved.value("nickel_progress").toObject());
    if (resetNativeUnread(state, disk) || preferNativeProgress(disk, savedBaseline)
        || (disk.valid && disk.status == 2))
        return skip("native-progress");
    // The same profile preparation as the first opening, now after Nickel
    // started instead of at the tap.
    if (!profilePrepared) {
        storage::ProfileRecovery recovery;
        profilePrepared = storage::prepareProfile(environment_.paths, &recovery);
        if (recovery.storageFailed)
            return skip("storage");
        if (recovery.recovered())
            pendingNotice = profileRecoveryNotice(
                    recovery, usbPath(environment_.paths.directory + "/backups"));
    }
    if (!historyProfile && annotationDatabaseEnabled()) {
        QString error;
        if (profilePrepared)
            historyProfile = ReadingHistory::openProfile(environment_.paths.directory, &error);
        if (!historyProfile)
            return skip("profile");
    }
    const bool assetsReady = environment_.assets.available();
    if (!interfaceFontsLoaded)
        interfaceFontsLoaded = assetsReady && ui::initializeInterfaceFonts(environment_.assets);
    if (!interfaceFontsLoaded)
        return skip("fonts");
    auto &interface = readerInterface();
    quiet.density = currentScreenDensity();
    quiet.largeText = nativeFontScale();
    interface.setDensity(quiet.density);
    interface.setLargeText(quiet.largeText);
    ui::setInterfaceWeight(ui::heavierTextSteps);
    quiet.saved = saved;
    quiet.bookDefaults = appearance::bookDefaults(preferences);
    quiet.size = size;
    quiet.diskBaseline = disk;
    const QFileInfo file(quiet.path);
    quiet.modified = file.lastModified();
    quiet.bytes = file.size();
    quiet.lease = preload.lease;
    quiet.orientation = std::make_unique<ReaderOrientation>(nativeOrientationSensor(),
                                                            turnReaderScreen, ownReaderRotation,
                                                            NativeLifecycle::pinScreenActive);
    quiet.orientation->configure(preferences);
    const auto hash = session->sourceHash();
    const auto baseline = disk.valid ? disk : savedBaseline;
    quiet.window = createReader(std::move(session), std::move(progress), false, quiet.identity,
                                environment_.paths.bookState(quiet.identity), hash, baseline, disk,
                                quiet.orientation.get(), QString(), true);
    const double created = building.nsecsElapsed() / 1e6;
    {
        // The first show would polish every widget with its style sheet.
        NativeFontScaleScope polishing;
        quiet.window->ensurePolished();
    }
    diagnostic("reader-quiet-built",
               { { "create_ms", created }, { "elapsed_ms", building.nsecsElapsed() / 1e6 } });
    return true;
}

bool ReaderCoordinator::quietMatches(const QJsonObject &saved, const QJsonObject &bookDefaults,
                                     QSize size, const QString &nativeLocation, bool restart,
                                     bool koboCloud) const
{
    if (!quiet.window || quiet.identity != run.identity || quiet.path != run.filePath || restart
        || koboCloud || saved != quiet.saved || bookDefaults != quiet.bookDefaults
        || size != quiet.size || !(run.diskBaseline == quiet.diskBaseline)
        || currentScreenDensity() != quiet.density || nativeFontScale() != quiet.largeText)
        return false;
    // Nickel's location is used only when Kobalt has no position of its own.
    if (!nativeLocation.isEmpty() && saved.value("position").toString().isEmpty()
        && saved.value("finished_position").toString().isEmpty())
        return false;
    const QFileInfo file(quiet.path);
    return file.lastModified() == quiet.modified && file.size() == quiet.bytes;
}

void ReaderCoordinator::dropQuietReader(bool keepSession)
{
    if (!quiet.window)
        return;
    auto kept = quiet.window->releaseQuiet();
    delete quiet.window.data();
    quiet.window.clear();
    quiet.orientation.reset();
    if (quiet.lease)
        quiet.lease->cancel();
    quiet.lease.reset();
    diagnostic("reader-quiet-dropped", { { "kept", keepSession && kept } });
    // load() asks for this only for the book it opens, whose identity and
    // path are already in run.
    if (keepSession)
        run.session = std::move(kept);
}

void ReaderCoordinator::cancelPreload()
{
    if (!preload.session)
        return;
    if (preload.lease)
        preload.lease->cancel();
    preload.session.reset();
    preload.lease.reset();
    diagnostic("book-preload-cancelled");
}

#ifdef KOBALT_SCENARIO_PLAYER
void ReaderCoordinator::startScenarios()
{
    const auto path = environment_.paths.directory + "/scenarios/scenarios.json";
    if (scenariosStarted || !kobalt::diagnosticsActive() || !QFile::exists(path))
        return;
    scenariosStarted = true;
    QPointer<ReaderCoordinator> guard(this);
    // Let the opening finish its queued work before the first step.
    kobalt::qt::later(2000, this, [guard, path] {
        if (guard)
            guard->runScenarios(path);
    });
}

void ReaderCoordinator::runScenarios(const QString &path)
{
    QFile source(path);
    const auto file = source.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(source.readAll()).object()
            : QJsonObject();
    kobalt::scenario::Options options;
    options.filter = file.value("filter").toString();
    // A check that only needs to see each scenario work can run fewer repetitions.
    if (file.value("repeat_scale").toDouble() > 0)
        options.repeatScale = file.value("repeat_scale").toDouble();
    const auto books = file.value("books").toObject();
    for (auto it = books.begin(); it != books.end(); ++it)
        if (QFileInfo(it.value().toObject().value("file").toString()).fileName()
            == QFileInfo(run.filePath).fileName())
            options.openBook = it.key();
    // A book entry marked `current` stands for whichever book is open, so
    // that a run can measure a book that is not in the package.
    if (options.openBook.isEmpty())
        for (auto it = books.begin(); it != books.end(); ++it)
            if (it.value().toObject().value("current").toBool())
                options.openBook = it.key();
    // Books open from Nickel's library. The host can open the first book again
    // from a copy of its opening request, but no other book.
    kobalt::scenario::Host host;
    const auto openKey = options.openBook;
    host.opens = [this, openKey](const QString &key) { return key == openKey && scenarioOpening; };
    host.open = [this, openKey](const QString &key) {
        if (key != openKey || !scenarioOpening)
            return false;
        homeSleep->finishSave();
        auto opening = std::make_shared<std::unique_ptr<NativeOpening>>(scenarioOpening->copy());
        return *opening && begin([opening] { return std::move(*opening); });
    };
    // The player asks for this while no book is open. The next opening
    // then parses and lays out the book again.
    host.forget = [this](const QString &) {
        // The closed book stays in memory for a quick return. A cold opening
        // must not reuse it.
        run.session.reset();
        QDir(environment_.paths.documentCache()).removeRecursively();
        QFile::remove(environment_.paths.layoutCache());
    };
    host.reader = [this]() -> QWidget * { return run.reader.data(); };
    host.busy = [this] {
        return run.reader
                && (run.reader->busy()
                    || run.reader->property("eval_sleep_cover_pending").toBool());
    };
    host.idle = [this] { return idleForBenchmark(); };
    host.wifiControl = [] { return NativeSyncNetwork::canTurnOff(); };
    host.nativeSync = [this]() -> QString {
        static const NativeSync native;
        auto *manager = native.manager();
        if (!manager)
            return QStringLiteral("this firmware has no N3SyncManager::sync()");
        QElapsedTimer call;
        call.start();
        native.sync(manager);
        const double callMs = call.nsecsElapsed() / 1e6;
        const bool started = native.syncing(manager);
        kobalt::diagnostic("native-sync-start",
                           kobalt::qt::object({ { "call_ms", callMs }, { "syncing", started } }),
                           false);
        if (!started)
            return QStringLiteral("Nickel did not start a sync");
        // Log the end, so samples can be matched to the sync they overlap.
        // The Wi-Fi power saver reads isSyncing() on this thread too.
        auto *poll = new QTimer(this);
        auto elapsed = std::make_shared<QElapsedTimer>();
        elapsed->start();
        connect(poll, &QTimer::timeout, this, [poll, elapsed] {
            auto *current = native.manager();
            const bool running = current && native.syncing(current);
            if (running && elapsed->elapsed() < 600000)
                return;
            poll->stop();
            poll->deleteLater();
            kobalt::diagnostic("native-sync-end",
                               kobalt::qt::object({ { "elapsed_ms", elapsed->nsecsElapsed() / 1e6 },
                                                    { "ended", !running } }),
                               false);
        });
        poll->start(100);
        return {};
    };
    host.nativeSyncing = [] {
        static const NativeSync native;
        auto *manager = native.manager();
        return manager && native.syncing(manager);
    };
    host.wifiOff = []() -> QString {
        static const NativeRadio radio;
        auto *manager = radio.manager();
        if (!manager)
            return QStringLiteral("this firmware has no WirelessManager::off()");
        // Its own name, so this harness call neither uses up the one-time
        // main-thread report for KOSync's wifi-off nor looks like it in the log.
        mainthread::AllowBlocking allow("scenario wifi step");
        mainthread::BlockingOperation wait("scenario-wifi-off");
        radio.off(manager);
        return {};
    };
    host.wifiUp = [] {
        static const NativeRadio radio;
        auto *manager = radio.manager();
        return manager && radio.up(manager);
    };
    host.realMilliseconds = nickel::bootTimeClock();
    // Nickel asks canSuspend before each standby, so a refusal keeps the
    // device awake. A kernel wake lock does not work here: Nickel reads
    // wakeup_count on the GUI thread before it suspends, and that read waits
    // while a lock is held, which freezes the reader and the run with it.
    host.keepAwake = [](bool awake) { NativeLifecycle::holdAwake(awake); };
    host.sleepPersistence = [this](const QJsonObject &request) {
        auto &options = scenario::sleepPersistenceOptions();
        const auto action = request.value("action").toString();
        if (request.isEmpty()) {
            homeSleep->finishSave();
            options = {};
            return QJsonObject();
        }
        const auto error = [](const QString &message) {
            return qt::object({ { "error", message } });
        };
        if (action == "configure") {
            homeSleep->takeSaveMeasurement();
            const auto mode = request.value("mode").toString();
            if (mode != "stock" && mode != "page" && mode != "cover")
                return error("invalid sleep persistence configuration");
            options.mode = int(mode == "stock"           ? sleep::Mode::Stock
                                       : mode == "cover" ? sleep::Mode::Cover
                                                         : sleep::Mode::Page);
            return QJsonObject();
        }
        if (action == "drain") {
            auto result = homeSleep->takeSaveMeasurement();
            result["mode"] = sleep::value(sleep::Mode(options.mode));
            if (options.mode != int(sleep::Mode::Stock) && !result.value("saved").toBool()
                && !result.contains("error"))
                result["error"] = QStringLiteral("the close did not persist a sleep snapshot");
            return result;
        }
        if (options.mode == int(sleep::Mode::Cover)
            && (!run.reader || !run.reader->property("eval_sleep_cover_ready").toBool()))
            return error("the cover did not decode; refusing to measure a fallback page");
        if (action == "ready")
            return QJsonObject();
        return error("unknown sleep benchmark action");
    };
    host.dictionary = [] {
        const auto services = readerHostServices();
        const auto dictionary = services.dictionary ? services.dictionary() : nullptr;
        return dictionary && !dictionary->choices().isEmpty();
    };
    host.locate = [](QWidget *widget, const QPoint &point) -> QPair<QWindow *, QPointF> {
        auto *window = widget->window();
        return { window->windowHandle(), QPointF(widget->mapTo(window, point)) };
    };
    // Kobalt's touch filter takes a spontaneous mouse press in its windows as
    // a sign that Qt synthesizes mouse events, and stops translating touch.
    // Send the presses to the widget under the point instead.
    host.press = [](QWidget *widget, const QPoint &point, bool press) {
        static QPointer<QWidget> pressed;
        QWidget *target = press ? nullptr : pressed.data();
        if (!target) {
            target = widget->childAt(point);
            if (!target)
                target = widget;
        }
        pressed = press ? target : nullptr;
        const QPoint local = target == widget ? point : target->mapFrom(widget, point);
        QMouseEvent event(press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                          QPointF(local), QPointF(widget->mapToGlobal(point)), Qt::LeftButton,
                          press ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    kobalt::diagnostic(
            "scenario-run-start",
            kobalt::qt::object({ { "scenarios", file.value("scenarios").toArray().size() } }),
            false);
    // With `chapter_variants`, every scenario runs twice: with the book
    // opened as a whole, then in chapter mode. The first scenario of a run
    // must open the book again, so that the choice applies.
    const bool variants = file.value("chapter_variants").toBool();
    QVector<QJsonObject> results;
    for (int pass = 0; pass < (variants ? 2 : 1); ++pass) {
        if (variants)
            kobalt::renderingMode::setOverride(pass);
        for (auto result : kobalt::scenario::run(file, host, options)) {
            if (variants) {
                const QString variant = QString::fromLatin1(pass ? "chapter" : "whole-book");
                result["variant"] = variant;
                result["id"] = result.value("id").toString() + QLatin1Char('.') + variant;
            }
            results.append(result);
        }
    }
    if (variants)
        kobalt::renderingMode::setOverride(-1);
    auto header = kobalt::qt::object({ { "kind", "kobalt-device-run" },
                                       { "target", QString("qt%1").arg(QT_VERSION >> 16) },
                                       { "version", QString::fromLatin1(KOBALT_VERSION) },
                                       { "qt", QString::fromLatin1(qVersion()) } });
    QFile version("/mnt/onboard/.kobo/version");
    if (version.open(QIODevice::ReadOnly)) {
        const auto device = kobalt::nickel::diagnosticDeviceFields(version.read(512));
        for (auto it = device.begin(); it != device.end(); ++it)
            header[it.key()] = it.value();
    }
    // One line per scenario, kept apart from the rotating event log.
    QSaveFile output(environment_.paths.directory + "/diagnostics/scenarios.jsonl");
    bool saved = false;
    if (output.open(QIODevice::WriteOnly)) {
        output.write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n');
        for (const auto &result : results)
            output.write(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n');
        saved = output.commit();
    }
    kobalt::diagnostic("scenario-run-end",
                       kobalt::qt::object({ { "results", results.size() }, { "saved", saved } }));
    // Tell whoever started the run that it ended. When the run lost its
    // reader, the next opening shows the notice instead.
    const auto notice = kobalt::benchmarkNotice(
            results, saved, usbPath(environment_.paths.directory + "/diagnostics"));
    if (!run.reader || !run.reader->showNotice(notice))
        pendingNotice = notice;
}
#endif

ReaderCoordinator::~ReaderCoordinator()
{
    // The quiet window calls back into this coordinator.
    dropQuietReader(false);
}
} // namespace kobalt::nickel
