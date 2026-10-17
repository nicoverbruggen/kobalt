#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/style/theme.h"
#include "reader_p.h"
#include "support/strings.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "platform/api/kobo_refresh.h"
#include "reading/appearance_settings.h"
#include "support/diagnostics.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "storage/reader_storage.h"
#include "interfaces/reader/settings/settings_panel.h"
#include "font_registration.h"
#include "assets/bundled_fonts.h"
#include <QFontDatabase>

namespace kobalt::reader {
namespace strings = kobalt::strings;
using namespace kobalt::storage;
namespace m = kobalt::ui::metrics;
using kobalt::ui::readerStyle;
using kobalt::ui::ControlIcon;
using kobalt::ui::buttonIcon;
using kobalt::platform::textRefresh;

kobalt::headerfooter::Values Reader::chromeValues() {
    kobalt::headerfooter::Values values;
    values.title = model->title();
    values.author = model->author();
    values.chapter = model->chapterTitle().isEmpty() ? strings::get("reader.reading") : model->chapterTitle();
    values.time = services.clock();
    values.chapterPage = model->chapterPage();
    values.chapterPages = model->chapterPages();
    values.page = model->page() + 1;
    values.pages = model->pages();
    const auto battery = services.battery();
    values.battery = battery.percent;
    values.charging = battery.charging;
    values.deviceStatus = !hideNativeStatus();
    const auto estimates = book->estimates();
    values.chapterMilliseconds = estimates.chapterMilliseconds;
    values.bookMilliseconds = estimates.bookMilliseconds;
    values.learningPace = estimates.learningPace;
    return values;
}

kobalt::headerfooter::Layout Reader::customChrome() const {
    // Until Custom has a layout of its own, it starts from the layout in force.
    return kobalt::headerfooter::customLayout(preferences, page->chrome);
}

bool Reader::customModified() const {
    using namespace kobalt::headerfooter;
    const auto saved = preferences.value(customKey);
    return saved.isObject() &&
           fromJson(saved.toObject(), presetLayout(Preset::Default)) != presetLayout(Preset::Default);
}

bool Reader::chromeTicks() const {
    using kobalt::headerfooter::Field;
    for (const auto field : page->chrome.slotFields)
        if (field == Field::Time || field == Field::Battery || field == Field::BatteryPercent ||
            field == Field::BatteryWarning || field == Field::ChapterTimeLeft || field == Field::BookTimeLeft)
            return true;
    return false;
}

void Reader::loadChrome() {
    const auto chrome = kobalt::headerfooter::select(preferences);
    chromePreset = chrome.preset;
    const int top = page->topHeight(), bottom = page->bottomHeight();
    page->chrome = chrome.layout;
    showChapterText();
    syncChromeControls();
    if (top == page->topHeight() && bottom == page->bottomHeight())
        return;
    // The strips changed height, so the book gets a new viewport. With a
    // panel open the preview takes it, and closing the panel applies it.
    const QSize viewport(width(), height() - page->topHeight() - page->bottomHeight());
    if (appearanceEdit.panelOpen())
        mutateAppearance([viewport](ReaderSession &preview) {
            preview.resize(viewport);
        });
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
}

bool Reader::saveChrome(kobalt::headerfooter::Preset preset, const kobalt::headerfooter::Layout *custom) {
    auto next = preferences;
    next[kobalt::headerfooter::presetKey] = kobalt::headerfooter::presetValue(preset);
    if (custom)
        next[kobalt::headerfooter::customKey] = kobalt::headerfooter::toJson(*custom);
    if (next != preferences && !writePreferences(next)) {
        syncChromeControls();
        appearanceEdit.setError(strings::get("reader.could_not_save_reader_settings"));
        refresh();
        return false;
    }
    preferences = next;
    appearanceEdit.clearError();
    loadChrome();
    refresh();
    return true;
}

void Reader::syncChromeControls() {
    using namespace kobalt::headerfooter;
    const auto values = chromeValues();
    const auto contents = [&values](const Layout &layout) {
        kobalt::reader::SlotContents result;
        for (int slot = 0; slot < slotCount; ++slot)
            result[slot] = content(layout.slotFields[slot], values, layout.uppercase[slot]);
        return result;
    };
    // The view shows and edits the layout in force.
    const auto &current = page->chrome;
    slotMap->setSlots(current, contents(current));
    const auto custom = customChrome();
    const bool modified = customModified();
    for (auto *card : presetCards) {
        card->setLayout(card->preset() == Preset::Custom ? custom : presetLayout(card->preset()));
        card->setModified(card->preset() == Preset::Custom && modified);
        card->setChecked(card->preset() == chromePreset);
    }
    for (auto *button : headerSizeButtons)
        button->setChecked(button->property("text_size").toString() == current.headerSize);
    for (auto *button : footerSizeButtons)
        button->setChecked(button->property("text_size").toString() == current.footerSize);
    for (auto *button : progressBarButtons)
        button->setChecked(button->property("switch_on").toBool() == current.progressBar);
    // A modified Custom layout carries a star, as on its card.
    const bool starred = chromePreset == Preset::Custom && modified;
    headerFooterRow->setText(starred ? strings::get("header_footer.preset.modified").arg(presetLabel(chromePreset))
                                     : presetLabel(chromePreset));
    headerFooterRow->setAccessibleName(
        strings::get("header_footer.slot_value")
            .arg(strings::get("header_footer.title"),
                 starred ? strings::get("header_footer.preset.modified_name").arg(presetLabel(chromePreset))
                         : presetLabel(chromePreset)));
    if (fieldChooser->isVisible())
        fieldChooser->open(fieldChooser->slot(), current.slotFields[fieldChooser->slot()], values,
                           current.uppercase[fieldChooser->slot()]);
}

QWidget *Reader::chromeView() const {
    if (fieldChooser && fieldChooser->isVisible())
        return fieldChooser;
    if (headerFooterView && headerFooterView->isVisible())
        return headerFooterView;
    return nullptr;
}

void Reader::openHeaderFooterView() {
    if (model->temporaryTypography() || !menu->isVisible())
        return;
    fieldChooser->hide();
    syncChromeControls();
    headerFooterView->show();
    headerFooterView->raise();
    refresh();
}

void Reader::closeChromeViews() {
    if (!chromeView())
        return;
    fieldChooser->hide();
    headerFooterView->hide();
    refresh();
}

void Reader::openFieldChooser(int slot) {
    if (model->temporaryTypography() || !menu->isVisible())
        return;
    const auto &current = page->chrome;
    fieldChooser->open(slot, current.slotFields[slot], chromeValues(), current.uppercase[slot]);
    headerFooterView->hide();
    fieldChooser->show();
    fieldChooser->raise();
    refresh();
}

// A choice or the close cell returns to Header and footer.
void Reader::closeFieldChooser() {
    if (!fieldChooser->isVisible())
        return;
    fieldChooser->hide();
    headerFooterView->show();
    headerFooterView->raise();
    refresh();
}

void Reader::refreshHostPresentation() {
    static_cast<reader::SettingsPanel *>(general)->showDictionaries(
        services.downloadableDictionaries && !(services.devicePreview && services.devicePreview()));
    updateMenuStatus();
    resizeEvent(nullptr);
    refresh();
}

bool Reader::hideNativeStatus() const {
    return services.desktopUi && !(services.devicePreview && services.devicePreview()) &&
           preferences.value("native_hide_status_bar").toBool(true);
}

void Reader::openSettings() {
    if (model->temporaryTypography())
        return;
    model->cancelPendingTurn();
    if (services.desktopUi) {
        hideStatusOption->setChecked(preferences.value("native_hide_status_bar").toBool(true));
        hideDeviceOptionsOption->setChecked(preferences.value("native_hide_device_options").toBool(true));
    }
    hideRevealedLinks(false);
    dismissLookup();
    closeSearch();
    lightPopup->hide();
    chapterGraph->hide();
    bookmarkList->hide();
    acknowledgments->hide();
    appearanceEdit.clearContinuation();
    restoringDefaults = false;
    settingsPages->setCurrentIndex(DisplaySettings);
    resetDefaults->setText(strings::get("reader.restore_appearance_defaults"));
    resetDefaults->setEnabled(true);
    // Keep the typography draft until Settings is applied.
    menu->hide();
    const bool showingTopControls = !topControls->isVisible();
    topControls->show();
    if (showingTopControls)
        topControls->raise();
    bottomControls->hide();
    choices->hide();
    settingsError->clear();
    settingsError->hide();
    darkOption->setChecked(model->state().value("dark_mode").toBool(false));
    inheritTextLayoutOption->setChecked(preferences.value(appearance::inheritTextLayoutKey).toBool(true));
    hideNonlinearOption->setChecked(model->state().value("hide_nonlinear_fragments").toBool(false));
    animationOption->setChecked(preferences.value("page_animations").toBool(false));
    bionicOption->setChecked(model->state().value("bold_reading").toString("off") == "bold");
    linkIndicatorOption->setChecked(preferences.value("show_link_indicator").toBool(false));
    edgeLightOption->setChecked(preferences.value("edge_light_gestures").toBool(true));
    denseGraphOption->setChecked(preferences.value("dense_stats_chart").toBool(false));
    pinStatsOption->setChecked(preferences.value("pin_stats").toBool(false));
    inheritHeaderFooterOption->setChecked(preferences.value(appearance::inheritHeaderFooterKey).toBool(true));
    showSleepScreen();
    showPageNavigation();
    showTapZones();
    if (services.automaticBookOpening)
        automaticOpeningOption->setChecked(services.automaticBookOpening());
    screenRefreshMode = kobalt::screenRefresh(preferences.value("screen_refresh").toString());
    showScreenRefresh();
    fitSettings();
    general->show();
    general->raise();
    refresh();
}

void Reader::fitSettings() {
    const int top = navigation ? navigation->toolbarHeight() : int(m::panel::toolbarHeight);
    const int bottom = navigation ? navigation->contentBottom(size()) : height() - int(m::panel::footerHeight);
    const int panelHeight = static_cast<SettingsPanel *>(general)->fitContent(QSize(width(), bottom - top));
    general->setGeometry(0, top, width(), panelHeight);
}

void Reader::fitAppearance() {
    const int top = navigation ? navigation->toolbarHeight() : int(m::panel::toolbarHeight);
    const int bottom = navigation ? navigation->contentBottom(size()) : height();
    const int height = menu->fitHeight(width(), bottom - top);
    menu->setGeometry(panelBounds(height));
}

bool Reader::writePreferences(const QJsonObject &next) {
    auto global = appearance::globalPreferences(next);
    appearance::rememberTextLayout(global, model->state());
    const auto previous = storage::readJson(book->paths().preferences());
    if (!writeJson(book->paths().preferences(), global)) {
        showSettingsWriteError();
        return false;
    }
    if (book->saveLayoutPreferences(next))
        return true;
    writeJson(book->paths().preferences(), previous);
    showSettingsWriteError();
    return false;
}

void Reader::applySettings() {
    if (model->temporaryTypography())
        return;
    // Leaving an unanswered confirmation must not select or save defaults.
    static_cast<SettingsPanel *>(general)->dismissRestore();
    // Edits made before the preview opened have no session yet. Apply them
    // together with these settings once the preview holds them.
    if (appearanceEdit.queued() && appearanceEdit.previewState() == AppearanceEditSession::PreviewState::Opening) {
        appearanceEdit.deferApply();
        return;
    }
    auto next = preferences;
    next["bold_reading"] = bionicOption->isChecked() ? QStringLiteral("bold") : QStringLiteral("off");
    next["dark_mode"] = darkOption->isChecked();
    next[appearance::inheritTextLayoutKey] = inheritTextLayoutOption->isChecked();
    next["hide_nonlinear_fragments"] = hideNonlinearOption->isChecked();
    next["page_animations"] = animationOption->isChecked();
    next["show_link_indicator"] = linkIndicatorOption->isChecked();
    next["edge_light_gestures"] = edgeLightOption->isChecked();
    next["dense_stats_chart"] = denseGraphOption->isChecked();
    next["pin_stats"] = pinStatsOption->isChecked();
    next[appearance::inheritHeaderFooterKey] = inheritHeaderFooterOption->isChecked();
    next["screen_refresh"] = kobalt::screenRefreshValue(screenRefreshMode);
    if (services.desktopUi) {
        next["native_hide_status_bar"] = hideStatusOption->isChecked();
        next["native_hide_device_options"] = hideDeviceOptionsOption->isChecked();
    }
    if (restoringDefaults)
        next[kobalt::headerfooter::presetKey] =
            kobalt::headerfooter::presetValue(kobalt::headerfooter::Preset::Default);
    const auto rotation = orientation->settings();
    for (auto it = rotation.begin(); it != rotation.end(); ++it)
        next[it.key()] = it.value();
    if (!writePreferences(next)) {
        bionicOption->setChecked(model->requestedState().value("bold_reading").toString("off") == "bold");
        settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
        settingsError->show();
        appearanceEdit.cancelNavigation();
        refresh();
        return;
    }
    preferences = next;
    navigation->setStatusVisible(!hideNativeStatus());
    const auto chrome = kobalt::headerfooter::select(preferences);
    chromePreset = chrome.preset;
    page->chrome = chrome.layout;
    syncChromeControls();
    applyEdgeLight();
    chapterGraph->setDense(preferences.value("dense_stats_chart").toBool(false));
    pinStats = preferences.value("pin_stats").toBool(false);
    auto settings =
        appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview()->requestedState() : model->state();
    settings["dark_mode"] = darkOption->isChecked();
    settings["hide_nonlinear_fragments"] = hideNonlinearOption->isChecked();
    settings["bold_reading"] = bionicOption->isChecked() ? QStringLiteral("bold") : QStringLiteral("off");
    if (restoringDefaults) {
        const auto defaults = kobalt::appearance::typographyDefaults();
        for (auto it = defaults.begin(); it != defaults.end(); ++it)
            settings[it.key()] = it.value();
    }
    model->apply(settings, QSize(width(), height() - page->topHeight() - page->bottomHeight()));
    applyingText = model->busy();
    if (appearanceEdit.destination() == AppearanceDestination::None)
        appearanceEdit.request(AppearanceDestination::Controls);
    appearanceEdit.beginApply(!model->busy() && appearanceEdit.edited() && appearanceEdit.ready()
                                  ? kobalt::reader::AppearanceEditSession::Retention::Reuse
                                  : kobalt::reader::AppearanceEditSession::Retention::Release);
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
    refresh();
}

void Reader::applyDarkMode(bool enabled) {
    if (model->temporaryTypography())
        return;
    auto next = preferences;
    next["dark_mode"] = enabled;
    if (!writePreferences(next)) {
        darkOption->setChecked(preferences.value("dark_mode").toBool(false));
        settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
        settingsError->show();
        return;
    }
    preferences = next;
    settingsError->clear();
    settingsError->hide();
    pendingDarkMode = enabled;
    model->setDarkMode(enabled);
    if (appearanceEdit.preview())
        appearanceEdit.preview()->setDarkMode(enabled);
    refresh();
}

void Reader::applyBoldReading(bool enabled) {
    if (model->temporaryTypography())
        return;
    const auto mode = enabled ? QStringLiteral("bold") : QStringLiteral("off");
    auto next = preferences;
    next["bold_reading"] = mode;
    if (!writePreferences(next)) {
        bionicOption->setChecked(model->requestedState().value("bold_reading").toString("off") == "bold");
        return;
    }
    preferences = next;
    settingsError->clear();
    settingsError->hide();
    auto settings = model->requestedState();
    settings["bold_reading"] = mode;
    model->apply(settings);
    // Keep a pending typography draft in the same mode, including a preview
    // that has not finished opening yet.
    if (appearanceEdit.preview()) {
        mutateAppearance([mode](ReaderSession &preview) {
            auto settings = preview.requestedState();
            settings["bold_reading"] = mode;
            preview.apply(settings);
        });
    }
    refresh();
}

void Reader::applyAutomaticOpening(bool enabled) {
    if (!services.automaticBookOpening || !services.setAutomaticBookOpening)
        return;
    const bool saved = services.setAutomaticBookOpening(enabled);
    automaticOpeningOption->setChecked(services.automaticBookOpening());
    if (!saved) {
        settingsError->setText(strings::get("native_ui.could_not_save_reader_preference"));
        settingsError->show();
        return;
    }
    settingsError->clear();
    settingsError->hide();
}

void Reader::applyTheme(bool dark) {
    if (themeReady && darkTheme == dark)
        return;
    themeReady = true;
    darkTheme = dark;
    const QColor paper = dark ? Qt::black : Qt::white;
    const QColor ink = dark ? Qt::white : Qt::black;
    QPalette colors = palette();
    for (const auto role : {QPalette::Window, QPalette::Base, QPalette::Button, QPalette::HighlightedText})
        colors.setColor(role, paper);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::Highlight})
        colors.setColor(role, ink);
    colors.setColor(QPalette::Mid, QColor("#999999"));
    colors.setColor(QPalette::Midlight, dark ? QColor("#333333") : QColor("#dddddd"));
    // Theme this window only. Nickel keeps its own palette when we close.
    // Set the palette, then the style sheet, so the sheet's own background
    // and text colors reach widgets that paint from their palette. Setting a
    // style sheet takes the gray roles from the platform; no sheet sets them,
    // so restore the theme's rule and pressed grays afterwards.
    const auto applyColors = [&colors](QWidget *widget) {
        widget->setPalette(colors);
        if (widget->property("reader_style").isValid())
            readerStyle(widget, widget->property("reader_style").toString());
        auto styled = widget->palette();
        for (const auto role : {QPalette::Mid, QPalette::Midlight})
            for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
                styled.setColor(group, role, colors.color(group, role));
        widget->setPalette(styled);
    };
    setProperty("reader_dark", dark);
    applyColors(this);
    textRefresh(this);
    for (auto *widget : findChildren<QWidget *>()) {
        applyColors(widget);
        textRefresh(widget);
        if (auto *button = qobject_cast<QPushButton *>(widget))
            kobalt::ui::refreshButtonIcon(button);
    }
    update();
}

