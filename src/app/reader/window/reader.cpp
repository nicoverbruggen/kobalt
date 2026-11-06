#include "reading/remaining_time.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "reader_p.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "ui/input/input.h"
#include "ui/style/fonts.h"
#include "app/reader/views/sleep_screen.h"
#include "app/reader/views/opening.h"
#include "app/reader/views/kosync_notice.h"
#include "ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include "reading/appearance_settings.h"
#include "reading/page_navigation.h"
#include "reading/sleep_screen.h"
#include "render/page_refresh_regions.h"
#include "support/diagnostics.h"
#include "ui/style/metrics.h"
#include "reading/reading_density.h"
#include "ui/panel_timing.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include <QApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QFontInfo>
#include <QHideEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QKeyEvent>
#include <QScreen>
#include <QWindow>
#include <QShowEvent>
#include <QTime>
#include <QTouchEvent>
#include <utility>
#ifdef KOBALT_SCENARIO_PLAYER
#  include "scenario/sleep_persistence.h"
#endif

namespace kobalt::reader {
namespace strings = kobalt::strings;
using namespace kobalt::storage;
namespace metrics = kobalt::ui::metrics;
using kobalt::Choice;
using kobalt::platform::textRefresh;
using kobalt::ui::makeButton;

Reader::~Reader()
{
    mainthread::AllowBlocking allow("closing reader");
    if (kosyncClient)
        kosyncClient->persistSyncTime();
    if (closeWithoutSaving)
        book->discardPendingSaves();
    flushDiagnostics(1000);
    // Member destruction can send events before QObject removes our filters.
    qApp->removeEventFilter(this);
    for (auto *widget : findChildren<QWidget *>())
        widget->removeEventFilter(this);
    // Qt drops these connections only in ~QObject, after the members, the book
    // among them, are gone. Hiding the window in ~QWidget can change the
    // application state and would call back into a half-destroyed reader.
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(QGuiApplication::primaryScreen(), nullptr, this, nullptr);
    delete annotationExportPicker.data();
    cancelKosync();
    kosyncPage->dismissEditor();
    book->onEstimatesChanged({});
    session->cancelPagePreview();
    if (pageScrubber) {
        pageScrubber->cancel();
        pageScrubber->cancelled = {};
    }
    session->renderProgressChanged = {};
    renderNoticeTimer.stop();
    session->cancelPendingTurn();
    session->turnBoundary = {};
    page->cancelHold();
    brightnessHide.stop();
    lightFeedback.stop();
    annotationModeHide.stop();
    lightPopup->changed = {};
    delete lightPopup;
    light.reset();
    presentation->clearSleepScreen();
    presentation->resumeBackground();
    kobalt::ui::setInterfaceFontFamily({});
    kobalt::ui::setInterfaceMonoFontFamily({});
    kobalt::ui::setHeaderFooterFontFamily({});
    kobalt::ui::setHeaderFooterAddedWeight(2);
}

bool Reader::takesInput() const
{
    return isVisible() && !lifecycleHidden && !closing && !exitPending && !exportRunning
            && !services.suspended();
}

void Reader::checkpoint()
{
    // The host asks before sleep, USB or leaving the app, and needs the
    // position on storage when this returns. Android and desktop also ask
    // once a book opens, a known exception while the first page is shown.
    mainthread::AllowBlocking allow("host checkpoint");
    if (exitPending || exportRunning || closing) {
        book->stopReading(true);
        return;
    }
    // Hosts can checkpoint before our application-state callback. Freeze the
    // clock before any save can block, so background I/O is not reading time.
    if (services.suspended())
        book->sleepReading(preferences);
    save(true);
}

void Reader::observeReading()
{
    if (closing || exitPending || lifecycleHidden || !isVisible() || services.suspended()) {
        if (!closing && !exitPending && services.suspended())
            book->sleepReading(preferences);
        else
            book->stopReading(exitPending || closing);
        return;
    }
    const bool wasReading = book->readingActive();
    book->observeReading(!topControls->isVisible() && !general->isVisible() && !choices->isVisible()
                         && !(imageViewer && imageViewer->isVisible())
                         && !dictionaryPanel->isVisible() && !searchPanel->isVisible()
                         && !linksActive() && !appearanceEdit.panelOpen() && !syncDialog
                         && !kosyncDialog && !finishDialog);
    if (clockTimer && book->readingActive() != wasReading) {
        startClock();
        updateMenuStatus();
    }
}

void Reader::prepareShutdown(std::function<void()> finished, bool allowFailedSave)
{
    allowFailedTransition = allowFailedSave;
    if (closeWithoutSaving) {
        // Hosts also call this while retiring an accepted reader close. The
        // explicit quit waives failed saves, but already queued work must drain.
        // Defer completion so the host cannot delete us inside closeEvent().
        qt::later(10, this, [this, finished] {
            if (pendingStorageSave())
                prepareShutdown(finished, allowFailedTransition);
            else
                finished();
        });
        return;
    }
    prepareTransition(std::move(finished), false);
}

void Reader::prepareUsb(std::function<void()> finished)
{
    allowFailedTransition = false;
    prepareTransition(std::move(finished), true);
}

void Reader::prepareTransition(std::function<void()> finished, bool usb)
{
    if (imageViewer && imageViewer->isVisible())
        imageViewer->closeImage();
    book->retryFailedReadingSave();
    const auto request = ++closeRequest;
    setProperty("reader_transition_save_failed", false);
    if (closeSaveError) {
        closeSaveError->hide();
        closeSaveError->deleteLater();
        closeSaveError.clear();
    }
    if (pageScrubber)
        pageScrubber->cancel();
    sleepImage = {};
    presentation->clearSleepScreen();
    session->cancelPendingTurn();
    book->stopReading(true);
    lifecycleHidden = true;
    exitPending = exportRunning = false;
    closing = true;
    cancelKosync();
    kosyncPage->dismissEditor();
    clockTimer->stop();
    setEnabled(false);
    auto completed = std::make_shared<bool>(false);
    auto complete = [this, request, usb, finished](const QString &location, const QString &error,
                                                   bool mapped) {
        if (request != closeRequest)
            return;
        saveTransition(request, usb, location, error, mapped, finished);
    };
    // Bound the engine mapping wait, not required storage acknowledgements.
    // USB must not release a session that still owns unsaved reading time.
    // Each request has its own deadline, so a superseded request still reports
    // its timeout. The name lets tests check and run it instead of waiting.
    auto *deadline = new QTimer(this);
    deadline->setObjectName("rrTransitionMappingDeadline");
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this, [deadline, completed, complete, usb] {
        deadline->deleteLater();
        if (std::exchange(*completed, true))
            return;
        kobalt::diagnostic(usb ? "usb-save-timeout" : "shutdown-save-timeout");
        complete({}, {}, false);
    });
    deadline->start(2000);
    session->nativePosition([completed, complete, timer = QPointer<QTimer>(deadline)](
                                    const QString &location, const QString &error) {
        // Once the mapping has returned, the deadline has nothing left to do.
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
        if (!std::exchange(*completed, true))
            complete(location, error, true);
    });
}

void Reader::saveTransition(quint64 request, bool usb, const QString &location,
                            const QString &error, bool mapped, std::function<void()> finished,
                            int failures)
{
    if (request != closeRequest)
        return;
    const QPointer<Reader> guard(this);
    const auto again = [this, request, usb, location, error, mapped, finished](int failures) {
        kobalt::qt::later(
                100, this, [this, request, usb, location, error, mapped, finished, failures] {
                    saveTransition(request, usb, location, error, mapped, finished, failures);
                });
    };
    // Polling an active operation does not spend or renew its retry budget.
    if (pendingStorageSave()) {
        again(failures);
        return;
    }
    if (failures >= 3) {
        setProperty("reader_transition_save_failed", true);
        kobalt::diagnostic(usb ? "usb-save-failed" : "shutdown-save-failed",
                           { { "attempts", failures } }, kobalt::DiagnosticSeverity::Error);
        if (allowFailedTransition || closeWithoutSaving) {
            closeWithoutSaving = true;
            finished();
        } else
            transitionSaveError(
                    [this, again] {
                        book->retryFailedReadingSave();
                        again(0);
                    },
                    finished);
        return;
    }
    const auto retry = [again, failures] { again(failures + 1); };
    if (bookmarks->hasFailedSave() || highlights->hasFailedSave()) {
        bookmarks->retryFailedSave();
        highlights->retryFailedSave();
        retry();
        return;
    }
    book->checkpointForClose([this, guard, request, location, error, mapped, finished,
                              retry](bool saved) {
        if (!guard || request != closeRequest)
            return;
        if (!saved) {
            retry();
            return;
        }
        book->finishReading([this, guard, request, location, error, mapped, finished,
                             retry](bool saved) {
            if (!guard || request != closeRequest)
                return;
            if (!saved) {
                retry();
                return;
            }
            if (mapped) {
                mainthread::AllowBlocking allow("final native progress before storage handoff");
                book->writeNative(location, error);
                if (book->nativeSaveResult() == ReadingBook::NativeSave::Failed) {
                    retry();
                    return;
                }
            }
            book->checkpointForClose([this, guard, request, finished, retry](bool saved) {
                if (!guard || request != closeRequest)
                    return;
                if (!saved) {
                    retry();
                    return;
                }
                finished();
            });
        });
    });
}

void Reader::prepareSleepCover()
{
    const auto request = ++sleepCoverRequest;
    sleepCover = {};
    sleepCoverSource = {};
#ifdef KOBALT_SCENARIO_PLAYER
    setProperty("eval_sleep_cover_pending", false);
    setProperty("eval_sleep_cover_ready", false);
#endif
    auto mode = sleep::mode(preferences);
#ifdef KOBALT_SCENARIO_PLAYER
    if (scenario::sleepPersistenceOptions().mode >= 0)
        mode = sleep::Mode(scenario::sleepPersistenceOptions().mode);
#endif
    if (!presentation->supportsSleepScreen() || mode != sleep::Mode::Cover) {
        return;
    }
    // Decode off the GUI thread while the book is available. Keep at most
    // 16 MiB, only for the cover option, and reuse it in either orientation.
    QPointer<Reader> guard(this);
#ifdef KOBALT_SCENARIO_PLAYER
    setProperty("eval_sleep_cover_pending", true);
#endif
    session->coverImage(
            { 2048, 2048 },
            [guard, request](QImage image, QFileInfo source) {
                if (guard && guard->sleepCoverRequest == request) {
                    guard->sleepCover = std::move(image);
                    guard->sleepCoverSource = std::move(source);
#ifdef KOBALT_SCENARIO_PLAYER
                    guard->setProperty("eval_sleep_cover_ready", !guard->sleepCover.isNull());
                    guard->setProperty("eval_sleep_cover_pending", false);
#endif
                }
            },
            services.coverImagePath);
}

void Reader::prepareSleepOverlay()
{
    if (sleepOverlayLoading || !presentation->supportsSleepScreen()
        || services.sleepOverlayDirectories.isEmpty())
        return;
    sleepOverlayLoading = true;
    const QPointer<Reader> guard(this);
    session->sleepOverlay(services.sleepOverlayDirectories, [guard](QImage image) {
        if (!guard)
            return;
        guard->sleepOverlayLoading = false;
        const bool choosing = guard->sleepPopup
                && guard->sleepPopup->objectName() == QStringLiteral("rrSleepScreenPopup");
        if (!choosing && sleep::mode(guard->preferences) != sleep::Mode::Overlay)
            return;
        guard->sleepOverlay = std::move(image);
        if (choosing)
            guard->updateSleepOverlayOption();
    });
}

sleep::Snapshot Reader::powerScreenSnapshot()
{
    if (exportRunning)
        return exitSleepSnapshot;
    if (lifecycleHidden && !sleepImage.isNull())
        return { sleepImage,     sleepOrientation, sleepProgress,
                 sleepRemaining, darkTheme,        sleep::mode(preferences),
                 false,          sleepPercent,     activeSleepTile };
    return captureSleepScreen();
}

sleep::Snapshot Reader::captureSleepScreen()
{
    // RGB32 needs four bytes per pixel. Bound the optional capture to 16 MiB;
    // oversized viewports keep Nickel's power screen instead.
    const qint64 pixels = qint64(page->width()) * page->height();
    const auto *modal = QApplication::activeModalWidget();
    const bool foreground = !modal || modal == this || isAncestorOf(modal);
    auto mode = sleep::mode(preferences);
#ifdef KOBALT_SCENARIO_PLAYER
    if (scenario::sleepPersistenceOptions().mode >= 0)
        mode = sleep::Mode(scenario::sleepPersistenceOptions().mode);
#endif
    if (mode == sleep::Mode::Stock || !presentation->supportsSleepScreen() || !isVisible()
        || !foreground || pixels <= 0 || pixels > 4 * 1024 * 1024 || !readyForPresentation()
        || appearanceEdit.panelOpen() || session->temporaryTypography())
        return {};
    const bool useCover = mode == sleep::Mode::Cover && !sleepCover.isNull();
    auto frame = useCover ? sleepCoverBackground(sleepCover, page->size()) : page->snapshot(true);
    if (frame.isNull())
        return {};
    const bool useOverlay = mode == sleep::Mode::Overlay && !sleepOverlay.isNull();
    if (useOverlay) {
        dimSleepPage(frame, darkTheme);
        sleep::drawOverlay(frame, sleepOverlay);
    }
    // A book marked as finished is done, wherever its last page sits.
    const int percent =
            book->finishedHere() ? 100 : pageProgressPercent(session->page(), session->pages());
    sleep::Snapshot snapshot{ std::move(frame),
                              kobalt::shownOrientation(QGuiApplication::primaryScreen()),
                              strings::get("sleep.percent_read").arg(percent),
                              sleepTimeLeft(book->estimates().bookMilliseconds,
                                            remainingTime::approximate(preferences)),
                              darkTheme,
                              mode,
                              !useCover && !useOverlay,
                              percent,
                              sleep::tile(preferences) };
    if (useCover && sleepCoverSource.isFile()) {
        snapshot.coverPath = sleepCoverSource.absoluteFilePath();
        snapshot.coverBytes = sleepCoverSource.size();
        snapshot.coverModified = sleepCoverSource.lastModified().toMSecsSinceEpoch();
    }
    return snapshot;
}

