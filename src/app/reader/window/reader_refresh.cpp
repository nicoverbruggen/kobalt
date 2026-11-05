#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/panel_timing.h"
#include "reader_p.h"
#include "reading/appearance_settings.h"
#include "reading/page_navigation.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include <QElapsedTimer>
#include <QtMath>
#include <initializer_list>
#include <utility>

namespace kobalt::reader {
namespace strings = kobalt::strings;

void Reader::refreshRenderNotice()
{
    if (!busyNotice)
        return;
    renderNoticeTimer.stop();
    const auto progress = session->renderProgress();
    const bool active = progress.active && !resizePending && !closing && !lifecycleHidden;
    const bool showNotice = active && progress.elapsedMilliseconds >= 1000;
    const char *message = strings::key("reader.applying_appearance_changes");
    switch (progress.reason) {
    case RenderReason::EnableBionic:
        message = strings::key("reader.enabling_bold_reading");
        break;
    case RenderReason::DisableBionic:
        message = strings::key("reader.disabling_bold_reading");
        break;
    case RenderReason::PageLayout:
        message = strings::key("reader.updating_page_layout");
        break;
    case RenderReason::Appearance:
        break;
    }
    const auto text = strings::get(message);
    const int percent = progress.overallPercent();
    const QRect progressRect = showNotice && !property("reader_dark").toBool()
                    && page->image.text("kobalt.animation_text_only") == "true"
            ? busyNotice->progressUpdateRect(percent, text)
            : QRect();
    setProperty("reader_progress_update", progressRect);
    busyNotice->setDelayedProgress(active, progress.elapsedMilliseconds, percent, text);
    if (active && !showNotice) {
        renderNoticeTimer.start(int(qMax<qint64>(1, 1000 - progress.elapsedMilliseconds)));
    }
}

void Reader::refresh()
{
    if (lifecycleHidden)
        return;
    // A KOSync result that waited while the reader was hidden continues now.
    if (kosyncReaderReady && !kosyncReaderRetry.isActive() && isVisible() && !services.suspended())
        continueKosyncWhenReady(std::move(kosyncReaderReady));
    const bool darkReady = publishPendingDarkMode();
    const bool dark = savingDarkMode >= 0 ? bool(savingDarkMode)
            : pendingDarkMode >= 0        ? bool(pendingDarkMode)
                                          : session->state().value("dark_mode").toBool(false);
    const bool darkEnabled = savingDarkMode < 0 && pendingDarkMode < 0 && !session->busy()
            && !session->temporaryTypography();
    if (!restoringDefaults)
        darkOption->setChecked(dark);
    darkOption->setEnabled(darkEnabled);
    lightPopup->setDarkModeShortcut(preferences.value("light_dark_mode_shortcut").toBool(false),
                                    dark, darkEnabled);
    if (recordedOpening && !kosyncStarted)
        synchronizeKosync();
    showKosyncChoice();
    // Reporting a page to Nickel's reading session asks Nickel for its
    // database, the account and the book. On a page turn that waits until
    // the new page is painted.
    const bool turnFrame = !session->busy() && session->turnDirection() != 0
            && session->image().cacheKey() != page->image.cacheKey();
    if (!turnFrame || book->usesLocalPace())
        observeReading();
    // A sync can call into the host's network; on a turn it starts after the
    // new page is painted, in the refresh that paintTurn() requests.
    if (!turnFrame)
        syncKosyncWhileReading();
    if (!turnFrame)
        turnPainted = false;
    if (!darkReady)
        return;
    // syncFrameState consumes the completed jump and its return history.
    // Refresh the destination's text, strips and bookmark ribbon together.
    const bool passageFrame =
            !session->busy() && (!pendingPassage.position.isEmpty() || returningFromLink);
    syncFrameState();
    finishAppearanceApply();
    // A synced position waits while a panel or dialog is open. Check again
    // once the reader is free, including after an apply above closes Settings.
    if (progressCheckDeferred && !progressBlocked())
        progressTick();
    // The KOsync choice was checked above, before an apply could close
    // Settings. Without this second check it waits for the next refresh.
    showKosyncChoice();
    const bool pickingFont = syncFontPicker();
    showChapterText();
    // Publish only the final viewport. A cancelled intermediate layout must
    // not replace the placeholder while the window is still being resized.
    if (resizePending && !resizeTimer.isActive() && !session->busy()
        && (!appearanceEdit.preview() || !appearanceEdit.preview()->busy()))
        page->setViewportTransition(false);
    const auto nextImage = session->image();
    const auto previousPageImage = page->image.cacheKey();
    showPageImage(nextImage);
    syncLinkDiscovery();
    bookmarkList->setExcerptFont(bookFace());
    syncAnnotations();
    // The panel needs far longer to draw a page than this function needs
    // for the rest of its work. On a plain page turn, paint now and let the
    // controls, the chapter graph and the saved position follow.
    const bool readingTurn = !resizePending && isVisible() && !appearanceEdit.panelOpen()
            && !topControls->isVisible() && !general->isVisible() && !choices->isVisible()
            && !syncDialog && !kosyncDialog && !finishDialog && !services.suspended()
            && !applyingText && !session->busy() && session->turnDirection() != 0
            && nextImage.cacheKey() != previousPageImage;
    if (readingTurn) {
        paintTurn();
        return;
    }
    syncAppearanceControls(pickingFont);
    syncReadingControls();
    // Jumps and layout changes join the activity batch. Layout memory is
    // saved separately because it is a user setting.
    saveSoon();
    // Wait for the selected preview page before clearing the dimmed frame.
    // Settings can also close the menu later than the tap, after applying.
    if ((flashAfterMenu || flashAfterPagePreview) && (!flashAfterPagePreview || !session->busy())
        && isVisible() && !appearanceEdit.panelOpen()
        && (flashAfterPagePreview || !topControls->isVisible()) && !general->isVisible()
        && !choices->isVisible()) {
        const bool preview = std::exchange(flashAfterPagePreview, false);
        flashAfterMenu = false;
        requestFlash(preview ? "page-preview" : "menu");
        page->update();
    }
    // Panels invalidate their own pixels. Repaint the reading area only
    // when its content changes, including a completed typography preview.
    if (!readingTurn)
        page->refreshContent(isVisible() && !appearanceEdit.panelOpen() && !topControls->isVisible()
                             && !general->isVisible() && !choices->isVisible() && !syncDialog
                             && !kosyncDialog && !finishDialog && !services.suspended()
                             && !applyingText && (session->turnDirection() != 0 || passageFrame)
                             && nextImage.cacheKey() != previousPageImage);
    if (!recordedOpening && pagePainted)
        recordOpening();
    publishResize();
    if (appearanceEdit.finishPending() && appearanceEdit.ready()) {
        resumeAppearanceApply();
        return;
    }
    if (exitPending) {
        finishExit();
        return;
    }
    openPendingDestination();
    syncNavigation();
    finishAnnotationPresentation();
}

bool Reader::publishPendingDarkMode()
{
    if (pendingDarkMode < 0)
        return true;
    // Main and preview frames use one worker but arrive separately.
    // Publish the new page and UI palette together, never a mixed pair.
    if (session->busy() || (appearanceEdit.preview() && appearanceEdit.preview()->busy()))
        return false;
    const bool target = std::exchange(pendingDarkMode, -1);
    if (session->state().value("dark_mode").toBool(false) != target
        || (appearanceEdit.preview()
            && appearanceEdit.preview()->state().value("dark_mode").toBool(false) != target)) {
        settingsError->setText(strings::get("reader.could_not_change_dark_mode"));
        settingsError->show();
        return false;
    }
    return true;
}

void Reader::syncFrameState()
{
    syncBookmarks();
    if (!session->busy() && !pendingSearchHit.start.isEmpty()) {
        const auto hit = std::exchange(pendingSearchHit, {});
        const auto epoch = searchEpoch;
        const auto imageKey = session->image().cacheKey();
        QPointer<Reader> guard(this);
        session->rangeRects(hit.start, hit.end, [guard, epoch, imageKey](QVector<QRect> rects) {
            if (guard && guard->searchEpoch == epoch && !guard->lifecycleHidden
                && guard->page->image.cacheKey() == imageKey)
                guard->page->mark(rects);
        });
    }
    // A preview has a different frame from the book and cannot start a
    // lookup. Do not cancel its panel-dismiss gesture when that frame updates.
    // A new frame, such as a loaded image, must not look up the old text.
    // It must not drop a tap or swipe in progress either.
    if (!appearanceEdit.panelOpen()
        && (session->busy() || session->image().cacheKey() != page->image.cacheKey()))
        dismissLookup(true);
    if (!session->busy() && !pendingPassage.position.isEmpty()) {
        // Accept the visit only after the engine confirms a different passage.
        const auto origin = std::exchange(pendingPassage, {});
        if (session->error().isEmpty()
            && session->state().value("position").toString() != origin.position) {
            // One return point covers a chain of pinned reference shortcuts.
            // Ordinary links can still nest above it and return normally.
            if (!origin.pinnedShortcut || linkHistory.isEmpty()
                || !linkHistory.last().pinnedShortcut)
                linkHistory.append(origin);
        }
    }
    if (returningFromLink && !session->busy()) {
        returningFromLink = false;
        if (!linkHistory.isEmpty()
            && session->state().value("position").toString() == linkHistory.last().position)
            linkHistory.removeLast();
    }
    applyTheme(session->state().value("dark_mode").toBool(false));
}

void Reader::finishAppearanceApply()
{
    if (appearanceEdit.applying() && !session->busy()) {
        const bool switchingAppearance = appearanceEdit.appearanceDestination();
        const bool returnToReading = appearanceEdit.destination() == AppearanceDestination::Reading;
        const bool successful = session->error().isEmpty();
        appearanceEdit.completeApply(successful, exitPending);
        if (successful) {
            book->rememberAppearance();
            // Keep Settings covering the page until the destination preview
            // is ready. Showing the reading bars here adds a full refresh.
            if (!switchingAppearance) {
                choices->hide();
                menu->hide();
                general->hide();
                if (returnToReading) {
                    topControls->hide();
                    bottomControls->hide();
                } else
                    showControls();
            }
            if (!appearanceEdit.panelDestination())
                appearanceEdit.clearDestination();
        }
    }
}

bool Reader::syncFontPicker()
{
    const bool pickingFont = choosingTypography && choices->isVisible() && choices->isFontPicker();
    if (choosingTypography && appearanceEdit.panelOpen() && panels->currentIndex() == 0)
        menu->setVisible(!pickingFont && !general->isVisible());
    return pickingFont;
}

// Fills the header and footer slots. An error or image warning takes the
// footer's place until it clears.
void Reader::showChapterText()
{
    page->cleanRefresh(false);
    page->notice = !session->error().isEmpty() ? session->error() : session->imageWarning();
    const auto values = chromeValues();
    for (int slot = 0; slot < kobalt::headerfooter::slotCount; ++slot)
        page->slotContents[slot] = kobalt::headerfooter::content(
                page->chrome.slotFields[slot], values, page->chrome.uppercase[slot]);
    const auto text = page->headerText();
    header->setText(text.isEmpty() ? values.chapter : text);
    header->setAccessibleName(header->text());
}

void Reader::showPageImage(const QImage &nextImage)
{
    page->setPageAnimationStyle(services.softwarePageAnimations ? pageTurn::selected(preferences)
                                                                : pageTurn::Style::Off);
    auto displayedImage = nextImage;
    QPointer<ReaderSession> displayedSource = session;
    if (appearanceEdit.edited() && appearanceEdit.ready() && appearanceEdit.preview()) {
        const auto *preview = appearanceEdit.preview();
        // Opening a panel keeps the current book frame. Use the isolated
        // preview only after appearance or viewport dimensions change.
        if (session->busy() || !appearance::same(preview->state(), session->state())
            || preview->image().size() != nextImage.size()) {
            displayedImage = preview->image();
            displayedSource = appearanceEdit.preview();
        }
    }
    if (displayedImage.cacheKey() != page->image.cacheKey()) {
        // Temporary search and link marks belong to the old page geometry.
        // Clear them without cancelling the gesture that edits the preview.
        page->mark({});
        page->animationDirection = isVisible() && !appearanceEdit.panelOpen()
                        && !topControls->isVisible() && !general->isVisible()
                        && !linkReturn->isVisible() && !syncDialog && !kosyncDialog && !finishDialog
                        && !services.suspended()
                        && (services.softwarePageAnimations
                                    ? pageTurn::selected(preferences) != pageTurn::Style::Off
                                    : preferences.value("page_animations").toBool(false))
                        && pageAnimationsAvailable(services.animationSupport())
                ? session->turnDirection()
                : 0;
    }
    if (!page->viewportTransition()) {
        const bool changed = displayedImage.cacheKey() != page->image.cacheKey();
        page->image = std::move(displayedImage);
        if (changed) {
            page->setReflowText({});
            const auto key = page->image.cacheKey();
            const auto source = displayedSource;
            // Prepare the sample only after the page is visible. Engine access
            // stays on the worker thread and never delays this frame's paint.
            ui::afterNextPaint(page, [this, source, key] {
                if (!source || source->busy() || page->image.cacheKey() != key
                    || page->viewportTransition() || lifecycleHidden || closing)
                    return;
                const QPointer<Reader> guard(this);
                source->pageSummary(source->page(), [guard, key](QString, QString text) {
                    if (guard && guard->page->image.cacheKey() == key
                        && !guard->page->viewportTransition())
                        guard->page->setReflowText(text);
                });
            });
        }
    }
    // Only a turn forward counts towards the session's pace; jumps and reflows do not.
    if (!session->busy())
        book->pace().observe(session->page(), session->turnDirection() > 0);
}

void Reader::paintTurn()
{
    // render_ms ends here: the engine's page and the GUI thread's wait for it.
    // paint_ms covers the damage, the paint and the flush that follow.
    const double rendered = turnStarted.isValid() ? turnStarted.nsecsElapsed() / 1e6 : 0;
    QElapsedTimer painting;
    if (turnStarted.isValid())
        painting.start();
    page->progressFraction = pageProgressFraction(session->page(), session->pages());
    page->preparedDamage = session->damage();
    page->preparedFrom = session->damageFrom();
    // The Refresh the screen setting decides which turns flash.
    ++pagesSinceFlash;
    const bool chapterStart = session->turnDirection() > 0 && session->chapterPage() == 1;
    const auto mode = kobalt::screenRefresh(preferences.value("screen_refresh").toString());
    if (kobalt::refreshOnTurn(mode, pagesSinceFlash, chapterStart))
        requestFlash(mode == kobalt::ScreenRefresh::Smart || mode == kobalt::ScreenRefresh::Chapter
                             ? "chapter"
                             : "pages");
    if (kobalt::diagnosticsActive())
        kobalt::diagnostic(
                "page-paint-start",
                { { "render_id", double(session->renderId()) }, { "page", session->page() } },
                false);
    page->refreshContent(true, true);
    if (turnStarted.isValid()) {
        kobalt::diagnostic("page-turn-painted",
                           { { "render_id", double(session->renderId()) },
                             { "page", session->page() },
                             { "elapsed_ms", turnStarted.nsecsElapsed() / 1e6 },
                             { "render_ms", rendered },
                             { "paint_ms", painting.nsecsElapsed() / 1e6 } },
                           false);
        turnStarted.invalidate();
    }
    // The screen composes the flushed page from the event loop. Return
    // to it now; the second pass finds the page unchanged and does the rest.
    turnPainted = true;
    QTimer::singleShot(0, this, [this] { refresh(); });
}

void Reader::syncAppearanceControls(bool pickingFont)
{
    auto *settings =
            appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : session;
    previewLabel->setText(appearanceEdit.error());
    previewLabel->setVisible(!pickingFont && !appearanceEdit.error().isEmpty());
    if (appearanceEdit.previewState() == AppearanceEditSession::PreviewState::Opening) {
        return;
    }
    if (!sizeSlider->interacting()) {
        const int halfPoints = kobalt::pixelHalfPoints(settings->fontSize());
        sizeSlider->setValue(halfPoints);
        findChild<QLabel *>("rrTextSizeValue")
                ->setText(strings::get("reader.text_size_value")
                                  .arg(QString::number(halfPoints / 2.0, 'f', halfPoints % 2)));
    }
    const auto optical = settings->opticalSizeAxis();
    ui::controlRowOf(opticalSizeSlider)->setVisible(optical.available());
    if (optical.available() && !opticalSizeSlider->interacting()) {
        opticalSizeSlider->setRange(qCeil(optical.minimum), qFloor(optical.maximum));
        QVector<int> stops{ opticalSizeSlider->minimum() };
        for (const int size : { 6, 8, 10, 12, 14, 18, 24, 36, 48, 72, 96, 144 }) {
            if (size > opticalSizeSlider->minimum() && size < opticalSizeSlider->maximum()) {
                stops.append(size);
            }
        }
        stops.append(opticalSizeSlider->maximum());
        opticalSizeSlider->setMarkers(stops, ui::SettingSlider::MarkerMode::Stops);
        opticalSizeSlider->setValue(qRound(settings->opticalSize()));
        const auto caption = strings::get("reader.text_size_value")
                                     .arg(QString::number(opticalSizeSlider->value()));
        findChild<QLabel *>("rrOpticalSizeValue")->setText(caption);
    }
    const auto bionic = settings->state().value("bold_reading").toString("off");
    const bool normalWeight = bionic == "off";
    const auto weightAxis = settings->weightAxis();
    // The selected-font button follows the displayed page's appearance.
    const ui::SampleAppearance sampleAppearance{
        normalWeight ? settings->state().value("weight_steps").toInt(settings->thickness()) : 0,
        bionic
    };
    choices->setFontAppearance(sampleAppearance);
    fontButton->setAppearance(sampleAppearance);
    for (const auto *name : { "rrFontWeight", "rrLighter", "rrHeavier", "rrVariableFontWeight",
                              "rrVariableLighter", "rrVariableHeavier" })
        if (auto *widget = findChild<QWidget *>(name))
            widget->setEnabled(normalWeight);
    ui::controlRowOf(variableWeightSlider)->setVisible(weightAxis.available());
    // Navigation places this panel above the bottom bar. Moving it below
    // the toolbar here would invalidate it before navigation moves it back.
    fitAppearance();
    if (!weightSlider->interacting()) {
        weightSlider->setValue(normalWeight ? settings->thickness() : 0);
        findChild<QLabel *>("rrFontWeightValue")
                ->setText(normalWeight
                                  ? strings::get("reader.weight_value").arg(settings->thickness())
                                  : strings::get("reader.bionic_weight_locked"));
    }
    if (weightAxis.available() && !variableWeightSlider->interacting()) {
        variableWeightSlider->setRange(weightAxis.minimum, weightAxis.maximum);
        QVector<int> stops{ weightAxis.minimum };
        for (const int weight : { 100, 200, 300, 400, 450, 500, 600, 700, 800, 900 }) {
            if (weight > weightAxis.minimum && weight < weightAxis.maximum) {
                stops.append(weight);
            }
        }
        stops.append(weightAxis.maximum);
        variableWeightSlider->setMarkers(stops, ui::SettingSlider::MarkerMode::Stops);
        variableWeightSlider->setPageStep(50);
        variableWeightSlider->setValue(normalWeight ? settings->weight() : weightAxis.defaultValue);
        findChild<QLabel *>("rrVariableFontWeightValue")
                ->setText(normalWeight ? QString::number(settings->weight())
                                       : strings::get("reader.bionic_weight_locked"));
    }
    if (!spacingSlider->interacting()) {
        spacingSlider->setValue(settings->spacing());
        findChild<QLabel *>("rrLineSpacingValue")
                ->setText(strings::get("common.percent").arg(settings->spacing()));
    }
    if (!marginSlider->interacting()) {
        marginSlider->setValue(kobalt::marginDesign(settings->margin()));
        findChild<QLabel *>("rrMarginsValue")
                ->setText(QString("%1").arg(kobalt::marginDesign(settings->margin())));
    }
    if (!verticalMarginSlider->interacting()) {
        verticalMarginSlider->setValue(kobalt::marginDesign(settings->topMargin()));
        findChild<QLabel *>("rrVerticalMarginsValue")
                ->setText(QString::number(kobalt::marginDesign(settings->topMargin())));
    }
    fontButton->setFace(settings->fontFace());
    for (auto *button : hyphenButtons)
        button->setChecked(button->property("hyphenation").toString() == settings->hyphenation());
    for (auto *button : alignmentButtons)
        button->setChecked(button->property("alignment").toString() == settings->alignment());
}

void Reader::syncReadingControls()
{
    if (session->busy())
        pageScrubber->cancel();
    QVector<QPair<int, QString>> chapters;
    for (const auto &chapter : session->chapters())
        chapters.append({ chapter.page, chapter.title });
    pageScrubber->setChapterJumps(pageNavigation::barButtons(preferences)
                                  == pageNavigation::BarButtons::Chapters);
    pageScrubber->setBook(session->page(), session->pages(), chapters, session->image().size());
    pageScrubber->setEnabled(!session->busy());
    const auto chapter = session->chapterTitle().isEmpty() ? strings::get("reader.contents")
                                                           : session->chapterTitle();
    chapterButton->setText(chapter);
    chapterButton->setAccessibleName(strings::get("reader.contents_detail").arg(chapter));
    page->progressFraction = pageProgressFraction(session->page(), session->pages());
    if (!session->busy())
        applyingText = false;
    refreshRenderNotice();
    findChild<QPushButton *>("rrLight")->setEnabled(services.lightAvailable() && !session->busy()
                                                    && !appearanceEdit.applying());
    findChild<QPushButton *>("rrLight")->setChecked(lightPopup->isVisible()
                                                    || appearanceEdit.destination()
                                                            == AppearanceDestination::Light);
    findChild<QPushButton *>("rrTypography")->setEnabled(!session->busy());
    findChild<QPushButton *>("rrLayout")->setEnabled(!session->busy());
    // Settings applies the newest draft, or waits for the preview to open.
    // Its click handler ignores taps while a panel's changes apply.
    findChild<QPushButton *>("rrSettings")
            ->setEnabled(!session->busy() || appearanceEdit.applying());
    findChild<QPushButton *>("rrTypography")
            ->setChecked(!general->isVisible() && appearanceEdit.panelOpen()
                         && panels->currentIndex() == 0);
    findChild<QPushButton *>("rrLayout")
            ->setChecked(!general->isVisible() && appearanceEdit.panelOpen()
                         && panels->currentIndex()
                                 != int(kobalt::reader::AppearancePage::Typography));
    findChild<QPushButton *>("rrSettings")->setChecked(general->isVisible());
    const bool settingsOpen = general->isVisible();
    for (auto *action : std::initializer_list<QWidget *>{
                 toolbarComposition.exitAction, toolbarComposition.settings,
                 toolbarComposition.bookmarks, toolbarComposition.annotate,
                 toolbarComposition.search })
        if (action)
            action->setVisible(!settingsOpen);
    toolbarComposition.formattingWarning->setVisible(!settingsOpen && session->formattingWarning());
    findChild<QPushButton *>("rrGraph")->setEnabled(!session->busy() && !appearanceEdit.applying());
    findChild<QPushButton *>("rrGraph")->setChecked(chapterGraph->isVisible()
                                                    || appearanceEdit.destination()
                                                            == AppearanceDestination::Stats);
    if (chapterGraph->isVisible())
        updateGraph();
    updateOrientationBlock();
    for (auto *button : rotationButtons) {
        button->setChecked(!orientation->automatic()
                           && button->property("orientation").toInt()
                                   == int(orientation->manual()));
    }
    if (autoRotateOption) {
        autoRotateOption->setChecked(orientation->automatic());
    }
    updateLinkReturn();
}

void Reader::publishResize()
{
    if (resizePending
        && (session->busy() || (appearanceEdit.preview() && appearanceEdit.preview()->busy()))) {
        kobalt::diagnostic(
                "reader-resize-wait",
                { { "width", width() },
                  { "height", height() },
                  { "main_busy", session->busy() },
                  { "preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy() },
                  { "updates_enabled", updatesEnabled() } },
                false);
    }
    if (resizePending && !resizeTimer.isActive() && !session->busy()
        && (!appearanceEdit.edited() || !appearanceEdit.preview()->busy())) {
        // refreshContent already queued the reflowed page. Do not repaint it
        // again after that flush, which would add a second refresh.
        resizePending = false;
        kobalt::diagnostic(
                "reader-resize-end",
                { { "width", width() },
                  { "height", height() },
                  { "main_busy", session->busy() },
                  { "preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy() },
                  { "updates_enabled", updatesEnabled() },
                  { "resize_pending", resizePending } });
    }
}

void Reader::openPendingDestination()
{
    if (appearanceEdit.panelDestination() && !session->busy() && !appearanceEdit.applying()) {
        if (appearanceEdit.externalDestination()) {
            if (!appearanceEdit.panelOpen() && !general->isVisible()) {
                const auto destination = appearanceEdit.takeDestination();
                if (!lifecycleHidden && !services.suspended() && !exitPending) {
                    if (destination == AppearanceDestination::Stats)
                        openGraph();
                    else if (destination == AppearanceDestination::Chapters)
                        showChapters();
                    else if (destination == AppearanceDestination::Seek)
                        openSeek();
                    else if (destination == AppearanceDestination::Search)
                        openSearch();
                    else if (destination == AppearanceDestination::Bookmarks)
                        openBookmarks();
                    else
                        lightPopup->open();
                }
            }
        } else
            openPanel(appearanceEdit.panelIndex());
    }
}

// Stats uses the shared source count on every platform. A failed count keeps
// the chapter navigation and the existing page-length fallback available.
void Reader::updateGraph()
{
    const bool counted = session->totalWords() >= 0;
    const auto graph = counted ? session->chapterGraph()
                               : kobalt::chapterPageLengths(session->chapters(), session->pages());
    chapterGraph->setData(graph, counted ? session->graphSelection() : session->contentsSelection(),
                          true, {});
    const auto contents = session->chapters();
    const int next = session->contentsSelection().next;
    int nextPages = 0;
    if (next >= 0 && next < contents.size()) {
        const int start = contents[next].page;
        int end = session->pages();
        for (const auto &chapter : contents)
            if (chapter.page > start)
                end = qMin(end, chapter.page);
        nextPages = qMax(1, end - start);
    }
    chapterGraph->setChapterNumber(session->contentsSelection().current + 1, contents.size());
    updateReadingStats();
    chapterGraph->setProgress(session->chapterTitle(), session->chapterPage(),
                              session->chapterPages(), nextPages, session->page(),
                              session->pages());
}

void Reader::updateMenuStatus()
{
    const auto time = services.clock();
    const bool clockChanged = menuClock->text() != time;
    if (clockChanged)
        menuClock->setText(time);
    const auto battery = services.battery();
    menuBattery->setStatus(battery.percent, battery.charging);
    const auto reading = book->sessionReadingMilliseconds();
    const auto session = reading > 300000
            ? strings::get("reader.session_minutes").arg(reading / 60000)
            : QString();
    if (menuSession->text() != session)
        menuSession->setText(session);
    menuSession->setVisible(!hideNativeStatus() && !session.isEmpty());
    if (clockChanged && navigation && topControls->isVisible())
        navigation->updateSessionGeometry();
    if (chapterGraph && chapterGraph->isVisible())
        updateReadingStats();
}

// Stats and the reading strips use the same platform or session estimates.
void Reader::updateReadingStats()
{
    const auto reading = book->sessionReadingMilliseconds();
    const auto estimates = book->estimates();
    chapterGraph->setReading(int(reading / 60000), session->totalWords(),
                             estimates.chapterMilliseconds, estimates.bookMilliseconds);
}

} // namespace kobalt::reader