// The face the book is displayed in, registered with Qt. Publisher fonts use
// the book's own face when Qt can load it, and the default otherwise.
QString Reader::bookFace() {
    QString face = model->fontFace();
    if (face.isEmpty())
        face = model->state().value("text_font_face").toString();
    bool loadable = false;
    for (const auto &font : model->fontCatalog())
        loadable |= font.family == face;
    if (!loadable)
        face = QString::fromLatin1(kobalt::defaultFontFace);
    registerPickerFonts(face);
    return face;
}

void Reader::registerPickerFonts(const QString &family) {
    const auto registerFont = [this](const QString &path, bool copyForQt, const QString &family) {
        // Qt 5.2 loads another copy when registering a reading font. Its
        // private preview face is already loaded, so keep samples on that path.
        const auto registration =
            ui::registerPrivateInterfaceFont(path, family, QT_VERSION < QT_VERSION_CHECK(5, 3, 0));
        if (registration == ui::PrivateFontRegistration::Native)
            registerPickerFont(path, copyForQt, registeredFontPaths, registeredFonts);
    };
    // Match the engine's source precedence so duplicate sideloads cannot
    // replace a bundled face in Qt's font-name previews.
    // Qt 5.2 on firmware 4 keeps the first face it sees for a weight and
    // slant. A bold face whose PANOSE weight says "medium" is filed under
    // normal weight there, so register each family's regular face first.
    for (const auto &font : model->fontCatalog()) {
        if (!family.isEmpty() && font.family != family)
            continue;
        if (family.isEmpty() && font.origin == FontOrigin::Interface) {
            const auto state = appearanceEdit.edited() && appearanceEdit.ready()
                                   ? appearanceEdit.preview()->requestedState()
                                   : model->state();
            if (font.family != supplementalFontFamily(state.value("language").toString()) &&
                font.family != preferences.value("default_font_face").toString(kobalt::defaultFontFace) &&
                font.family != state.value("font_face").toString())
                continue;
        }
        for (const auto &file : font.files)
            if (file.contains("Regular", Qt::CaseInsensitive))
                registerFont(file, font.copyForQt, font.family);
        for (const auto &file : font.files)
            if (!file.contains("Regular", Qt::CaseInsensitive))
                registerFont(file, font.copyForQt, font.family);
    }
}

