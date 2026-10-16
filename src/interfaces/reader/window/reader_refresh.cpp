#include "interfaces/reader/ui/controls/text.h"
#include "reader_p.h"
#include "reading/appearance_settings.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include <QtMath>
#include <utility>

namespace kobalt::reader {
namespace strings = kobalt::strings;

void Reader::refreshRenderNotice() {
    if (!busyNotice)
        return;
    renderNoticeTimer.stop();
    const auto progress = model->renderProgress();
    const bool active = progress.active && !closing && !lifecycleHidden;
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
    const QRect progressRect =
        showNotice && !property("reader_dark").toBool() && page->image.text("kobalt.animation_text_only") == "true"
            ? busyNotice->progressUpdateRect(percent, text)
            : QRect();
    setProperty("reader_progress_update", progressRect);
    busyNotice->setDelayedProgress(active, progress.elapsedMilliseconds, percent, text);
    if (active && !showNotice) {
        renderNoticeTimer.start(int(qMax<qint64>(1, 1000 - progress.elapsedMilliseconds)));
    }
}

void Reader::refresh() {
    if (lifecycleHidden)
        return;
    // Reporting a page to Nickel's reading session asks Nickel for its
    // database, the account and the book. On a page turn that waits until
    // the new page is painted.
    const bool turnFrame =
        !model->busy() && model->turnDirection() != 0 && model->image().cacheKey() != page->image.cacheKey();
    if (!turnFrame)
        observeReading();
    const bool afterTurn = !turnFrame && std::exchange(turnPainted, false);
    if (!publishPendingDarkMode())
        return;
    // syncFrameState consumes the completed jump and its return history.
    // Refresh the destination's text, strips and bookmark ribbon together.
    const bool passageFrame = !model->busy() && (!pendingPassage.position.isEmpty() || returningFromLink);
    syncFrameState();
    finishAppearanceApply();
    const bool pickingFont = syncFontPicker();
    showChapterText();
    const auto nextImage = model->image();
    const auto previousPageImage = page->image.cacheKey();
    showPageImage(nextImage);
    syncLinkDiscovery();
    // The panel needs far longer to draw a page than this function needs
    // for the rest of its work. On a plain page turn, paint now and let the
    // controls, the chapter graph and the saved position follow.
    const bool readingTurn = isVisible() && !appearanceEdit.panelOpen() && !topControls->isVisible() &&
                             !general->isVisible() && !choices->isVisible() && !syncDialog && !finishDialog &&
                             !services.suspended() && !applyingText && !model->busy() && model->turnDirection() != 0 &&
                             nextImage.cacheKey() != previousPageImage;
    if (readingTurn) {
        paintTurn();
        return;
    }
    syncAppearanceControls(pickingFont);
    syncReadingControls();
    if (afterTurn)
        saveSoon();
    else
        save();
    // Returning to the page from the menu flashes once to clear ghosting.
    // The menu can close later than the tap, after Settings apply.
    if (flashAfterMenu && isVisible() && !appearanceEdit.panelOpen() && !topControls->isVisible() &&
        !general->isVisible() && !choices->isVisible()) {
        flashAfterMenu = false;
        requestFlash("menu");
        page->update();
    }
    // Panels invalidate their own pixels. Repaint the reading area only
    // when its content changes, including a completed typography preview.
    if (!readingTurn)
        page->refreshContent(isVisible() && !appearanceEdit.panelOpen() && !topControls->isVisible() &&
                             !general->isVisible() && !choices->isVisible() && !syncDialog && !finishDialog &&
                             !services.suspended() && !applyingText && (model->turnDirection() != 0 || passageFrame) &&
                             nextImage.cacheKey() != previousPageImage);
    if (!recordedOpening)
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
}

bool Reader::publishPendingDarkMode() {
    if (pendingDarkMode < 0)
        return true;
    // Main and preview frames use one worker but arrive separately.
    // Publish the new page and UI palette together, never a mixed pair.
    if (model->busy() || (appearanceEdit.preview() && appearanceEdit.preview()->busy()))
        return false;
    const bool target = std::exchange(pendingDarkMode, -1);
    if (model->state().value("dark_mode").toBool(false) != target ||
        (appearanceEdit.preview() && appearanceEdit.preview()->state().value("dark_mode").toBool(false) != target)) {
        settingsError->setText(strings::get("reader.could_not_change_dark_mode"));
        settingsError->show();
        return false;
    }
    return true;
}

void Reader::syncFrameState() {
    syncBookmarks();
    if (!model->busy() && !pendingSearchHit.start.isEmpty()) {
        const auto hit = std::exchange(pendingSearchHit, {});
        const auto epoch = searchEpoch;
        QPointer<Reader> guard(this);
        model->rangeRects(hit.start, hit.end, [guard, epoch](QVector<QRect> rects) {
            if (guard && guard->searchEpoch == epoch && !guard->lifecycleHidden)
                guard->page->mark(rects);
        });
    }
    // A preview has a different frame from the book and cannot start a
    // lookup. Do not cancel its panel-dismiss gesture when that frame updates.
    // A new frame, such as a loaded image, must not look up the old text.
    // It must not drop a tap or swipe in progress either.
    if (!appearanceEdit.panelOpen() && (model->busy() || model->image().cacheKey() != page->image.cacheKey()))
        dismissLookup(true);
    if (!model->busy() && !pendingPassage.position.isEmpty()) {
        // Accept the visit only after the engine confirms a different passage.
        const auto origin = std::exchange(pendingPassage, {});
        if (model->error().isEmpty() && model->state().value("position").toString() != origin.position) {
            linkHistory.append(origin);
        }
    }
    if (returningFromLink && !model->busy()) {
        returningFromLink = false;
        if (!linkHistory.isEmpty() && model->state().value("position").toString() == linkHistory.last().position)
            linkHistory.removeLast();
    }
    applyTheme(model->state().value("dark_mode").toBool(false));
}

void Reader::finishAppearanceApply() {
    if (appearanceEdit.applying() && !model->busy()) {
        const bool switchingAppearance = appearanceEdit.appearanceDestination();
        const bool returnToReading = appearanceEdit.destination() == AppearanceDestination::Reading;
        const bool successful = model->error().isEmpty();
        appearanceEdit.completeApply(successful, exitPending);
        if (successful) {
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

bool Reader::syncFontPicker() {
    const bool pickingFont = choosingTypography && choices->isVisible() && choices->isFontPicker();
    if (choosingTypography && appearanceEdit.panelOpen() && panels->currentIndex() == 0)
        menu->setVisible(!pickingFont && !general->isVisible());
    return pickingFont;
}

// Fills the header and footer slots. An error or image warning takes the
// footer's place until it clears.
void Reader::showChapterText() {
    page->cleanRefresh(false);
    page->notice = !model->error().isEmpty() ? model->error() : model->imageWarning();
    const auto values = chromeValues();
    for (int slot = 0; slot < kobalt::headerfooter::slotCount; ++slot)
        page->slotContents[slot] =
            kobalt::headerfooter::content(page->chrome.slotFields[slot], values, page->chrome.uppercase[slot]);
    const auto text = page->headerText();
    header->setText(text.isEmpty() ? values.chapter : text);
    header->setAccessibleName(header->text());
}

void Reader::showPageImage(const QImage &nextImage) {
    if (nextImage.cacheKey() != page->image.cacheKey()) {
        page->animationDirection =
            isVisible() && !appearanceEdit.panelOpen() && !topControls->isVisible() && !general->isVisible() &&
                    !linkReturn->isVisible() && !syncDialog && !finishDialog && !services.suspended() &&
                    preferences.value("page_animations").toBool(false) && services.animationsAvailable()
                ? model->turnDirection()
                : 0;
    }
    page->image = nextImage;
    if (appearanceEdit.edited() && appearanceEdit.ready() && appearanceEdit.preview()) {
        const auto *preview = appearanceEdit.preview();
        // Opening a panel keeps the current book frame. Use the isolated
        // preview only after appearance or viewport dimensions change.
        if (model->busy() || !appearance::same(preview->state(), model->state()) ||
            preview->image().size() != nextImage.size())
            page->image = preview->image();
    }
    // Only a turn forward counts towards the session's pace; jumps and reflows do not.
    if (!model->busy())
        book->pace().observe(model->page(), model->turnDirection() > 0);
}

void Reader::paintTurn() {
    page->progressFraction =
        model->pages() > 0 ? qBound(qreal(0), qreal(model->page() + 1) / model->pages(), qreal(1)) : 0;
    page->preparedDamage = model->damage();
    page->preparedFrom = model->damageFrom();
    // The Refresh the screen setting decides which turns flash.
    ++pagesSinceFlash;
    const bool chapterStart = model->turnDirection() > 0 && model->chapterPage() == 1;
    const auto mode = kobalt::screenRefresh(preferences.value("screen_refresh").toString());
    if (kobalt::refreshOnTurn(mode, pagesSinceFlash, chapterStart))
        requestFlash(mode == kobalt::ScreenRefresh::Smart || mode == kobalt::ScreenRefresh::Chapter ? "chapter"
                                                                                                    : "pages");
    page->refreshContent(true, true);
    if (turnStarted.isValid()) {
        kobalt::diagnostic("page-turn-painted", {{"elapsed_ms", double(turnStarted.elapsed())}}, false);
        turnStarted.invalidate();
    }
    // The screen composes the flushed page from the event loop. Return
    // to it now; the second pass finds the page unchanged and does the rest.
    turnPainted = true;
    QTimer::singleShot(0, this, [this] {
        refresh();
    });
}

void Reader::syncAppearanceControls(bool pickingFont) {
    auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
    previewLabel->setText(appearanceEdit.error());
    previewLabel->setVisible(!pickingFont && !appearanceEdit.error().isEmpty());
    if (appearanceEdit.previewState() == AppearanceEditSession::PreviewState::Opening) {
        return;
    }
    if (!sizeSlider->interacting()) {
        const int halfPoints = kobalt::pixelHalfPoints(settings->fontSize());
        sizeSlider->setValue(halfPoints);
        findChild<QLabel *>("rrTextSizeValue")
            ->setText(
                strings::get("reader.text_size_value").arg(QString::number(halfPoints / 2.0, 'f', halfPoints % 2)));
    }
    const auto optical = settings->opticalSizeAxis();
    opticalSizeSlider->parentWidget()->setVisible(optical.available());
    if (optical.available() && !opticalSizeSlider->interacting()) {
        opticalSizeSlider->setRange(qCeil(optical.minimum), qFloor(optical.maximum));
        QVector<int> stops{opticalSizeSlider->minimum()};
        for (const int size : {6, 8, 10, 12, 14, 18, 24, 36, 48, 72, 96, 144}) {
            if (size > opticalSizeSlider->minimum() && size < opticalSizeSlider->maximum()) {
                stops.append(size);
            }
        }
        stops.append(opticalSizeSlider->maximum());
        opticalSizeSlider->setMarkers(stops, ui::SettingSlider::MarkerMode::Stops);
        opticalSizeSlider->setValue(qRound(settings->opticalSize()));
        const auto caption = strings::get("reader.text_size_value").arg(QString::number(opticalSizeSlider->value()));
        findChild<QLabel *>("rrOpticalSizeValue")->setText(caption);
    }
    const auto bionic = settings->state().value("bold_reading").toString("off");
    const bool normalWeight = bionic == "off";
    const auto weightAxis = settings->weightAxis();
    // The font samples follow the displayed page, so a thickness change
    // updates them in the same screen update as the book text.
    const ui::SampleAppearance sampleAppearance{
        normalWeight ? settings->state().value("weight_steps").toInt(settings->thickness()) : 0, bionic};
    choices->setFontAppearance(sampleAppearance);
    fontButton->setAppearance(sampleAppearance);
    for (const auto *name :
         {"rrFontWeight", "rrLighter", "rrHeavier", "rrVariableFontWeight", "rrVariableLighter", "rrVariableHeavier"})
        if (auto *widget = findChild<QWidget *>(name))
            widget->setEnabled(normalWeight);
    variableWeightSlider->parentWidget()->setVisible(weightAxis.available());
    // Navigation places this panel above the bottom bar. Moving it below
    // the toolbar here would invalidate it before navigation moves it back.
    fitAppearance();
    if (!weightSlider->interacting()) {
        weightSlider->setValue(normalWeight ? settings->thickness() : 0);
        findChild<QLabel *>("rrFontWeightValue")
            ->setText(normalWeight ? strings::get("reader.weight_value").arg(settings->thickness())
                                   : strings::get("reader.bionic_weight_locked"));
    }
    if (weightAxis.available() && !variableWeightSlider->interacting()) {
        variableWeightSlider->setRange(weightAxis.minimum, weightAxis.maximum);
        QVector<int> stops{weightAxis.minimum};
        for (const int weight : {100, 200, 300, 400, 450, 500, 600, 700, 800, 900}) {
            if (weight > weightAxis.minimum && weight < weightAxis.maximum) {
                stops.append(weight);
            }
        }
        stops.append(weightAxis.maximum);
        variableWeightSlider->setMarkers(stops, ui::SettingSlider::MarkerMode::Stops);
        variableWeightSlider->setPageStep(50);
        variableWeightSlider->setValue(normalWeight ? settings->weight() : weightAxis.defaultValue);
        findChild<QLabel *>("rrVariableFontWeightValue")
            ->setText(normalWeight ? QString::number(settings->weight()) : strings::get("reader.bionic_weight_locked"));
    }
    if (!spacingSlider->interacting()) {
        spacingSlider->setValue(settings->spacing());
        findChild<QLabel *>("rrLineSpacingValue")->setText(strings::get("common.percent").arg(settings->spacing()));
    }
    if (!marginSlider->interacting()) {
        marginSlider->setValue(kobalt::marginDesign(settings->margin()));
        findChild<QLabel *>("rrMarginsValue")->setText(QString("%1").arg(kobalt::marginDesign(settings->margin())));
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

void Reader::syncReadingControls() {
    if (!bookSlider->interacting()) {
        bookSlider->setRange(0, qMax(0, model->pages() - 1));
        QVector<int> chapterStarts;
        for (const auto &chapter : model->chapters())
            chapterStarts.append(chapter.page);
        bookSlider->setMarkers(chapterStarts);
        bookSlider->setValue(model->page());
        progressLabel->setText(!model->error().isEmpty() ? model->error()
                               : !model->imageWarning().isEmpty()
                                   ? model->imageWarning()
                                   : strings::get("reader.menu_page_of").arg(model->page() + 1).arg(model->pages()));
    }
    bookSlider->setEnabled(!model->busy() && model->pages() > 1);
    const auto chapter = model->chapterTitle().isEmpty() ? strings::get("reader.contents") : model->chapterTitle();
    chapterButton->setText(chapter);
    chapterButton->setAccessibleName(strings::get("reader.contents_detail").arg(chapter));
    page->progressFraction =
        model->pages() > 0 ? qBound(qreal(0), qreal(model->page() + 1) / model->pages(), qreal(1)) : 0;
    if (!model->busy())
        applyingText = false;
    refreshRenderNotice();
    findChild<QPushButton *>("rrLight")->setEnabled(services.lightAvailable() && !model->busy() &&
                                                    !appearanceEdit.applying());
    findChild<QPushButton *>("rrLight")->setChecked(lightPopup->isVisible() ||
                                                    appearanceEdit.destination() == AppearanceDestination::Light);
    findChild<QPushButton *>("rrTypography")->setEnabled(!model->busy());
    findChild<QPushButton *>("rrLayout")->setEnabled(!model->busy());
    // Settings applies the newest draft, or waits for the preview to open.
    // Its click handler ignores taps while a panel's changes apply.
    findChild<QPushButton *>("rrSettings")->setEnabled(!model->busy() || appearanceEdit.applying());
    previousPage->setEnabled(model->canTurn() && model->page() > 0);
    nextPage->setEnabled(model->canTurn() && model->page() + 1 < model->pages());
    findChild<QPushButton *>("rrTypography")
        ->setChecked(!general->isVisible() && appearanceEdit.panelOpen() && panels->currentIndex() == 0);
    findChild<QPushButton *>("rrLayout")
        ->setChecked(!general->isVisible() && appearanceEdit.panelOpen() &&
                     panels->currentIndex() != kobalt::reader::TypographyPage);
    findChild<QPushButton *>("rrSettings")->setChecked(general->isVisible());
    findChild<QPushButton *>("rrGraph")->setEnabled(!model->busy() && !appearanceEdit.applying());
    findChild<QPushButton *>("rrGraph")->setChecked(chapterGraph->isVisible() ||
                                                    appearanceEdit.destination() == AppearanceDestination::Stats);
    updateGraph();
    updateOrientationBlock();
    for (auto *button : rotationButtons) {
        button->setChecked(!orientation->automatic() &&
                           button->property("orientation").toInt() == int(orientation->manual()));
    }
    if (autoRotateOption) {
        autoRotateOption->setChecked(orientation->automatic());
    }
    updateLinkReturn();
}

void Reader::publishResize() {
    if (resizePending && (model->busy() || (appearanceEdit.preview() && appearanceEdit.preview()->busy()))) {
        kobalt::diagnostic("reader-resize-wait",
                           {{"width", width()},
                            {"height", height()},
                            {"main_busy", model->busy()},
                            {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                            {"updates_enabled", updatesEnabled()}},
                           false);
    }
    if (resizePending && !model->busy() && (!appearanceEdit.edited() || !appearanceEdit.preview()->busy())) {
        // refreshContent already queued the reflowed page. Do not repaint it
        // again after that flush, which would add a second refresh.
        resizePending = false;
        kobalt::diagnostic("reader-resize-end",
                           {{"width", width()},
                            {"height", height()},
                            {"main_busy", model->busy()},
                            {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                            {"updates_enabled", updatesEnabled()},
                            {"resize_pending", resizePending}});
    }
}

void Reader::openPendingDestination() {
    if (appearanceEdit.panelDestination() && !model->busy() && !appearanceEdit.applying()) {
        if (appearanceEdit.externalDestination()) {
            if (!appearanceEdit.panelOpen() && !general->isVisible()) {
                const auto destination = appearanceEdit.takeDestination();
                if (!lifecycleHidden && !services.suspended() && !exitPending) {
                    if (destination == AppearanceDestination::Stats)
                        openGraph();
                    else if (destination == AppearanceDestination::Chapters)
                        showChapters();
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
void Reader::updateGraph() {
    const bool counted = model->totalWords() >= 0;
    const auto graph = counted ? model->chapterGraph() : kobalt::chapterPageLengths(model->chapters(), model->pages());
    chapterGraph->setData(graph, counted ? model->graphSelection() : model->contentsSelection(), true, {});
    const auto contents = model->chapters();
    const int next = model->contentsSelection().next;
    int nextPages = 0;
    if (next >= 0 && next < contents.size()) {
        const int start = contents[next].page;
        int end = model->pages();
        for (const auto &chapter : contents)
            if (chapter.page > start)
                end = qMin(end, chapter.page);
        nextPages = qMax(1, end - start);
    }
    chapterGraph->setChapterNumber(model->contentsSelection().current + 1, contents.size());
    updateReadingStats();
    chapterGraph->setProgress(model->chapterTitle(), model->chapterPage(), model->chapterPages(), nextPages,
                              model->page(), model->pages());
}

void Reader::updateMenuStatus() {
    const auto time = services.clock();
    if (menuClock->text() != time)
        menuClock->setText(time);
    const auto battery = services.battery();
    menuBattery->setStatus(battery.percent, battery.charging);
    const auto reading = book->readingMilliseconds();
    // With Stats always shown, its Session cell already gives the reading time.
    const auto session =
        reading > 300000 && !statsPinned() ? strings::get("reader.session_minutes").arg(reading / 60000) : QString();
    if (menuSession->text() != session)
        menuSession->setText(session);
    menuSession->setVisible(!hideNativeStatus() && !session.isEmpty());
    if (chapterGraph && chapterGraph->isVisible())
        updateReadingStats();
}

// Stats and the reading strips use the same platform or session estimates.
void Reader::updateReadingStats() {
    const auto reading = book->readingMilliseconds();
    const auto estimates = book->estimates();
    chapterGraph->setReading(int(reading / 60000), model->totalWords(), estimates.chapterMilliseconds,
                             estimates.bookMilliseconds);
}

}
