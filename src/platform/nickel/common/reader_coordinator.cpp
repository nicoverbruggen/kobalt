#include "reading/appearance_settings.h"
#include "reader_coordinator.h"
#include "usb_transition.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "book_menu.h"
#include "browser_handoff.h"
#include "interfaces/reader_window.h"
#include "reading/reading_book.h"
#include "storage/reader_storage.h"
#include "reading/reader_session.h"
#include "engine/crengine/engine_activity.h"
#include "native_lifecycle.h"
#include "native_authorization.h"
#include "native_publication.h"
#include "native_progress.h"
#include "native_reading_session.h"
#include "native_dictionary.h"
#include "publication/native_resources.h"
#include "reading_bridge.h"
#include "engine_positions.h"
#include "native_ui.h"
#include "native_fallback.h"
#include "platform/api/reader_orientation.h"
#include "orientation.h"
#include "support/diagnostics.h"
#include "reader_environment.h"
#include "reader_host_services.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader_interface.h"
#include "interfaces/opening_message.h"
#include "platform/nickel/display/screen_density.h"
#include <NickelHook.h>
#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QScreen>
#include <QThread>
#include <utility>

namespace kobalt::nickel {
using kobalt::storage::readJson;
using kobalt::storage::writeJson;
namespace {
QPointer<ReaderCoordinator> activeCoordinator;
bool benchmarkTypographyEnabled = false;
}

extern "C" __attribute__((visibility("default"))) int kobalt_benchmark_typography_v1(const char *contentId,
                                                                                     int enable) {
    if (!qApp || QThread::currentThread() != qApp->thread() || !contentId || (enable != 0 && enable != 1) ||
        kobalt::appearance::benchmarkTypography(QString::fromUtf8(contentId)).isEmpty() || !activeCoordinator ||
        (enable && !activeCoordinator->idleForBenchmark()))
        return 0;
    benchmarkTypographyEnabled = enable != 0;
    return 1;
}

bool ReaderCoordinator::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::Show && lifecycle->usbPromptExpected() && run.reader && run.reader->isVisible() &&
        !usbDialog_.active()) {
        auto *dialog = qobject_cast<QWidget *>(object);
        if (dialog && QByteArray(dialog->metaObject()->className()) == "ConfirmationDialog") {
            QPointer<QWidget> candidate(dialog);
            QPointer<ReaderWindow> reader(run.reader);
            qt::later(0, this, [this, candidate, reader] {
                if (candidate && candidate->isVisible() && reader && reader == run.reader && reader->isVisible()) {
                    usbDialog_.present(candidate, reader);
                }
            });
        }
    }
    return false;
}

ReaderCoordinator::ReadingRun::ReadingRun() = default;
ReaderCoordinator::ReadingRun::~ReadingRun() = default;