void Reader::sleepReader()
{
    // The device is going to sleep; no tap can turn the page until it wakes.
    mainthread::AllowBlocking allow("sleep");
    if (imageViewer && imageViewer->isVisible())
        imageViewer->closeImage();
    book->retryFailedReadingSave();
    if (orientation) {
        orientation->freeze();
    }
    if (pageScrubber)
        pageScrubber->cancel();
    if (lifecycleHidden)
        return;
    const auto sleepRequest = ++closeRequest;
    exportRunning = false;
    setEnabled(true);
    setProperty("reader_sleep_clean_pending", true);
    session->cancelPendingTurn();
    book->sleepReading(preferences);
    auto snapshot = captureSleepScreen();
    sleepOrientation = snapshot.orientation;
    sleepProgress = snapshot.progress;
    sleepRemaining = snapshot.remaining;
    sleepPercent = snapshot.percent;
    activeSleepTile = snapshot.tile;
    sleepBattery = services.battery();
    const auto status = sleepBattery.fullyCharged ? SleepStatus::Charged
            : sleepBattery.charging               ? SleepStatus::Charging
                                                  : SleepStatus::Sleeping;
    sleepImage = sleepScreenImage(std::move(snapshot.image), sleepProgress, sleepRemaining,
                                  darkTheme, sleepBattery.percent, status, snapshot.dimPage,
                                  snapshot.tile, sleepPercent);
    services.cancelAnimation();
    page->animationDirection = 0;
    setProperty("reader_animation_direction", 0);
    appearanceEdit.clearContinuation();
    lifecycleHidden = true;
    clockTimer->stop();
    // The session worker writes to the same database as the bookmark. Let it
    // finish before attempting the nonblocking bookmark transaction. Keep the
    // sleep interval intact so a short sleep can resume the same local session.
    QPointer<Reader> guard(this);
    mainthread::blocking("native sleep save");
    QElapsedTimer saveDeadline;
    saveDeadline.start();
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
        auto completed = std::make_shared<bool>(false);
        book->drainReadingSave([completed, waiting = QPointer<QEventLoop>(&loop)](bool) {
            *completed = true;
            if (waiting)
                waiting->quit();
        });
        if (!*completed) {
            timeout.start(2500);
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
    }
    if (!guard || closeRequest != sleepRequest || !lifecycleHidden)
        return;
    saveNativeProgress();
    // This error means the transaction never began. Another Nickel writer can
    // outlast the session queue's retries. Retry only that failure, within the
    // same sleep budget; other failures and progress conflicts keep their policy.
    while (book->nativeSaveResult() == ReadingBook::NativeSave::Failed
           && book->nativeSaveError()
                   == strings::get("error.nickel_database_is_busy_local_position_was_retained")
           && saveDeadline.elapsed() < 2500) {
        QEventLoop retry;
        QTimer::singleShot(
                int(qBound(qint64(1), qint64(2500) - saveDeadline.elapsed(), qint64(100))), &retry,
                &QEventLoop::quit);
        connect(this, &QObject::destroyed, &retry, &QEventLoop::quit);
        retry.exec(QEventLoop::ExcludeUserInputEvents);
        if (!guard || closeRequest != sleepRequest || !lifecycleHidden)
            return;
        saveNativeProgress();
    }
    checkpoint();
    // A device can stay asleep until its battery runs out, so the last sync
    // time cannot wait for the book to close. The sleep sync below starts
    // after this point; finishKosync() queues its time.
    kosyncClient->persistSyncTime();
    // Fold waiting saves into the main profile file before a long sleep.
    bookmarks->flush();
    highlights->flush();
    kobalt::storage::checkpointProfile();
    flushDiagnostics(1000);
    closeSearch();
    dismissLookup();
    hideKosyncOpening();
    kosyncPage->dismissEditor();
    kosyncPage->setStatus(kosyncMessage, false);
    kosyncStarted = false;
    if (kosyncDialog) {
        kosyncDialog->hide();
        kosyncDialog->deleteLater();
        kosyncDialog = nullptr;
    }
    refreshRenderNotice();
    if (syncDialog)
        syncDialog->hide();
    if (finishDialog)
        finishDialog->hide();
    // The power saver turns Wi-Fi off once the sleep sync has ended.
    flushKosync([this] { saveWifiPowerAtSleep(); }, kosyncClient->schedule().onSleepWake);
    // Keep the page on screen until Nickel's sleep view replaces it, as in
    // Nickel's own reader. sleepViewReady() submits that view.
}

void Reader::prepareWake()
{
    if (lifecycleHidden && !isVisible())
        presentation->pauseBackground(true);
}

void Reader::updateSleepBattery()
{
    if (!lifecycleHidden || sleepImage.isNull()) {
        return;
    }
    const auto battery = services.battery();
    if (battery.charging == sleepBattery.charging
        && battery.fullyCharged == sleepBattery.fullyCharged
        && ((!battery.charging && !battery.fullyCharged)
            || battery.percent == sleepBattery.percent)) {
        return;
    }
    sleepBattery = battery;
    const auto status = battery.fullyCharged ? SleepStatus::Charged
            : battery.charging               ? SleepStatus::Charging
                                             : SleepStatus::Sleeping;
    // Paint over the card in the retained frame. Reusing the composed background
    // avoids another cover decode or another dimming pass while asleep.
    const auto tile = activeSleepTile;
    if (tile == sleep::Tile::None)
        return;
    sleepImage = sleepScreenImage(std::move(sleepImage), sleepProgress, sleepRemaining, darkTheme,
                                  battery.percent, status, false, tile, sleepPercent);
    if (!isVisible()) {
        presentation->updateSleepScreen(
                sleepImage,
                sleepScreenCardRect(sleepImage.size(), !sleepRemaining.isEmpty(), tile));
    }
}

void Reader::cancelWake()
{
    if (lifecycleHidden && !isVisible())
        presentation->resumeBackground();
}

void Reader::sleepViewReady()
{
    if (!lifecycleHidden || !isVisible())
        return;
    if (!sleepImage.isNull()) {
        presentation->presentSleepScreen(sleepImage, sleepOrientation);
    }
    // Hiding a top-level window immediately submits the backing store
    // underneath it. Paint Nickel's sleep view first, or that store still
    // contains the library. Do not pump unrelated events during handoff.
    // Submit that paint as one full GC16 refresh. It replaces the page, which
    // Nickel's own partial sleep refreshes on colour screens would leave
    // behind, so the panel flashes once on every device.
    setProperty("reader_preparing_handoff", true);
    presentation->resumeAndRepaintBackground();
    setProperty("reader_preparing_handoff", false);
    setProperty("reader_sleep_handoff", true);
    hide();
    setProperty("reader_sleep_handoff", false);
}

void Reader::wakeReader()
{
    cancelWifiPowerAtSleep();
    cancelKosync();
    kosyncStarted = false;
    kosyncWaking = true;
    kosyncBackgroundFailures = 0;
    sleepImage = {};
    presentation->clearSleepScreen();
    setProperty("reader_sleep_clean_pending", false);
    lifecycleHidden = false;
    if (sleep::mode(preferences) == sleep::Mode::Overlay)
        prepareSleepOverlay();
    QTimer::singleShot(0, this, [this] { synchronizeKosync(); });
    if (finishDialog)
        finishDialog->show();
    setProperty("reader_preparing_wake", true);
    if (orientation) {
        orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
    }
    showFullScreen();
    book->invalidateProgress();
    startClock();
    refresh();
    QTimer::singleShot(0, this, [this] {
        for (auto *widget : findChildren<QWidget *>())
            QCoreApplication::sendPostedEvents(widget, QEvent::UpdateLater);
        QCoreApplication::sendPostedEvents(this, QEvent::UpdateLater);
        QCoreApplication::sendPostedEvents(this, QEvent::UpdateRequest);
        setProperty("reader_preparing_wake", false);
        if (!lifecycleHidden && isVisible())
            repaint();
    });
}

void Reader::saveNativeProgress()
{
    // Nickel's Volume API runs here only at lifecycle boundaries.
    mainthread::AllowBlocking allow("native progress");
    if (!book->canWriteNative() || session->image().isNull() || !session->error().isEmpty()
        || appearanceEdit.panelOpen() || importingProgress || syncDialog) {
        return;
    }
    const auto anchor = session->state().value("position").toString();
    if (anchor.isEmpty()
        || (anchor == savedNativeAnchor && session->page() == savedNativePage
            && session->pages() == savedNativePages)) {
        return;
    }
    QString error;
    const auto location = session->savedNativePosition(&error);
    if (!book->writeNative(location, error)) {
        return;
    }
    QJsonObject state;
    book->checkpoint(&state, true);
    const auto result = state.value("native_writeback").toString();
    if (result == "verified" || result == "time-only") {
        savedNativeAnchor = anchor;
        savedNativePage = session->page();
        savedNativePages = session->pages();
    }
}

void Reader::progressTick()
{
    if (closing || exportRunning)
        return;
    if (services.suspended() || book->deviceTransition() || !isVisible()) {
        if (syncDialog)
            syncDialog->hide();
        return;
    }
    if (importingProgress)
        return;
    if (progressBlocked()) {
        if (syncDialog)
            syncDialog->hide();
        progressCheckDeferred = true;
        return;
    }
    progressCheckDeferred = false;
    book->pollProgress();
    showProgressChoice();
}

QVector<QWidget *> Reader::popups() const
{
    // Header and footer and the field list cover the panels, under every other pop-up.
    return { imageViewer,
             annotationPopover,
             headerFooterView,
             fieldChooser,
             busyNotice,
             brightnessNotice,
             annotationModeNotice,
             fontDefaultDialog.data(),
             settingsFontPopup.data(),
             kosyncPage ? kosyncPage->editorDialog.data() : nullptr,
             bookmarkList ? bookmarkList->pinDialog() : nullptr,
             kosyncDialog.data(),
             linkDialog.data(),
             debugDialog.data(),
             syncDialog.data(),
             finishDialog.data(),
             refreshPopup.data(),
             sleepPopup.data(),
             pageNavigationPopup.data(),
             tapZonesPopup.data(),
             navigationBarPopup.data(),
             menuViewPopup.data(),
             tappableLinksPopup.data(),
             marksActions.data(),
             removeDialog.data(),
             annotationExportPicker.data(),
             customReplaceDialog.data(),
             annotationErrorDialog.data(),
             formattingWarningDialog.data(),
             settingsWriteErrorDialog.data() };
}

bool Reader::popup(QObject *object) const
{
    for (auto *widget : popups())
        if (widget && widget == object)
            return true;
    return false;
}

void Reader::raisePopups()
{
    if (raisingPopups)
        return;
    raisingPopups = true;
    for (auto *widget : popups())
        if (widget && widget->isVisible())
            widget->raise();
    raisingPopups = false;
}

bool Reader::event(QEvent *event)
{
    // Qt can already be delivering a batch of posted events when input
    // queues a command. Apply it before this window starts a queued paint.
    if (event->type() == lightCommandEventType() || event->type() == QEvent::UpdateRequest) {
        dispatchLightGesture();
        if (event->type() == lightCommandEventType())
            return true;
    }
    if (event->type() == QEvent::UpdateRequest && isVisible()) {
        setProperty("reader_navigation_dismissal", false);
        const bool panelVisible = navigation && navigation->isVisible() && topControls->isVisible()
                && navigationActive >= 0;
        // Compare completed frames, not the action that requested a close.
        // Appearance edits keep their panel until the asynchronous apply ends.
        const bool panelClosed = navigationPanelPainted && !panelVisible;
        navigationPanelPainted = panelVisible;
        if (panelClosed) {
            const auto overlays = popups();
            const bool overlayVisible =
                    std::any_of(overlays.cbegin(), overlays.cend(), [](const QWidget *widget) {
                        return widget && widget->isVisible();
                    });
            const bool dismissed = navigation && navigation->isVisible() && topControls->isVisible()
                    && !appearanceEdit.panelOpen() && !general->isVisible() && !choices->isVisible()
                    && !chapterGraph->isVisible() && !lightPopup->isVisible()
                    && !bookmarkList->isVisible() && !seekOpen && !session->busy()
                    && !dictionaryPanel->isVisible() && !searchPanel->isVisible() && !linksActive()
                    && !lookupPending && !acknowledgments->isVisible() && !closeSaveError
                    && !services.suspended() && !overlayVisible;
            setProperty("reader_navigation_dismissal", dismissed);
            setProperty("reader_navigation_dismissal_plain",
                        page->image.text("kobalt.animation_text_only") == "true");
        }
        // Only the plain reading bars can cross an unchanged text page
        // with a partial refresh. Other panels may contain color.
        setProperty("reader_plain_controls",
                    !appearanceEdit.panelOpen() && !general->isVisible() && !choices->isVisible()
                            && !chapterGraph->isVisible() && !linkReturn->isVisible()
                            && !dictionaryPanel->isVisible() && !searchPanel->isVisible()
                            && !linksActive() && !lookupPending && !acknowledgments->isVisible()
                            && !bookmarkList->isVisible() && !lightPopup->isVisible()
                            && !busyNotice->isVisible() && !syncDialog && !kosyncDialog
                            && !finishDialog && !annotationErrorDialog && !formattingWarningDialog
                            && !settingsWriteErrorDialog && !closeSaveError && !services.suspended()
                            && page->image.text("kobalt.animation_text_only") == "true");
        // These panels hold text and line controls. Changes inside them can
        // use a partial refresh; showing the whole panel keeps its policy.
        QWidget *partialPanel = nullptr;
        if (lightPopup->isVisible()) {
            partialPanel = lightPopup;
        } else if (appearanceEdit.panelOpen() && menu->isVisible() && !busyNotice->isVisible()
                   && !choices->isVisible()) {
            partialPanel = menu;
        }
        setProperty("reader_partial_controls",
                    partialPanel ? QRect(partialPanel->mapTo(this, QPoint()), partialPanel->size())
                                 : QRect());
    }
    return QWidget::event(event);
}

