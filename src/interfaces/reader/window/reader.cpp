#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "reader_p.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "ui/input.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include "reading/appearance_settings.h"
#include "model/page_refresh_regions.h"
#include "support/diagnostics.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "storage/reader_storage.h"
#include <QApplication>
#include <QJsonArray>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QHideEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QWindow>
#include <QShowEvent>
#include <QTime>
#include <QTouchEvent>
#include <utility>

namespace kobalt::reader {
namespace strings = kobalt::strings;
using namespace kobalt::storage;
namespace m = kobalt::ui::metrics;
using kobalt::ui::makeButton;
using kobalt::platform::textRefresh;
using kobalt::Choice;

Reader::~Reader() {
    model->renderProgressChanged = {};
    renderNoticeTimer.stop();
    model->cancelPendingTurn();
    model->turnBoundary = {};
    page->cancelHold();
    brightnessHide.stop();
    lightPopup->changed = {};
    delete lightPopup;
    light.reset();
    presentation->resumeBackground();
    for (int id : registeredFonts)
        QFontDatabase::removeApplicationFont(id);
}

void Reader::checkpoint() {
    save(true);
}

void Reader::observeReading() {
    if (closing || lifecycleHidden || !isVisible() || services.suspended()) {
        book->stopReading();
        return;
    }
    book->observeReading();
}

void Reader::prepareShutdown(std::function<void()> finished) {
    prepareTransition(std::move(finished), 0);
}
void Reader::prepareUsb(std::function<void()> finished) {
    prepareTransition(std::move(finished), 2000);
}
void Reader::prepareTransition(std::function<void()> finished, int timeoutMs) {
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    model->cancelPendingTurn();
    book->stopReading();
    lifecycleHidden = true;
    exitPending = false;
    closing = true;
    clockTimer->stop();
    setEnabled(false);
    save(true);
    auto completed = std::make_shared<bool>(false);
    if (timeoutMs) {
        kobalt::qt::later(qMax(0, timeoutMs - int(elapsed->elapsed())), this, [completed, finished] {
            if (std::exchange(*completed, true)) {
                return;
            }
            kobalt::diagnostic("usb-save-timeout");
            finished();
        });
    }
    model->nativePosition(
        [this, completed, elapsed, timeoutMs, finished](const QString &location, const QString &error) {
            if (std::exchange(*completed, true)) {
                return;
            }
            if (timeoutMs && elapsed->elapsed() >= timeoutMs) {
                kobalt::diagnostic("usb-save-timeout");
            } else {
                // Keep a newer Nickel bookmark if reconciliation or the locked
                // comparison rejects this save. Device transitions cannot ask
                // the reader to resolve a conflict before they continue.
                book->writeNative(location, error);
                save(true);
            }
            finished();
        });
}

void Reader::sleepReader() {
    if (lifecycleHidden)
        return;
    setProperty("reader_sleep_clean_pending", true);
    model->cancelPendingTurn();
    book->stopReading();
    services.cancelAnimation();
    page->animationDirection = 0;
    setProperty("reader_animation_direction", 0);
    appearanceEdit.clearContinuation();
    saveNativeProgress();
    checkpoint();
    closeSearch();
    dismissLookup();
    lifecycleHidden = true;
    clockTimer->stop();
    refreshRenderNotice();
    if (syncDialog)
        syncDialog->hide();
    if (finishDialog)
        finishDialog->hide();
    // Keep the page on screen until Nickel's sleep view replaces it, as in
    // Nickel's own reader. sleepViewReady() submits that view.
}

void Reader::prepareWake() {
    if (lifecycleHidden && !isVisible())
        presentation->pauseBackground(true);
}

void Reader::cancelWake() {
    if (lifecycleHidden && !isVisible())
        presentation->resumeBackground();
}

void Reader::sleepViewReady() {
    if (!lifecycleHidden)
        return;
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

void Reader::wakeReader() {
    setProperty("reader_sleep_clean_pending", false);
    lifecycleHidden = false;
    if (finishDialog)
        finishDialog->show();
    setProperty("reader_preparing_wake", true);
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

void Reader::saveNativeProgress() {
    if (!book->canWriteNative() || model->image().isNull() || !model->error().isEmpty() || appearanceEdit.panelOpen() ||
        importingProgress || syncDialog) {
        return;
    }
    const auto anchor = model->state().value("position").toString();
    if (anchor.isEmpty() ||
        (anchor == savedNativeAnchor && model->page() == savedNativePage && model->pages() == savedNativePages)) {
        return;
    }
    QString error;
    const auto location = model->savedNativePosition(&error);
    if (!book->writeNative(location, error)) {
        return;
    }
    QJsonObject state;
    book->checkpoint(&state);
    const auto result = state.value("native_writeback").toString();
    if (result == "verified" || result == "time-only") {
        savedNativeAnchor = anchor;
        savedNativePage = model->page();
        savedNativePages = model->pages();
    }
}

void Reader::progressTick() {
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
        return;
    }
    book->pollProgress();
    showProgressChoice();
}

QVector<QWidget *> Reader::popups() const {
    return {busyNotice,
            brightnessNotice,
            fontDefaultDialog.data(),
            linkDialog.data(),
            debugDialog.data(),
            syncDialog.data(),
            finishDialog.data(),
            refreshPopup.data(),
            removeDialog.data(),
            bookmarkErrorDialog.data(),
            formattingWarningDialog.data(),
            settingsWriteErrorDialog.data()};
}

bool Reader::popup(QObject *object) const {
    for (auto *widget : popups())
        if (widget && widget == object)
            return true;
    return false;
}

void Reader::raisePopups() {
    if (raisingPopups)
        return;
    raisingPopups = true;
    for (auto *widget : popups())
        if (widget && widget->isVisible())
            widget->raise();
    raisingPopups = false;
}

bool Reader::event(QEvent *event) {
    if (event->type() == QEvent::UpdateRequest && isVisible()) {
        // Only the plain reading bars can cross an unchanged text page
        // with a partial refresh. Other panels may contain color.
        setProperty("reader_plain_controls",
                    !appearanceEdit.panelOpen() && !general->isVisible() && !choices->isVisible() &&
                        !chapterGraph->isVisible() && !linkReturn->isVisible() && !dictionaryPanel->isVisible() &&
                        !searchPanel->isVisible() && !linksActive() && !lookupPending &&
                        !acknowledgments->isVisible() && !bookmarkList->isVisible() && !lightPopup->isVisible() &&
                        !busyNotice->isVisible() && !syncDialog && !finishDialog && !bookmarkErrorDialog &&
                        !formattingWarningDialog && !settingsWriteErrorDialog && !services.suspended() &&
                        page->image.text("kobalt.animation_text_only") == "true");
        // These panels hold text and line controls. Changes inside them can
        // use a partial refresh; showing the whole panel keeps its policy.
        QWidget *partialPanel = nullptr;
        if (lightPopup->isVisible()) {
            partialPanel = lightPopup;
        } else if (appearanceEdit.panelOpen() && menu->isVisible() && !busyNotice->isVisible() &&
                   !choices->isVisible()) {
            partialPanel = menu;
        }
        setProperty("reader_partial_controls",
                    partialPanel ? QRect(partialPanel->mapTo(this, QPoint()), partialPanel->size()) : QRect());
    }
    return QWidget::event(event);
}

bool Reader::eventFilter(QObject *watched, QEvent *event) {
    const auto type = event->type();
    if (watched == brightnessNotice && type == QEvent::Hide && isVisible() && !lifecycleHidden && !closing &&
        !exportRunning && !exitPending && !services.suspended()) {
        requestFlash("light-swipe");
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
    // While a panel's changes apply, its controls ignore input. Disabling
    // them instead would repaint the whole visible panel in disabled colors,
    // one more screen update before it closes.
    const bool controlInput = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease ||
                              type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove ||
                              type == QEvent::TouchBegin || type == QEvent::TouchUpdate || type == QEvent::TouchEnd ||
                              type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::Shortcut;
    if (appearanceEdit.applying() && controlInput) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target &&
            (target == menu || target == general || menu->isAncestorOf(target) || general->isAncestorOf(target))) {
            event->accept();
            return true;
        }
    }
    // Reflow temporarily blocks rotation without repainting its buttons in
    // disabled colors. Keep the selected orientation visible until it changes.
    auto *rotationButton = qobject_cast<QPushButton *>(watched);
    if (controlInput && rotationButton &&
        (rotationButtons.contains(rotationButton) || rotationButton == autoRotateOption) && rotationControlsBlocked()) {
        event->accept();
        return true;
    }
    if (watched == menu && type == QEvent::Paint) {
        schedulePreviewWarm();
    }
    // Panels declare their placement through geometry. The reading status strip
    // owns its separator, including when an existing panel moves or closes.
    if (watched->parent() == this && dynamic_cast<ui::Panel *>(watched) &&
        (type == QEvent::Show || type == QEvent::Hide || type == QEvent::Move || type == QEvent::Resize) &&
        !statusBoundaryPending) {
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
    // Panels are raised as they open, and a panel created later starts
    // above older widgets. Either can cover a centred pop-up, so put the
    // visible pop-ups back on top whenever another child moves up.
    if ((type == QEvent::ZOrderChange || type == QEvent::Show) && watched->parent() == this && !popup(watched))
        raisePopups();
    if ((type == QEvent::KeyPress || type == QEvent::KeyRelease) && isVisible() && !lifecycleHidden &&
        !services.suspended() && QApplication::activeModalWidget() == this) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int direction = services.pageButtonDirection(key);
        if (direction) {
            // Nickel can deliver either physical keys or mapped arrows.
            // Consume the pair so it cannot also navigate the library.
            if (type == QEvent::KeyPress) {
                if (!closing && !book->deviceTransition() && services.remoteKeyActivity)
                    services.remoteKeyActivity(this, key);
                if (!key->isAutoRepeat())
                    pageButton(direction);
            }
            event->accept();
            return true;
        }
    }
    const bool touch = type == QEvent::TouchBegin || type == QEvent::TouchUpdate || type == QEvent::TouchEnd ||
                       type == QEvent::TouchCancel;
    const bool mouse = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick ||
                       type == QEvent::MouseMove || type == QEvent::MouseButtonRelease;
    if (!touch && !mouse)
        return QWidget::eventFilter(watched, event);
    bool &dismissing = touch ? dismissingFontTouch : dismissingFontMouse;
    const bool begin =
        type == QEvent::TouchBegin || type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick;
    if (begin) {
        dismissing = false;
        if (isVisible() && lightPopup->isVisible()) {
            QPointF point;
            if (touch) {
                const auto points = kobalt::ui::touchPositions(static_cast<QTouchEvent *>(event), true);
                if (points.isEmpty())
                    return QWidget::eventFilter(watched, event);
                point = points.first();
            } else
                point = kobalt::ui::mousePosition(static_cast<QMouseEvent *>(event), true);
            const QRect panelRect(lightPopup->mapToGlobal(QPoint()), lightPopup->size());
            const QRect toolbarRect(topControls->mapToGlobal(QPoint()), topControls->size());
            if (!panelRect.contains(point.toPoint()) && !toolbarRect.contains(point.toPoint()) &&
                !(navigation && navigation->contains(point.toPoint()))) {
                dismissing = true;
                lightPopup->hide();
            }
        }
        if (isVisible() && choosingTypography && choices->isVisible() && !fontDefaultDialog) {
            QPointF point;
            if (touch) {
                const auto points = kobalt::ui::touchPositions(static_cast<QTouchEvent *>(event), true);
                if (points.isEmpty())
                    return QWidget::eventFilter(watched, event);
                point = points.first();
            } else
                point = kobalt::ui::mousePosition(static_cast<QMouseEvent *>(event), true);
            const auto openingMenu = [&](const char *name) {
                const auto *button = findChild<QPushButton *>(name);
                return button->isVisible() && button->isEnabled() &&
                       QRect(button->mapToGlobal(QPoint()), button->size()).contains(point.toPoint());
            };
            if (!openingMenu("rrLight") && !openingMenu("rrSettings") &&
                !QRect(choices->mapToGlobal(QPoint()), choices->size()).contains(point.toPoint())) {
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
    if (type == QEvent::TouchEnd || type == QEvent::TouchCancel || type == QEvent::MouseButtonRelease)
        dismissing = false;
    event->accept();
    return true;
}

bool Reader::readyForPresentation() const {
    const QSize viewport(width(), qMax(1, height() - page->topHeight() - page->bottomHeight()));
    return isVisible() && updatesEnabled() && !resizePending && !model->busy() && !page->image.isNull() &&
           page->image.size() == viewport &&
           (model->resourceStats().value("pending_images").toInt() == 0 || !model->error().isEmpty());
}

void Reader::showEvent(QShowEvent *event) {
    kobalt::diagnostic("reader-visible");
    // A modal window still lets Nickel repaint its library behind us.
    // Those background flushes can request GC16 over our reading page.
    presentation->pauseBackground();
    if (orientation)
        orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
    QWidget::showEvent(event);
    resizeText();
    observeReading();
    // The first refresh runs before the window is visible. Record the
    // opening once it is; a busy model retries from its next refresh.
    if (!recordedOpening)
        QTimer::singleShot(0, this, [this] {
            if (!recordedOpening)
                recordOpening();
        });
}

void Reader::hideEvent(QHideEvent *event) {
    renderNoticeTimer.stop();
    busyNotice->setDelayedProgress(false, 0, 0);
    model->cancelPendingTurn();
    book->stopReading();
    appearanceEdit.clearContinuation();
    closeSearch();
    services.cancelAnimation();
    page->animationDirection = 0;
    setProperty("reader_animation_direction", 0);
    dismissLookup();
    kobalt::diagnostic("reader-hidden");
    if (light)
        lightPopup->hide();
    if (orientation)
        orientation->stop();
    presentation->resumeBackground();
    resizePending = false;
    setUpdatesEnabled(true);
    QWidget::hideEvent(event);
}

void Reader::resizeEvent(QResizeEvent *) {
    if (isVisible() && windowHandle() && windowHandle()->isExposed() && !page->image.isNull() && updatesEnabled()) {
        // Kobo Qt replaces the backing store when the screen turns. Keep
        // painting the current page until reflow finishes, or the new buffer
        // reaches the panel as black while the engine is busy.
        resizePending = true;
        const auto viewport = QSize(width(), qMax(1, height() - page->topHeight() - page->bottomHeight()));
        kobalt::diagnostic("reader-resize-begin",
                           {{"width", width()},
                            {"height", height()},
                            {"viewport_width", viewport.width()},
                            {"viewport_height", viewport.height()},
                            {"main_busy", model->busy()},
                            {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                            {"updates_enabled", updatesEnabled()},
                            {"resize_pending", resizePending}});
    }
    lightPopup->hide();
    dismissLookup();
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
    topControls->setGeometry(0, 0, width(), navigation ? navigation->toolbarHeight() : m::panel::toolbarHeight);
    bottomControls->setGeometry(m::panel::footer(size()));
    fitAppearance();
    chapterGraph->resize(width(), chapterGraph->height());
    choices->setGeometry(rect());
    fitSettings();
    acknowledgments->setGeometry(rect());
    searchPanel->setGeometry(rect());
    const int bookmarkTop = navigation ? navigation->statusHeight() : 0;
    bookmarkList->setGeometry(0, bookmarkTop, width(),
                              (navigation ? navigation->contentBottom(size()) : height()) - bookmarkTop);
    busyNotice->setGeometry(rect());
    busyNotice->card()->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    busyNotice->centerCard();
    brightnessNotice->centerIn(size());
    if (syncDialog)
        syncDialog->setGeometry(rect());
    if (finishDialog)
        finishDialog->setGeometry(rect());
    resizeText();
    syncNavigation();
}

void Reader::closeEvent(QCloseEvent *event) {
    model->cancelPendingTurn();
    appearanceEdit.clearContinuation();
    closeSearch();
    dismissLookup();
    if (light)
        lightPopup->hide();
    if (!closing) {
        kobalt::diagnostic("exit-requested");
        event->ignore();
        exitPending = true;
        save(true);
        if (appearanceEdit.panelOpen() || general->isVisible())
            dismissControls();
        finishExit();
        return;
    }
    setProperty("reader_exit_clean_pending", true);
    setProperty("reader_preparing_handoff", true);
    presentation->resumeAndRepaintBackground();
    setProperty("reader_preparing_handoff", false);
    if (orientation)
        orientation->stop();
    book->releaseNative();
    save(true);
    model->updated = {};
    model->renderProgressChanged = {};
    model->turnBoundary = {};
    renderNoticeTimer.stop();
    busyNotice->setDelayedProgress(false, 0, 0);
    if (finished)
        finished(model->retained());
    QWidget::closeEvent(event);
}

bool Reader::rotationControlsBlocked() const {
    return model->busy() || (appearanceEdit.edited() && appearanceEdit.preview()->busy());
}

void Reader::updateOrientationBlock() {
    if (orientation)
        orientation->setBlocked(lifecycleHidden || services.suspended() || topControls->isVisible() || model->busy() ||
                                appearanceEdit.panelOpen() || bool(syncDialog) || bool(finishDialog) ||
                                bool(bookmarkErrorDialog) || bool(formattingWarningDialog) ||
                                bool(settingsWriteErrorDialog) || dictionaryPanel->isVisible() ||
                                searchPanel->isVisible() || linksActive() || lookupPending);
}

void Reader::closeSearch() {
    ++searchEpoch;
    pendingSearchHit = {};
    model->cancelSearch();
    if (searchPanel) {
        searchPanel->stop();
        searchPanel->hide();
    }
    updateOrientationBlock();
    syncNavigation();
}

void Reader::openSearch() {
    model->cancelPendingTurn();
    if (model->busy() || appearanceEdit.panelOpen() || appearanceEdit.applying())
        return;
    dismissLookup();
    dismissControls();
    if (searchOriginal.isEmpty()) {
        searchOriginal = model->state().value("position").toString();
        searchOriginalPage = model->page();
    }
    searchPanel->setExcerptFont(bookFace());
    searchPanel->setChapters(model->chapters());
    searchPanel->setGeometry(rect());
    searchPanel->open(model->state().value("position").toString() != searchOriginal, searchOriginalPage);
    if (orientation)
        orientation->setBlocked(true);
    for (auto *widget : searchPanel->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(searchPanel);
}

bool Reader::linksActive() const {
    return linkPending || bool(linkDialog) || (linkPanel && linkPanel->isVisible());
}

void Reader::dismissLinkConfirmation() {
    if (linkDialog) {
        linkDialog->hide();
        linkDialog->deleteLater();
        linkDialog.clear();
    }
}

void Reader::dismissLinks() {
    ++linkEpoch;
    linkPending = false;
    if (linkPanel)
        linkPanel->hide();
    dismissLinkConfirmation();
}

void Reader::updateLinkReturn() {
    if (!linkReturn)
        return;
    syncLinkDiscovery();
    const bool available = !linkHistory.isEmpty() && !model->busy() && !lifecycleHidden && !closing && !linksActive() &&
                           !lookupPending && !general->isVisible() && !acknowledgments->isVisible() &&
                           !bookmarkList->isVisible() && !lightPopup->isVisible() && !syncDialog && !finishDialog &&
                           (bottomControls->isVisible() ||
                            (topControls->isVisible() && !menu->isVisible() && !chapterGraph->isVisible())) &&
                           !choices->isVisible() && !searchPanel->isVisible() && !dictionaryPanel->isVisible();
    linkReturnAvailable = available;
    linkReturn->setVisible(available);
    const int footerHeight = m::panel::footerHeight + (available ? m::links::returnHeight + m::panel::footerGap : 0);
    bottomControls->setGeometry(0, height() - footerHeight, width(), footerHeight);
    syncNavigation();
}

bool Reader::openLink(QPoint point) {
    if (model->busy() || closing || lifecycleHidden || services.suspended() || syncDialog || finishDialog)
        return false;
    for (const auto &link : model->pageLinks()) {
        QRect bounds;
        for (const auto &rect : link.rectangles) {
            if (rect.adjusted(-kobalt::ui::px(6), -kobalt::ui::px(6), kobalt::ui::px(6), kobalt::ui::px(6))
                    .contains(point)) {
                bounds = rect;
                break;
            }
        }
        if (bounds.isEmpty())
            continue;
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
        model->linkDetails(link.source, [guard, epoch, bounds, point](const kobalt::LinkDetails &details) {
            if (!guard || guard->linkEpoch != epoch || guard->lifecycleHidden || guard->services.suspended())
                return;
            auto *reader = guard.data();
            reader->linkPending = false;
            if (details.kind == kobalt::LinkKind::None) {
                reader->dismissLookup();
                reader->updateLinkReturn();
                return;
            }
            reader->linkDialog = new kobalt::ui::ModalOverlay(reader);
            reader->linkDialog->setObjectName("rrLinkActions");
            reader->linkDialog->dismissed = [reader] {
                reader->dismissLookup();
                reader->updateLinkReturn();
            };
            auto *card = reader->linkDialog->card();
            card->rows()->setContentsMargins(m::control::border, m::control::border, m::control::border,
                                             m::control::border);
            card->rows()->setSpacing(0);
            const auto action = [card](const char *name, const QString &label, kobalt::ui::ControlIcon icon,
                                       std::function<void()> callback) {
                auto *button = kobalt::ui::makeCaptionButton(card, card->rows(), name, label, std::move(callback));
                button->setRule(card->rows()->count() > 1 ? kobalt::ui::Edges(Qt::TopEdge) : kobalt::ui::Edges{});
                button->setIconVisible(true);
                button->setChevron(false);
                kobalt::ui::buttonIcon(button, icon);
                kobalt::ui::readerStyle(button,
                                        QString("QPushButton{min-height:%1px;max-height:%1px;padding:0;border:none;}")
                                            .arg(m::links::actionHeight));
                return button;
            };
            action("rrLinkGo", strings::get("reader.follow_link"), kobalt::ui::ControlIcon::Link,
                   [reader, details, bounds] {
                       if (reader->model->busy())
                           return;
                       reader->dismissLinkConfirmation();
                       if (details.kind == kobalt::LinkKind::Passage)
                           reader->goToPassage(details.target);
                       else if (details.kind == kobalt::LinkKind::Website) {
                           auto *dialog = new kobalt::WebsiteDialog(reader);
                           reader->linkDialog = dialog;
                           dialog->browserAvailable = [reader] {
                               return reader->services.browserAvailable();
                           };
                           dialog->openBrowser = [reader](const QUrl &url) {
                               return !reader->closing && !reader->lifecycleHidden && !reader->services.suspended() &&
                                      reader->services.openBrowser(url, reader);
                           };
                           dialog->dismissed = [reader] {
                               reader->dismissLookup();
                               reader->updateLinkReturn();
                           };
                           dialog->open(details.target);
                           textRefresh(dialog);
                       } else {
                           reader->linkPanel->open(
                               details, bounds.translated(0, reader->page->topHeight()),
                               reader->rect().adjusted(0, reader->page->topHeight(), 0, -reader->page->bottomHeight()));
                           textRefresh(reader->linkPanel);
                       }
                       reader->updateOrientationBlock();
                       reader->updateLinkReturn();
                   });
            action("rrLinkDictionary", strings::get("reader.link_dictionary"), kobalt::ui::ControlIcon::Dictionary,
                   [reader, point] {
                       reader->dismissLookup();
                       reader->dismissControls();
                       reader->lookupWord(point, false);
                   });
            action("rrLinkDismiss", strings::get("reader.dismiss_link"), kobalt::ui::ControlIcon::Close, [reader] {
                reader->dismissLookup();
                reader->updateLinkReturn();
            });
            const int popupWidth = qMin<int>(m::links::popupWidth, reader->width() - 2 * m::notice::sideClearance);
            card->fitToWidth(popupWidth);
            reader->linkDialog->anchorTo(bounds.translated(0, reader->page->topHeight()));
            reader->linkDialog->show();
            reader->linkDialog->raise();
            reader->linkDialog->setFocus();
            for (auto *widget : reader->linkDialog->findChildren<QWidget *>())
                textRefresh(widget);
            reader->updateOrientationBlock();
            reader->updateLinkReturn();
        });
        return true;
    }
    return false;
}

void Reader::goToPassage(const QString &target) {
    if (target.isEmpty() || model->busy() || closing || lifecycleHidden || services.suspended()) {
        return;
    }
    const LinkVisit origin{model->state().value("position").toString(), model->page(), page->chapterTitle};
    if (!model->goTo(target)) {
        return;
    }
    pendingPassage = origin;
    dismissLookup();
    dismissControls();
    refresh();
}

void Reader::dismissLookup(bool keepGesture) {
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
    if (dictionaryPanel)
        dictionaryPanel->hide();
    updateOrientationBlock();
}

void Reader::lookupWord(QPoint point, bool checkLink) {
    if (closing || lifecycleHidden || services.suspended() || model->busy() || appearanceEdit.panelOpen() ||
        topControls->isVisible() || choices->isVisible() || acknowledgments->isVisible() || syncDialog ||
        finishDialog || dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive())
        return;
    if (checkLink && openLink(point))
        return;
    const auto epoch = ++lookupEpoch;
    lookupPending = true;
    updateLinkReturn();
    if (orientation)
        orientation->setBlocked(true);
    QPointer<Reader> guard(this);
    model->wordAt(point, [guard, epoch](const kobalt::WordSelection &word) {
        if (!guard || guard->lookupEpoch != epoch || guard->lifecycleHidden || guard->services.suspended())
            return;
        auto *reader = guard.data();
        reader->lookupPending = false;
        if (word.text.isEmpty()) {
            reader->updateOrientationBlock();
            reader->updateLinkReturn();
            return;
        }
        reader->page->mark(word.rectangles);
        QRect bounds;
        for (const auto &rect : word.rectangles)
            bounds = bounds.united(rect);
        const auto result = reader->dictionary->lookup(reader->book->identity(), word.text);
        if (reader->lookupEpoch != epoch || reader->lifecycleHidden)
            return;
        reader->lookupText = word.text;
        reader->lookupBounds = bounds.translated(0, reader->page->topHeight());
        reader->dictionaryPanel->setChoices(reader->dictionary->choices());
        reader->dictionaryPanel->setBookFont(reader->bookFace());
        reader->dictionaryPanel->open(word.text, result, reader->lookupBounds);
        textRefresh(reader->dictionaryPanel);
        for (auto *widget : reader->dictionaryPanel->findChildren<QWidget *>())
            textRefresh(widget);
    });
}

// Wake at the next wall-clock minute or the next reading minute of the
// menu status, whichever comes first.
void Reader::startClock() {
    const auto clock = 60000 - QTime::currentTime().msecsSinceStartOfDay() % 60000;
    const auto reading = 60000 - book->readingMilliseconds() % 60000;
    clockTimer->start(int(qMin<qint64>(clock, reading)) + 50);
}

void Reader::configureNavigation() {
    navigation = std::make_unique<kobalt::reader::Navigation>(this, toolbarComposition);
    navigation->setStatusVisible(!hideNativeStatus());
    chapterGraph->setTopPadding(navigation->statsTopPadding());
    topControls->setGeometry(0, 0, width(), navigation->toolbarHeight());
    topControls->layout()->activate();
    for (auto *panel : {static_cast<kobalt::ui::Panel *>(menu), static_cast<kobalt::ui::Panel *>(chapterGraph),
                        static_cast<kobalt::ui::Panel *>(lightPopup)})
        panel->setEdges(navigation->panelEdge());
    navigation->returnRequested = [this] {
        linkReturn->click();
    };
    navigation->stayRequested = [this] {
        linkHistory.clear();
        updateLinkReturn();
        syncNavigation();
    };
    navigation->activated = [this](kobalt::NavigationDestination destination) {
        model->cancelPendingTurn();
        bookmarkList->hide();
        using Nav = kobalt::NavigationDestination;
        if (destination == Nav::Text)
            selectPanel(0);
        else if (destination == Nav::Layout)
            selectPanel(1);
        else if (destination == Nav::Light)
            toggleLight();
        else if (destination == Nav::Stats)
            toggleGraph();
        else {
            const auto target =
                destination == Nav::Chapters ? AppearanceDestination::Chapters : AppearanceDestination::Search;
            if (target == AppearanceDestination::Chapters && choices->isVisible() && !choosingTypography) {
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
    };
}

QRect Reader::panelBounds(int panelHeight) const {
    return navigation ? navigation->panelBounds(size(), panelHeight)
                      : QRect(0, m::panel::toolbarHeight, width(), panelHeight);
}

// Pinned Stats has no tab. It opens above the tabs with the reading
// controls and steps aside while another panel covers that area.
bool Reader::statsPinned() const {
    return pinStats;
}

void Reader::syncStatusBoundary() {
    const int boundary = page->topHeight();
    bool adjacent = false;
    for (auto *child : children()) {
        auto *panel = dynamic_cast<ui::Panel *>(child);
        if (panel && panel->isVisible() && panel->y() == boundary && panel->x() == 0 && panel->width() == width()) {
            adjacent = true;
            break;
        }
    }
    page->setPanelBelowHeader(boundary > 0 && adjacent);
}

void Reader::hideRevealedLinks(bool restoreControls) {
    if (revealingLinks && restoreControls && !lifecycleHidden && !closing) {
        topControls->show();
        bottomControls->show();
    }
    revealingLinks = false;
    revealedPosition.clear();
    page->revealLinks({});
}

void Reader::showRevealedLinks() {
    model->cancelPendingTurn();
    revealingLinks = true;
    revealedPosition = model->state().value("position").toString();
    // Reveal mode must expose links beneath the ordinary toolbars too.
    topControls->hide();
    bottomControls->hide();
    syncNavigation();
}

void Reader::leaveRevealedLinks() {
    dismissControls();
}

void Reader::syncLinkDiscovery() {
    if (!linkDiscovery)
        return;
    const bool blocked = model->busy() || lifecycleHidden || closing || services.suspended() ||
                         appearanceEdit.panelOpen() || general->isVisible() || menu->isVisible() ||
                         choices->isVisible() || chapterGraph->isVisible() || lightPopup->isVisible() ||
                         bookmarkList->isVisible() || acknowledgments->isVisible() || searchPanel->isVisible() ||
                         dictionaryPanel->isVisible() || linksActive() || lookupPending || syncDialog || finishDialog;
    if (blocked || (revealingLinks && revealedPosition != model->state().value("position").toString()))
        hideRevealedLinks();
    const int count = model->pageLinks().size();
    if (!count)
        hideRevealedLinks();
    const bool controls = topControls->isVisible() || revealingLinks;
    linkDiscovery->setVisible(!blocked && controls && count > 0);
    linkIndicator->setVisible(!blocked && !controls && count > 0 &&
                              preferences.value("show_link_indicator").toBool(false));
    QVector<QRect> rectangles;
    if (revealingLinks)
        for (const auto &link : model->pageLinks())
            rectangles += link.rectangles;
    page->revealLinks(rectangles);
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
        const int box =
            qMin(strip - 2 * int(m::links::stripButtonInset),
                 QFontMetrics(kobalt::ui::interfaceFont(text)).height() + 2 * int(m::links::stripButtonInset));
        restyle(button, styles::outlined() + QString("QPushButton{%1min-height:0;padding:0 %2px;margin:%3px 0;}")
                                                 .arg(styles::font(text))
                                                 .arg(int(m::links::stripButtonPadding))
                                                 .arg(qMax(0, (strip - box) / 2)));
        button->setFixedHeight(strip);
    };
    const auto fits = [](int strip, int text) {
        return strip >= QFontMetrics(kobalt::ui::chromeFont(text)).height();
    };

    const QString label = strings::get("reader.highlight_links").arg(count);
    if (linkIndicator->isVisible()) {
        // The indicator sits at the right of the footer, level with the page
        // number. Without a footer it stays just above the bottom edge.
        QRect strip = page->footerTextRect();
        if (strip.isEmpty())
            strip = QRect(0, height() - page->bottomHeight() - m::reading::stripHeight(page->footerSize), width(),
                          m::reading::stripHeight(page->footerSize));
        strip.translate(page->mapTo(this, QPoint()));
        const int text = page->footerTextPixels;
        linkIndicator->setText(QString::number(count));
        linkIndicator->setAccessibleName(label);
        linkIndicator->setToolTip(label);
        linkIndicator->setIcon(kobalt::ui::controlIcon(kobalt::ui::ControlIcon::Link, ink));
        linkIndicator->setIconSize(QSize(text, text));
        stripButton(linkIndicator, strip.height(), text);
        linkIndicator->adjustSize();
        linkIndicator->move(strip.right() + 1 - m::reading::headerSide - linkIndicator->width(), strip.y());
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
        const bool top = fits(page->topHeight(), page->headerTextPixels);
        const int strip = top ? page->topHeight() : page->bottomHeight();
        const int text = top ? page->headerTextPixels : page->footerTextPixels;
        linkDiscovery->setVisible(top || fits(strip, text));
        linkDiscovery->setEdges({});
        linkDiscovery->setDividers(top ? Qt::BottomEdge : Qt::TopEdge);
        // Children paint their own background, so keep them off the divider.
        const int border = m::control::border;
        row->setContentsMargins(m::reading::headerSide, top ? 0 : border, m::reading::headerSide, top ? border : 0);
        const QString stripText = QString("QLabel{font-family:'%1';%2color:%3;}")
                                      .arg(kobalt::ui::chromeFont().family())
                                      .arg(styles::font(text))
                                      .arg(ink.name());
        restyle(linkDiscoveryHint, stripText);
        restyle(linkDiscoveryExit, stripText);
        linkDiscovery->setGeometry(0, top ? 0 : height() - strip, width(), strip);
    } else {
        // Above the tabs the row matches the return row: the same height, the
        // icon in the same column and a divider instead of a second heavy rule.
        const int rule = m::chrome::heavyRule;
        linkDiscovery->setEdges(Qt::TopEdge);
        linkDiscovery->setDividers(navigation ? Qt::BottomEdge : kobalt::ui::Edges());
        row->setContentsMargins(m::chrome::inset, rule, m::chrome::inset, m::control::border);
        linkDiscoveryIcon->setPixmap(kobalt::ui::controlIcon(kobalt::ui::ControlIcon::Link, ink)
                                         .pixmap(m::control::iconSize, m::control::iconSize));
        restyle(linkDiscoveryHint,
                QString("QLabel{%1color:%2;}").arg(styles::font(m::chrome::labelText)).arg(ink.name()));
        restyle(linkDiscoveryToggle, styles::outlined() + "QPushButton{" + styles::font(m::chrome::labelText) + "}");
        linkDiscoveryToggle->setText(label);
        // Cover the navigation's own heavy rule, so its tabs start below this divider.
        const int bottom = navigation ? navigation->navigationTop() + rule : bottomControls->y();
        const int rowHeight = m::links::returnHeight + rule;
        linkDiscovery->setGeometry(0, qMax(0, bottom - rowHeight), width(), rowHeight);
    }
    if (linkDiscovery->isVisible())
        linkDiscovery->raise();
}

void Reader::syncNavigation() {
    syncStatusBoundary();
    if (navigation) {
        navigation->setStatusVisible(!hideNativeStatus());
    }
    syncLinkDiscovery();
    if (!navigation)
        return;
    if (topControls->isVisible()) {
        auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        QString face = settings->fontFace();
        if (face.isEmpty())
            face = settings->state().value("text_font_face").toString();
        if (face.isEmpty())
            face = QString::fromLatin1(kobalt::defaultFontFace);
        if (face != navigationFontFace) {
            navigationFontFace = face;
            bool loadable = false;
            for (const auto &font : model->fontCatalog())
                loadable |= font.family == face;
            if (!loadable)
                face = QString::fromLatin1(kobalt::defaultFontFace);
            registerPickerFonts(face);
            navigation->setTextFace(face);
        }
    }
    const bool chapters = choices->isVisible() && !choosingTypography;
    const bool pinned = statsPinned();
    using Nav = kobalt::NavigationDestination;
    const auto update = [&] {
        const int active = chapters                                                                ? int(Nav::Chapters)
                           : choosingTypography && choices->isVisible() && choices->isFontPicker() ? int(Nav::Text)
                           : menu->isVisible()       ? (panels->currentIndex() == 0 ? int(Nav::Text) : int(Nav::Layout))
                           : lightPopup->isVisible() ? int(Nav::Light)
                           : chapterGraph->isVisible() && !pinned ? int(Nav::Stats)
                                                                  : -1;
        navigation->update(
            size(),
            {bottomControls, menu, chapterGraph,
             bookmarkList->isVisible() ? static_cast<QWidget *>(bookmarkList) : static_cast<QWidget *>(choices),
             chapters || bookmarkList->isVisible(),
             topControls->isVisible() && !searchPanel->isVisible() && !acknowledgments->isVisible(),
             !model->busy() && !appearanceEdit.applying(), services.lightAvailable(), active,
             linkReturnAvailable ? strings::get("reader.return_page").arg(linkHistory.last().page + 1) : QString(),
             strings::get("navigation.page_count").arg(model->page() + 1).arg(model->pages()), topActions, general,
             lightPopup, pinned,
             linkHistory.isEmpty() || linkHistory.last().chapter.isEmpty()
                 ? QString()
                 : strings::get("reader.jumped_from_chapter").arg(linkHistory.last().chapter)});
    };
    update();
    syncLinkDiscovery();
    if (pinned) {
        const QRect bounds = navigation->panelBounds(size(), chapterGraph->height());
        // Settings always takes precedence; Stats with Settings is too busy.
        bool covered = !topControls->isVisible() || general->isVisible();
        for (const QWidget *panel : {static_cast<QWidget *>(menu), static_cast<QWidget *>(lightPopup),
                                     static_cast<QWidget *>(choices), static_cast<QWidget *>(general),
                                     static_cast<QWidget *>(searchPanel), static_cast<QWidget *>(dictionaryPanel),
                                     static_cast<QWidget *>(acknowledgments), static_cast<QWidget *>(bookmarkList)})
            covered = covered || (panel->isVisible() && panel->geometry().intersects(bounds));
        if (covered == chapterGraph->isVisible()) {
            if (covered)
                chapterGraph->hide();
            else {
                updateGraph();
                chapterGraph->setGeometry(navigation->panelBounds(size(), chapterGraph->height()));
                chapterGraph->open();
                navigation->stackPanel(chapterGraph);
            }
            update();
        }
        pinnedStatsOpen = chapterGraph->isVisible();
    } else if (pinnedStatsOpen) {
        // Unpinning closes what the pin opened; the Stats tab opens it again.
        pinnedStatsOpen = false;
        chapterGraph->hide();
        update();
    }
    if (choosingTypography && choices->isVisible()) {
        if (choices->isFontPicker()) {
            const int bottom = navigation->contentBottom(size());
            const int pickerHeight = qMin(choices->fontPanelHeight(), qMax(1, bottom - navigation->toolbarHeight()));
            choices->setFontGeometry(QRect(0, bottom - pickerHeight, width(), pickerHeight));
        }
        // Dropdowns cover the reading-panel rules raised by navigation.
        choices->raise();
    }
}

void Reader::showControls() {
    const bool topVisible = topControls->isVisible();
    const bool opening = !topVisible && !bottomControls->isVisible();
    model->cancelPendingTurn();
    if (opening) {
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
    if (opening) {
        // The display submits disjoint bars separately. Paint their completed
        // frame together so they appear in one screen update.
        setProperty("reader_controls_opening", true);
        update();
    }
}

void Reader::dismissControls() {
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

void Reader::toggleLight() {
    model->cancelPendingTurn();
    chapterGraph->hide();
    if (lightPopup->isVisible()) {
        lightPopup->hide();
        return;
    }
    appearanceEdit.request(AppearanceDestination::Light);
    choices->hide();
    if (general->isVisible())
        applySettings();
    else if (appearanceEdit.panelOpen())
        finishPreview();
    else {
        appearanceEdit.clearDestination();
        menu->hide();
        showControls();
        lightPopup->open();
    }
}

void Reader::showChapters() {
    model->cancelPendingTurn();
    lightPopup->hide();
    chapterGraph->hide();
    QVector<Choice> items;
    QString current;
    for (const auto &chapter : model->chapters()) {
        const int index = items.size();
        const auto name = QString("rrChapter%1").arg(index);
        items.append({chapter.title, chapter.position, name, {}, chapter.depth, chapter.page});
        if (index == model->contentsSelection().current)
            current = name;
    }
    choices->selected = [this](const QString &position) {
        if (model->goTo(position))
            dismissControls();
    };
    choosingTypography = false;
    menu->hide();
    bottomControls->hide();
    // Open at the size navigation gives the list, so the current chapter's page stays in view.
    const int top = navigation ? navigation->statusHeight() : 0;
    choices->setGeometry(0, top, width(), (navigation ? navigation->contentBottom(size()) : height()) - top);
    choices->setChapterPosition(strings::get("stats.page_of").arg(model->page() + 1).arg(model->pages()));
    choices->showChoices(model->title(), std::move(items), current);
    refresh();
}

void Reader::toggleGraph() {
    model->cancelPendingTurn();
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

void Reader::openGraph() {
    model->cancelPendingTurn();
    menu->hide();
    general->hide();
    choices->hide();
    topControls->show();
    bottomControls->hide();
    updateGraph();
    if (navigation)
        chapterGraph->setGeometry(navigation->panelBounds(size(), chapterGraph->height()));
    chapterGraph->open();
    if (navigation)
        navigation->stackPanel(chapterGraph);
    refresh();
}

// A fullscreen list pages with the hardware buttons, as with its own arrows.
// Returns whether such a list is open; the book behind it never turns.
bool Reader::pageList(int direction) {
    QWidget *list = choices->isVisible() && !choosingTypography ? static_cast<QWidget *>(choices)
                    : bookmarkList->isVisible()                 ? static_cast<QWidget *>(bookmarkList)
                    : searchPanel->isVisible()                  ? static_cast<QWidget *>(searchPanel)
                    : acknowledgments->isVisible()              ? acknowledgments
                                                                : nullptr;
    if (!list)
        return false;
    // PagerBar has no meta-object, so qobject_cast cannot select it.
    for (auto *child : list->findChildren<QWidget *>()) {
        auto *pager = dynamic_cast<ui::PagerBar *>(child);
        if (!pager || !pager->isVisible())
            continue;
        auto *button = direction > 0 ? pager->next : pager->previous;
        if (button->isVisible() && button->isEnabled())
            button->click();
        break;
    }
    return true;
}

void Reader::pageButton(int direction) {
    if ((direction != -1 && direction != 1) || !isEnabled() || !isVisible() || lifecycleHidden ||
        services.suspended() || closing)
        return;
    const auto *modal = QApplication::activeModalWidget();
    if (modal && modal != this)
        return;
    for (auto *widget : popups())
        if (widget && widget->isVisible() && dynamic_cast<ui::ModalOverlay *>(widget))
            return;
    if (pageList(direction))
        return;
    if (!model->canTurn() || appearanceEdit.panelOpen() || general->isVisible() || choices->isVisible() ||
        (chapterGraph->isVisible() && !statsPinned()) || revealingLinks || dictionaryPanel->isVisible() ||
        searchPanel->isVisible() || linksActive() || lookupPending || acknowledgments->isVisible() ||
        bookmarkList->isVisible() || lightPopup->isVisible())
        return;
    turnPage(direction);
}

void Reader::turnPage(int direction) {
    turnStarted.start();
    if (finishDialog || syncDialog || bookmarkErrorDialog || formattingWarningDialog || settingsWriteErrorDialog ||
        !model->canTurn() || closing || exitPending || lifecycleHidden || services.suspended() ||
        book->deviceTransition() || appearanceEdit.panelOpen() || appearanceEdit.applying())
        return;
    if (!model->busy() && direction > 0 && model->nextPage() == model->page() && book->offerFinish()) {
        showFinishChoice();
        return;
    }
    model->turn(direction);
}

// The layout that bookmark pages belong to, including the page area size.
QString Reader::layoutKey() const {
    return kobalt::bookmarkLayoutKey(model->pages(), model->state(),
                                     QSize(width(), height() - page->topHeight() - page->bottomHeight()));
}

void Reader::syncBookmarks() {
    if (!bookmarks->available() || model->busy() || model->pages() <= 0)
        return;
    page->bookmarked = bookmarks->sync() && bookmarks->marked(model->page());
}

void Reader::toggleBookmark() {
    if (bookmarks->toggle(model->page()) != kobalt::BookmarkSession::Toggle::Removed)
        return;
    page->bookmarked = false;
    page->refreshContent();
    saveSoon();
}

void Reader::requestFlash(const char *reason) {
    setProperty("reader_full_flash", true);
    pagesSinceFlash = 0;
    kobalt::diagnostic("full-flash", {{"reason", reason}}, false);
}

void Reader::recordOpening() {
    if (closing || exportRunning || lifecycleHidden || !isVisible() || model->busy() || model->pages() <= 0 ||
        syncDialog || finishDialog || services.suspended() || book->deviceTransition())
        return;
    recordedOpening = true;
    // A newer synced position waits for the reader's choice.
    saveNativeProgress();
    save(true);
}

void Reader::finishExit() {
    if (!exitPending || closing || exportRunning || model->busy() || appearanceEdit.panelOpen() ||
        appearanceEdit.applying() || appearanceEdit.finishPending() || lifecycleHidden)
        return;
    if (finishDialog)
        return;
    if (!services.suspended() && !book->deviceTransition()) {
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
    exportRunning = true;
    dismissProgressChoice();
    // Keep the last reader frame while asynchronous writeback completes.
    setUpdatesEnabled(false);
    setEnabled(false);
    model->nativePosition([this](const QString &location, const QString &mappingError) {
        save();
        if (!book->writeNative(location, mappingError)) {
            exportRunning = false;
            setEnabled(true);
            setUpdatesEnabled(true);
            showProgressChoice();
            return;
        }
        save(true);
        closing = true;
        close();
    });
}

// A page turn does not have to reach storage before the next one. Write
// the position when reading pauses for two seconds, and at the latest 30
// seconds after the first unsaved turn. Closing, sleep, USB and power-off
// call save() at once.
void Reader::saveSoon() {
    if (!saveTimer) {
        saveTimer = new QTimer(this);
        saveTimer->setSingleShot(true);
        connect(saveTimer, &QTimer::timeout, this, [this] {
            if (!closing)
                save();
        });
    }
    if (!saveTimer->isActive())
        unsavedSince.start();
    if (unsavedSince.elapsed() >= 30000) {
        save();
        return;
    }
    saveTimer->start(2000);
}

// `snapshot` marks a close, sleep or shutdown.
void Reader::save(bool snapshot) {
    if (saveTimer)
        saveTimer->stop();
    QJsonObject state;
    if (!book->checkpoint(&state)) {
        page->progress = strings::get("reader.could_not_save_reading_position");
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
    state["page"] = model->page();
    state["pages"] = model->pages();
    state["cached_pages"] = model->cachedPages();
    state["chapters"] = model->chapters().size();
#ifndef KOBALT_READER_HARDWARE
    QJsonArray links;
    for (const auto &link : model->pageLinks()) {
        QJsonArray rectangles;
        for (const auto &r : link.rectangles)
            rectangles.append(kobalt::qt::array({r.x(), r.y() + page->topHeight(), r.width(), r.height()}));
        links.append(kobalt::qt::object({{"source", link.source}, {"rectangles", rectangles}}));
    }
    state["links"] = links;
    QJsonArray counts, positions, titles, pages;
    for (const auto &chapter : model->chapterGraph()) {
        counts.append(double(chapter.words));
        positions.append(chapter.position);
        titles.append(chapter.title);
        pages.append(chapter.page);
    }
    state["word_counts"] = kobalt::qt::object({{"ready", model->totalWords() >= 0},
                                               {"total_words", double(model->totalWords())},
                                               {"words", counts},
                                               {"positions", positions},
                                               {"titles", titles},
                                               {"mapped_pages", pages},
                                               {"current", model->graphSelection().current},
                                               {"next", model->graphSelection().next}});

#endif
    state["resource_cache"] = model->resourceStats();
    state["busy"] = model->busy();
    state["page_text_only"] = model->image().text("kobalt.animation_text_only") == "true";
    state["page_quality_mono"] = !kobalt::pageRefreshRegions(model->image(), false).isEmpty();
    state["page_quality_colour"] = !kobalt::pageRefreshRegions(model->image(), true).isEmpty();
    state["native_ui_font"] = services.uiFont().family();
    state["native_ui_font_resolved"] = QFontInfo(services.uiFont()).family();
    state["ui_font_alias"] = font().family();
    state["ui_font_substitute"] = kobalt::ui::interfaceFontFamily();
    state["ui_font_resolved"] = QFontInfo(font()).family();
    state["interface_weight"] = kobalt::ui::interfaceWeight();
    state["reading_chrome_font"] = QFontInfo(page->font()).family();
    state["session_reading_ms"] = double(book->readingMilliseconds());
    state["menu_session_text"] = menuSession->text();
    state["menu_clock_font"] = QFontInfo(menuClock->font()).family();
    state["menu_battery_font"] = QFontInfo(menuBattery->font()).family();
    state["ui_icon_set"] = QStringLiteral("Lucide");
    state["typography_button_font"] = QFontInfo(findChild<QPushButton *>("rrTypography")->font()).family();
    state["benchmark_typography"] = model->temporaryTypography();
    state["display_settings"] = preferences;
    state["header_text_pixels"] = page->headerTextPixels;
    state["footer_text_pixels"] = page->footerTextPixels;
    state["reading_top"] = page->topHeight();
    state["reading_bottom"] = page->bottomHeight();
    state["reading_image_height"] = model->image().height();
    state["preview_image_height"] = appearanceEdit.preview() ? appearanceEdit.preview()->image().height() : 0;
    state["screen_orientation"] = int(QGuiApplication::primaryScreen()->orientation());
    state["auto_rotation_available"] = orientation && orientation->automaticAvailable();
    state["viewport_width"] = width();
    state["viewport_height"] = height();
    state["clock_text"] = page->clockText;
    state["header_text"] = page->chapterTitle;
    state["contents_page"] = choices->chapterPage();
    state["font_samples"] = choices->fontSamples();
    state["native_font_families"] = QJsonArray::fromStringList(pickerNativeFamilies);
    state["font_button_family"] = fontButton->resolvedFace();
    state["graph_page"] = chapterGraph->graphPage();
    state["graph_pages"] = chapterGraph->graphPages();
    state["active_panel"] =
        QString::fromLatin1(chapterGraph->isVisible()    ? "chapters"
                            : general->isVisible()       ? "settings"
                            : appearanceEdit.panelOpen() ? panels->currentIndex() == 0 ? "typography" : "layout"
                                                         : "");
    state["appearance_panel_open"] = appearanceEdit.panelOpen();
    state["preview_ready"] = appearanceEdit.ready();
    state["preview_busy"] = appearanceEdit.preview() && appearanceEdit.preview()->busy();
    state["preview_settings"] = appearanceEdit.ready() ? appearanceEdit.preview()->state() : QJsonObject();
    state["typography_panel"] = menu->isVisible();
    state["bookmarked"] = page->bookmarked;
    state["bookmark_count"] = bookmarks->available() ? int(bookmarks->bookmarks().size()) : -1;
    state["bookmark_pages"] = [this] {
        QJsonArray pages;
        for (int p : bookmarks->pages())
            pages.append(p);
        return pages;
    }();
    writeJson(book->paths().stateReport, state);
}

}

namespace kobalt {
ReaderWindow *createReaderWindow(std::unique_ptr<ReadingBook> book, ReaderOrientation *orientation,
                                 ReaderHostServices services) {
    return new reader::Reader(std::move(book), orientation, std::move(services));
}
}