ReaderCoordinator::ReaderCoordinator(ReaderEnvironment &environment, CoordinatorBindings bindings, QObject *parent)
    : QObject(parent), environment_(environment), bindings_(bindings), lifecycle(new kobalt::NativeLifecycle(this)),
      authorization(new kobalt::NativeAuthorization(this)) {
    activeCoordinator = this;
    qApp->installEventFilter(this);
    authorization->revoked = [this](kobalt::NativeAuthorization::Reason) {
        homeAfterClose = false;
        if (run.sourceLease)
            run.sourceLease->revoke();
        timer.stop();
        if (run.reader)
            run.reader->hide();
        // Save the local checkpoint and release all content without native
        // progress writeback or retrying the retained opening request.
        disposeForTransition([this] {
            status("authorization-lost");
        });
    };
    lifecycle->sleep = [this] {
        homeAfterClose = false;
        authorization->setBlocked(true);
        timer.stop();
        if (run.reader) {
            // The browser owns the screen while this reader is already
            // hidden. Keep its saved position without creating a sleep cover.
            if (BrowserHandoff::activeFor(run.reader))
                run.reader->checkpoint();
            else
                run.reader->sleepReader();
        } else if (busy || run.fallbackAlert)
            disposeForTransition([] {});
    };
    lifecycle->sleepViewReady = [this] {
        if (run.reader)
            run.reader->sleepViewReady();
    };
    lifecycle->beforeWake = [this] {
        if (run.reader)
            run.reader->prepareWake();
    };
    lifecycle->cancelWake = [this] {
        if (run.reader)
            run.reader->cancelWake();
    };
    lifecycle->wake = [this] {
        if (lifecycle->draining)
            return;
        authorization->setBlocked(false);
        timer.start();
        if (run.reader || run.model) {
            const bool allowed = authorization->checkNow();
            if (allowed && run.reader && !BrowserHandoff::activeFor(run.reader))
                run.reader->wakeReader();
            else if (!allowed && (run.reader || run.model) && !lifecycle->draining) {
                // A deferred or unavailable check must not leave an invisible
                // reader holding Nickel's backing store indefinitely.
                kobalt::diagnostic("resume-authorization-unavailable");
                disposeForTransition([this] {
                    status("authorization-unavailable");
                });
            }
        }
    };
    lifecycle->usbPrompt = [this] {
        if (!run.reader || !run.reader->isVisible() || usbDialog_.active()) {
            return;
        }
        for (auto *widget : QApplication::allWidgets()) {
            if (widget->isVisible() && QByteArray(widget->metaObject()->className()) == "ConfirmationDialog") {
                usbDialog_.present(widget, run.reader);
                return;
            }
        }
    };
    lifecycle->prepareUsb = [this](std::function<void()> ready) {
        homeAfterClose = false;
        authorization->setBlocked(true);
        timer.stop();
        prepareUsbTransition(
            run.reader, cleanupCallbacks,
            [this] {
                kobalt::diagnostic("usb-release-start");
                disposeForTransition([] {});
            },
            [this, ready = std::move(ready)] {
                environment_.assets.invalidate();
                ready();
            });
    };
    lifecycle->prepareShutdown = [this](std::function<void()> ready) {
        homeAfterClose = false;
        authorization->setBlocked(true);
        timer.stop();
        // Revocation can dispose the reader while its bookmark is being
        // exported. Keep the shutdown continuation with the shared drain.
        cleanupCallbacks.append(std::move(ready));
        if (run.reader)
            run.reader->prepareShutdown([this] {
                disposeForTransition([] {});
            });
        else
            disposeForTransition([] {});
    };
    if (bindings_.installInput)
        bindings_.installInput(this);
    connect(&timer, &QTimer::timeout, this, [this] {
        poll();
    });
    timer.setInterval(250);
    if (!lifecycle->blocked())
        timer.start();
}

bool interceptHomeKey(QKeyEvent *event) {
    return activeCoordinator && activeCoordinator->handleHomeKey(event);
}

bool ReaderCoordinator::handleHomeKey(QKeyEvent *event) {
    if (!bindings_.goHome || (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)) {
        return false;
    }
    if (event->key() != Qt::Key_Home || event->modifiers() != Qt::NoModifier || lifecycle->blocked() ||
        kobalt::NativeLifecycle::suspended()) {
        return false;
    }
    const bool readerVisible = run.reader && run.reader->isVisible() && !BrowserHandoff::activeFor(run.reader);
    const bool openingVisible = (run.loading && run.loading->window() && run.loading->window()->isVisible()) ||
                                (run.fallbackAlert && run.fallbackAlert->isVisible());
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
            disposeForTransition([this] {
                finishHomeNavigation();
            });
        }
    }
    event->accept();
    return true;
}

void ReaderCoordinator::finishHomeNavigation() {
    if (std::exchange(homeAfterClose, false) && !lifecycle->blocked() && !kobalt::NativeLifecycle::suspended()) {
        bindings_.goHome();
    }
}

bool ReaderCoordinator::idleForBenchmark() const {
    return !busy && !run.reader && !run.fallbackAlert && !lifecycle->draining;
}

bool ReaderCoordinator::available() const {
    return lifecycle->connected() && authorization->available() && kobalt::NativeReadingSession::available();
}