bool Reader::eventFilter(QObject *watched, QEvent *event)
{
    const auto type = event->type();
    // This filter sees every event in the application. Leave at once for the
    // frequent types it never handles. A new branch for one of these types
    // must remove it from this list.
    switch (type) {
    case QEvent::Timer:
    case QEvent::MetaCall:
    case QEvent::SockAct:
    case QEvent::UpdateRequest:
    case QEvent::UpdateLater:
    case QEvent::DeferredDelete:
    case QEvent::LayoutRequest:
    case QEvent::ChildAdded:
    case QEvent::ChildPolished:
    case QEvent::ChildRemoved:
        return false;
    default:
        break;
    }
    if (type == QEvent::TouchBegin || type == QEvent::TouchUpdate
        || type == QEvent::MouseButtonPress || type == QEvent::KeyPress) {
        kobalt::diagnosticInput();
    }
    // Check the cheap event type before the diagnostics state; this filter
    // sees every event in the application.
    if ((type == QEvent::TouchBegin || type == QEvent::TouchEnd || type == QEvent::TouchCancel
         || type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
         || type == QEvent::KeyPress || type == QEvent::KeyRelease)
        && kobalt::diagnosticsActive()) {
        const bool input = type != QEvent::TouchCancel;
        kobalt::diagnostic("input-event",
                           { { "type", int(type) },
                             { "source_ms",
                               input ? double(static_cast<QInputEvent *>(event)->timestamp()) : 0 },
                             { "spontaneous", event->spontaneous() } },
                           false);
    }
    // Widgets that forward a press, such as a canvas, send a copy without the
    // input's time. Keep the time of the press the platform delivered.
    if ((type == QEvent::TouchBegin || type == QEvent::MouseButtonPress || type == QEvent::KeyPress)
        && event->spontaneous())
        lastPressTime = quint32(static_cast<QInputEvent *>(event)->timestamp());
    if (watched == bookmarkList && type == QEvent::Hide)
        closeMarksActions();
    if (type == QEvent::Show
        && (watched == topControls || watched == general || watched == choices
            || watched == dictionaryPanel || watched == searchPanel || watched == linkPanel
            || watched == linkDialog || watched == syncDialog || watched == kosyncDialog
            || watched == finishDialog)) {
        book->pauseLocalPace();
    }
    if (watched == menu && type == QEvent::Hide && fieldChooser) {
        fieldChooser->hide();
        headerFooterView->hide();
    }
    if ((watched == brightnessNotice || watched == annotationModeNotice) && type == QEvent::Hide
        && isVisible() && !lifecycleHidden && !closing && !exportRunning && !exitPending
        && !services.suspended()) {
        requestFlash(watched == brightnessNotice ? "light-swipe" : "annotation-mode");
        page->update();
    }
    // Kobo screens have no hover. Qt still sends hover events when a touch
    // moves between widgets, and repaints any widget that tracks hover, which
    // refreshes unchanged pixels. No reader widget draws a hover state.
    if (ui::isHoverEvent(event)) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target && (target == this || isAncestorOf(target)))
            return true;
    }
    if ((type == QEvent::KeyPress || type == QEvent::KeyRelease) && isVisible() && !lifecycleHidden
        && !services.suspended() && QApplication::activeModalWidget() == this) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int direction = services.pageButtonDirection(key);
        if (direction) {
            // Consume both events before a sheet's input barrier handles them.
            if (type == QEvent::KeyPress) {
                if (!closing && !book->deviceTransition() && services.remoteKeyActivity)
                    services.remoteKeyActivity(this, key);
                if (!key->isAutoRepeat())
                    pageButton(direction, quint32(key->timestamp()));
            }
            event->accept();
            return true;
        }
    }
    // While a panel's changes apply, its controls ignore input. Disabling
    // them instead would repaint the whole visible panel in disabled colors,
    // one more screen update before it closes.
    const bool controlInput = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
            || type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove
            || type == QEvent::TouchBegin || type == QEvent::TouchUpdate || type == QEvent::TouchEnd
            || type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::Shortcut;
    if (controlInput && imageViewer && imageViewer->isVisible()) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target && (target == this || isAncestorOf(target)) && target != imageViewer
            && !imageViewer->isAncestorOf(target)) {
            event->accept();
            return true;
        }
    }
    if (controlInput && isVisible() && chromeView()) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target && (target == this || isAncestorOf(target))) {
            bool inPopup = false;
            for (auto *view : popups())
                if (view && view->isVisible() && (view == target || view->isAncestorOf(target))) {
                    inPopup = true;
                    break;
                }
            // Only the sheet and any confirmation above it accept reader
            // input. Host controls such as Screenshot remain independent.
            if (!inPopup) {
                event->accept();
                return true;
            }
        }
    }
    if (appearanceEdit.applying() && controlInput) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target
            && (target == menu || target == general || menu->isAncestorOf(target)
                || general->isAncestorOf(target))) {
            event->accept();
            return true;
        }
    }
    // Reflow temporarily blocks rotation without repainting its buttons in
    // disabled colors. Keep the selected orientation visible until it changes.
    auto *rotationButton = controlInput ? qobject_cast<QPushButton *>(watched) : nullptr;
    if (controlInput && rotationButton
        && (rotationButtons.contains(rotationButton) || rotationButton == autoRotateOption)
        && rotationControlsBlocked()) {
        event->accept();
        return true;
    }
    if (watched == menu && type == QEvent::Paint) {
        schedulePreviewWarm();
    }
    // Panels declare their placement through geometry. The reading status strip
    // owns its separator, including when an existing panel moves or closes.
    if (watched->parent() == this && dynamic_cast<ui::Panel *>(watched)
        && (type == QEvent::Show || type == QEvent::Hide || type == QEvent::Move
            || type == QEvent::Resize)
        && !statusBoundaryPending) {
        statusBoundaryPending = true;
        QTimer::singleShot(0, this, [this] {
            statusBoundaryPending = false;
            syncStatusBoundary();
        });
    }
    // The overlay itself only receives taps outside the refresh card.
    if (refreshPopup && watched == refreshPopup && type == QEvent::MouseButtonRelease) {
        closeScreenRefresh();
        return true;
    }
    if (sleepPopup && watched == sleepPopup && type == QEvent::MouseButtonRelease) {
        closeSleepScreen();
        return true;
    }
    if (tapZonesPopup && watched == tapZonesPopup && type == QEvent::MouseButtonRelease) {
        closeTapZones();
        return true;
    }
    if (navigationBarPopup && watched == navigationBarPopup && type == QEvent::MouseButtonRelease) {
        closeNavigationBarButtons();
        return true;
    }
    if (tappableLinksPopup && watched == tappableLinksPopup && type == QEvent::MouseButtonRelease) {
        closeTappableLinks();
        return true;
    }
    if (menuViewPopup && watched == menuViewPopup && type == QEvent::MouseButtonRelease) {
        closeMenuView();
        return true;
    }
    if (pageNavigationPopup && watched == pageNavigationPopup
        && type == QEvent::MouseButtonRelease) {
        closePageNavigation();
        return true;
    }
    // Panels are raised as they open, and a panel created later starts
    // above older widgets. Either can cover a centred pop-up, so put the
    // visible pop-ups back on top whenever another child moves up.
    if ((type == QEvent::ZOrderChange || type == QEvent::Show) && watched->parent() == this
        && !popup(watched))
        raisePopups();
    const bool touch = type == QEvent::TouchBegin || type == QEvent::TouchUpdate
            || type == QEvent::TouchEnd || type == QEvent::TouchCancel;
    const bool mouse = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick
            || type == QEvent::MouseMove || type == QEvent::MouseButtonRelease;
    if (!touch && !mouse)
        return QWidget::eventFilter(watched, event);
    bool &dismissing = touch ? dismissingFontTouch : dismissingFontMouse;
    const bool begin = type == QEvent::TouchBegin || type == QEvent::MouseButtonPress
            || type == QEvent::MouseButtonDblClick;
    if (begin) {
        dismissing = false;
        // Only a press on a widget of this window decides a dismissal. A reader
        // shown in a canvas, as on the Mac and Android, also sees the canvas's
        // own press. Its screen position does not match the embedded widgets'
        // mapToGlobal(), so it would close a pop-up that was pressed. The
        // position is mapped through the pressed widget for the same reason.
        auto *target = qobject_cast<QWidget *>(watched);
        const bool inWindow = target && (target == this || isAncestorOf(target));
        const auto pressPoint = [&](QPointF &point) {
            if (touch) {
                const auto points = kobalt::ui::touchPositions(static_cast<QTouchEvent *>(event));
                if (points.isEmpty())
                    return false;
                point = target->mapToGlobal(points.first().toPoint());
            } else
                point = target->mapToGlobal(
                        kobalt::ui::mousePosition(static_cast<QMouseEvent *>(event)).toPoint());
            return true;
        };
        if (inWindow && isVisible() && choosingTypography && choices->isVisible()
            && !fontDefaultDialog) {
            QPointF point;
            if (!pressPoint(point))
                return QWidget::eventFilter(watched, event);
            const auto openingMenu = [&](const char *name) {
                const auto *button = findChild<QPushButton *>(name);
                return button->isVisible() && button->isEnabled()
                        && QRect(button->mapToGlobal(QPoint()), button->size())
                                   .contains(point.toPoint());
            };
            if (!openingMenu("rrLight") && !openingMenu("rrSettings")
                && !QRect(choices->mapToGlobal(QPoint()), choices->size())
                            .contains(point.toPoint())) {
                dismissing = true;
                choices->hide();
                refresh();
            }
        }
    }
    if (!dismissing)
        return QWidget::eventFilter(watched, event);
    // Consume the whole gesture so an outside tap cannot also turn a page
    // or apply the draft through the button underneath the dropdown.
    if (type == QEvent::TouchEnd || type == QEvent::TouchCancel
        || type == QEvent::MouseButtonRelease)
        dismissing = false;
    event->accept();
    return true;
}

bool Reader::readyForPresentation() const
{
    const QSize viewport = bodySize();
    return isVisible() && updatesEnabled() && !resizePending && !session->busy()
            && !page->image.isNull() && page->image.size() == viewport
            && (session->resourceStats().value("pending_images").toInt() == 0
                || !session->error().isEmpty());
}

void Reader::setOpeningTiming(const QElapsedTimer &started, const QJsonObject &stages)
{
    openingStarted = started;
    // Showing starts when the host hands the window over, so the host's own
    // preparation of the screen counts as part of it.
    showing.start();
    for (auto it = stages.begin(); it != stages.end(); ++it)
        openingStages[it.key()] = it.value();
}

void Reader::reportShown()
{
    if (!kobalt::diagnosticsActive())
        return;
    auto fields = openingStages;
    fields["show_ms"] = showing.nsecsElapsed() / 1e6;
    if (openingStarted.isValid())
        fields["total_ms"] = openingStarted.nsecsElapsed() / 1e6;
    kobalt::diagnostic("reader-shown", fields, false);
}

void Reader::locateText(const QString &text, std::function<void(QPoint)> found)
{
    auto settled = std::make_shared<bool>(false);
    QPointer<Reader> guard(this);
    session->search(text, [guard, text, found, settled](kobalt::SearchBatch batch) {
        if (!guard || *settled)
            return;
        // A hit inside a longer word, such as "set" in "reset", does not count.
        const auto joins = [](const QString &side, bool last) {
            return !side.isEmpty() && side.at(last ? side.size() - 1 : 0).isLetterOrNumber();
        };
        for (const auto &hit : batch.hits) {
            if (hit.text.compare(text, Qt::CaseInsensitive) != 0 || joins(hit.before, true)
                || joins(hit.after, false))
                continue;
            *settled = true;
            guard->session->cancelSearch();
            if (hit.page != guard->session->page())
                guard->session->goTo(hit.start);
            guard->locateOnPage(hit, found, 0);
            return;
        }
        if (batch.done) {
            *settled = true;
            found({ -1, -1 });
        }
    });
}

