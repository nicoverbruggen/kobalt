#include "reader_p.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include <QTimer>
#include "app/reader/views/kosync_notice.h"
#include <chrono>
#include <utility>
#include <QLocale>

namespace kobalt::reader {
namespace {
QString unavailableConnection(const char *stage)
{
    const auto message = strings::get("kosync.no_connection");
    diagnostic("kosync-network-unavailable",
               qt::object({ { "stage", stage },
                            { "code", "kosync.no_connection" },
                            { "guidance", message } }));
    return message;
}
} // namespace

QByteArray Reader::kosyncDocument() const
{
    return kosyncClient->filenameMatching() ? session->syncFilenameDigest() : session->syncDigest();
}

void Reader::kosyncStatus(const QString &message)
{
    kosyncMessage = message;
    kosyncPage->setLastSync(kosyncClient->lastSync());
    kosyncPage->setStatus(message,
                          kosyncClient->busy() || kosyncConnecting || kosyncAuthenticating
                                  || bool(kosyncReaderReady));
    updateKosyncToolbar();
    if (general->isVisible() && !lifecycleHidden && !closing && !exportRunning) {
        fitSettings();
        refresh();
    }
}

void Reader::buildKosync()
{
    if (services.kosyncHttp)
        kosyncHttp = services.kosyncHttp();
    kosyncClient = std::make_unique<kosync::Client>(book->paths().syncAccount(), nullptr,
                                                    kosyncHttp.get());
    if (kosyncClient->signedIn() && !book->usesNativeCloudSync() && services.syncNetwork)
        kosyncNetwork = services.syncNetwork();
    kosyncClient->setDefaultDeviceName(services.deviceName.isEmpty()
                                               ? QStringLiteral("Kobalt")
                                               : services.deviceName + QStringLiteral(" (Kobalt)"));
    kosyncPage->setNativeCloudSync(book->usesNativeCloudSync(), book->isKoboCloudBook());
    const auto account = [this] {
        kosyncPage->setFilenameMatching(kosyncClient->filenameMatching());
        kosyncPage->setDeviceName(kosyncClient->customDeviceName(),
                                  kosyncClient->defaultDeviceName());
        kosyncPage->setAccount(kosyncClient->server(), kosyncClient->username(),
                               kosyncClient->signedIn(), kosyncClient->automatic());
        kosyncPage->setSchedule(kosyncClient->schedule());
        updateKosyncToolbar();
    };
    account();
    kosyncPage->filenameMatchingChanged = [this](bool enabled) {
        if (enabled == kosyncClient->filenameMatching())
            return;
        const bool saved = kosyncClient->setFilenameMatching(enabled);
        if (saved) {
            cancelKosync();
            kosyncProgress = {};
            kosyncPositionFailed = false;
            kosyncPromptDeferred = false;
            restartKosync();
        }
        kosyncPage->setFilenameMatching(kosyncClient->filenameMatching());
        kosyncStatus(strings::get(!saved ? "kosync.storage_error"
                                          : enabled && session->syncFilenameDigest().isEmpty()
                                          ? "kosync.filename_unavailable"
                                          : "kosync.matching_changed"));
    };
    kosyncPage->deviceNameChanged = [this](const QString &name) {
        const bool saved = kosyncClient->setDeviceName(name);
        kosyncPage->setDeviceName(kosyncClient->customDeviceName(),
                                  kosyncClient->defaultDeviceName());
        kosyncStatus(strings::get(saved ? "kosync.device_name_saved" : "kosync.storage_error"));
    };
    kosyncPage->loginRequested = [this, account] {
        if (kosyncClient->busy() || kosyncConnecting || kosyncAuthenticating || kosyncReaderReady)
            return;
        const auto server = kosyncPage->server(), username = kosyncPage->username();
        const auto password = kosyncPage->takePassword();
        if (!kosync::validLogin(server, username, password)) {
            kosyncStatus(strings::get("kosync.invalid_account"));
            return;
        }
        kosyncAuthenticating = true;
        prepareKosyncNetwork(true, [this, account, server, username, password] {
            kosyncDeadline.start(10000);
            kosyncClient->login(server, username, password, [this, account](const QString &error) {
                kosyncDeadline.stop();
                kosyncAuthenticating = false;
                if (error.isEmpty()) {
                    ++kosyncEpoch;
                    kosyncProgress = {};
                    kosyncPositionFailed = false;
                    kosyncPromptDeferred = false;
                    restartKosync();
                    account();
                }
                kosyncStatus(error.isEmpty() ? strings::get("kosync.signed_in") : error);
            });
            if (kosyncClient->busy())
                kosyncStatus(strings::get("kosync.signing_in"));
        });
    };
    kosyncPage->logoutRequested = [this, account] {
        ++kosyncEpoch;
        if (kosyncClient->logout()) {
            kosyncProgress = {};
            kosyncPositionFailed = false;
            kosyncPromptDeferred = false;
            restartKosync();
            account();
            kosyncStatus(strings::get("kosync.signed_out"));
        } else
            kosyncStatus(strings::get("kosync.storage_error"));
    };
    kosyncPage->automaticChanged = [this, account](bool enabled) {
        if (!kosyncClient->setAutomatic(enabled))
            kosyncStatus(strings::get("kosync.storage_error"));
        account();
        if (enabled)
            restartKosync();
        kosyncStatus(kosyncMessage);
    };
    kosyncPage->scheduleChanged = [this, account](const kosync::Schedule &schedule) {
        if (!kosyncClient->setSchedule(schedule))
            kosyncStatus(strings::get("kosync.storage_error"));
        kosyncBackgroundFailures = 0;
        account();
        kosyncStatus(kosyncMessage);
    };
    kosyncPage->syncRequested = [this] { synchronizeKosync(true); };
    updateKosyncFeatures();
    const auto status = book->usesNativeCloudSync()
            ? strings::get("kosync.native_sync")
            : strings::get(kosyncClient->signedIn() ? "kosync.signed_in" : "kosync.signed_out");
    kosyncStatus(kosyncClient->storageError().isEmpty() ? status : kosyncClient->storageError());
    kosyncFinishingElapsed.invalidate();
    // Names let tests check and run the timers instead of waiting for them.
    kosyncDeadline.setObjectName("rrKosyncDeadline");
    kosyncOpeningGrace.setObjectName("rrKosyncOpeningGrace");
    kosyncDeadline.setSingleShot(true);
    connect(&kosyncDeadline, &QTimer::timeout, this, [this] {
        const auto code = kosyncConnecting ? "kosync.connection_timeout"
                : kosyncReaderReady        ? "kosync.reader_timeout"
                                           : "kosync.timeout_error";
        diagnostic("kosync-deadline",
                   qt::object({ { "code", code },
                                { "guidance", strings::get(code) },
                                { "connecting", kosyncConnecting },
                                { "request_busy", kosyncClient->busy() },
                                { "suspended", services.suspended() },
                                { "reader_waiting", bool(kosyncReaderReady) } }));
        ++kosyncEpoch;
        if (kosyncNetwork)
            kosyncNetwork->cancel();
        // Only background connections run under this deadline; interactive
        // ones start it after they connect. A connection that never answers
        // counts as a failed background attempt.
        if (kosyncConnecting)
            ++kosyncBackgroundFailures;
        kosyncConnecting = false;
        kosyncAuthenticating = false;
        kosyncClient->cancel();
        kosyncFlushWaiting = false;
        finishKosync(strings::get(code));
    });
    kosyncOpeningGrace.setSingleShot(true);
    connect(&kosyncOpeningGrace, &QTimer::timeout, this, [this] {
        if (kosyncOpening)
            showKosyncOpening();
        finishKosyncPresentation();
    });
    kosyncReaderRetry.setInterval(100);
    connect(&kosyncReaderRetry, &QTimer::timeout, this, [this] {
        if (kosyncReaderReady)
            continueKosyncWhenReady(std::move(kosyncReaderReady));
    });
}

// While you read, a changed position syncs once the chosen interval has passed
// since the last submission: 30 seconds for "every page". No timer runs, so a
// reader left open wakes nothing; its last position goes up at the next page
// or, if those syncs are on, at sleep or close.
void Reader::syncKosyncWhileReading()
{
    if (!kosyncStarted)
        return;
    const auto position = session->state().value("position").toString();
    if (position.isEmpty() || position == kosyncSeenPosition)
        return;
    kosyncSeenPosition = position;
    const auto schedule = kosyncClient->schedule();
    if (!kosync::syncsWhileReading(schedule)
        || kosyncClock() - kosyncSubmitted < kosync::readingIntervalMilliseconds(schedule))
        return;
    synchronizeKosync();
}

// Monotonic time, which on Kobo and Android leaves out time asleep, so the
// interval counts reading time.
qint64 Reader::kosyncClock() const
{
    if (services.monotonicMilliseconds) {
        // A host clock that fails reports -1; use the steady clock instead.
        const auto now = services.monotonicMilliseconds();
        if (now >= 0)
            return now;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

void Reader::updateKosyncFeatures()
{
    // A desktop e-reader preview shows what a Kobo offers. A desktop window
    // or a phone layout offers only what its host supports.
    const bool ereaderPreview =
            services.devicePreview && services.devicePreview() && !services.phoneLayout;
    kosyncPage->setScheduleFeatures(services.kosyncSleepWake || ereaderPreview,
                                    services.kosyncWifiControl || ereaderPreview);
}

void Reader::updateKosyncToolbar()
{
    if (!toolbarComposition.sync || !kosyncClient)
        return;
    toolbarComposition.sync->setVisible(kosyncClient->signedIn() && !kosyncClient->automatic()
                                        && !book->usesNativeCloudSync());
    toolbarComposition.sync->setEnabled(!kosyncClient->busy() && !kosyncConnecting
                                        && !kosyncAuthenticating && !kosyncReaderReady);
}

void Reader::requestManualKosync()
{
    if (!kosyncClient->signedIn() || kosyncClient->automatic() || book->usesNativeCloudSync()
        || kosyncDialog)
        return;
    if (!kosyncClient->manualExplanation()) {
        synchronizeKosync(true);
        return;
    }
    auto *dialog =
            new ui::ChoiceModal(this,
                                { strings::get("kosync.manual_prompt_description"),
                                  strings::get("kosync.manual_prompt"),
                                  {} },
                                { strings::get("common.cancel"), "rrKosyncManualCancel" },
                                { strings::get("kosync.sync_now"), "rrKosyncManualConfirm" });
    dialog->setObjectName("rrKosyncManualPrompt");
    kosyncDialog = dialog;
    const auto dismiss = [this, dialog] {
        dialog->hide();
        dialog->deleteLater();
        kosyncDialog = nullptr;
        refresh();
    };
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, dismiss);
    dialog->dismissed = dismiss;
    connect(dialog->primaryButton(), &QPushButton::clicked, this, [this, dismiss] {
        dismiss();
        const bool saved = kosyncClient->setManualExplanation(false);
        if (!saved) {
            kosyncStatus(strings::get("kosync.storage_error"));
            refresh();
            page->notice = kosyncMessage;
            page->update();
            return;
        }
        synchronizeKosync(true);
    });
    dialog->show();
    dialog->raise();
    dialog->fitCard();
    refresh();
}

void Reader::prepareKosyncPresentation(std::function<void(bool)> finished)
{
    if (kosyncStarted || book->usesNativeCloudSync() || !kosyncClient->signedIn()
        || !kosyncAutomatic() || !kosyncClient->schedule().onOpen || book->pendingProgress()
        || book->deviceTransition() || session->pages() <= 0 || session->temporaryTypography()) {
        finished(true);
        return;
    }
    // Keep the host's loading screen for a fast exchange. The same request
    // continues behind a cancellable popover once the reader is presented.
    kosyncPresentationReady = std::move(finished);
    kosyncStarted = true;
    kosyncSubmitted = kosyncClock();
    kosyncOpening = true;
    kosyncOpeningGrace.start(500);
    kosyncDeadline.start(10000);
    prepareKosyncNetwork(false, [this] { startKosyncExchange(false); });
}

bool Reader::wifiPowerSaverVisible() const
{
    // E-reader previews leave the row out, as Kobo does until beta 7.
    return bool(services.turnOffWifi);
}

bool Reader::wifiPowerSaverApplies(bool sleeping) const
{
    if (!services.turnOffWifi || !preferences.value(wifiPowerSaverKey).toBool(false)
        || menuBluetooth->active())
        return false;
    // Nickel's own sync carries store books and books from services that
    // sync through it, and uploads their position when the book closes.
    if (book->usesNativeCloudSync())
        return false;
    // Automatic KOSync expects a connection while the book is open. At sleep
    // its last sync has ended, and wake connects again if it syncs.
    return sleeping || !kosyncClient->signedIn() || !kosyncAutomatic();
}

void Reader::saveWifiPower(std::function<void()> next)
{
    if (!wifiPowerSaverApplies()) {
        next();
        return;
    }
    QPointer<Reader> guard(this);
    services.turnOffWifi(this, "wifi-off before reader input", [guard, next](bool turnedOff) {
        if (!guard)
            return;
        diagnostic("wifi-power-saver",
                   qt::object({ { "turned_off", turnedOff }, { "moment", "open" } }));
        next();
    });
}

// Nickel turns Wi-Fi off itself about 20 seconds after the sleep screen. With
// the power saver on, Kobalt does it as soon as the sleep sync has ended.
void Reader::saveWifiPowerAtSleep()
{
    cancelWifiPowerAtSleep();
    if (!lifecycleHidden || closing || !wifiPowerSaverApplies(true))
        return;
    auto *request = new QObject(this);
    wifiSleepRequest = request;
    services.turnOffWifi(
            request, "wifi-off at sleep", [request = QPointer<QObject>(request)](bool turnedOff) {
                diagnostic("wifi-power-saver",
                           qt::object({ { "turned_off", turnedOff }, { "moment", "sleep" } }));
                if (request)
                    request->deleteLater();
            });
}

void Reader::cancelWifiPowerAtSleep()
{
    delete wifiSleepRequest.data();
}

void Reader::finishKosyncPresentation()
{
    kosyncOpeningGrace.stop();
    auto finished = std::exchange(kosyncPresentationReady, {});
    if (finished)
        finished(!closing && !lifecycleHidden);
}

void Reader::synchronizeKosync(bool interactive)
{
    if (book->usesNativeCloudSync()) {
        kosyncStarted = true;
        if (interactive)
            kosyncStatus(strings::get("kosync.native_sync"));
        return;
    }
    if (kosyncProgress.pending()) {
        if (!interactive && !kosyncAutomatic())
            return;
        if (interactive) {
            kosyncPositionFailed = false;
            kosyncStatus(strings::get("kosync.pending"));
        } else {
            // A deferred choice comes back once per interval, not on every page.
            kosyncSubmitted = kosyncClock();
        }
        kosyncPromptDeferred = false;
        showKosyncChoice();
        return;
    }
    if (!kosyncClient->signedIn() || (!interactive && !kosyncAutomatic()) || kosyncClient->busy()
        || kosyncConnecting || kosyncAuthenticating || kosyncReaderReady || kosyncDialog || closing
        || lifecycleHidden || exportRunning || services.suspended() || book->deviceTransition()
        || book->pendingProgress() || (!interactive && session->busy())
        || session->temporaryTypography() || !isVisible() || session->pages() <= 0
        || (!interactive && topControls->isVisible()))
        return;
    const bool opening = !kosyncStarted && !interactive;
    // Opening and waking each have their own switch. A skipped one still
    // counts as started, so periodic syncs and Sync now continue as usual.
    if (opening
        && !(std::exchange(kosyncWaking, false) ? kosyncClient->schedule().onSleepWake
                                                : kosyncClient->schedule().onOpen)) {
        kosyncStarted = true;
        // The interval starts now, so the first page does not do the skipped sync.
        kosyncSubmitted = kosyncClock();
        return;
    }
    if (!interactive && !opening
        && !kosyncProgress.needsSync(session->state().value("position").toString()))
        return;
    kosyncStarted = true;
    kosyncSubmitted = kosyncClock();
    if (interactive)
        kosyncBackgroundFailures = 0;
    if (!interactive)
        kosyncDeadline.start(10000);
    if (opening)
        showKosyncOpening();
    if (opening || interactive)
        prepareKosyncNetwork(interactive, [this, interactive] {
            if (interactive) {
                kosyncDeadline.start(10000);
                showKosyncOpening();
            }
            startKosyncExchange(false);
        });
    else if (kosyncNetwork && !kosyncNetwork->connected()) {
        // Without permission to turn on Wi-Fi, or after repeated failures,
        // the position stays local until the next connected sync.
        if (!kosyncClient->schedule().wakeWifi
            || kosyncBackgroundFailures >= kosync::backgroundConnectionLimit) {
            finishKosync(unavailableConnection("periodic"));
            return;
        }
        prepareKosyncNetwork(false, [this] { startKosyncExchange(false); });
    } else {
        kosyncBackgroundFailures = 0;
        startKosyncExchange(false);
    }
}

void Reader::prepareKosyncNetwork(bool interactive, std::function<void()> ready)
{
    if (!kosyncNetwork && services.syncNetwork)
        kosyncNetwork = services.syncNetwork();
    if (!kosyncNetwork) {
        ready();
        return;
    }
    const auto epoch = kosyncEpoch;
    kosyncConnecting = true;
    // Opening, wake and periodic syncs connect silently and turn off the
    // Wi-Fi they turned on when they end. Syncs the user asks for leave
    // Wi-Fi as it is.
    if (!interactive)
        kosyncReleaseWifi = true;
    kosyncStatus(strings::get("kosync.connecting"));
    kosyncNetwork->prepare(interactive, [this, epoch, ready, interactive](bool connected) {
        if (epoch != kosyncEpoch)
            return;
        kosyncConnecting = false;
        if (!interactive)
            kosyncBackgroundFailures = connected ? 0 : kosyncBackgroundFailures + 1;
        if (!connected) {
            const auto message = unavailableConnection("preparation");
            finishKosync(message);
            if (interactive && !general->isVisible() && !closing && !lifecycleHidden) {
                page->notice = message;
                page->update();
            }
            return;
        }
        ready();
    });
}

void Reader::hideKosyncOpening()
{
    if (!kosyncOpening)
        return;
    kosyncOpening = false;
    if (kosyncDialog) {
        const bool refreshPage = kosyncDialog->isVisible() && isVisible() && !lifecycleHidden
                && !closing && !exportRunning && !exitPending && !services.suspended();
        kosyncDialog->hide();
        kosyncDialog->deleteLater();
        kosyncDialog = nullptr;
        if (refreshPage) {
            // The unchanged book image does not include the notice's damage.
            requestFlash("kosync-dismiss");
            page->update();
        }
    }
}

void Reader::showKosyncOpening()
{
    auto *dialog = new KosyncNotice(this);
    kosyncOpening = true;
    kosyncDialog = dialog;
    dialog->setObjectName("rrKosyncOpening");
    const auto skip = [this] {
        cancelKosync();
        kosyncStatus(strings::get("kosync.skipped"));
        refresh();
    };
    connect(dialog->primaryButton(), &QPushButton::clicked, this, skip);
    dialog->dismissed = skip;
    dialog->completed = [this, dialog] {
        if (kosyncDialog != dialog)
            return;
        hideKosyncOpening();
        refresh();
    };
    dialog->show();
    dialog->raise();
    dialog->fitCard();
    refresh();
}

void Reader::cancelKosync()
{
    ++kosyncEpoch;
    if (kosyncNetwork)
        kosyncNetwork->cancel();
    releaseKosyncWifi();
    kosyncConnecting = kosyncAuthenticating = false;
    kosyncClient->cancel();
    kosyncDeadline.stop();
    kosyncOpeningGrace.stop();
    kosyncReaderRetry.stop();
    kosyncReaderReady = {};
    kosyncPresentationReady = {};
    kosyncFinished = {};
    kosyncFinishing = false;
    kosyncFlushWaiting = false;
    hideKosyncOpening();
}

// Nickel's off() waits for child processes on this thread. At sleep, wake
// and close the reader takes no input until it returns: wake cancels before
// it shows the reader. While the reader takes input, the guard reports the
// wait as wifi-off.
void Reader::releaseKosyncWifi()
{
    if (!std::exchange(kosyncReleaseWifi, false) || !kosyncNetwork)
        return;
    const char *reason = closing || exportRunning ? "wifi-off while closing"
            : lifecycleHidden                     ? "wifi-off at sleep"
                                                  : nullptr;
    if (!reason) {
        kosyncNetwork->release();
        return;
    }
    mainthread::AllowBlocking allow(reason);
    kosyncNetwork->release();
}

// The next automatic sync is an opening sync again. A wake whose sync could
// not run yet, for example while signed out, no longer decides which switch
// that sync follows.
void Reader::restartKosync()
{
    kosyncStarted = false;
    kosyncWaking = false;
}

void Reader::finishKosync(const QString &message)
{
    kosyncReaderRetry.stop();
    kosyncReaderReady = {};
    // The sleep sync, and an older request that it waited for, end after
    // sleepReader() saved the sync time. Queue the new time without waiting:
    // the device may not wake before its battery runs out.
    if (lifecycleHidden && !closing)
        kosyncClient->queueSyncTime();
    if (kosyncFlushWaiting) {
        kosyncFlushWaiting = false;
        startKosyncExchange(true);
        return;
    }
    kosyncPage->setLastSync(kosyncClient->lastSync());
    releaseKosyncWifi();
    if (kosyncNetwork)
        kosyncNetwork->cancel();
    kosyncConnecting = kosyncAuthenticating = false;
    const bool finishing = kosyncFinishing;
    const bool opening = kosyncOpening;
    auto *notice = dynamic_cast<KosyncNotice *>(kosyncDialog.data());
    const bool showSuccess = !finishing && !closing && !lifecycleHidden && !exportRunning && notice
            && notice->isVisible() && message == strings::get("kosync.up_to_date");
    if (showSuccess)
        notice->complete();
    else
        hideKosyncOpening();
    kosyncDeadline.stop();
    kosyncFinishing = false;
    auto finished = std::move(kosyncFinished);
    kosyncFinished = {};
    kosyncMessage = message;
    kosyncPage->setStatus(message, false);
    updateKosyncToolbar();
    if (!finishing && !closing && !lifecycleHidden && !exportRunning) {
        // A failure can need more lines than the preceding Syncing message.
        if (general->isVisible())
            fitSettings();
        if (opening && message != strings::get("kosync.up_to_date")
            && message != strings::get("kosync.pending"))
            page->notice = message;
        refresh();
        showKosyncChoice();
    }
    finishKosyncPresentation();
    if (finished)
        finished();
}

void Reader::flushKosync(std::function<void()> finished, bool enabled)
{
    // A local checkpoint is already durable. Final uploads use only an existing
    // connection and never resolve a remote conflict while closing or asleep.
    if (kosyncConnecting || kosyncAuthenticating || kosyncReaderReady) {
        // Keep a connection Kobalt turned on for the final upload below;
        // finishKosync, or the cancel when it is not up, turns it off.
        const bool release = std::exchange(kosyncReleaseWifi, false);
        cancelKosync();
        kosyncReleaseWifi = release;
    }
    if (!enabled || book->usesNativeCloudSync() || !kosyncClient->signedIn() || !kosyncAutomatic()
        || kosyncProgress.pending() || book->pendingProgress() || session->temporaryTypography()
        || session->pages() <= 0 || kosyncDocument().isEmpty()
        || session->state().value("position").toString().isEmpty()) {
        cancelKosync();
        finished();
        return;
    }
    if (kosyncNetwork && !kosyncNetwork->connected()) {
        cancelKosync();
        kosyncStatus(unavailableConnection("final"));
        finished();
        return;
    }
    hideKosyncOpening();
    kosyncFinishingElapsed.start();
    kosyncFinishing = true;
    kosyncFinished = std::move(finished);
    kosyncDeadline.start(kosyncFinishBudget);
    // Let an older PUT finish before sending the final position. Aborting its
    // reply cannot guarantee that the server has stopped processing the write.
    kosyncFlushWaiting = kosyncClient->busy();
    if (!kosyncFlushWaiting)
        startKosyncExchange(true);
}

void Reader::continueKosyncWhenReady(std::function<void()> ready)
{
    if (book->pendingProgress() || book->deviceTransition() || closing || lifecycleHidden
        || services.suspended() || session->busy() || session->temporaryTypography()
        || exportRunning || (!isVisible() && !kosyncOpening)) {
        kosyncReaderReady = std::move(ready);
        // A hidden or sleeping reader does not check every 100 ms: refresh()
        // resumes the wait once the reader is shown, and wake starts over.
        const bool hidden =
                lifecycleHidden || services.suspended() || (!isVisible() && !kosyncOpening);
        if (hidden)
            kosyncReaderRetry.stop();
        if (!kosyncReaderRetry.isActive()) {
            diagnostic("kosync-reader-wait",
                       qt::object({ { "model_busy", session->busy() },
                                    { "hidden", !isVisible() },
                                    { "suspended", services.suspended() },
                                    { "pending_progress", book->pendingProgress() },
                                    { "device_transition", book->deviceTransition() },
                                    { "exporting", exportRunning } }));
            if (!hidden)
                kosyncReaderRetry.start();
            kosyncStatus(strings::get("kosync.syncing"));
        }
        return;
    }
    kosyncReaderRetry.stop();
    kosyncReaderReady = {};
    ready();
}

void Reader::startKosyncExchange(bool finishing)
{
    const auto document = kosyncDocument();
    if (document.isEmpty()) {
        finishKosync(strings::get(kosyncClient->filenameMatching() ? "kosync.filename_unavailable"
                                                                   : "kosync.unavailable"));
        return;
    }
    const auto epoch = kosyncEpoch;
    kosync::Progress snapshot;
    snapshot.position = session->state().value("position").toString();
    snapshot.percentage =
            session->pages() > 1 ? double(session->page()) / (session->pages() - 1) : 0;
    // Fetch before every write, including the bounded final attempt. A failed
    // fetch cannot authorize an overwrite of another device's progress.
    kosyncClient->pull(
            document,
            [this, document, epoch, finishing, snapshot](kosync::Progress remote,
                                                         const QString &error) {
                if (epoch != kosyncEpoch)
                    return;
                if (!error.isEmpty()) {
                    finishKosync(error);
                    return;
                }
                if (finishing && (book->pendingProgress() || book->deviceTransition())) {
                    finishKosync(strings::get("kosync.retry_later"));
                    return;
                }
                // A native Wi-Fi dialog or a pending page can outlast the GET. Keep its
                // response until the reader settles; the original deadline and Cancel
                // still own the attempt. Never repeat a PUT to recover this state.
                auto apply = [this, document, epoch, finishing, snapshot, remote] {
                    if (epoch != kosyncEpoch)
                        return;
                    auto local = snapshot;
                    if (!finishing) {
                        local.position = session->state().value("position").toString();
                        local.percentage = session->pages() > 1
                                ? double(session->page()) / (session->pages() - 1)
                                : 0;
                    }
                    const auto action = kosyncProgress.observe(remote, local.position,
                                                               kosyncClient->deviceId());
                    if (action == kosync::Reconciler::Action::Choose) {
                        finishKosync(strings::get("kosync.pending"));
                        return;
                    }
                    const bool renamed = remote.deviceId == kosyncClient->deviceId()
                            && remote.device != kosyncClient->deviceName()
                            && !local.position.isEmpty();
                    if (action == kosync::Reconciler::Action::Current && !renamed) {
                        finishKosync(strings::get(kosyncClient->recordSyncSuccess()
                                                          ? "kosync.up_to_date"
                                                          : "kosync.storage_error"));
                        return;
                    }
                    kosyncClient->push(
                            document, local, [this, epoch, local](const QString &pushError) {
                                if (epoch != kosyncEpoch)
                                    return;
                                if (pushError.isEmpty())
                                    kosyncProgress.uploaded(local.position);
                                finishKosync(
                                        !pushError.isEmpty()
                                                ? pushError
                                                : strings::get(kosyncClient->recordSyncSuccess()
                                                                       ? "kosync.up_to_date"
                                                                       : "kosync.storage_error"));
                            });
                    if (kosyncClient->busy())
                        kosyncStatus(strings::get("kosync.syncing"));
                };
                if (finishing)
                    apply();
                else
                    continueKosyncWhenReady(std::move(apply));
            },
            !finishing && kosyncOpening);
    if (kosyncClient->busy())
        kosyncStatus(strings::get("kosync.syncing"));
}

void Reader::showKosyncChoice()
{
    if (!kosyncProgress.pending() || kosyncPositionFailed || kosyncPromptDeferred || kosyncDialog
        || closing || lifecycleHidden || !isVisible() || session->busy() || services.suspended()
        || book->deviceTransition() || book->pendingProgress() || syncDialog || finishDialog
        || topControls->isVisible() || general->isVisible() || annotationMode
        || searchPanel->isVisible() || dictionaryPanel->isVisible()
        || (imageViewer && imageViewer->isVisible()) || linksActive() || lookupPending
        || appearanceEdit.panelOpen())
        return;
    const auto remote = kosyncProgress.candidate();
    auto *dialog = new ui::ChoiceModal(
            this,
            { strings::get("kosync.comparing_positions"), strings::get("kosync.resume_title"), {} },
            { strings::get("kosync.keep_local"), "rrKosyncKeep" },
            { strings::get("kosync.use_remote"), "rrKosyncResume" });
    kosyncDialog = dialog;
    dialog->setObjectName("rrKosyncChoice");
    dialog->setDimmed(true);
    const auto dismiss = [this, dialog] {
        dialog->hide();
        dialog->deleteLater();
        kosyncDialog = nullptr;
    };
    const auto keep = [this, dismiss] {
        kosyncProgress.keepLocal();
        kosyncPositionFailed = false;
        dismiss();
        kosyncStatus(strings::get("kosync.kept_local"));
    };
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, keep);
    // Closing the choice does not authorize a write over the remote position.
    dialog->dismissed = [this, dismiss] {
        kosyncPromptDeferred = true;
        dismiss();
    };
    connect(dialog->primaryButton(), &QPushButton::clicked, this, [this, remote, dismiss] {
        if (session->busy() || closing || lifecycleHidden || services.suspended())
            return;
        dismiss();
        if (!session->goToSyncedPosition(remote.position, [this](const QString &error) {
                if (error.isEmpty()) {
                    kosyncProgress.acceptRemote(session->state().value("position").toString());
                    // Both writes are queued on the profile writer thread, which
                    // runs them in queue order, so the position lands before the
                    // sync time. Only an earlier sync time still waiting in the
                    // queue could land first. The position is a routine write
                    // that the next sync puts on storage, so a power loss can keep
                    // the new sync time and lose the position; the next sync then
                    // asks again.
                    save(true, true);
                    if (!kosyncClient->recordSyncSuccess()) {
                        kosyncStatus(strings::get("kosync.storage_error"));
                        refresh();
                        return;
                    }
                } else {
                    // Do not offer the same invalid position indefinitely. Keep
                    // writes blocked until the user explicitly keeps local progress.
                    kosyncPositionFailed = true;
                    kosyncClient->setAutomatic(false);
                    updateKosyncToolbar();
                    kosyncPage->setAccount(kosyncClient->server(), kosyncClient->username(), true,
                                           kosyncClient->automatic());
                }
                page->notice = error.isEmpty() ? strings::get("kosync.resumed") : error;
                kosyncStatus(page->notice);
                refresh();
            }))
            kosyncStatus(strings::get("kosync.retry_later"));
    });
    dialog->setGeometry(rect());
    dialog->show();
    dialog->raise();
    dialog->fitCard();
    const auto epoch = kosyncEpoch;
    const auto localChapter = session->chapterTitle();
    const double localPercentage =
            session->pages() > 1 ? 100.0 * session->page() / (session->pages() - 1) : 0;
    dialog->primaryButton()->setEnabled(false);
    QPointer<ui::ChoiceModal> guard(dialog);
    session->describeSyncedPosition(
            remote.position,
            [this, guard, epoch, remote, localChapter,
             localPercentage](SyncedPositionDescription info) {
                if (!guard || !guard->isVisible() || epoch != kosyncEpoch)
                    return;
                const bool compared =
                        info.comparison.status == PositionComparison::Status::Compared;
                const bool backward = compared && info.comparison.direction < 0;
                const auto direction = strings::get(!compared          ? "kosync.direction_unknown"
                                                            : backward ? "kosync.direction_backward"
                                                            : info.comparison.direction > 0
                                                            ? "kosync.direction_forward"
                                                            : "kosync.direction_same");
                auto *description = guard->descriptionLabel();
                const int width = guard->card()->width()
                        - 2 * int(ui::metrics::notice::cardMargins.left()) - 4;
                const auto chapter = [&](QString value) {
                    if (value.trimmed().isEmpty())
                        value = strings::get("kosync.chapter_unavailable");
                    return ui::elideInterfaceText(value.simplified(), description->font(), width);
                };
                const auto device = remote.device.trimmed().isEmpty()
                        ? strings::get("kosync.device_unknown")
                        : remote.device.simplified();
                const auto timestamp = remote.timestamp > 0
                        ? QDateTime::fromMSecsSinceEpoch(remote.timestamp * 1000).toLocalTime()
                        : QDateTime();
                const auto synced = timestamp.isValid()
                        ? strings::get("kosync.synced_at")
                                  .arg(QLocale().toString(timestamp, QLocale::ShortFormat))
                        : strings::get("kosync.time_unavailable");
                description->setText(
                        strings::get("kosync.position_comparison")
                                .arg(QLocale().toString(localPercentage, 'f', 1),
                                     chapter(localChapter),
                                     QLocale().toString(remote.percentage * 100, 'f', 1),
                                     chapter(info.chapter),
                                     ui::elideInterfaceText(device, description->font(), width),
                                     synced, direction));
                guard->findChild<QLabel *>("rrModalTitle")
                        ->setText(strings::get(backward ? "kosync.resume_backward_title"
                                                        : "kosync.resume_title"));
                guard->primaryButton()->setEnabled(true);
                if (backward)
                    guard->secondaryButton()->setFocus(Qt::OtherFocusReason);
                guard->fitCard();
                refresh();
            });
}
} // namespace kobalt::reader
