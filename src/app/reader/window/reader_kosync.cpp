#include "reader_p.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include <QTimer>
#include "app/reader/views/kosync_notice.h"
#include "app/reader/views/sync_choice.h"
#include <utility>
#include <QLocale>

namespace kobalt::reader {
QByteArray Reader::kosyncDocument() const
{
    return kosyncClient->filenameMatching() ? session->syncFilenameDigest() : session->syncDigest();
}

void Reader::kosyncStatus(const QString &message)
{
    kosyncMessage = message;
    kosyncPage->setLastSync(kosyncClient->lastSync());
    kosyncPage->setStatus(message, syncCoordinator->busy());
    updateKosyncToolbar();
    if (general->isVisible() && !lifecycleHidden && !closing && !exportRunning) {
        fitSettings();
        refresh();
    }
}

void Reader::buildKosync()
{
    kosyncIntervalClock = ReadingTimer(services.monotonicMilliseconds);
    kosyncIntervalClock.start();
    if (services.kosyncHttp)
        kosyncHttp = services.kosyncHttp();
    kosyncClient = std::make_unique<kosync::Client>(book->paths().syncAccount(), nullptr,
                                                    kosyncHttp.get());
    using Coordinator = kosync::SyncCoordinator;
    syncCoordinator = std::make_unique<Coordinator>(
            *kosyncClient, services.syncNetwork,
            [this] {
                Coordinator::Context state;
                state.document = kosyncDocument();
                state.local.position = session->state().value("position").toString();
                // With only a part of the book laid out, the page numbers
                // are those of the part. The percentage is of the whole book.
                state.local.percentage = session->state().contains("book_fraction")
                        ? session->progressFraction()
                        : session->pages() > 1 ? double(session->page()) / (session->pages() - 1)
                                               : 0;
                state.metadata = { session->syncFilename(), session->title().trimmed(),
                                   session->author().trimmed() };
                const bool hidden =
                        lifecycleHidden || services.suspended() || (!isVisible() && !kosyncOpening);
                const bool waiting = book->pendingProgress() || book->deviceTransition() || closing
                        || lifecycleHidden || services.suspended() || session->busy()
                        || session->temporaryTypography() || exportRunning
                        || (!isVisible() && !kosyncOpening);
                state.readiness = hidden ? Coordinator::Readiness::Hidden
                        : waiting        ? Coordinator::Readiness::Waiting
                                         : Coordinator::Readiness::Ready;
                state.finalWriteAllowed = !book->pendingProgress() && !book->deviceTransition();
                state.queueSyncTime = lifecycleHidden && !closing;
                state.suspended = services.suspended();
                state.waitEvidence = qt::object({ { "model_busy", session->busy() },
                                                  { "hidden", !isVisible() },
                                                  { "suspended", state.suspended },
                                                  { "pending_progress", book->pendingProgress() },
                                                  { "device_transition", book->deviceTransition() },
                                                  { "exporting", exportRunning } });
                state.releaseBlockingReason = closing || exportRunning ? "wifi-off while closing"
                        : lifecycleHidden                              ? "wifi-off at sleep"
                                                                       : nullptr;
                return state;
            },
            [this](const Coordinator::Event &event) {
                switch (event.kind) {
                case Coordinator::EventKind::Status:
                    kosyncStatus(event.message);
                    break;
                case Coordinator::EventKind::InteractiveReady:
                    showKosyncOpening();
                    break;
                case Coordinator::EventKind::Cancelled:
                    ++kosyncChoiceEpoch;
                    kosyncOpeningGrace.stop();
                    kosyncPresentationReady = {};
                    hideKosyncOpening();
                    break;
                case Coordinator::EventKind::FinalStarted:
                    hideKosyncOpening();
                    break;
                case Coordinator::EventKind::Completed: {
                    const QPointer<Reader> guard(this);
                    finishKosync(event.message, event.finalUpload);
                    if (guard && event.interactiveConnectionFailed && !general->isVisible()
                        && !closing && !lifecycleHidden) {
                        page->notice = event.message;
                        page->update();
                    }
                    break;
                }
                }
            },
            features::enabled(preferences, features::Kosync) && kosyncClient->signedIn()
                    && !book->usesNativeCloudSync(),
            this);
    kosyncClient->setDefaultDeviceName(services.deviceName.isEmpty()
                                               ? QStringLiteral("Kobalt")
                                               : services.deviceName + QStringLiteral(" (Kobalt)"));
    kosyncPage->setNativeCloudSync(book->nativeSyncPolicy());
    const auto account = [this] {
        kosyncPage->setFilenameMatching(kosyncClient->filenameMatching());
        kosyncPage->setSendMetadata(kosyncClient->sendMetadata());
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
            syncCoordinator->resetProgress();
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
    kosyncPage->sendMetadataChanged = [this](bool enabled) {
        // Applies from the next upload. A failed save keeps the old choice.
        if (!kosyncClient->setSendMetadata(enabled))
            kosyncStatus(strings::get("kosync.storage_error"));
        kosyncPage->setSendMetadata(kosyncClient->sendMetadata());
    };
    kosyncPage->deviceNameChanged = [this](const QString &name) {
        const bool saved = kosyncClient->setDeviceName(name);
        kosyncPage->setDeviceName(kosyncClient->customDeviceName(),
                                  kosyncClient->defaultDeviceName());
        kosyncStatus(strings::get(saved ? "kosync.device_name_saved" : "kosync.storage_error"));
    };
    kosyncPage->loginRequested = [this, account] {
        if (syncCoordinator->busy())
            return;
        const auto server = kosyncPage->server(), username = kosyncPage->username();
        const auto password = kosyncPage->takePassword();
        if (!kosync::validLogin(server, username, password)) {
            kosyncStatus(strings::get("kosync.invalid_account"));
            return;
        }
        syncCoordinator->login(server, username, password, [this, account](const QString &error) {
            if (error.isEmpty()) {
                ++kosyncChoiceEpoch;
                kosyncPositionFailed = false;
                kosyncPromptDeferred = false;
                restartKosync();
                account();
            }
            kosyncStatus(error.isEmpty() ? strings::get("kosync.signed_in") : error);
        });
    };
    kosyncPage->logoutRequested = [this, account] {
        cancelKosync();
        if (kosyncClient->logout()) {
            syncCoordinator->resetProgress();
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
        syncCoordinator->resetBackgroundFailures();
        account();
        kosyncStatus(kosyncMessage);
    };
    kosyncPage->syncRequested = [this] { synchronizeKosync(true); };
    updateKosyncFeatures();
    const auto status = book->usesNativeCloudSync()
            ? strings::get("kosync.native_sync")
            : strings::get(kosyncClient->signedIn() ? "kosync.signed_in" : "kosync.signed_out");
    kosyncStatus(kosyncClient->storageError().isEmpty() ? status : kosyncClient->storageError());
    kosyncOpeningGrace.setObjectName("rrKosyncOpeningGrace");
    kosyncOpeningGrace.setSingleShot(true);
    connect(&kosyncOpeningGrace, &QTimer::timeout, this, [this] {
        if (kosyncOpening)
            showKosyncOpening();
        finishKosyncPresentation();
    });
}

// While you read, a changed position syncs once the chosen interval has passed
// since the last submission: 30 seconds for "every page". No timer runs, so a
// reader left open wakes nothing; its last position goes up at the next page
// or, if those syncs are on, at sleep or close.
void Reader::syncKosyncWhileReading()
{
    if (!features::enabled(preferences, features::Kosync))
        return;
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

// The host determines whether standby counts. Keep one origin throughout the
// opening; a failed or backward sample must not make an interval suddenly due.
qint64 Reader::kosyncClock() const
{
    return kosyncIntervalClock.elapsed();
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
    toolbarComposition.sync->setVisible(features::enabled(preferences, features::Kosync)
                                        && kosyncClient->signedIn() && !kosyncClient->automatic()
                                        && !book->usesNativeCloudSync() && !zenMode());
    toolbarComposition.sync->setEnabled(!syncCoordinator->busy());
}

void Reader::requestManualKosync()
{
    if (!features::enabled(preferences, features::Kosync))
        return;
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
    syncCoordinator->start(kosync::SyncCoordinator::Connection::Background, true);
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
    if (!features::enabled(preferences, features::Kosync)) {
        kosyncStarted = true;
        return;
    }
    if (book->usesNativeCloudSync()) {
        kosyncStarted = true;
        if (interactive)
            kosyncStatus(strings::get("kosync.native_sync"));
        return;
    }
    if (syncCoordinator->pendingProgress()) {
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
    if (!kosyncClient->signedIn() || (!interactive && !kosyncAutomatic()) || syncCoordinator->busy()
        || kosyncDialog || closing || lifecycleHidden || exportRunning || services.suspended()
        || book->deviceTransition() || book->pendingProgress() || (!interactive && session->busy())
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
        && !syncCoordinator->needsSync(session->state().value("position").toString()))
        return;
    kosyncStarted = true;
    kosyncSubmitted = kosyncClock();
    if (opening)
        showKosyncOpening();
    using Connection = kosync::SyncCoordinator::Connection;
    const auto connection = interactive    ? Connection::Interactive
            : opening                      ? Connection::Background
            : syncCoordinator->connected() ? Connection::Existing
            : kosyncClient->schedule().wakeWifi
                    && syncCoordinator->backgroundFailures() < kosync::backgroundConnectionLimit
            ? Connection::Background
            : Connection::Unavailable;
    syncCoordinator->start(connection, opening || interactive);
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
    if (syncCoordinator)
        syncCoordinator->cancel();
}

// The next automatic sync is an opening sync again. A wake whose sync could
// not run yet, for example while signed out, no longer decides which switch
// that sync follows.
void Reader::restartKosync()
{
    kosyncStarted = false;
    kosyncWaking = false;
}

void Reader::finishKosync(const QString &message, bool finishing)
{
    kosyncPage->setLastSync(kosyncClient->lastSync());
    const bool opening = kosyncOpening;
    auto *notice = dynamic_cast<KosyncNotice *>(kosyncDialog.data());
    const bool showSuccess = !finishing && !closing && !lifecycleHidden && !exportRunning && notice
            && notice->isVisible() && message == strings::get("kosync.up_to_date");
    if (showSuccess)
        notice->complete();
    else
        hideKosyncOpening();
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
}

void Reader::flushKosync(std::function<void()> finished, bool enabled)
{
    // Lifecycle policy stays here. The coordinator owns cancellation, the
    // existing connection and the bounded handoff from an older upload.
    const bool eligible = enabled && !book->usesNativeCloudSync() && kosyncClient->signedIn()
            && kosyncAutomatic() && !book->pendingProgress() && !session->temporaryTypography()
            && session->pages() > 0 && !kosyncDocument().isEmpty()
            && !session->state().value("position").toString().isEmpty();
    syncCoordinator->flush(eligible, std::move(finished));
}

void Reader::showKosyncChoice()
{
    if (!features::enabled(preferences, features::Kosync))
        return;
    if (!syncCoordinator->pendingProgress() || kosyncPositionFailed || kosyncPromptDeferred
        || positionPromptOpen() || closing || lifecycleHidden || !isVisible() || session->busy()
        || services.suspended() || book->deviceTransition() || book->pendingProgress()
        || topControls->isVisible() || general->isVisible() || annotationMode
        || searchPanel->isVisible() || dictionaryPanel->isVisible()
        || (imageViewer && imageViewer->isVisible()) || linksActive() || lookupPending
        || appearanceEdit.panelOpen())
        return;
    const auto remote = syncCoordinator->candidate();
    auto *dialog = new ui::ChoiceModal(
            this,
            { strings::get("kosync.comparing_positions"), strings::get("kosync.resume_title"), {} },
            { strings::get("kosync.stay_here"), "rrKosyncKeep" },
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
        syncCoordinator->keepLocal();
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
                    syncCoordinator->acceptRemote(session->state().value("position").toString());
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
    const auto epoch = kosyncChoiceEpoch;
    dialog->primaryButton()->setEnabled(false);
    QPointer<ui::ChoiceModal> guard(dialog);
    session->describeSyncedPosition(
            remote.position, [this, guard, epoch, remote](SyncedPositionDescription info) {
                if (!guard || !guard->isVisible() || epoch != kosyncChoiceEpoch)
                    return;
                const bool compared =
                        info.comparison.status == PositionComparison::Status::Compared;
                const int direction = compared ? info.comparison.direction : 0;
                const auto links = session->chapters();
                SyncJump jump;
                jump.pages = session->bookPages();
                jump.here = session->bookPage();
                jump.synced = compared && info.page < jump.pages ? info.page : -1;
                jump.starts = syncChapterStarts(links);
                jump.nested = std::any_of(links.cbegin(), links.cend(),
                                          [](const ChapterLink &link) { return link.depth > 0; });
                jump.fromChapter = session->chapterTitle();
                jump.fromParent = syncChapterParent(links, jump.fromChapter, jump.here);
                if (compared) {
                    jump.toChapter = info.chapter;
                    jump.toParent = syncChapterParent(links, info.chapter, jump.synced);
                }
                const auto percent = [&jump](int page) {
                    return jump.pages > 1 ? 100.0 * page / (jump.pages - 1) : 0.0;
                };
                jump.fromPercent = jump.here >= 0 ? percent(jump.here) : session->progressFraction() * 100;
                // The synced page in this layout keeps the graph and the row in
                // agreement. The reported percentage is the fallback.
                jump.toPercent = jump.synced >= 0 ? percent(jump.synced) : remote.percentage * 100;
                jump.device = remote.device.trimmed().isEmpty()
                        ? strings::get("kosync.device_unknown")
                        : remote.device.simplified();
                jump.time = syncTimeText(remote.timestamp > 0 ? QDateTime::fromMSecsSinceEpoch(
                                                                        remote.timestamp * 1000)
                                                                        .toLocalTime()
                                                              : QDateTime(),
                                         QDateTime::currentDateTime());
                guard->descriptionLabel()->hide();
                // The view sits between the heading and the actions, with the
                // heading's side inset and room above the actions.
                auto *details = new QWidget(guard->card());
                details->setObjectName("rrKosyncChoiceDetails");
                auto *layout = new QVBoxLayout(details);
                layout->setContentsMargins(ui::metrics::chrome::inset, 0,
                                           ui::metrics::chrome::inset,
                                           ui::metrics::syncChoice::bottomGap);
                auto *view = new SyncChoiceView(details);
                view->setJump(jump);
                layout->addWidget(view);
                auto *rows = guard->card()->rows();
                rows->insertWidget(rows->count() - 1, details);
                // A layout shows a widget added to a visible card only on the
                // next event loop turn. fitCard() must measure it now.
                details->show();
                // Forward, the jump is the bold action. Backward, staying is
                // bold and focused, so the safer choice is the default.
                const auto title = direction > 0 ? strings::key("kosync.ahead_title")
                        : direction < 0          ? strings::key("kosync.behind_title")
                                                 : strings::key("kosync.resume_title");
                guard->titleLabel()->setText(strings::get(title));
                auto *go = guard->primaryButton(), *stay = guard->secondaryButton();
                go->setText(strings::get(direction > 0 ? strings::key("kosync.jump_ahead")
                                                 : direction < 0
                                                 ? strings::key("kosync.go_back")
                                                 : strings::key("kosync.use_remote")));
                ui::setModalActionPrimary(go, direction >= 0);
                ui::setModalActionPrimary(stay, direction < 0);
                go->setEnabled(true);
                if (direction < 0)
                    stay->setFocus(Qt::OtherFocusReason);
                guard->fitCard();
                refresh();
            });
}
} // namespace kobalt::reader