// The turn to the hit's page renders first; its rectangles exist only then.
void Reader::locateOnPage(const kobalt::SearchHit &hit, std::function<void(QPoint)> found,
                          int attempt)
{
    if (session->busy() || session->page() != hit.page) {
        if (attempt >= 500) {
            found({ -1, -1 });
            return;
        }
        QPointer<Reader> guard(this);
        kobalt::qt::later(20, this, [guard, hit, found, attempt] {
            if (guard)
                guard->locateOnPage(hit, found, attempt + 1);
        });
        return;
    }
    QPointer<Reader> guard(this);
    session->rangeRects(hit.start, hit.end, [guard, found](QVector<QRect> rectangles) {
        if (!guard || rectangles.isEmpty())
            found({ -1, -1 });
        else
            found(rectangles.first().center() + QPoint(0, guard->page->topHeight()));
    });
}

void Reader::locateLink(const QString &label, std::function<void(QPoint)> found)
{
    findLink(session->pageLinks(), 0, label, std::move(found));
}

void Reader::findLink(QVector<kobalt::PageLink> links, int index, QString label,
                      std::function<void(QPoint)> found)
{
    if (index >= links.size()) {
        found({ -1, -1 });
        return;
    }
    const auto link = links.at(index);
    QPointer<Reader> guard(this);
    session->linkDetails(
            link.source, [guard, links, index, label, found, link](kobalt::LinkDetails details) {
                if (!guard)
                    return;
                if (details.label == label && !link.rectangles.isEmpty())
                    found(link.rectangles.first().center() + QPoint(0, guard->page->topHeight()));
                else
                    guard->findLink(links, index + 1, label, found);
            });
}

void Reader::showEvent(QShowEvent *event)
{
    kobalt::diagnostic("reader-visible");
    // The first show ends the opening once the page has painted.
    if (!shownOnce) {
        shownOnce = true;
        if (!showing.isValid())
            showing.start();
        kobalt::ui::afterNextPaint(page, [this] {
            pagePainted = true;
            reportShown();
            if (!recordedOpening)
                recordOpening();
            showStartupNotice();
        });
    }
    // A modal window still lets Nickel repaint its library behind us.
    // Those background flushes can request GC16 over our reading page.
    presentation->pauseBackground();
    if (orientation)
        orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
    QWidget::showEvent(event);
    resizeText();
    observeReading();
    if (clockTimer)
        startClock();
    // The first refresh runs before the window is visible. Record the
    // opening once it is and its page has painted; a busy session retries from
    // its next refresh.
    if (!recordedOpening)
        QTimer::singleShot(0, this, [this] {
            if (!recordedOpening && pagePainted)
                recordOpening();
        });
}

void Reader::hideEvent(QHideEvent *event)
{
    if (imageViewer && imageViewer->isVisible())
        imageViewer->closeImage();
    if (clockTimer)
        clockTimer->stop();
    if (pageScrubber)
        pageScrubber->cancel();
    renderNoticeTimer.stop();
    busyNotice->setDelayedProgress(false, 0, 0);
    session->cancelPendingTurn();
    book->stopReading(closing || exitPending || lifecycleHidden);
    appearanceEdit.clearContinuation();
    closeSearch();
    services.cancelAnimation();
    page->animationDirection = 0;
    setProperty("reader_animation_direction", 0);
    dismissLookup();
    kobalt::diagnostic("reader-hidden");
    if (light)
        lightPopup->hide();
    if (orientation) {
        orientation->stop(property("reader_sleep_handoff").toBool());
    }
    presentation->resumeBackground();
    resizeTimer.stop();
    page->setViewportTransition(false);
    resizePending = false;
    setUpdatesEnabled(true);
    QWidget::hideEvent(event);
}

kobalt::SafeArea Reader::safeArea() const
{
    return services.safeArea ? services.safeArea(size()) : kobalt::SafeArea{};
}

QSize Reader::bodySize() const
{
    const auto safe = safeArea();
    return { width(),
             qMax(1,
                  height() - safe.top - safe.bottom - page->topHeight() - page->bottomHeight()) };
}

void Reader::paintEvent(QPaintEvent *event)
{
    ReaderWindow::paintEvent(event);
    if (!page || page->y() <= 0)
        return;
    QPainter painter(this);
    painter.fillRect(QRect(0, 0, width(), page->y()), palette().color(QPalette::Window));
    if (!page->bookmarked || lifecycleHidden || (topControls && topControls->isVisible())
        || (general && general->isVisible()) || (imageViewer && imageViewer->isVisible()))
        return;
    // The page stays below the camera band. Only the bookmark continues
    // through it, so its notch and the reading layout keep their positions.
    const auto ribbon = page->ribbonRect().translated(page->pos());
    painter.fillRect(QRect(ribbon.x(), 0, ribbon.width(), page->y()),
                     palette().color(QPalette::WindowText));
}