bool ReaderCoordinator::begin(const RetainOpening &retainOpening) {
    if (!available())
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
    if (kobalt::previewBook(request->volume()))
        return false;
    QString nextIdentity, nextPath;
    const auto route = kobalt::readerBookCandidateRoute(request->volume(), &nextIdentity, &nextPath);
    if (route == kobalt::ReaderBookRoute::Unsupported)
        return false;
    // The loading view draws in the interface faces. Check only those files
    // now and leave the whole bundle for after its first paint. When they
    // cannot be read, check everything now so recovery can show its alert.
    if (!kobalt::ui::initializeOpeningFonts(environment_.assets)) {
        const bool assetsReady = environment_.assets.available();
        if (!assetsReady || !kobalt::ui::initializeInterfaceFonts(environment_.assets))
            return missingAssetsBeforeOpening(assetsReady);
    }
    const bool discardRetained = run.model && nextIdentity != run.identity;
    run.identity = nextIdentity;
    run.filePath = nextPath;
    if (!writeJson(activePath, kobalt::qt::object({{"book", run.identity}}))) {
        kobalt::diagnostic("session-arm-failed");
        return false;
    }
    if (!authorization->begin(run.identity)) {
        release();
        return false;
    }
    run.opening = std::move(request);
    busy = true;
    watch();
    run.openingTimer.start();
    run.openingTimings = {};
    QWidget *background = QApplication::activeWindow();
    for (auto *window : QApplication::topLevelWidgets())
        if (window->isVisible() && window->inherits("MainWindowView")) {
            background = window;
            break;
        }
    const auto preferences = readJson(environment_.paths.preferences());
    run.interface = &kobalt::readerInterface();
    run.interface->setDensity(kobalt::currentScreenDensity());
    run.loading = run.interface->createOpening(background, preferences, readerHostServices().presentation);
    run.openingStage = kobalt::OpeningStage::Content;
    run.openingPercent = -1;
    run.progressPainted = -1000;
    updateOpening(kobalt::OpeningStage::Content, 0);
    connect(run.loading->window(), &QDialog::rejected, this, [this] {
        cancelOpening();
    });
    const auto epoch = generation;
    run.loading->afterFirstPaint([this, epoch, route, discardRetained] {
        if (epoch == generation && busy && run.opening && run.loading) {
            if (discardRetained) {
                if (run.sourceLease) {
                    run.sourceLease->cancel();
                }
                run.model.reset();
                run.sourceLease.reset();
            }
            prepareOpening(route);
        }
    });
    run.loading->window()->show();
    run.loading->window()->raise();
    return true;
}