void Reader::resizeText() {
    if (!isVisible() || closing)
        return;
    const QSize viewport(width(), qMax(1, height() - page->topHeight() - page->bottomHeight()));
    kobalt::diagnostic("reader-resize-text",
                       {{"viewport_width", viewport.width()},
                        {"viewport_height", viewport.height()},
                        {"target", appearanceEdit.preview() ? "preview" : "main"},
                        {"main_busy", model->busy()},
                        {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                        {"updates_enabled", updatesEnabled()},
                        {"resize_pending", resizePending}},
                       false);
    // The chapter preview is the visible page while editing. Reflow the
    // whole book once when applying the final size and appearance.
    bool resizedPreview = false;
    if (appearanceEdit.panelOpen()) {
        if (viewport != appearanceViewport) {
            mutateAppearance([viewport](ReaderSession &preview) {
                preview.resize(viewport);
            });
            resizedPreview = true;
        }
    } else
        model->resize(viewport);
    applyingText = model->busy();
    if (!resizedPreview)
        refresh();
    kobalt::diagnostic("reader-resize-refresh-return",
                       {{"viewport_width", viewport.width()},
                        {"viewport_height", viewport.height()},
                        {"target", appearanceEdit.preview() ? "preview" : "main"},
                        {"main_busy", model->busy()},
                        {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                        {"updates_enabled", updatesEnabled()},
                        {"resize_pending", resizePending}},
                       false);
}

void Reader::selectPanel(int index) {
    model->cancelPendingTurn();
    lightPopup->hide();
    chapterGraph->hide();
    if (general->isVisible()) {
        appearanceEdit.request(index == 0 ? AppearanceDestination::Text : AppearanceDestination::Layout);
        applySettings();
    } else if (menu->isVisible() &&
               (panels->currentIndex() == index ||
                (index == kobalt::reader::LayoutPage && panels->currentIndex() != kobalt::reader::TypographyPage))) {
        // The Layout tab closes the header and footer pages too.
        finishPreview();
    } else
        openPanel(index);
}

void Reader::openPanel(int index) {
    if (model->temporaryTypography())
        return;
    if (model->busy())
        return;
    model->cancelPendingTurn();
    appearanceEdit.cancelDeferredApply();
    // Only the selected font is drawn here. Load the other faces when the
    // font picker opens, and do not read font files for the Layout panel.
    if (index == 0 && !model->fontFace().isEmpty())
        registerPickerFonts(model->fontFace());
    panels->setCurrentIndex(index);
    if (!menu->isVisible() && !appearanceEdit.preview()) {
        warmAttemptGeneration = 0;
    }
    appearanceEdit.setPanelOpen(true);
    appearanceViewport = QSize(width(), height() - page->topHeight() - page->bottomHeight());
    appearanceEdit.cancelNavigation();
    general->hide();
    choices->hide();
    bottomControls->hide();
    topControls->show();
    // Finish layout while hidden so showing the panel does not invalidate
    // its previous position in the reading area. Include variable-font rows.
    syncAppearanceControls(false);
    if (navigation)
        menu->setGeometry(navigation->panelBounds(size(), menu->height()));
    menu->show();
    if (navigation)
        navigation->stackPanel(menu);
    else
        menu->raise();
    menu->layout()->activate();
    refresh();
}

void Reader::schedulePreviewWarm() {
    if (!appearanceEdit.panelOpen() || !menu->isVisible() || general->isVisible() || appearanceEdit.preview() ||
        model->busy() || lifecycleHidden || exitPending)
        return;
    const quint64 panelGeneration = appearanceEdit.panelGeneration();
    if (warmAttemptGeneration == panelGeneration || warmScheduledGeneration == panelGeneration)
        return;
    warmScheduledGeneration = panelGeneration;
    QTimer::singleShot(0, this, [this, panelGeneration] {
        if (warmScheduledGeneration == panelGeneration)
            warmScheduledGeneration = 0;
        if (appearanceEdit.panelGeneration() != panelGeneration || !appearanceEdit.panelOpen() || !menu->isVisible() ||
            general->isVisible() || appearanceEdit.preview() || model->busy() || lifecycleHidden || exitPending)
            return;
        warmAttemptGeneration = panelGeneration;
        appearanceEdit.open(
            *model, QSize(width(), height() - page->topHeight() - page->bottomHeight()),
            [this] {
                refresh();
            },
            [this] {
                if (!appearanceEdit.edited() && !appearanceEdit.ready()) {
                    appearanceEdit.resetPreview();
                    appearanceEdit.clearError();
                    return;
                }
                if (exitPending || appearanceEdit.finishPending() || appearanceEdit.externalDestination())
                    resumeAppearanceApply();
                else
                    refresh();
            });
    });
}

void Reader::mutateAppearance(std::function<void(ReaderSession &)> operation) {
    appearanceViewport = QSize(width(), height() - page->topHeight() - page->bottomHeight());
    appearanceEdit.mutate(
        *model, QSize(width(), height() - page->topHeight() - page->bottomHeight()),
        [this] {
            refresh();
        },
        [this] {
            if (exitPending || appearanceEdit.finishPending() || appearanceEdit.externalDestination())
                resumeAppearanceApply();
            else
                refresh();
        },
        std::move(operation));
    refresh();
}

void Reader::finishPreview() {
    if (model->temporaryTypography())
        return;
    if (!appearanceEdit.panelOpen())
        return;
    if (appearanceEdit.preview() && !appearanceEdit.edited()) {
        appearanceEdit.resetPreview();
        appearanceEdit.clearError();
    }
    if (appearanceEdit.preview() && !appearanceEdit.ready() && !appearanceEdit.error().isEmpty()) {
        // A failed preview has no settings to apply. Dismiss it without
        // trapping the reader now that panels have no Cancel action.
        orientation->configure(preferences);
        appearanceEdit.resetPreview();
        appearanceEdit.setPanelOpen(false);
        appearanceEdit.cancelDeferredApply();
        choices->hide();
        menu->hide();
        if (appearanceEdit.destination() == AppearanceDestination::Reading) {
            topControls->hide();
            bottomControls->hide();
        } else
            showControls();
        if (!appearanceEdit.panelDestination())
            appearanceEdit.clearDestination();
        refresh();
        return;
    }
    // A rendering preview already holds the newest request. Only edits made
    // before it opened must wait for it.
    if (appearanceEdit.preview() && !appearanceEdit.ready()) {
        appearanceEdit.deferApply();
        return;
    }
    appearanceEdit.cancelDeferredApply();
    auto next = preferences;
    const auto rotation = orientation->settings();
    for (auto it = rotation.begin(); it != rotation.end(); ++it)
        next[it.key()] = it.value();
    if (next != preferences && !writePreferences(next)) {
        appearanceEdit.cancelNavigation();
        appearanceEdit.setError(strings::get("reader.could_not_save_reader_settings"));
        refresh();
        return;
    }
    preferences = next;
    appearanceEdit.clearError();
    const QSize viewport(width(), height() - page->topHeight() - page->bottomHeight());
    if (appearanceEdit.ready())
        model->apply(appearanceEdit.preview()->requestedState(), viewport);
    else if (model->image().size() != viewport)
        model->apply(model->state(), viewport);
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
    if (appearanceEdit.destination() == AppearanceDestination::None)
        appearanceEdit.request(AppearanceDestination::Controls);
    applyingText = model->busy();
    appearanceEdit.beginApply();
    refresh();
}

// Continues a Settings or panel apply that waited for the preview to open.
void Reader::resumeAppearanceApply() {
    if (general->isVisible())
        applySettings();
    else
        finishPreview();
}

// Both edges follow one setting. The right edge needs adjustable warmth.
void Reader::applyEdgeLight() {
    page->brightnessEdge = preferences.value("edge_light_gestures").toBool(true) && services.lightAvailable();
    page->warmthEdge = page->brightnessEdge && warmthSupported;
    if (lightPopup) {
        lightPopup->setEdgeGesturesEnabled(page->brightnessEdge);
    }
}

}