void Reader::resizeEvent(QResizeEvent *)
{
    // Paper fills the cutout bands; the page and the menu bars stay inside them.
    const auto safe = safeArea();
    layout()->setContentsMargins(0, safe.top, 0, safe.bottom);
    layout()->activate();
    page->setCornerRadii(safe.top ? 0 : safe.cornerRadius, safe.bottom ? 0 : safe.cornerRadius);
    // Panels that place themselves, such as the dictionary, read the band.
    setProperty("reader_safe_top", safe.top);
    if (navigation) {
        navigation->setSafeArea(safe.top, safe.bottom);
        navigation->setInlineStatus(isShortHeight(height(), ui::density()));
    }

    if (imageViewer)
        imageViewer->setGeometry(rect().adjusted(0, safe.top, 0, -safe.bottom));
    annotationFrame.clear();
    if (annotationPopover)
        annotationPopover->hide();
    if (isVisible() && windowHandle() && windowHandle()->isExposed() && !page->image.isNull()
        && updatesEnabled()) {
        // Kobo Qt replaces the backing store when the screen turns. Keep
        // painting the current page until reflow finishes, or the new buffer
        // reaches the panel as black while the engine is busy.
        resizePending = true;
        const auto viewport = bodySize();
        kobalt::diagnostic(
                "reader-resize-begin",
                { { "width", width() },
                  { "height", height() },
                  { "viewport_width", viewport.width() },
                  { "viewport_height", viewport.height() },
                  { "main_busy", session->busy() },
                  { "preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy() },
                  { "updates_enabled", updatesEnabled() },
                  { "resize_pending", resizePending } });
    }
    lightPopup->hide();
    dismissLookup();
    if (marksActions) {
        marksActions->setGeometry(rect());
        marksActions->fitCard();
    }
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
    topControls->setGeometry(0, 0, width(),
                             navigation ? navigation->toolbarHeight()
                                        : metrics::panel::toolbarHeight);
    bottomControls->setGeometry(metrics::panel::footer(size()));
    fitAppearance();
    if (navigation)
        chapterGraph->setAvailableHeight(navigation->contentBottom(size())
                                         - navigation->toolbarHeight());
    chapterGraph->resize(width(), chapterGraph->height());
    choices->setGeometry(rect());
    fitSettings();
    // Full-screen views start below the camera band, as the menu does.
    const auto safeRect = rect().adjusted(0, safe.top, 0, -safe.bottom);
    acknowledgments->setGeometry(safeRect);
    searchPanel->setGeometry(safeRect);
    const int bookmarkTop = navigation ? navigation->statusHeight() : 0;
    bookmarkList->setGeometry(0, bookmarkTop, width(),
                              (navigation ? navigation->contentBottom(size()) : height())
                                      - bookmarkTop);
    busyNotice->setGeometry(rect());
    busyNotice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    busyNotice->centerCard();
    brightnessNotice->centerIn(size());
    annotationModeNotice->centerIn(size());
    if (syncDialog)
        syncDialog->setGeometry(rect());
    if (kosyncDialog)
        kosyncDialog->setGeometry(rect());
    if (finishDialog)
        finishDialog->setGeometry(rect());
    if (closeSaveError)
        closeSaveError->setGeometry(rect());
    if (isVisible() && !page->image.isNull() && bodySize() != page->image.size()) {
        resizePending = true;
        const auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        const int columns = preferences.value("two_columns_landscape").toBool(false)
                        && bodySize().width() > bodySize().height()
                ? 2
                : 1;
        const int gap = qRound(
                settings->fontSize()
                * preferences.value("column_spacing_percent").toInt(defaultColumnSpacingPercent)
                / 100.0);
        page->reflowLayout = { settings->fontSize(), settings->spacing(),
                               QMargins(settings->margin(), settings->topMargin(),
                                        settings->margin(), settings->bottomMargin()),
                               columns, gap };
        page->setViewportTransition(true);
        resizeTimer.start();
        page->refreshContent();
    } else {
        resizeTimer.stop();
        resizeText();
    }
    syncNavigation();
}

void Reader::closeEvent(QCloseEvent *event)
{
    if (imageViewer && imageViewer->isVisible())
        imageViewer->closeImage();
    session->cancelPendingTurn();
    appearanceEdit.clearContinuation();
    closeSearch();
    dismissLookup();
    if (light)
        lightPopup->hide();
    if (!closing) {
        event->ignore();
        if (exitPending || closeSaveError)
            return;
        kobalt::diagnostic("exit-requested");
        if (!closingTimes.started.isValid())
            closingTimes.started.start();
        event->ignore();
        exitPending = true;
        QElapsedTimer stopping;
        stopping.start();
        book->stopReading(true);
        closingTimes.add("session", stopping);
        diagnostic("exit-session-stopped", { { "elapsed_ms", double(stopping.elapsed()) } });
        if (appearanceEdit.panelOpen() || general->isVisible())
            dismissControls();
        finishExit();
        return;
    }
#ifdef KOBALT_SCENARIO_PLAYER
    if (!closeWithoutSaving && scenario::sleepPersistenceOptions().mode >= 0) {
        QElapsedTimer sleepDispatch;
        sleepDispatch.start();
        presentation->saveSleepScreen(exitSleepSnapshot);
        closingTimes.add("sleep_dispatch", sleepDispatch);
    }
#endif
    exitSleepSnapshot = {};
    setProperty("reader_exit_clean_pending", true);
    setProperty("reader_preparing_handoff", true);
    // Restore orientation while Home is paused, then repaint its backing store
    // before making it visible.
    if (orientation) {
        orientation->stop();
    }
    presentation->resumeAndRepaintBackground();
    setProperty("reader_preparing_handoff", false);
    QElapsedTimer step;
    step.start();
    book->releaseNative(!closeWithoutSaving);
    closingTimes.add("session", step);
    session->updated = {};
    session->renderProgressChanged = {};
    session->turnBoundary = {};
    renderNoticeTimer.stop();
    busyNotice->setDelayedProgress(false, 0, 0);
    if (finished)
        finished(session->retained());
    QWidget::closeEvent(event);
}

bool Reader::rotationControlsBlocked() const
{
    return session->busy() || (appearanceEdit.edited() && appearanceEdit.preview()->busy());
}

void Reader::updateOrientationBlock()
{
    if (orientation)
        orientation->setBlocked(lifecycleHidden || services.suspended() || topControls->isVisible()
                                || session->busy() || (imageViewer && imageViewer->isVisible())
                                || appearanceEdit.panelOpen() || bool(syncDialog)
                                || bool(kosyncDialog) || bool(finishDialog)
                                || bool(annotationErrorDialog) || bool(formattingWarningDialog)
                                || bool(settingsWriteErrorDialog) || dictionaryPanel->isVisible()
                                || searchPanel->isVisible() || linksActive() || lookupPending);
}

void Reader::closeSearch()
{
    ++searchEpoch;
    pendingSearchHit = {};
    session->cancelSearch();
    if (searchPanel) {
        searchPanel->stop();
        searchPanel->hide();
    }
    updateOrientationBlock();
    syncNavigation();
}

void Reader::openSeek()
{
    if (session->busy() || appearanceEdit.applying()) {
        return;
    }
    if (appearanceEdit.panelOpen() || general->isVisible()) {
        appearanceEdit.request(AppearanceDestination::Seek);
        if (general->isVisible()) {
            applySettings();
        } else {
            finishPreview();
        }
        return;
    }
    if (seekOpen) {
        if (pageNavigation::menuView(preferences) == pageNavigation::MenuView::Seek)
            return;
        seekOpen = false;
        pageScrubber->cancel();
        showControls();
        return;
    }
    choices->hide();
    lightPopup->hide();
    chapterGraph->hide();
    seekOpen = true;
    updateLinkReturn();
    pageScrubber->showCurrentPage();
}

void Reader::openSearch(const QString &query)
{
    session->cancelPendingTurn();
    if (!isVisible() || lifecycleHidden || closing || services.suspended() || session->busy()
        || appearanceEdit.panelOpen() || appearanceEdit.applying())
        return;
    if (query.isEmpty())
        dismissLookup();
    else
        clearAnnotationDraft();
    dismissControls();
    if (searchOriginal.isEmpty()) {
        searchOriginal = session->state().value("position").toString();
        searchOriginalPage = session->page();
    }
    searchPanel->setExcerptFont(bookFace());
    searchPanel->setChapters(session->chapters());
    const auto safe = safeArea();
    searchPanel->setGeometry(rect().adjusted(0, safe.top, 0, -safe.bottom));
    searchPanel->open(session->state().value("position").toString() != searchOriginal,
                      searchOriginalPage, query);
    if (orientation)
        orientation->setBlocked(true);
    for (auto *widget : searchPanel->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(searchPanel);
}

bool Reader::linksActive() const
{
    return linkPending || bool(linkDialog) || (linkPanel && linkPanel->isVisible());
}

void Reader::dismissLinkConfirmation()
{
    if (linkDialog) {
        linkDialog->hide();
        linkDialog->deleteLater();
        linkDialog.clear();
    }
}

void Reader::dismissLinks()
{
    ++linkEpoch;
    linkPending = false;
    if (linkPanel) {
        const bool wasVisible = linkPanel->isVisible();
        linkPanel->hide();
        if (wasVisible) {
            requestFlash("footnote");
            page->update();
        }
    }
    dismissLinkConfirmation();
}

void Reader::updateLinkReturn()
{
    if (!linkReturn)
        return;
    syncLinkDiscovery();
    const bool available = !linkHistory.isEmpty() && !session->busy() && !lifecycleHidden
            && !closing && !linksActive() && !lookupPending && !general->isVisible()
            && !acknowledgments->isVisible() && !bookmarkList->isVisible()
            && !lightPopup->isVisible() && !syncDialog && !kosyncDialog && !finishDialog
            && (bottomControls->isVisible()
                || (topControls->isVisible() && !menu->isVisible() && !chapterGraph->isVisible()))
            && !choices->isVisible() && !searchPanel->isVisible() && !dictionaryPanel->isVisible();
    linkReturnAvailable = available;
    linkReturn->setVisible(available);
    const int footerHeight = metrics::panel::footerHeight
            + (available ? metrics::links::returnHeight + metrics::panel::footerGap : 0);
    bottomControls->setGeometry(0, height() - footerHeight, width(), footerHeight);
    syncNavigation();
}

bool Reader::openLink(QPoint point, bool direct)
{
    if (session->busy() || closing || lifecycleHidden || services.suspended() || syncDialog
        || kosyncDialog || finishDialog)
        return false;
    if (point.x() < 0 || point.x() >= page->width() || point.y() < 0
        || point.y() >= page->height() - page->topHeight() - page->bottomHeight())
        return false;
    const auto links = session->pageLinks();
    const auto hit = linkAtPoint(links, point,
                                 direct ? preferences.value("tappable_links").toString("footnotes")
                                        : QString("all"),
                                 int(metrics::links::revealPadding), direct ? 0 : ui::px(6));
    if (hit.index >= 0) {
        const auto &link = links[hit.index];
        const auto bounds = hit.bounds;
        dismissLookup();
        linkPending = true;
        // A link chosen in reveal mode opens like a held link, over the page
        // alone. Restoring the toolbars would cover the link's sheet.
        hideRevealedLinks(false);
        syncNavigation();
        const auto epoch = ++linkEpoch;
        if (orientation)
            orientation->setBlocked(true);
        page->mark(link.rectangles);
        QPointer<Reader> guard(this);
        kobalt::ui::PanelTiming timing("footnote");
        session->linkDetails(
                link.source,
                [guard, epoch, bounds, timing](const kobalt::LinkDetails &details) mutable {
                    if (!guard || guard->linkEpoch != epoch || guard->lifecycleHidden
                        || guard->services.suspended())
                        return;
                    timing.stage("details");
                    auto *reader = guard.data();
                    reader->linkPending = false;
                    if (details.kind == kobalt::LinkKind::None) {
                        reader->dismissLookup();
                        reader->updateLinkReturn();
                        return;
                    }
                    if (details.kind == kobalt::LinkKind::Passage) {
                        reader->goToPassage(details.target);
                    } else if (details.kind == kobalt::LinkKind::Website) {
                        auto *dialog = new kobalt::WebsiteDialog(reader);
                        reader->linkDialog = dialog;
                        dialog->browserAvailable = [reader] {
                            return reader->services.browserAvailable();
                        };
                        dialog->openBrowser = [reader](const QUrl &url) {
                            return !reader->closing && !reader->lifecycleHidden
                                    && !reader->services.suspended()
                                    && reader->services.openBrowser(url, reader);
                        };
                        dialog->dismissed = [reader] {
                            reader->dismissLookup();
                            reader->updateLinkReturn();
                        };
                        dialog->open(details.target);
                        textRefresh(dialog);
                    } else {
                        reader->linkPanel->open(
                                details,
                                bounds.translated(reader->page->mapTo(
                                        reader, QPoint(0, reader->page->topHeight()))),
                                reader->page->geometry().adjusted(0, reader->page->topHeight(), 0,
                                                                  -reader->page->bottomHeight()));
                        timing.finish(reader->linkPanel);
                        textRefresh(reader->linkPanel);
                    }
                    reader->updateOrientationBlock();
                    reader->updateLinkReturn();
                });
        return true;
    }
    return false;
}

void Reader::goToPassage(const QString &target, bool pinnedShortcut)
{
    if (target.isEmpty() || session->busy() || closing || lifecycleHidden || services.suspended()) {
        return;
    }
    const LinkVisit origin{ session->state().value("position").toString(), session->page(),
                            (session->chapterTitle().isEmpty() ? strings::get("reader.reading")
                                                               : session->chapterTitle())
                                    .toUpper(),
                            pinnedShortcut };
    if (!session->goTo(target)) {
        return;
    }
    pendingPassage = origin;
    dismissLookup();
    dismissControls();
    refresh();
}

void Reader::dismissLookup(bool keepGesture)
{
    annotationModeHide.stop();
    if (annotationModeNotice)
        annotationModeNotice->hide();
    dismissLinks();
    ++lookupEpoch;
    lookupPending = false;
    if (keepGesture)
        page->stopHold();
    else
        page->cancelHold();
    brightnessHide.stop();
    if (brightnessNotice)
        brightnessNotice->hide();
    page->mark({});
    if (dictionaryPanel) {
        const bool wasVisible = dictionaryPanel->isVisible();
        dictionaryPanel->hide();
        if (wasVisible) {
            requestFlash("dictionary");
            page->update();
        }
    }
    updateOrientationBlock();
}

void Reader::dismissDictionary()
{
    if (annotationMode)
        dismissLookup();
    else
        clearAnnotationDraft();
    updateLinkReturn();
}

void Reader::lookupSelection()
{
    if (annotationStart.isEmpty() || annotationEnd.isEmpty())
        return;
    const auto epoch = ++lookupEpoch;
    lookupPending = true;
    updateOrientationBlock();
    updateLinkReturn();
    QPointer<Reader> guard(this);
    kobalt::ui::PanelTiming timing("dictionary");
    session->textRange(
            annotationStart, annotationEnd, [guard, epoch, timing](TextRange range) mutable {
                if (!guard || guard->lookupEpoch != epoch || guard->lifecycleHidden
                    || guard->services.suspended())
                    return;
                timing.stage("range");
                guard->lookupPending = false;
                const auto query = range.text.simplified();
                if (query.isEmpty()) {
                    guard->updateOrientationBlock();
                    guard->updateLinkReturn();
                    return;
                }
                QRect bounds;
                for (const auto &rect : guard->draftHighlightRects)
                    bounds = bounds.united(rect);
                guard->lookupText = query;
                guard->lookupBounds = bounds.translated(
                        guard->page->mapTo(guard, QPoint(0, guard->page->topHeight())));
                const auto result = guard->dictionary->lookup(guard->book->identity(), query);
                if (!guard || guard->lookupEpoch != epoch || guard->lifecycleHidden)
                    return;
                timing.stage("lookup");
                timing.field("chars", result.html.size());
                guard->dictionaryPanel->setChoices(guard->dictionary->choices());
                guard->dictionaryPanel->setBookFont(guard->bookFace());
                guard->dictionaryPanel->open(query, result, guard->lookupBounds);
                timing.finish(guard->dictionaryPanel);
                guard->syncAnnotationControls();
                guard->updateOrientationBlock();
                guard->updateLinkReturn();
            });
}

int Reader::millisecondsOfDay() const
{
    return services.millisecondsOfDay ? services.millisecondsOfDay()
                                      : QTime::currentTime().msecsSinceStartOfDay();
}

void Reader::startClock()
{
    startClock(millisecondsOfDay());
}

// Wake once a minute, just after the wall-clock minute changes. The session
// minutes appear only in the reading menu, which pauses the reading clock and
// refreshes its status when it opens, so no other minute needs a wakeup. The
// reader's other periodic work runs on the same tick.
void Reader::startClock(int now)
{
    // A restart after the minute changed, before its tick ran, keeps that tick.
    if (clockTimer->isActive() && now / 60000 != clockMinute) {
        clockTimer->start(0);
        return;
    }
    clockMinute = now / 60000;
    clockTimer->start(60000 - now % 60000 + 50);
}

void Reader::configureNavigation()
{
    navigation = std::make_unique<kobalt::reader::Navigation>(this, toolbarComposition);
    navigation->setScrubber(pageScrubber);
    navigation->setStatusVisible(!hideNativeStatus());
    chapterGraph->setTopPadding(navigation->statsTopPadding());
    topControls->setGeometry(0, 0, width(), navigation->toolbarHeight());
    topControls->layout()->activate();
    for (auto *panel :
         { static_cast<kobalt::ui::Panel *>(menu), static_cast<kobalt::ui::Panel *>(chapterGraph),
           static_cast<kobalt::ui::Panel *>(lightPopup) })
        panel->setEdges(navigation->panelEdge());
    navigation->returnRequested = [this] { linkReturn->click(); };
    navigation->stayRequested = [this] {
        linkHistory.clear();
        updateLinkReturn();
        syncNavigation();
    };
    navigation->activated = [this](kobalt::NavigationDestination destination) {
        activateNavigation(destination);
    };
}

void Reader::activateNavigation(kobalt::NavigationDestination destination)
{
    using Nav = kobalt::NavigationDestination;
    session->cancelPendingTurn();
    bookmarkList->hide();
    if (destination != Nav::Seek) {
        pageScrubber->cancel();
    }
    if (destination == Nav::Seek)
        openSeek();
    else if (destination == Nav::Text)
        selectPanel(0);
    else if (destination == Nav::Layout)
        selectPanel(1);
    else if (destination == Nav::Light)
        toggleLight();
    else if (destination == Nav::Stats)
        toggleGraph();
    else {
        const auto target = destination == Nav::Chapters ? AppearanceDestination::Chapters
                                                         : AppearanceDestination::Search;
        if (target == AppearanceDestination::Chapters && choices->isVisible()
            && !choosingTypography) {
            choices->hide();
            showControls();
        } else if (appearanceEdit.panelOpen() || general->isVisible()) {
            appearanceEdit.request(target);
            if (general->isVisible())
                applySettings();
            else
                finishPreview();
        } else if (target == AppearanceDestination::Chapters)
            showChapters();
        else
            openSearch();
    }
    syncNavigation();
}

QRect Reader::panelBounds(int panelHeight) const
{
    return navigation ? navigation->panelBounds(size(), panelHeight)
                      : QRect(0, metrics::panel::toolbarHeight, width(), panelHeight);
}

void Reader::syncStatusBoundary()
{
    const int boundary = page->topHeight();
    bool adjacent = false;
    for (auto *child : children()) {
        auto *panel = dynamic_cast<ui::Panel *>(child);
        if (panel && panel->isVisible() && panel->y() == boundary && panel->x() == 0
            && panel->width() == width()) {
            adjacent = true;
            break;
        }
    }
    page->setPanelBelowHeader(boundary > 0 && adjacent);
}

void Reader::hideRevealedLinks(bool restoreControls)
{
    if (revealingLinks && restoreControls && !lifecycleHidden && !closing) {
        topControls->show();
        bottomControls->show();
    }
    revealingLinks = false;
    revealedPosition.clear();
    page->revealLinks({});
}

void Reader::showRevealedLinks()
{
    session->cancelPendingTurn();
    revealingLinks = true;
    revealedPosition = session->state().value("position").toString();
    // Reveal mode must expose links beneath the ordinary toolbars too.
    topControls->hide();
    bottomControls->hide();
    syncNavigation();
}

void Reader::leaveRevealedLinks()
{
    dismissControls();
}

void Reader::syncLinkDiscovery()
{
    if (!linkDiscovery)
        return;
    const bool blocked = annotationMode || session->busy() || lifecycleHidden || closing
            || services.suspended() || appearanceEdit.panelOpen() || general->isVisible()
            || menu->isVisible() || choices->isVisible() || chapterGraph->isVisible()
            || lightPopup->isVisible() || bookmarkList->isVisible() || acknowledgments->isVisible()
            || searchPanel->isVisible() || dictionaryPanel->isVisible() || linksActive()
            || lookupPending || syncDialog || kosyncDialog || finishDialog;
    if (blocked
        || (revealingLinks && revealedPosition != session->state().value("position").toString()))
        hideRevealedLinks();
    const int count = session->pageLinks().size();
    if (!count)
        hideRevealedLinks();
    const bool controls = topControls->isVisible() || revealingLinks;
    const bool linksAvailable = !blocked && controls && count > 0;
    pageScrubber->setLinksAction(linksAvailable && !revealingLinks
                                         ? strings::get("reader.highlight_links").arg(count)
                                         : QString());
    linkDiscovery->setVisible(linksAvailable && revealingLinks);
    linkIndicator->setVisible(!blocked && !controls && count > 0
                              && preferences.value("show_link_indicator").toBool(false));
    QVector<QRect> rectangles, footnotes;
    if (revealingLinks)
        for (const auto &link : session->pageLinks()) {
            rectangles += link.rectangles;
            if (link.kind == LinkKind::Footnote)
                for (const auto &rect : link.rectangles)
                    if (narrowFootnote(rect))
                        footnotes += rect;
        }
    page->revealLinks(rectangles, footnotes);
    if (blocked || !count)
        return;

    namespace styles = kobalt::ui::styles;
    const QColor ink = palette().color(QPalette::WindowText);
    // Styles change only with the place and the strip text size, so skip
    // unchanged ones: this runs on every navigation update.
    const auto restyle = [](QWidget *widget, const QString &style) {
        if (widget->property("link_style").toString() == style)
            return;
        widget->setProperty("link_style", style);
        kobalt::ui::readerStyle(widget, style);
    };
    // Strip buttons keep the strip's height as their touch area and draw a
    // compact outlined box inside it, in the strip's text size.
    const auto stripButton = [&](QPushButton *button, int strip, int text) {
        const int box = qMin(strip - 2 * int(metrics::links::stripButtonInset),
                             ui::interfaceLineHeight(ui::interfaceFont(text))
                                     + 2 * int(metrics::links::stripButtonInset));
        restyle(button,
                styles::outlined()
                        + QString("QPushButton{%1min-height:0;padding:0 %2px;margin:%3px 0;}")
                                  .arg(styles::font(text))
                                  .arg(int(metrics::links::stripButtonPadding))
                                  .arg(qMax(0, (strip - box) / 2)));
        button->setFixedHeight(strip);
    };
    const auto fits = [](int strip, int text) {
        return strip >= ui::interfaceLineHeight(ui::headerFooterFont(text));
    };

    const QString label = strings::get("reader.highlight_links").arg(count);
    if (linkIndicator->isVisible()) {
        // The indicator sits at the right of the footer, level with the page
        // number. Without a footer it stays just above the bottom edge.
        QRect strip = page->footerTextRect();
        if (strip.isEmpty())
            strip = QRect(0,
                          height() - page->bottomHeight()
                                  - metrics::reading::stripHeight(page->chrome.footerSize),
                          width(), metrics::reading::stripHeight(page->chrome.footerSize));
        strip.translate(page->mapTo(this, QPoint()));
        const int text = page->footerTextPixels();
        linkIndicator->setText(QString::number(count));
        linkIndicator->setAccessibleName(label);
        linkIndicator->setToolTip(label);
        linkIndicator->setIcon(kobalt::ui::controlIcon(kobalt::ui::ControlIcon::Link, ink));
        linkIndicator->setIconSize(QSize(text, text));
        stripButton(linkIndicator, strip.height(), text);
        linkIndicator->adjustSize();
        linkIndicator->move(strip.right() + 1 - metrics::reading::headerSide
                                    - linkIndicator->width(),
                            strip.y());
        linkIndicator->raise();
    }

    auto *row = static_cast<QHBoxLayout *>(linkDiscovery->layout());
    linkDiscoveryIcon->setVisible(!revealingLinks);
    linkDiscoveryToggle->setVisible(!revealingLinks);
    linkDiscoveryExit->setVisible(revealingLinks);
    linkDiscoveryHint->setText(revealingLinks ? strings::get("reader.tap_revealed_link").toUpper()
                                              : strings::get("reader.long_press_link"));
    // In reveal mode the bar only labels the mode. Taps pass to the page, so
    // anything but a link leaves, the strip included.
    linkDiscovery->setAttribute(Qt::WA_TransparentForMouseEvents, revealingLinks);
    linkDiscoveryHint->setWordWrap(!revealingLinks);
    linkDiscoveryHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (revealingLinks) {
        // The label replaces the header or footer strip, never the book text,
        // in the strip's own face and size. Without a strip that fits, a tap on
        // unlinked text still leaves.
        const bool top = fits(page->topHeight(), page->headerTextPixels());
        const int strip = top ? page->topHeight() : page->bottomHeight();
        const int text = top ? page->headerTextPixels() : page->footerTextPixels();
        linkDiscovery->setVisible(top || fits(strip, text));
        linkDiscovery->setEdges({});
        linkDiscovery->setDividers(top ? Qt::BottomEdge : Qt::TopEdge);
        // Children paint their own background, so keep them off the divider.
        const int border = metrics::control::border;
        row->setContentsMargins(metrics::reading::headerSide, top ? 0 : border,
                                metrics::reading::headerSide, top ? border : 0);
        const QString stripText = QString("QLabel{font-family:'%1';%2color:%3;}")
                                          .arg(kobalt::ui::headerFooterFont().family())
                                          .arg(styles::font(text))
                                          .arg(ink.name());
        restyle(linkDiscoveryHint, stripText);
        restyle(linkDiscoveryExit, stripText);
        linkDiscovery->setGeometry(0, top ? 0 : height() - strip, width(), strip);
    } else {
        // Above the tabs the row matches the return row: the same height, the
        // icon in the same column and a divider instead of a second heavy rule.
        const int rule = metrics::chrome::heavyRule;
        linkDiscovery->setEdges(Qt::TopEdge);
        linkDiscovery->setDividers(navigation ? Qt::BottomEdge : kobalt::ui::Edges());
        row->setContentsMargins(metrics::chrome::inset, rule, metrics::chrome::inset,
                                metrics::control::border);
        linkDiscoveryIcon->setPixmap(
                kobalt::ui::controlIcon(kobalt::ui::ControlIcon::Link, ink)
                        .pixmap(metrics::control::iconSize, metrics::control::iconSize));
        restyle(linkDiscoveryHint,
                QString("QLabel{%1color:%2;}")
                        .arg(styles::font(metrics::chrome::labelText))
                        .arg(ink.name()));
        restyle(linkDiscoveryToggle,
                styles::outlined() + "QPushButton{" + styles::font(metrics::chrome::labelText)
                        + "}");
        linkDiscoveryToggle->setText(label);
        // Cover the navigation's own heavy rule, so its tabs start below this divider.
        const int bottom = navigation ? navigation->navigationTop() + rule : bottomControls->y();
        const int rowHeight = metrics::links::returnHeight + rule;
        linkDiscovery->setGeometry(0, qMax(0, bottom - rowHeight), width(), rowHeight);
    }
    if (linkDiscovery->isVisible())
        linkDiscovery->raise();
}

void Reader::syncNavigation()
{
    syncPinnedBookmarks();
    updateKosyncToolbar();
    syncStatusBoundary();
    if (navigation) {
        navigation->setStatusVisible(!hideNativeStatus());
    }
    syncLinkDiscovery();
    if (!navigation)
        return;
    if (topControls->isVisible()) {
        auto *settings = appearanceEdit.edited() && appearanceEdit.ready()
                ? appearanceEdit.preview()
                : session;
        QString face = settings->fontFace();
        if (face.isEmpty())
            face = settings->state().value("text_font_face").toString();
        if (face.isEmpty())
            face = QString::fromLatin1(kobalt::defaultFontFace);
        if (face != navigationFontFace) {
            navigationFontFace = face;
            bool loadable = false;
            for (const auto &font : session->fontCatalog())
                loadable |= font.family == face;
            if (!loadable)
                face = QString::fromLatin1(kobalt::defaultFontFace);
            registerPickerFonts(face);
            navigation->setTextFace(face);
        }
    }
    const bool chapters = choices->isVisible() && !choosingTypography;
    // The field list takes the place Chapters takes, under the Layout tab.
    // Header and footer is a sheet over the page, placed like Font below.
    QWidget *cover = fieldChooser && fieldChooser->isVisible() ? fieldChooser : nullptr;
    const bool chromeSheet = !cover && headerFooterView && headerFooterView->isVisible();
    using Nav = kobalt::NavigationDestination;
    const auto update = [&] {
        const int active = cover || chromeSheet ? int(Nav::Layout)
                : chapters                      ? int(Nav::Chapters)
                : choosingTypography && choices->isVisible() && choices->isFontPicker()
                ? int(Nav::Text)
                : menu->isVisible()
                ? (panels->currentIndex() == 0 ? int(Nav::Text) : int(Nav::Layout))
                : lightPopup->isVisible()   ? int(Nav::Light)
                : chapterGraph->isVisible() ? int(Nav::Stats)
                : seekOpen                  ? int(Nav::Seek)
                                            : -1;
        navigationActive = active;
        const auto position = session->state().value("position").toString();
        if (imageSuggestionPosition != position) {
            imageSuggestionPosition = position;
            imageSuggestionDismissed = false;
        }
        const auto imageTarget = pageSingleImage(page->image);
        bool imageFootnote = false;
        if (!imageTarget.isEmpty())
            for (const auto &link : session->pageLinks())
                if (link.kind == LinkKind::Footnote)
                    for (const auto &rect : link.rectangles)
                        imageFootnote |= rect.intersects(imageTarget);
        const bool imageSuggestion = !imageSuggestionDismissed && !annotationMode && !imageFootnote
                && !session->busy() && !lifecycleHidden && !closing && !services.suspended()
                && !lookupPending && !linksActive() && !dictionaryPanel->isVisible() && !syncDialog
                && !kosyncDialog && !finishDialog && !imageTarget.isEmpty();
        pageScrubber->setImageAction(imageSuggestion ? strings::get("image.zoom_suggestion")
                                                     : QString());
        pageScrubber->setReturnAction(
                linkReturnAvailable
                        ? strings::get("reader.return_page").arg(linkHistory.last().page + 1)
                        : QString(),
                linkHistory.isEmpty() || linkHistory.last().chapter.isEmpty()
                        ? QString()
                        : strings::get("reader.jumped_from_chapter")
                                  .arg(linkHistory.last().chapter));
        navigation->update(
                size(),
                { bottomControls, menu, chapterGraph,
                  cover                               ? cover
                          : bookmarkList->isVisible() ? static_cast<QWidget *>(bookmarkList)
                                                      : static_cast<QWidget *>(choices),
                  cover || chapters || bookmarkList->isVisible(),
                  topControls->isVisible() && !searchPanel->isVisible()
                          && !acknowledgments->isVisible(),
                  !session->busy() && !appearanceEdit.applying(), services.lightAvailable(), active,
                  linkReturnAvailable
                          ? strings::get("reader.return_page").arg(linkHistory.last().page + 1)
                          : QString(),
                  topActions, general, lightPopup,
                  linkHistory.isEmpty() || linkHistory.last().chapter.isEmpty()
                          ? QString()
                          : strings::get("reader.jumped_from_chapter")
                                    .arg(linkHistory.last().chapter),
                  seekOpen,
                  pageNavigation::menuView(preferences) == pageNavigation::MenuView::Seek
                          ? int(Nav::Seek)
                          : pageNavigation::menuView(preferences) == pageNavigation::MenuView::Stats
                          ? int(Nav::Stats)
                          : -1,
                  (isNarrowWidth(width(), ui::density())
                   || preferences.value("hide_tab_labels").toBool(false)) });
    };
    update();
    syncLinkDiscovery();
    if (choosingTypography && choices->isVisible()) {
        if (choices->isFontPicker()) {
            const int bottom = navigation->contentBottom(size());
            const int pickerHeight =
                    qMin(choices->fontPanelHeight(), qMax(1, bottom - navigation->toolbarHeight()));
            choices->setFontGeometry(QRect(0, bottom - pickerHeight, width(), pickerHeight));
        }
        // Dropdowns cover the reading-panel rules raised by navigation.
        choices->raise();
    }
    if (chromeSheet) {
        const int bottom = navigation->contentBottom(size());
        // At most up to the toolbar, with the sheet's rule on the toolbar's. A
        // short screen has no room for the slot map there, so the sheet covers
        // the action row with its own bar, as Chapters does.
        const int top = isShortHeight(height(), ui::density())
                ? navigation->statusHeight()
                : navigation->toolbarHeight() - int(metrics::chrome::heavyRule);
        const int room = qMax(1, bottom - top);
        int sheetHeight = qMin(headerFooterView->preferredHeight(width()), room);
        if (room - sheetHeight < metrics::headerFooter::toolbarSnap) {
            sheetHeight = room;
        }
        headerFooterView->setGeometry(0, bottom - sheetHeight, width(), sheetHeight);
        headerFooterView->raise();
    }
}

void Reader::showControls()
{
    const bool topVisible = topControls->isVisible();
    const bool opening = !topVisible && !bottomControls->isVisible();
    kobalt::ui::PanelTiming timing("menu");
    session->cancelPendingTurn();
    if (opening) {
        // A page turn can defer control updates until after its first paint.
        // Opening the menu needs the completed page's navigation state now.
        syncReadingControls();
        flashAfterMenu = property("reader_dark").toBool();
        if (flashAfterMenu) {
            requestFlash("menu-open");
        }
    }
    dismissLinks();
    linkReturn->hide();
    updateMenuStatus();
    if (orientation)
        orientation->setBlocked(true);
    page->cleanRefresh(false);
    topControls->show();
    bottomControls->show();
    if (!topVisible) {
        topControls->raise();
    }
    if (!navigation) {
        bottomControls->raise();
    }
    updateLinkReturn();
    syncNavigation();
    if (!appearanceEdit.panelDestination()) {
        switch (pageNavigation::menuView(preferences)) {
        case pageNavigation::MenuView::Seek:
            seekOpen = true;
            chapterGraph->hide();
            syncNavigation();
            if (!pageScrubber->selecting())
                pageScrubber->showCurrentPage();
            break;
        case pageNavigation::MenuView::Stats:
            seekOpen = false;
            pageScrubber->cancel();
            openGraph();
            break;
        case pageNavigation::MenuView::None:
            break;
        }
    }
    syncAnnotationControls();
    if (opening) {
        // The display submits disjoint bars separately. Paint their completed
        // frame together so they appear in one screen update.
        setProperty("reader_controls_opening", true);
        update();
        timing.finish(topControls);
    }
}

void Reader::dismissControls()
{
    setProperty("reader_navigation_dismissal", false);
    if (pageScrubber)
        pageScrubber->cancel();
    hideRevealedLinks();
    lightPopup->hide();
    chapterGraph->hide();
    if (appearanceEdit.applying())
        return;
    if (general->isVisible() || appearanceEdit.panelOpen()) {
        appearanceEdit.request(AppearanceDestination::Reading);
        if (general->isVisible())
            applySettings();
        else
            finishPreview();
        return;
    }
    choices->hide();
    bookmarkList->hide();
    general->hide();
    menu->hide();
    topControls->hide();
    bottomControls->hide();
    appearanceEdit.discard();
    refresh();
}

void Reader::toggleLight()
{
    session->cancelPendingTurn();
    chapterGraph->hide();
    if (lightPopup->isVisible()) {
        lightPopup->hide();
        showControls();
        return;
    }
    appearanceEdit.request(AppearanceDestination::Light);
    choices->hide();
    if (general->isVisible())
        applySettings();
    else if (appearanceEdit.panelOpen())
        finishPreview();
    else {
        menu->hide();
        // Keep Light pending so the controls do not reopen the default view.
        showControls();
        appearanceEdit.clearDestination();
        lightPopup->open();
    }
}

void Reader::showChapters()
{
    session->cancelPendingTurn();
    lightPopup->hide();
    chapterGraph->hide();
    QVector<Choice> items;
    QString current;
    for (const auto &chapter : session->chapters()) {
        const int index = items.size();
        const auto name = QString("rrChapter%1").arg(index);
        items.append({ chapter.title, chapter.position, name, {}, chapter.depth, chapter.page });
        if (index == session->contentsSelection().current)
            current = name;
    }
    choices->selected = [this](const QString &position) {
        if (session->goTo(position))
            dismissControls();
    };
    choosingTypography = false;
    menu->hide();
    bottomControls->hide();
    // Open at the size navigation gives the list, so the current chapter's page stays in view.
    const int top = navigation ? navigation->statusHeight() : 0;
    choices->setGeometry(0, top, width(),
                         (navigation ? navigation->contentBottom(size()) : height()) - top);
    choices->setChapterPosition(
            strings::get("stats.page_of").arg(session->page() + 1).arg(session->pages()));
    choices->showChoices(session->title(), std::move(items), current);
    refresh();
}

void Reader::toggleGraph()
{
    session->cancelPendingTurn();
    lightPopup->hide();
    if (chapterGraph->isVisible()) {
        chapterGraph->hide();
        showControls();
        refresh();
        return;
    }
    appearanceEdit.request(AppearanceDestination::Stats);
    choices->hide();
    if (general->isVisible())
        applySettings();
    else if (appearanceEdit.panelOpen())
        finishPreview();
    else {
        appearanceEdit.clearDestination();
        openGraph();
    }
}

void Reader::openGraph()
{
    session->cancelPendingTurn();
    menu->hide();
    general->hide();
    choices->hide();
    topControls->show();
    bottomControls->hide();
    updateGraph();
    if (navigation) {
        chapterGraph->setAvailableHeight(navigation->contentBottom(size())
                                         - navigation->toolbarHeight());
        chapterGraph->setGeometry(navigation->panelBounds(size(), chapterGraph->height()));
    }
    chapterGraph->open();
    if (navigation)
        navigation->stackPanel(chapterGraph);
    refresh();
}

// The topmost sheet owns page buttons, even when its current page has no arrows.
bool Reader::pageList(int direction)
{
    const auto children = findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (int index = children.size() - 1; index >= 0; --index) {
        auto *view = children[index];
        if (!view->isVisible())
            continue;
        const auto *modal = dynamic_cast<ui::ModalOverlay *>(view);
        if ((modal && modal->inputBarrier()) || view == choices || view == bookmarkList
            || view == searchPanel || view == acknowledgments || view == fieldChooser
            || view == headerFooterView || view == dictionaryPanel || view == linkPanel
            || view == chapterGraph || view == general || view == menu) {
            ui::turnVisiblePageControl(view, direction);
            return true;
        }
    }
    return false;
}

void Reader::pageButton(int direction)
{
    pageButton(direction, 0);
}

void Reader::scriptedPageButton(int direction)
{
    pageButton(direction, 0, false);
}

void Reader::pageButton(int direction, quint32 sourceTime, bool userInput)
{
    if (resizePending)
        return;
    if (imageViewer && imageViewer->isVisible())
        return;
    if ((direction != -1 && direction != 1) || !isEnabled() || !isVisible() || lifecycleHidden
        || services.suspended() || closing || appearanceEdit.applying())
        return;
    const auto *modal = QApplication::activeModalWidget();
    if (modal && modal != this)
        return;
    if (pageList(direction))
        return;
    if (seekOpen && pageScrubber && pageScrubber->isVisible()) {
        pageScrubber->turnPreview(direction);
        return;
    }
    if (!session->canTurn() || appearanceEdit.panelOpen() || general->isVisible()
        || choices->isVisible() || chapterGraph->isVisible() || revealingLinks
        || dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive()
        || lookupPending || acknowledgments->isVisible() || bookmarkList->isVisible()
        || lightPopup->isVisible())
        return;
    turnPage(direction, sourceTime, userInput, ui::InputClock::Key);
}

void Reader::turnPage(int direction, quint32 sourceTime, bool userInput, ui::InputClock clock)
{
    if (annotationSaving) {
        return;
    }
    if (pageScrubber)
        pageScrubber->cancel();
    kobalt::diagnosticInput();
    if (finishDialog || syncDialog || kosyncDialog || annotationErrorDialog
        || formattingWarningDialog || settingsWriteErrorDialog || !session->canTurn() || closing
        || exitPending || lifecycleHidden || services.suspended() || book->deviceTransition()
        || appearanceEdit.panelOpen() || appearanceEdit.applying())
        return;
    if (!pageTurnClock.isValid())
        pageTurnClock.start();
    const qint64 delivered =
            services.inputMilliseconds ? services.inputMilliseconds() : pageTurnClock.elapsed();
    const bool accepted = !userInput || pageTurnFilter.accept(sourceTime, delivered, clock);
    if (kobalt::diagnosticsActive())
        kobalt::diagnostic("page-turn-input",
                           { { "direction", direction },
                             { "source_ms", double(sourceTime) },
                             { "accepted", accepted },
                             { "busy", session->busy() },
                             { "page", session->page() } },
                           false);
    if (!accepted)
        return;
    if (kobalt::diagnosticsActive()) {
        if (!turnStarted.isValid())
            turnStarted.start();
    } else {
        turnStarted.invalidate();
    }
    if (!annotationMode && !session->busy() && direction > 0
        && session->nextPage() == session->page() && book->offerFinish()) {
        turnStarted.invalidate();
        showFinishChoice();
        return;
    }
    if (session->turn(direction)) {
        if (annotationMode) {
            ++annotationEpoch;
            annotationPopover->hide();
        }
        ++savedHighlightEpoch;
    } else {
        turnStarted.invalidate();
    }
}

// The layout that bookmark pages belong to, including the page area size.
QString Reader::layoutKey() const
{
    return kobalt::bookmarkLayoutKey(session->pages(), session->state(), bodySize());
}

void Reader::syncBookmarks()
{
    if (!bookmarks->available() || session->busy() || session->pages() <= 0)
        return;
    const bool resolved = bookmarks->sync();
    page->bookmarked = resolved && bookmarks->marked(session->page());
    pageScrubber->setBookmarkedPages(resolved ? bookmarks->pages() : QVector<int>());
}

void Reader::toggleBookmark()
{
    if (session->busy())
        return;
    const auto &list = bookmarks->bookmarks();
    for (int i = 0; i < list.size(); ++i)
        if (list[i].pinned() && bookmarks->page(i) == session->page()) {
            askRemoveBookmark(list[i].position);
            return;
        }
    if (bookmarks->toggle(session->page()) != kobalt::BookmarkSession::Toggle::Removed)
        return;
    syncBookmarks();
    if (bookmarkList->isVisible() && !bookmarkList->annotationsShown())
        bookmarkList->showBookmarks(bookmarkRows());
    page->refreshContent();
    saveSoon();
}

void Reader::requestFlash(const char *reason)
{
    setProperty("reader_full_flash", true);
    pagesSinceFlash = 0;
    if (kobalt::diagnosticsActive())
        kobalt::diagnostic("full-flash", { { "reason", reason } }, false);
}

void Reader::recordOpening()
{
    if (closing || exportRunning || lifecycleHidden || !isVisible() || session->busy()
        || session->pages() <= 0 || syncDialog || finishDialog || services.suspended()
        || book->deviceTransition())
        return;
    recordedOpening = true;
    if (!kosyncStarted)
        synchronizeKosync();
    // A newer synced position waits for the reader's choice. The first page
    // is on screen, so the local save is queued, not waited for.
    save(true, true);
}

void Reader::finishExit()
{
    if (!exitPending || closing || exportRunning || closeSaveError || session->busy()
        || appearanceEdit.panelOpen() || appearanceEdit.applying() || appearanceEdit.finishPending()
        || lifecycleHidden || services.suspended())
        return;
    if (finishDialog || annotationErrorDialog || settingsWriteErrorDialog)
        return;
    if (!book->deviceTransition()) {
        book->observeProgress();
        if (book->pendingProgress()) {
            showProgressChoice();
            return;
        }
    }
    if (!finishChoiceResolved && book->offerFinish()) {
        showFinishChoice();
        return;
    }
    if (!closingTimes.started.isValid())
        closingTimes.started.start();
    diagnostic("exit-save-ready", { { "elapsed_ms", double(closingTimes.started.elapsed()) } });
    exportRunning = true;
    dismissProgressChoice();
#ifdef KOBALT_SCENARIO_PLAYER
    if (scenario::sleepPersistenceOptions().mode >= 0) {
        QElapsedTimer sleepCapture;
        sleepCapture.start();
        exitSleepSnapshot = captureSleepScreen();
        closingTimes.add("sleep_capture", sleepCapture);
    }
#endif
    book->stopReading(true);
    setEnabled(false);
    closeStage.start();
    const auto request = ++closeRequest;
    saveExit(request);
}

void Reader::saveExit(quint64 request)
{
    if (request != closeRequest || !exportRunning || lifecycleHidden)
        return;
    // Writer completion and GUI acknowledgement are different barriers. Wait
    // for both before allowing destruction or reporting a failed annotation.
    if (annotationSaving || bookmarks->saving() || highlights->saving() || book->savesPending()) {
        kobalt::qt::later(10, this, [this, request] { saveExit(request); });
        return;
    }
    if (bookmarks->hasFailedSave() || highlights->hasFailedSave()) {
        failExit(request, strings::get("annotations.change_not_saved"));
        return;
    }
    diagnostic("exit-checkpoint-start", { { "pending_wait_ms", double(closeStage.elapsed()) } });
    QElapsedTimer checkpointTimer;
    checkpointTimer.start();
    book->checkpointForClose([this, request, checkpointTimer](bool saved) {
        if (request != closeRequest || !exportRunning || lifecycleHidden)
            return;
        diagnostic("exit-checkpoint-end",
                   { { "elapsed_ms", double(checkpointTimer.elapsed()) }, { "saved", saved } });
        closingTimes.add("checkpoint", closeStage);
        closeStage.restart();
        if (!saved) {
            failExit(request);
            return;
        }
        diagnostic("exit-native-queued");
        session->nativePosition([this, request](const QString &location, const QString &error) {
            finishExitNative(request, location, error);
        });
    });
}

void Reader::finishExitNative(quint64 request, const QString &location, const QString &error)
{
    if (request != closeRequest || !exportRunning || lifecycleHidden)
        return;
    closingTimes.add("native", closeStage);
    closeStage.restart();
    const QPointer<Reader> guard(this);
    // stopReading already started the time save while local saving and mapping
    // ran. Let it release Nickel's database before writing the position.
    book->finishReading([this, guard, request, location, error](bool saved) {
        if (!guard || request != closeRequest || !exportRunning || lifecycleHidden)
            return;
        closingTimes.add("session", closeStage);
        closeStage.restart();
        if (!saved) {
            failExit(request);
            return;
        }
        book->writeNative(location, error);
        closingTimes.add("native", closeStage);
        closeStage.restart();
        const auto result = book->nativeSaveResult();
        if (result == ReadingBook::NativeSave::Conflict) {
            exportRunning = false;
            ++closeRequest;
            setEnabled(true);
            showProgressChoice();
            return;
        }
        if (result == ReadingBook::NativeSave::Failed) {
            failExit(request, book->nativeSaveError());
            return;
        }
        book->markRecent();
        // Keep the verified native baseline with the final local checkpoint.
        book->checkpointForClose([this, guard, request](bool saved) {
            if (!guard || request != closeRequest || !exportRunning || lifecycleHidden)
                return;
            closingTimes.add("checkpoint", closeStage);
            if (!saved) {
                failExit(request);
                return;
            }
            save(true, true, false);
            flushKosync([this, request] { finishExitSaved(request); },
                        kosyncClient->schedule().onClose);
        });
    });
}

void Reader::finishExitSaved(quint64 request)
{
    if (request != closeRequest || !exportRunning || lifecycleHidden)
        return;
    if (pendingStorageSave()) {
        kobalt::qt::later(10, this, [this, request] { finishExitSaved(request); });
        return;
    }
    closing = true;
    close();
}

void Reader::transitionSaveError(std::function<void()> retry, std::function<void()> discard)
{
    if (closeSaveError)
        return;
    setEnabled(true);
    auto *notice = new SaveErrorView(this, {}, true);
    closeSaveError = notice;
    notice->secondaryButton()->hide();
    notice->setGeometry(rect());
    const auto request = closeRequest;
    const auto dismiss = [this, notice] {
        notice->hide();
        notice->deleteLater();
        closeSaveError.clear();
        setEnabled(false);
    };
    connect(notice->primaryButton(), &QPushButton::clicked, this, [this, request, dismiss, retry] {
        if (request != closeRequest)
            return;
        dismiss();
        retry();
    });
    connect(notice->quitButton(), &QPushButton::clicked, this, [this, request, dismiss, discard] {
        if (request != closeRequest)
            return;
        closeWithoutSaving = true;
        dismiss();
        discard();
    });
    notice->fitCard();
    show();
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
}

void Reader::failExit(quint64 request, const QString &error)
{
    if (request != closeRequest || lifecycleHidden)
        return;
    ++closeRequest;
    exportRunning = false;
    setEnabled(true);
    if (closeSaveError)
        return;
    auto *notice = new SaveErrorView(this, error, closeSaveRetried);
    closeSaveError = notice;
    notice->setObjectName("rrCloseSaveError");
    notice->setGeometry(rect());
    const auto dismiss = [this, notice] {
        notice->hide();
        notice->deleteLater();
        closeSaveError.clear();
    };
    connect(notice->primaryButton(), &QPushButton::clicked, this, [this, dismiss] {
        dismiss();
        closeSaveRetried = true;
        book->retryFailedReadingSave();
        bookmarks->retryFailedSave();
        highlights->retryFailedSave();
        finishExit();
    });
    connect(notice->secondaryButton(), &QPushButton::clicked, this, [this, dismiss] {
        dismiss();
        cancelExit();
    });
    if (notice->quitButton())
        connect(notice->quitButton(), &QPushButton::clicked, this, [this, dismiss] {
            dismiss();
            ++closeRequest;
            exportRunning = false;
            closeWithoutSaving = true;
            closing = true;
            close();
        });
    notice->dismissed = [this, dismiss] {
        dismiss();
        cancelExit();
    };
    notice->fitCard();
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
}

void Reader::cancelExit()
{
    closeSaveRetried = false;
    ++closeRequest;
    exitPending = exportRunning = false;
    finishChoiceResolved = false;
    setEnabled(true);
    closingTimes = {};
    observeReading();
    refresh();
}

bool Reader::pendingStorageSave() const
{
    return book->savesPending() || book->readingSavePending() || bookmarks->saving()
            || highlights->saving() || annotationSaving || (reportWriter && !reportWriter->idle());
}

void Reader::finishStorageSave(std::function<void(bool)> finished, bool nativeAllowed)
{
    if (imageViewer && imageViewer->isVisible())
        imageViewer->closeImage();
    ++closeRequest;
    exitPending = exportRunning = false;
    closing = lifecycleHidden = true;
    if (nativeAllowed)
        book->stopReading(true);
    else {
        const QPointer<Reader> guard(this);
        book->releaseNative(false);
        if (!guard)
            return;
    }
    finishStorageData(std::move(finished), nativeAllowed);
}

void Reader::revokeNativeReading()
{
    ++closeRequest;
    exitPending = exportRunning = false;
    closing = lifecycleHidden = true;
    book->releaseNative(false);
}

void Reader::finishStorageData(std::function<void(bool)> finished, bool nativeAllowed, int failures)
{
    if (pendingStorageSave()) {
        kobalt::qt::later(10, this, [this, finished, nativeAllowed, failures] {
            finishStorageData(finished, nativeAllowed, failures);
        });
        return;
    }
    if (closeWithoutSaving) {
        finished(false);
        return;
    }
    if (failures >= 3) {
        setProperty("reader_transition_save_failed", true);
        kobalt::diagnostic("disposal-save-failed", { { "attempts", failures } },
                           kobalt::DiagnosticSeverity::Error);
        if (allowFailedTransition || closeWithoutSaving) {
            closeWithoutSaving = true;
            finished(false);
        } else
            transitionSaveError(
                    [this, finished, nativeAllowed] {
                        book->retryFailedReadingSave();
                        finishStorageData(finished, nativeAllowed);
                    },
                    [finished] { finished(false); });
        return;
    }
    const auto retry = [this, finished, nativeAllowed, failures] {
        kobalt::qt::later(100, this, [this, finished, nativeAllowed, failures] {
            finishStorageData(finished, nativeAllowed, failures + 1);
        });
    };
    if (bookmarks->hasFailedSave() || highlights->hasFailedSave()) {
        bookmarks->retryFailedSave();
        highlights->retryFailedSave();
        retry();
        return;
    }
    const QPointer<Reader> guard(this);
    book->checkpointForClose([this, guard, finished, nativeAllowed, retry](bool saved) {
        if (!guard)
            return;
        if (!saved) {
            retry();
            return;
        }
        if (!nativeAllowed) {
            finished(true);
            return;
        }
        book->finishReading([guard, finished, retry](bool saved) {
            if (!guard)
                return;
            if (saved)
                finished(true);
            else
                retry();
        });
    });
}

// Activity triggers a due batch while the device is already awake. No save
// timer wakes it. A failed batch retains its data through the cooldown.
void Reader::saveSoon()
{
    localSaveDue = true;
    if (!closing && !exportRunning && !lifecycleHidden && !session->busy()
        && book->activitySaveDue(lowBattery ? 60000 : 300000))
        runIdleSave(false);
}

void Reader::runIdleSave(bool beforeStandby)
{
    standbyFlushQueued = false;
    if (!localSaveDue || closing || exportRunning || lifecycleHidden || session->busy()
        || book->savesPending())
        return;
    localSaveDue = false;
    book->saveReadingActivity(lowBattery || beforeStandby);
}

bool Reader::flushBeforeStandby()
{
    // Standby pauses Qt timers. Let the visible confirmation dismiss first.
    auto *notice = dynamic_cast<KosyncNotice *>(kosyncDialog.data());
    if (!lifecycleHidden && notice && notice->completing())
        return false;
    // Let background saves finish their storage syncs before standby.
    if (book->savesPending() || book->readingSavePending() || bookmarks->saving()
        || highlights->saving() || kobalt::storage::profileCheckpointPending())
        return false;
    if (lowBattery && localSaveDue && !book->activitySaveCoolingDown() && !closing && !exportRunning
        && !lifecycleHidden) {
        // Never write from inside Nickel's suspend decision. Queue the save and
        // let Nickel ask again; a busy engine already keeps the device awake.
        if (!std::exchange(standbyFlushQueued, true))
            kobalt::qt::later(0, this, [this] { runIdleSave(true); });
        return false;
    }
    if (lowBattery && kobalt::storage::profileCheckpointDue()) {
        kobalt::storage::requestProfileCheckpoint();
        return false;
    }
    return true;
}

// `snapshot` marks a close, sleep or shutdown. `background` queues the
// checkpoint on the book's writer thread; a failure is only logged.
void Reader::save(bool snapshot, bool background, bool persist)
{
    localSaveDue = false;
    QJsonObject state = book->checkpointState();
    // Only a save that waits for storage, such as sleep, reports a failure.
    // Background saves are transient and only log theirs.
    if (persist && !book->checkpoint(&state, background) && !background) {
        page->notice = strings::get("reader.could_not_save_reading_position");
        page->refreshContent();
    }
#ifdef KOBALT_READER_HARDWARE
    // On a device the state snapshot is for inspection over USB. Write it at
    // those moments, not after every reading pause, unless live-state exists.
    if (!snapshot && !liveState)
        return;
#else
    (void)snapshot;
#endif
    if (book->paths().stateReport.isEmpty())
        return;
    state["page"] = session->page();
    state["pages"] = session->pages();
    state["cached_pages"] = session->cachedPages();
    state["chapters"] = session->chapters().size();
#ifndef KOBALT_READER_HARDWARE
    QJsonArray links;
    for (const auto &link : session->pageLinks()) {
        QJsonArray rectangles;
        for (const auto &r : link.rectangles)
            rectangles.append(
                    kobalt::qt::array({ r.x(), r.y() + page->topHeight(), r.width(), r.height() }));
        links.append(
                kobalt::qt::object({ { "source", link.source }, { "rectangles", rectangles } }));
    }
    state["links"] = links;
    QJsonArray counts, positions, titles, pages;
    for (const auto &chapter : session->chapterGraph()) {
        counts.append(double(chapter.words));
        positions.append(chapter.position);
        titles.append(chapter.title);
        pages.append(chapter.page);
    }
    state["word_counts"] = kobalt::qt::object({ { "ready", session->totalWords() >= 0 },
                                                { "total_words", double(session->totalWords()) },
                                                { "words", counts },
                                                { "positions", positions },
                                                { "titles", titles },
                                                { "mapped_pages", pages },
                                                { "current", session->graphSelection().current },
                                                { "next", session->graphSelection().next } });
    const auto estimates = book->estimates();
    state["reading_estimates"] =
            kobalt::qt::object({ { "chapter_ms", double(estimates.chapterMilliseconds) },
                                 { "book_ms", double(estimates.bookMilliseconds) },
                                 { "learning", estimates.learningPace } });

#endif
    state["resource_cache"] = session->resourceStats();
    state["busy"] = resizeTimer.isActive() || session->busy();
    state["page_text_only"] = session->image().text("kobalt.animation_text_only") == "true";
    state["page_quality_mono"] = !kobalt::pageRefreshRegions(session->image(), false).isEmpty();
    state["page_quality_colour"] = !kobalt::pageRefreshRegions(session->image(), true).isEmpty();
    state["native_ui_font"] = services.uiFont().family();
    state["native_ui_font_resolved"] = QFontInfo(services.uiFont()).family();
    state["ui_font_alias"] = font().family();
    state["ui_font_substitute"] = kobalt::ui::interfaceFontFamily();
    state["ui_font_resolved"] = QFontInfo(font()).family();
    state["interface_weight"] = kobalt::ui::interfaceWeight();
    state["reading_chrome_font"] = QFontInfo(page->font()).family();
    state["session_reading_ms"] = double(book->sessionReadingMilliseconds());
    state["menu_session_text"] = menuSession->text();
    state["menu_clock_font"] = QFontInfo(menuClock->font()).family();
    state["menu_battery_font"] = QFontInfo(menuBattery->font()).family();
    state["ui_icon_set"] = QStringLiteral("Lucide");
    state["typography_button_font"] =
            QFontInfo(findChild<QPushButton *>("rrTypography")->font()).family();
    state["benchmark_typography"] = session->temporaryTypography();
    state["display_settings"] = preferences;
    state["header_text_pixels"] = page->headerTextPixels();
    state["footer_text_pixels"] = page->footerTextPixels();
    state["header_footer_preset"] = kobalt::headerfooter::presetValue(chromePreset);
    state["reading_top"] = page->topHeight();
    state["reading_bottom"] = page->bottomHeight();
    state["reading_image_height"] = session->image().height();
    state["preview_image_height"] =
            appearanceEdit.preview() ? appearanceEdit.preview()->image().height() : 0;
    state["screen_orientation"] = int(QGuiApplication::primaryScreen()->orientation());
    state["auto_rotation_available"] = orientation && orientation->automaticAvailable();
    state["viewport_width"] = width();
    state["viewport_height"] = height();
    state["header_text"] = page->headerText();
    state["contents_page"] = choices->chapterPage();
    state["font_samples"] = choices->fontSamples();
    state["native_font_families"] = QJsonArray::fromStringList(pickerNativeFamilies);
    state["font_button_family"] = fontButton->resolvedFace();
    state["graph_page"] = chapterGraph->graphPage();
    state["graph_pages"] = chapterGraph->graphPages();
    state["active_panel"] =
            QString::fromLatin1(chapterGraph->isVisible()      ? "chapters"
                                        : general->isVisible() ? "settings"
                                        : appearanceEdit.panelOpen()
                                        ? panels->currentIndex() == 0 ? "typography" : "layout"
                                        : "");
    state["appearance_panel_open"] = appearanceEdit.panelOpen();
    state["preview_ready"] = appearanceEdit.ready();
    state["preview_queued"] = appearanceEdit.queued();
    state["preview_busy"] = appearanceEdit.preview() && appearanceEdit.preview()->busy();
    state["preview_settings"] =
            appearanceEdit.ready() ? appearanceEdit.preview()->state() : QJsonObject();
    state["typography_panel"] = menu->isVisible();
    state["annotation_dragging"] = annotationDragging;
    state["bookmarked"] = page->bookmarked;
    state["bookmark_count"] = bookmarks->available() ? int(bookmarks->bookmarks().size()) : -1;
    state["bookmark_pages"] = [this] {
        QJsonArray pages;
        for (int p : bookmarks->pages())
            pages.append(p);
        return pages;
    }();
    // Inspection reports never write or wait on the GUI thread. Pending
    // storage saves include this queue at sleep, USB and close.
    if (!reportWriter)
        reportWriter = std::make_unique<storage::RecordWriter>();
    reportWriter->post([path = book->paths().stateReport, state] { return writeJson(path, state); },
                       {});
}

} // namespace kobalt::reader

namespace kobalt {
ReaderWindow *createReaderWindow(std::unique_ptr<ReadingBook> book, ReaderOrientation *orientation,
                                 ReaderHostServices services)
{
    return new reader::Reader(std::move(book), orientation, std::move(services));
}
} // namespace kobalt