bool ReaderCoordinator::missingAssetsBeforeOpening(bool assetsReady) {
    kobalt::diagnostic(assetsReady ? "interface-fonts-unavailable" : "assets-unavailable");
    // Keep the native settings and reader choice available. This failure
    // needs no Kobalt window or asset-backed font, and consumes no book.
    const bool filesystemRecovery = !assetsReady && environment_.assets.hasFilesystemRecoveryFiles();
    if (filesystemRecovery)
        kobalt::diagnostic("asset-filesystem-recovery");
    run.fallbackAlert = kobalt::showMissingAssetsAlert(QStringLiteral(KOBALT_VERSION), filesystemRecovery);
    if (!run.fallbackAlert) {
        kobalt::diagnostic("assets-alert-unavailable");
        return false;
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
    return true;
}

void ReaderCoordinator::missingAssets(bool assetsReady) {
    kobalt::diagnostic(assetsReady ? "interface-fonts-unavailable" : "assets-unavailable");
    const bool filesystemRecovery = !assetsReady && environment_.assets.hasFilesystemRecoveryFiles();
    if (filesystemRecovery) {
        kobalt::diagnostic("asset-filesystem-recovery");
    }
    auto fallback = std::move(run.opening);
    release();
    run.fallbackAlert = kobalt::showMissingAssetsAlert(QStringLiteral(KOBALT_VERSION), filesystemRecovery);
    if (!run.fallbackAlert) {
        kobalt::diagnostic("assets-alert-unavailable");
        status("stock");
        if (fallback && !lifecycle->blocked() && !lifecycle->draining && !kobalt::NativeLifecycle::suspended()) {
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

void ReaderCoordinator::prepareOpening(kobalt::ReaderBookRoute candidate) {
    status("loading");
    const bool assetsReady = environment_.assets.available();
    bool fontsChanged = false;
    if (!assetsReady || !kobalt::ui::initializeInterfaceFonts(environment_.assets, &fontsChanged)) {
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
    } else if (route != kobalt::ReaderBookRoute::Direct || identity != run.identity || path != run.filePath) {
        fail(strings::get("reader_controller.native_publication_does_not_match_the_opening_request"));
    } else {
        run.orientation = std::make_unique<kobalt::ReaderOrientation>(
            kobalt::nativeOrientationSensor(), kobalt::turnReaderScreen, kobalt::ownReaderRotation);
        run.orientation->configure(readJson(environment_.paths.preferences()));
        run.statePath = environment_.paths.bookState(run.identity);
        load();
    }
}

void ReaderCoordinator::updateOpening(kobalt::OpeningStage stage, int percent) {
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
        kobalt::diagnostic(
            "opening-stage",
            kobalt::qt::object({{"stage", int(stage)}, {"percent", percent}, {"elapsed_ms", double(now)}}));
    run.openingStage = stage;
    run.openingPercent = percent;
    run.progressPainted = now;
    // Each operation advances one part of the bar. It never resets when
    // the engine moves to the next operation and does not estimate time.
    const int overall = (int(stage) * 100 + percent) / (int(kobalt::OpeningStage::Ready) + 1);
    run.loading->setProgress(overall);
}

void ReaderCoordinator::status(const QString &value, const QString &reason) {
    int nativeViews = 0, nativeReaders = 0;
    for (auto *widget : QApplication::allWidgets()) {
        const QByteArray type = widget->metaObject()->className();
        nativeViews += type == "ReadingView";
        nativeReaders += type == "PageImageReader";
    }
    kobalt::diagnostic("session", kobalt::qt::object({{"status", value},
                                                      {"failed", !reason.isEmpty()},
                                                      {"native_views", nativeViews},
                                                      {"native_readers", nativeReaders},
                                                      {"opening_timings", run.openingTimings}}));
    writeJson(sessionReport, kobalt::qt::object({{"status", value},
                                                 {"reason", reason},
                                                 {"book", run.identity},
                                                 {"native_views", nativeViews},
                                                 {"native_readers", nativeReaders},
                                                 {"retained_book", bool(run.model) && !busy},
                                                 {"opening_timings", run.openingTimings}}));
    nh_log("reader: %s %s", qPrintable(value), qPrintable(reason));
}

void ReaderCoordinator::hideLoading() {
    if (run.loading) {
        if (auto *window = run.loading->window())
            window->disconnect(this);
        run.loading.reset();
    }
}

void ReaderCoordinator::drainReadingJobs(std::function<void()> finished) {
    kobalt::ReaderSession::drain(std::move(finished));
}

void ReaderCoordinator::disposeForTransition(std::function<void()> finished) {
    usbDialog_.restore();
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
    if (run.reader) {
        run.reader->checkpoint();
        delete run.reader.data();
        run.reader.clear();
    }
    run.model.reset();
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

void ReaderCoordinator::release(bool endAuthorization) {
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
    run.model.reset();
    run.nativeProgress.reset();
    run.nativePublication.reset();
    run.opening.reset();
    run.orientation.reset();
    run.sourceLease.reset();
    busy = false;
    QFile::remove(activePath);
}

void ReaderCoordinator::cancelOpening() {
    release();
    status("cancelled");
}

void ReaderCoordinator::fail(const QString &reason, kobalt::OpeningFallback state, bool publicationRejected) {
    const bool automatic = state != kobalt::OpeningFallback::Failure;
    const bool protectedBook = state == kobalt::OpeningFallback::Protected;
    kobalt::diagnostic(protectedBook ? "opening-protected-handoff"
                       : automatic   ? "opening-remembered-handoff"
                                     : "opening-failed",
                       {{"error_code", kobalt::diagnosticErrorCode(reason)}});
    auto fallback = std::move(run.opening);
    // Keep the splash on screen while releasing failed loading resources.
    auto view = std::move(run.loading);
    if (view && view->window())
        view->window()->disconnect(this);
    release(false);
    status("stock", reason);
    run.loading = std::move(view);
    if (fallback && run.loading && !lifecycle->blocked() && !lifecycle->draining &&
        !kobalt::NativeLifecycle::suspended()) {
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
                const bool allowed = !lifecycle->blocked() && !lifecycle->draining &&
                                     !kobalt::NativeLifecycle::suspended() && authorization->checkNow();
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

bool ReaderCoordinator::sameBook() const {
    return busy && run.nativePublication && run.nativePublication->active();
}

void ReaderCoordinator::poll() {
#ifndef KOBALT_READER_HARDWARE
    kobalt::NativeProgress::testCommand();
    kobalt::NativeReadingSession::testCommand();
    kobalt::NativeAuthorization::testCommand();
    if (!kobalt::NativeLifecycle::suspended())
        kobalt::NativeDictionary::testCommand();
#endif
    if (lifecycle->blocked() || lifecycle->draining)
        return;
    if (run.reader) {
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

void ReaderCoordinator::watch() {
    if (!lifecycle->blocked() && !timer.isActive())
        timer.start();
}

void ReaderCoordinator::load() {
    if (kobalt::NativeLifecycle::suspended()) {
        cancelOpening();
        return;
    }
    run.orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
    // A retained document is data, never proof that access is still allowed.
    if (run.model && !authorization->checkNow()) {
        if (!lifecycle->draining)
            fail(strings::get("reader_controller.native_authorization_is_unavailable"));
        return;
    }
    QString error;
    run.nativePublication = std::make_unique<kobalt::NativePublication>();
    if (!run.nativePublication->open(run.opening->volume(), run.opening->bookmark(), &error,
                                     bindings_.publicationKeys ? bindings_.publicationKeys(*run.opening) : nullptr)) {
        fail(error);
        return;
    }
    if (run.nativePublication->identity() != run.identity || run.nativePublication->path() != run.filePath) {
        if (bindings_.publicationMismatch)
            bindings_.publicationMismatch(*run.nativePublication, run.identity, run.filePath);
        fail(strings::get("reader_controller.native_publication_does_not_match_the_opening_request"));
        return;
    }
    run.openingTimings["prepare_ms"] = run.openingTimer.elapsed();
    run.layoutStarted = run.openingTimer.elapsed();
    auto saved = readJson(run.statePath);
    if (saved.value("book").toString() != run.identity)
        saved = {};
    run.nativeProgress = std::make_unique<kobalt::NativeProgress>(run.identity);
    run.diskBaseline = run.nativeProgress->read();
    const auto savedBaseline = kobalt::ReadingProgress::fromJson(saved.value("nickel_progress").toObject());
    const bool useNative = kobalt::preferNativeProgress(run.diskBaseline, savedBaseline);
    run.baseline = run.diskBaseline.valid ? run.diskBaseline : savedBaseline;
    if (useNative)
        saved.remove("position");
    if (!run.model)
        run.model = std::make_unique<kobalt::ReaderSession>(environment_.fonts(), environment_.placeholderFamily());
    run.sourceLease = std::make_shared<kobalt::SourceLease>();
    const auto originalAppearance = saved;
    const auto temporaryAppearance =
        benchmarkTypographyEnabled ? kobalt::appearance::benchmarkTypography(run.identity) : QJsonObject();
    const auto preferences = kobalt::appearance::readerPreferences(readJson(environment_.paths.preferences()), saved);
    run.orientation->configure(preferences);
    saved["dark_mode"] = preferences.value("dark_mode").toBool(false);
    saved["bold_reading"] = preferences.value("bold_reading").toString("off");
    const auto size = run.interface->readingViewport(QGuiApplication::primaryScreen()->size(), preferences);
    run.model->open(
        {run.identity, run.filePath, size, saved,
         useNative ? run.diskBaseline.location : run.nativePublication->location(), environment_.paths.layoutCache(),
         run.sourceLease, run.nativePublication->resources(run.sourceLease), positionCodec(),
         environment_.paths.documentCache(), temporaryAppearance, originalAppearance,
         environment_.paths.directory + "/userstyle.css", kobalt::appearance::bookDefaults(preferences)},
        [this](const kobalt::OpeningResult &outcome) {
            run.openingTimings["load_and_layout_ms"] = run.openingTimer.elapsed() - run.layoutStarted;
            if (!outcome.ready()) {
                fail(kobalt::openingMessage(outcome), kobalt::OpeningFallback::Failure, outcome.rememberRejection());
                return;
            }
            if (!sameBook()) {
                fail(strings::get("reader_controller.native_publication_service_closed"));
                return;
            }
            run.expectedHash = run.model->sourceHash();
            run.openingTimings["resource_cache"] = run.model->resourceStats();
            showReader();
        },
        [this](kobalt::OpeningStage stage, int percent) {
            updateOpening(stage, percent);
        });
}

void ReaderCoordinator::showReader() {
    if (kobalt::NativeLifecycle::suspended()) {
        cancelOpening();
        return;
    }
    run.opening.reset();
    busy = false;
    auto book = std::make_unique<kobalt::ReadingBook>(
        std::move(run.model), run.statePath, environment_.paths, run.identity, run.expectedHash,
        kobalt::nickel::makeReadingBridge(run.identity, std::move(run.nativeProgress)), run.baseline, run.diskBaseline);
    auto services = readerHostServices();
    services.coverImagePath = run.nativePublication ? run.nativePublication->coverPath() : QString();
    services.automaticBookOpening = bindings_.automaticBookOpening;
    services.setAutomaticBookOpening = bindings_.setAutomaticBookOpening;
    const auto suspended = services.suspended;
    services.suspended = [this, suspended] {
        return usbDialog_.active() || suspended();
    };
    run.reader = run.interface->createWindow(std::move(book), run.orientation.get(), std::move(services));
    run.reader->finished = [this](std::unique_ptr<kobalt::ReaderSession> retained) {
        // closeEvent has accepted this close. Do not let the visibility
        // poll show the reader again before Qt processes deleteLater().
        usbDialog_.restore();
        run.reader.clear();
        if (!retained || lifecycle->blocked() || kobalt::NativeLifecycle::suspended()) {
            disposeForTransition([] {});
            return;
        }
        run.model = std::move(retained);
        if (run.sourceLease)
            run.sourceLease->cancel();
        // The old facade dies with the window. Drain its last jobs before
        // closing Nickel's service; idle RAM owns no native file handles.
        lifecycle->draining = true;
        ++generation;
        drainReadingJobs([this] {
            if (run.model)
                run.model->cancelSource();
            run.nativePublication.reset();
            run.opening.reset();
            run.orientation.reset();
            kobalt::releaseResourceConnections([this] {
                lifecycle->draining = false;
                QFile::remove(activePath);
                status("closed");
                finishHomeNavigation();
                // USB or revocation may have arrived during the barrier.
                if (!cleanupCallbacks.isEmpty()) {
                    disposeForTransition([] {});
                    return;
                }
                if (lifecycle->blocked() && !lifecycle->storageBlocked())
                    kobalt::engineActivity().pause(true);
                else if (!lifecycle->blocked()) {
                    authorization->setBlocked(false);
                    timer.start();
                }
            });
        });
    };
    run.reader->showFullScreen();
    // Showing the window paints it synchronously but also defers widget
    // updates. Finish those before removing the loader so its exposure
    // and the completed reading page share one compositor submission.
    for (auto *widget : run.reader->findChildren<QWidget *>())
        QCoreApplication::sendPostedEvents(widget, QEvent::UpdateLater);
    QCoreApplication::sendPostedEvents(run.reader, QEvent::UpdateLater);
    QCoreApplication::sendPostedEvents(run.reader, QEvent::UpdateRequest);
    hideLoading();
    run.openingTimings["total_ms"] = run.openingTimer.elapsed();
    status("reading");
}

ReaderCoordinator::~ReaderCoordinator() = default;
}
