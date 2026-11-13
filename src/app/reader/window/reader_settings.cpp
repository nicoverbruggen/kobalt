#include <QKeyEvent>
#include "reading/remaining_time.h"
#include "reading/rendering_mode.h"
#include "reading/sleep_screen.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/style/theme.h"
#include "reader_p.h"
#include "ui/style/styles.h"
#include "support/strings.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "platform/api/kobo_refresh.h"
#include "reading/appearance_settings.h"
#include "reading/reading_density.h"
#include "reading/page_navigation.h"
#include "support/diagnostics.h"
#include "ui/style/metrics.h"
#include "ui/panel_timing.h"
#include "storage/reader_storage.h"
#include "app/reader/settings/settings_panel.h"
#include "app/reader/settings/style_illustrations.h"
#include "assets/bundled_fonts.h"
#include <QCoreApplication>
#include <QRegularExpression>
#include <QFontDatabase>

namespace kobalt::reader {
namespace strings = kobalt::strings;
using namespace kobalt::storage;
namespace metrics = kobalt::ui::metrics;
using kobalt::platform::textRefresh;
using kobalt::ui::buttonIcon;
using kobalt::ui::ControlIcon;
using kobalt::ui::readerStyle;

namespace {
// A user changed these reader settings. Each changed key is one
// `setting-changed` event, so a surprising behavior can be traced back to a
// tap. Values are logged for true/false, numbers and short lowercase choice
// tokens such as "bold" or "smart". Other text, such as font names, is logged
// by key only: logs keep no input text. Text the user types, such as the
// sleep greeting, is logged by key only even when it looks like a token. A key the file did not hold yet is
// skipped: the first save of Settings writes every key, mostly defaults the
// reader never touched. Later saves compare every key.
void logSettingChanges(const QJsonObject &previous, const QJsonObject &next, bool defaults = false)
{
    const auto loggable = [](const QJsonValue &value) {
        static const QRegularExpression token(QStringLiteral("^[a-z0-9_-]{1,32}$"));
        return value.isBool() || value.isDouble()
                || (value.isString() && token.match(value.toString()).hasMatch());
    };
    const QStringList typed = { QString::fromLatin1(kobalt::sleep::greetingKey) };
    for (auto it = next.begin(); it != next.end(); ++it) {
        const auto before = previous.value(it.key());
        const auto after = it.value();
        if (before == after)
            continue;
        if (before.isUndefined())
            continue;
        QJsonObject fields;
        fields["setting"] = it.key();
        const bool values = !typed.contains(it.key());
        if (values && loggable(before))
            fields["previous"] = before;
        if (values && loggable(after))
            fields["value"] = after;
        if (defaults)
            fields["restore_defaults"] = true;
        kobalt::diagnostic("setting-changed", fields, kobalt::DiagnosticSeverity::Activity);
    }
}

QStringList fallbackFontDefaults(const QStringList &families)
{
    return { QString(defaultFontFace),
             families.contains(publisherSansFontFace) ? QString(publisherSansFontFace)
                                                      : QString("IBM Plex Sans"),
             QString(monospaceFontFace) };
}
} // namespace

kobalt::headerfooter::Values Reader::chromeValues()
{
    kobalt::headerfooter::Values values;
    values.title = session->title();
    values.author = session->author();
    values.chapter = session->chapterTitle().isEmpty() ? strings::get("reader.reading")
                                                       : session->chapterTitle();
    values.time = services.clock();
    values.chapterPage = session->chapterPage();
    values.chapterPages = session->chapterPages();
    values.page = session->page() + 1;
    values.pages = session->pages();
    // An engine with the text of only a part of the book shows rough page
    // numbers from its page estimate. The chapter is the spine item. Before
    // the estimate has a total, each page field shows its percent form.
    const auto &state = session->state();
    if (state.contains("book_fraction") && state.value("rough_pages").toInt() > 0
        && state.value("rough_item_pages").toInt() > 0) {
        values.page = state.value("rough_page").toInt();
        values.pages = state.value("rough_pages").toInt();
        values.chapterPage = state.value("rough_item_page").toInt();
        values.chapterPages = state.value("rough_item_pages").toInt();
        values.pagesEstimated = state.value("rough_pages_estimated").toBool();
        values.chapterPagesEstimated = state.value("rough_item_pages_estimated").toBool();
    } else if (state.contains("book_fraction")) {
        values.page = values.pages = values.chapterPage = values.chapterPages = 0;
        values.percentRead = session->progressPercent();
        const int itemPages = session->state().value("rough_item_pages").toInt();
        if (itemPages > 0)
            values.chapterPercentRead = pageProgressPercent(
                    session->state().value("rough_item_page").toInt() - 1, itemPages);
    }
    const auto battery = services.battery();
    values.battery = battery.percent;
    values.charging = battery.charging;
    values.deviceStatus = !hideNativeStatus();
    const auto estimates = book->estimates();
    values.chapterMilliseconds = estimates.chapterMilliseconds;
    values.bookMilliseconds = estimates.bookMilliseconds;
    values.learningPace = estimates.learningPace;
    values.approximateTime = remainingTime::approximate(preferences);
    values.decimalPercentages = headerfooter::decimalPercentages(preferences);
    values.pageTurnMilliseconds = pageTurnMilliseconds;
    if (state.contains("chapter_load_ms"))
        values.chapterLoadMilliseconds = qint64(state.value("chapter_load_ms").toDouble());
    return values;
}

kobalt::headerfooter::Layout Reader::customChrome() const
{
    // Until Custom has a layout of its own, it starts from the layout in force.
    return kobalt::headerfooter::customLayout(preferences, page->chrome);
}

bool Reader::customModified() const
{
    using namespace kobalt::headerfooter;
    const auto saved = preferences.value(customKey);
    const auto standard = presetLayout(Preset::Default);
    const auto custom = fromJson(saved.toObject(), standard);
    return saved.isObject()
            && (custom.slotFields != standard.slotFields || custom.uppercase != standard.uppercase
                || custom.progressBar != standard.progressBar);
}

bool Reader::chromeTicks() const
{
    using kobalt::headerfooter::Field;
    for (const auto field : page->chrome.slotFields)
        if (field == Field::Time || field == Field::Battery || field == Field::BatteryPercent
            || field == Field::BatteryPercentOnly || field == Field::BatteryWarning
            || field == Field::ChapterTimeLeft || field == Field::BookTimeLeft)
            return true;
    return false;
}

void Reader::loadChrome()
{
    const auto chrome = kobalt::headerfooter::select(preferences, services.phoneLayout);
    chromePreset = chrome.preset;
    const int top = page->topHeight(), bottom = page->bottomHeight();
    page->chrome = chrome.layout;
    showChapterText();
    syncChromeControls();
    if (top == page->topHeight() && bottom == page->bottomHeight())
        return;
    // The strips changed height, so the book gets a new viewport. With a
    // panel open the preview takes it, and closing the panel applies it.
    const QSize viewport = bodySize();
    if (appearanceEdit.panelOpen())
        mutateAppearance([viewport](ReaderSession &preview) { preview.resize(viewport); });
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
}

bool Reader::saveChrome(kobalt::headerfooter::Preset preset,
                        const kobalt::headerfooter::Layout *custom,
                        const kobalt::headerfooter::Layout *text)
{
    auto next = preferences;
    if (text || !next.value(kobalt::headerfooter::textKey).isObject())
        next[kobalt::headerfooter::textKey] =
                kobalt::headerfooter::textSettings(text ? *text : page->chrome);
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

void Reader::syncChromeControls()
{
    using namespace kobalt::headerfooter;
    // A changed field can start or end the need for the minute clock, and a
    // time-left field needs the reading pace.
    if (clockTimer && isVisible() && !services.suspended()) {
        startClock();
        if (paceNeeded())
            book->minuteTick();
    }
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
        auto configuration =
                card->preset() == Preset::Custom ? custom : presetLayout(card->preset());
        configuration.headerSize = configuration.footerSize = smallSize;
        configuration.headerEnabled = configuration.footerEnabled = true;
        card->setLayout(configuration);
        card->setModified(card->preset() == Preset::Custom && modified);
        card->setChecked(card->preset() == chromePreset);
    }
    headerSizeControl->setSize(current.headerSize, current.headerEnabled);
    footerSizeControl->setSize(current.footerSize, current.footerEnabled);
    for (auto *button : progressBarButtons)
        button->setChecked(button->property("switch_on").toBool() == current.progressBar);
    // A modified Custom layout carries a star, as on its card.
    const bool starred = chromePreset == Preset::Custom && modified;
    headerFooterRow->setText(
            starred ? strings::get("header_footer.preset.modified").arg(presetLabel(chromePreset))
                    : presetLabel(chromePreset));
    headerFooterRow->setAccessibleName(
            strings::get("header_footer.slot_value")
                    .arg(strings::get("header_footer.title"),
                         starred ? strings::get("header_footer.preset.modified_name")
                                           .arg(presetLabel(chromePreset))
                                 : presetLabel(chromePreset)));
    if (fieldChooser->isVisible())
        fieldChooser->open(fieldChooser->slot(), current.slotFields[fieldChooser->slot()], values,
                           current.uppercase[fieldChooser->slot()],
                           fieldChooser->slot() < 3 ? page->headerTextPixels()
                                                    : page->footerTextPixels());
}

QWidget *Reader::chromeView() const
{
    if (fieldChooser && fieldChooser->isVisible())
        return fieldChooser;
    if (headerFooterView && headerFooterView->isVisible())
        return headerFooterView;
    return nullptr;
}

void Reader::openHeaderFooterView()
{
    if (session->temporaryTypography() || !menu->isVisible())
        return;
    fieldChooser->hide();
    syncChromeControls();
    headerFooterView->show();
    headerFooterView->raise();
    refresh();
}

void Reader::closeChromeViews()
{
    if (!chromeView())
        return;
    fieldChooser->hide();
    headerFooterView->hide();
    refresh();
}

void Reader::openFieldChooser(int slot)
{
    if (session->temporaryTypography() || !menu->isVisible())
        return;
    const auto &current = page->chrome;
    fieldChooser->open(slot, current.slotFields[slot], chromeValues(), current.uppercase[slot],
                       slot < 3 ? page->headerTextPixels() : page->footerTextPixels());
    headerFooterView->hide();
    fieldChooser->show();
    fieldChooser->raise();
    refresh();
}

// A choice or the close cell returns to Header and footer.
void Reader::closeFieldChooser()
{
    if (!fieldChooser->isVisible())
        return;
    fieldChooser->hide();
    headerFooterView->show();
    headerFooterView->raise();
    refresh();
}

void Reader::refreshHostPresentation()
{
    if (services.desktopUi) {
        const bool phone = isPhoneScreen(size(), readingPpi.load());
        if (services.phoneLayout != phone) {
            services.phoneLayout = phone;
            for (auto *card : presetCards)
                if (card->preset() == headerfooter::Preset::Phone)
                    card->setVisible(phone);
            hideTabLabelsOption->setVisible(!phone);
            loadChrome();
            if (choices->isVisible() && choosingTypography)
                showFontPicker();
        }
    }
    auto *settings = static_cast<reader::SettingsPanel *>(general);
    const bool devicePreview = services.devicePreview && services.devicePreview();
    settings->showDictionaries(services.downloadableDictionaries && !devicePreview);
    settings->showPowerManagement(wifiPowerSaverVisible());
    updateKosyncFeatures();
    updateMenuStatus();
    resizeEvent(nullptr);
    refresh();
}

bool Reader::hideNativeStatus() const
{
    return services.desktopUi && !(services.devicePreview && services.devicePreview());
}

void Reader::openSettings()
{
    if (session->temporaryTypography())
        return;
    kobalt::ui::PanelTiming timing("settings");
    kosyncPage->setAccount(kosyncClient->server(), kosyncClient->username(),
                           kosyncClient->signedIn(), kosyncClient->automatic());
    kosyncPage->setLastSync(kosyncClient->lastSync());
    kosyncPage->setFilenameMatching(kosyncClient->filenameMatching());
    kosyncPage->setSendMetadata(kosyncClient->sendMetadata());
    kosyncPage->setDeviceName(kosyncClient->customDeviceName(), kosyncClient->defaultDeviceName());
    kosyncPage->setStatus(kosyncMessage, syncCoordinator->busy());
    session->cancelPendingTurn();
    hideRevealedLinks(false);
    dismissLookup();
    closeSearch();
    lightPopup->hide();
    chapterGraph->hide();
    bookmarkList->hide();
    acknowledgments->hide();
    appearanceEdit.clearContinuation();
    restoringDefaults = false;
    restoringPresets = false;
    for (const auto *key : appearance::fallbackFontKeys) {
        const auto family = preferences.value(key).toString();
        settingsFontFaces[key] = session->fontFaces().contains(family) ? family : QString();
    }
    settingsDefaultFontFace = preferences.value("default_font_face").toString(defaultFontFace);
    if (!settingsDefaultFontFace.isEmpty()
        && !session->fontFaces().contains(settingsDefaultFontFace))
        settingsDefaultFontFace = defaultFontFace;
    settingsUiFontFace = preferences.value("ui_font_face").toString();
    settingsUiMonoFontFace = preferences.value("ui_mono_font_face").toString();
    settingsHeaderFooterFontFace = preferences.value("header_footer_font_face").toString();
    settingsDictionaryFontFace = preferences.value("dictionary_font_face").toString();
    settingsDictionaryFontSize = preferences.value("dictionary_font_size").toInt(100);
    settingsHeaderFooterWeight = ui::headerFooterAddedWeight();
    settingsTimeRemainingFormat =
            remainingTime::approximate(preferences) ? "approximate" : "detailed";
    settingsPercentageFormat = headerfooter::decimalPercentages(preferences) ? "decimal" : "whole";
    settingsRenderingMode = renderingMode::value(renderingMode::fromPreferences(preferences));
    hideTabLabelsOption->setChecked(preferences.value("hide_tab_labels").toBool(false));
    saveWordsOption->setChecked(preferences.value("save_looked_up_words").toBool(true));
    zenModeOption->setChecked(preferences.value("zen_mode").toBool(false));
    static_cast<SettingsPanel *>(general)->hideBookOnSleepOption->setChecked(
            book->hideBookOnSleepScreen());
    if (wifiPowerSaverOption)
        wifiPowerSaverOption->setChecked(preferences.value(wifiPowerSaverKey).toBool(false));
    if (disableStandbyOption)
        disableStandbyOption->setChecked(preferences.value(disableReaderStandbyKey).toBool(false));
    for (auto *face : { &settingsUiFontFace, &settingsUiMonoFontFace, &settingsHeaderFooterFontFace,
                        &settingsDictionaryFontFace })
        if (!session->fontFaces().contains(*face))
            face->clear();
    showSettingsFonts();
    settingsPages->setCurrentIndex(int(SettingsPage::Display));
    resetDefaults->setText(strings::get("reader.restore_appearance_defaults"));
    resetDefaults->setEnabled(true);
    auto *resetPresets = static_cast<SettingsPanel *>(general)->resetPresets;
    resetPresets->setText(strings::get("text_presets.restore"));
    resetPresets->setEnabled(true);
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
    darkOption->setChecked(session->state().value("dark_mode").toBool(false));
    static_cast<SettingsPanel *>(general)->openingCoverOption->setChecked(
            !preferences.value("show_opening_cover").toBool(true));
    static_cast<SettingsPanel *>(general)->featurePreferences = preferences;
    static_cast<SettingsPanel *>(general)->updateFeatureOptions();
    showTextPresetSettings();
    inpageFootnotesOption->setChecked(session->state().value("inpage_footnotes").toBool(false));
    hideNonlinearOption->setChecked(
            session->state().value("hide_nonlinear_fragments").toBool(false));
    auto *displaySettings = static_cast<SettingsPanel *>(general);
    settingsVerticalSpacing =
            appearance::verticalSpacingMode(session->state().value("vertical_justification"));
    settingsVerticalSpacingScope =
            appearance::verticalSpacingScope(session->state().value("vertical_spacing_scope"));
    displaySettings->landscapeColumnsOption->setChecked(
            preferences.value("two_columns_landscape").toBool(false));
    settingsColumnSpacing =
            preferences.value("column_spacing_percent").toInt(defaultColumnSpacingPercent);
    displaySettings->columnSpacingButton->setEnabled(
            displaySettings->landscapeColumnsOption->isChecked());
    displaySettings->columnSpacingButton->setCaption(
            SettingsPanel::columnSpacingLabel(settingsColumnSpacing));
    settingsWordSpacing = session->state().value("word_spacing").toInt(100);
    settingsParagraphSpacing = appearance::paragraphSpacing(preferences.value("paragraph_spacing"));
    settingsParagraphIndent = appearance::paragraphIndent(preferences.value("paragraph_indent"));
    settingsBeautifier = appearance::normalizationSettings(preferences);
    settingsBeautifier[appearance::normalizationPresetKey] =
            appearance::normalizationPreset(preferences);
    settingsTracking = session->state().value("justification_tracking").toInt(0);
    settingsHangingPunctuation = session->state().value("hanging_punctuation").toBool(false);
    settingsWidowsOrphans = session->state().value("widows_orphans").toString("publisher");
    showAdvancedText();
    animationOption->setChecked(services.softwarePageAnimations
                                        ? pageTurn::selected(preferences) == pageTurn::Style::Sweep
                                        : preferences.value("page_animations").toBool(false));
    bionicOption->setChecked(session->state().value("bold_reading").toString("off") == "bold");
    edgeLightOption->setChecked(preferences.value("edge_light_gestures").toBool(true));
    lightGestureFeedbackOption->setChecked(
            preferences.value("show_light_gesture_feedback").toBool(true));
    denseGraphOption->setChecked(preferences.value("dense_stats_chart").toBool(false));
    showSleepScreen();
    showPageNavigation();
    showTapZones();
    showTappableLinks();
    minimalAnnotationOption->setChecked(minimalAnnotationMode());
    showNavigationBarButtons();
    showChapterGroups();
    showMenuView();
    if (services.automaticBookOpening)
        automaticOpeningOption->setChecked(services.automaticBookOpening());
    screenRefreshMode = kobalt::screenRefresh(preferences.value("screen_refresh").toString());
    showScreenRefresh();
    static_cast<SettingsPanel *>(general)->setZenLayout(zenMode());
    fitSettings();
    general->show();
    general->raise();
    refresh();
    timing.finish(general);
}

void Reader::showAdvancedText(int changed)
{
    auto *panel = static_cast<SettingsPanel *>(general);
    const auto preset = appearance::normalizationPreset(settingsBeautifier);
    panel->beautifierButton->setCaption(
            strings::get(("settings.normalization_" + preset).toUtf8().constData()).toUpper());
    const QString wordSpacing = settingsWordSpacing == 80 ? "narrow"
            : settingsWordSpacing == 120                  ? "wide"
                                                          : "normal";
    panel->wordSpacingButton->setCaption(
            strings::get(("settings.word_spacing_" + wordSpacing).toUtf8().constData()).toUpper());
    panel->paragraphSpacingButton->setCaption(
            strings::get("settings.paragraph_adjustments_value")
                    .arg(SettingsPanel::paragraphSpacingLabel(settingsParagraphSpacing),
                         SettingsPanel::paragraphSpacingLabel(settingsParagraphIndent))
                    .toUpper());
    const QString tracking = settingsTracking == 15 ? "more"
            : settingsTracking == 5                 ? "subtle"
                                                    : "off";
    panel->trackingButton->setCaption(
            strings::get(("settings.justification_tracking_" + tracking).toUtf8().constData())
                    .toUpper());
    panel->hangingPunctuationButton->setCaption(
            strings::get(settingsHangingPunctuation ? "common.on" : "common.off").toUpper());
    panel->widowsOrphansButton->setCaption(
            strings::get(("settings.widows_orphans_" + settingsWidowsOrphans).toUtf8().constData())
                    .toUpper());
    panel->verticalSpacingButton->setCaption(
            strings::get(
                    ("settings.vertical_spacing_" + settingsVerticalSpacing).toUtf8().constData())
                    .toUpper());
    cancelStylingSamples();
    panel->invalidateStylingSamples(changed);
}

void Reader::cancelStylingSamples()
{
    for (auto &render : stylingSampleRenders)
        if (render)
            render->store(true);
}

int Reader::stylingChoice(int feature) const
{
    using Feature = SettingsPanel::AdvancedText;
    switch (Feature(feature)) {
    case Feature::ParagraphSpacing:
        return 0;
    case Feature::WordSpacing:
        return settingsWordSpacing == 80 ? 0 : settingsWordSpacing == 120 ? 2 : 1;
    case Feature::Tracking:
        return settingsTracking == 15 ? 2 : settingsTracking == 5 ? 1 : 0;
    case Feature::HangingPunctuation:
        return settingsHangingPunctuation ? 1 : 0;
    case Feature::Pagination:
        return settingsWidowsOrphans == "allow" ? 2 : settingsWidowsOrphans == "avoid" ? 1 : 0;
    case Feature::VerticalSpacing:
        return settingsVerticalSpacing == "expand"    ? 2
                : settingsVerticalSpacing == "center" ? 1
                                                      : 0;
    }
    return 0;
}

void Reader::renderStylingSamples(int requested)
{
    auto *panel = static_cast<SettingsPanel *>(general);
    if (!general->isVisible() || panel->settingsPages->currentIndex() != int(SettingsPage::Styling)
        || page->image.isNull())
        return;
    using Feature = SettingsPanel::AdvancedText;
    // The sample uses the book's typography with the Styling choices made so
    // far, which are applied to the book only when Settings closes.
    auto settings = styleSampleSettings(session->state());
    settings["dark_mode"] = darkOption->isChecked();
    settings["word_spacing"] = settingsWordSpacing;
    settings["paragraph_spacing"] = settingsParagraphSpacing;
    settings["paragraph_indent"] = settingsParagraphIndent;
    for (const auto *key : appearance::normalizationKeys)
        settings[key] = settingsBeautifier.value(key);
    settings["justification_tracking"] = settingsTracking;
    settings["hanging_punctuation"] = settingsHangingPunctuation;
    settings["widows_orphans"] = settingsWidowsOrphans;
    settings["vertical_justification"] = settingsVerticalSpacing;
    settings["vertical_spacing_scope"] = settingsVerticalSpacingScope;
    for (const auto *key : appearance::fallbackFontKeys)
        settings[key] = settingsFontFaces.value(key).toString();
    const auto size = page->image.size();
    const int margin = settings.value("margin_pixels").toInt();
    const auto textPage = panel->stylingTextPageSize(size, margin);
    // Crops keep a little paper beside the text.
    const int side = qMin(margin, int(ui::px(16)));
    const int top = settings.value("top_margin_pixels").toInt();
    auto crop = panel->stylingCropSize(textPage, margin);
    crop = QSize(qMin(textPage.width(), crop.width() + 2 * side),
                 qMin(textPage.height() - top, crop.height()));
    const QPoint cropOrigin(margin - side, top);

    const struct
    {
        Feature feature;
        const char *key;
        QVector<QJsonValue> values;
    } requests[] = {
        { Feature::WordSpacing, "word_spacing", { 80, 100, 120 } },
        { Feature::ParagraphSpacing, "paragraph_spacing", { settingsParagraphSpacing } },
        { Feature::Tracking, "justification_tracking", { 0, 5, 15 } },
        { Feature::HangingPunctuation, "hanging_punctuation", { false, true } },
        { Feature::Pagination,
          "widows_orphans",
          { QStringLiteral("publisher"), QStringLiteral("avoid"), QStringLiteral("allow") } },
        { Feature::VerticalSpacing,
          "vertical_justification",
          { QStringLiteral("off"), QStringLiteral("center"), QStringLiteral("expand") } },
    };

    registerPickerFonts(QString::fromLatin1(defaultFontFace));
    for (const auto &entry : requests) {
        const int feature = int(entry.feature);
        if (feature != requested)
            continue;
        const bool spacing = entry.feature == Feature::WordSpacing
                || entry.feature == Feature::Tracking
                || entry.feature == Feature::HangingPunctuation;
        StyleSampleRequest request;
        request.key = entry.key;
        request.values = entry.values;
        const bool vertical = entry.feature == Feature::VerticalSpacing;
        if (spacing) {
            // Spacing depends on the font, so these samples use the book's typography.
            request.settings = settings;
            // Tracking only acts on justified lines, so its sample is justified.
            if (entry.feature == Feature::Tracking)
                request.settings["text_alignment"] = QStringLiteral("justify");
            request.page = textPage;
        } else {
            request.settings = styleSamplePageSettings(settings);
            request.page = styleSamplePageSize();
        }
        if (entry.feature == Feature::ParagraphSpacing) {
            // Use the layout sample's opening paragraphs with room beside
            // the text to see first-line indentation and continuation lines.
            request.settings["font_pixels"] = readingPixels(30);
            request.settings["paragraph_indent"] = settingsParagraphIndent;
            const int margin = readingPixels(96), paper = readingPixels(64);
            request.settings["margin_pixels"] = margin;
            request.cropOrigin = { margin - paper, styleSampleReadingArea().top() };
            request.crop = { request.page.width() - 2 * margin + 2 * paper, readingPixels(640) };
        }
        if (entry.feature == Feature::HangingPunctuation) {
            // A fixed sample includes hyphens and closing quotes at the text edge.
            // Keep both guides and enough paper to show the overhang.
            const qreal ratio = qMax<qreal>(1, panel->devicePixelRatio());
            const auto samplePixels = [ratio](int pixels) {
                return qRound(readingPixels(pixels) * ratio);
            };
            request.settings["text_alignment"] = QStringLiteral("justify");
            request.settings["hyphenation"] = QStringLiteral("on");
            request.settings["font_face"] = QString(defaultFontFace);
            request.settings["font_pixels"] = samplePixels(40);
            request.settings["line_spacing"] = 130;
            request.settings["line_spacing_version"] = 1;
            request.settings["margin_pixels"] = samplePixels(48);
            request.settings["top_margin_pixels"] = 0;
            request.page = { samplePixels(650), samplePixels(1000) };
            request.cropOrigin = { 0, 0 };
            request.crop = { request.page.width(), samplePixels(300) };
            request.marginGuides = true;
            request.cropRatio = ratio;
        }
        request.thumbnail = panel->stylingPageSize(request.page, vertical);
        if (vertical) {
            request.readingArea = styleSampleReadingArea();
            request.detail = panel->stylingEdgeSize(request.page);
        }
        if (spacing && entry.feature != Feature::HangingPunctuation) {
            request.crop = crop;
            request.cropOrigin = cropOrigin;
        }
        if (entry.feature == Feature::Pagination || vertical) {
            panel->setStylingSample(entry.feature, drawStyleIllustration(request));
            continue;
        }
        auto &render = stylingSampleRenders[feature];
        if (render)
            render->store(true);
        // The same request in the same orientation shows its earlier result
        // at once, without the engine.
        const bool landscape = width() > height();
        const auto key = styleSampleKey(session->fontInputs(), request);
        StyleSample cached;
        if (stylingSampleCache.find(landscape, key, &cached)) {
            render.reset();
            panel->setStylingSample(entry.feature, cached);
            continue;
        }
        panel->setStylingSample(entry.feature, {});
        render = request.cancelled = std::make_shared<std::atomic_bool>(false);
        ++stylingSamplesPending;
        renderStyleSample(session->fontInputs(), request, this,
                          [this, panel, entry = entry.feature, cancelled = render, landscape,
                           key](StyleSample sample) {
                              --stylingSamplesPending;
                              if (cancelled->load() || !sample.valid())
                                  return;
                              stylingSampleCache.insert(landscape, key, sample);
                              panel->setStylingSample(entry, sample);
                          });
    }
}

void Reader::fitSettings()
{
    const int top = navigation ? navigation->statusHeight()
                               : int(metrics::panel::toolbarHeight - metrics::chrome::actionHeight);
    const int panelHeight =
            static_cast<SettingsPanel *>(general)->fitContent(QSize(width(), height() - top));
    general->setGeometry(0, top, width(), panelHeight);
}

void Reader::fitAppearance()
{
    const int top = navigation ? navigation->toolbarHeight() : int(metrics::panel::toolbarHeight);
    const int bottom = navigation ? navigation->contentBottom(size()) : height();
    const int height = menu->fitHeight(width(), bottom - top);
    menu->setGeometry(panelBounds(height));
}

pageButtons::Buttons Reader::buttonControls() const
{
    auto buttons = services.buttonControls
            ? pageButtons::held(services.buttonControls(),
                                services.pageButtonsRotated && services.pageButtonsRotated())
            : pageButtons::Buttons{};
    for (auto &button : buttons) {
        QKeyEvent event(QEvent::KeyPress, button.key, Qt::NoModifier);
        const int nativeDirection = services.pageButtonDirection(&event);
        if (nativeDirection)
            button.defaultDirection = nativeDirection;
    }
    return buttons;
}

void Reader::saveButtonControls(const QJsonObject &actions)
{
    if (savingButtonControls || savingDarkMode >= 0 || savingDenseGraph)
        return;
    auto next = appearance::globalPreferences(preferences);
    appearance::rememberTextLayout(next, session->state());
    appearance::rememberPageMargins(next, session->state());
    next[pageButtons::preferenceKey] = actions;
    logSettingChanges(qt::object({ { pageButtons::preferenceKey,
                                     preferences.value(pageButtons::preferenceKey).toObject() } }),
                      qt::object({ { pageButtons::preferenceKey, actions } }));
    savingButtonControls = true;
    static_cast<SettingsPanel *>(general)->buttonControlsButton->setEnabled(false);
    if (!preferencesWriter)
        preferencesWriter = std::make_unique<storage::RecordWriter>();
    QPointer<Reader> guard(this);
    preferencesWriter->post([path = book->paths().preferences(),
                             next] { return storage::writePreferences(path, next); },
                            [guard, actions](bool saved) {
                                qt::invoke(
                                        QCoreApplication::instance(),
                                        [guard, actions, saved] {
                                            if (!guard)
                                                return;
                                            guard->savingButtonControls = false;
                                            saves::settled();
                                            if (saved)
                                                guard->preferences[pageButtons::preferenceKey] =
                                                        actions;
                                            else
                                                guard->showSettingsWriteError();
                                            guard->refresh();
                                        },
                                        Qt::QueuedConnection);
                            });
}

bool Reader::writePreferences(const QJsonObject &next)
{
    // Do not replace a pending asynchronous dark-mode save.
    if (savingButtonControls || savingDarkMode >= 0 || savingDenseGraph)
        return false;
    // Known exception: a settings change waits for its write, so each caller
    // can undo the change when the write fails.
    mainthread::AllowBlocking allow("settings write");
    auto global = appearance::globalPreferences(next);
    appearance::rememberTextLayout(global, session->state());
    appearance::rememberPageMargins(global, session->state());
    // On the writer thread, after any queued layout memory, so neither write
    // loses the other's change.
    if (!preferencesWriter)
        preferencesWriter = std::make_unique<storage::RecordWriter>();
    ++preferencesWrites;
    if (!preferencesWriter->writeNow([path = book->paths().preferences(), global] {
            return kobalt::storage::writePreferences(path, global);
        })) {
        showSettingsWriteError();
        return false;
    }
    logSettingChanges(preferences, next, restoringDefaults);
    return true;
}

void Reader::applySettings()
{
    if (savingButtonControls || savingDarkMode >= 0 || savingDenseGraph
        || session->temporaryTypography())
        return;
    // Leaving an unanswered confirmation must not select or save defaults.
    static_cast<SettingsPanel *>(general)->dismissRestore();
    static_cast<SettingsPanel *>(general)->dismissStandbyConfirmation();
    static_cast<SettingsPanel *>(general)->dismissAutomaticOpeningConfirmation();
    // Edits made before the preview opened have no session yet. Apply them
    // together with these settings once the preview holds them.
    if (appearanceEdit.queued()
        && appearanceEdit.previewState() == AppearanceEditSession::PreviewState::Opening) {
        appearanceEdit.deferApply();
        return;
    }
    auto next = preferences;
    next["default_font_face"] = settingsDefaultFontFace;
    next["ui_font_face"] = settingsUiFontFace;
    next["ui_mono_font_face"] = settingsUiMonoFontFace;
    next["header_footer_font_face"] = settingsHeaderFooterFontFace;
    next["dictionary_font_face"] = settingsDictionaryFontFace;
    next["dictionary_font_size"] = settingsDictionaryFontSize;
    next["header_footer_added_weight"] = settingsHeaderFooterWeight;
    next["hide_tab_labels"] = hideTabLabelsOption->isChecked();
    next["save_looked_up_words"] = saveWordsOption->isChecked();
    next["zen_mode"] = zenModeOption->isChecked();
    if (wifiPowerSaverOption)
        next[wifiPowerSaverKey] = wifiPowerSaverOption->isChecked();
    if (disableStandbyOption)
        next[disableReaderStandbyKey] = disableStandbyOption->isChecked();
    next[remainingTime::preferenceKey] = settingsTimeRemainingFormat;
    next[headerfooter::percentageFormatKey] = settingsPercentageFormat;
    next[renderingMode::preferenceKey] = settingsRenderingMode;
    for (const auto *key : appearance::fallbackFontKeys)
        next[key] = settingsFontFaces.value(key).toString();
    next["bold_reading"] =
            bionicOption->isChecked() ? QStringLiteral("bold") : QStringLiteral("off");
    next["dark_mode"] = darkOption->isChecked();
    for (const auto *key : features::keys)
        next[key] =
                static_cast<SettingsPanel *>(general)->featurePreferences.value(key).toBool(false);
    next["hide_nonlinear_fragments"] = hideNonlinearOption->isChecked();
    next["inpage_footnotes"] = inpageFootnotesOption->isChecked();
    next["vertical_justification"] = settingsVerticalSpacing;
    next["vertical_spacing_scope"] = settingsVerticalSpacingScope;
    next.remove("columns");
    next["two_columns_landscape"] =
            static_cast<SettingsPanel *>(general)->landscapeColumnsOption->isChecked();
    next["column_spacing_percent"] = settingsColumnSpacing;
    next["word_spacing"] = settingsWordSpacing;
    next["paragraph_spacing"] = settingsParagraphSpacing;
    next["paragraph_indent"] = settingsParagraphIndent;
    for (const auto *key : appearance::normalizationKeys)
        next[key] = settingsBeautifier.value(key);
    next[appearance::normalizationPresetKey] = appearance::normalizationPreset(settingsBeautifier);
    next.remove("avoid_wasteful_styling");
    next["justification_tracking"] = settingsTracking;
    next["hanging_punctuation"] = settingsHangingPunctuation;
    next["widows_orphans"] = settingsWidowsOrphans;
    if (services.softwarePageAnimations)
        next[pageTurn::preferenceKey] = pageTurn::value(
                animationOption->isChecked() ? pageTurn::Style::Sweep : pageTurn::Style::Off);
    else
        next["page_animations"] = animationOption->isChecked();
    next["edge_light_gestures"] = edgeLightOption->isChecked();
    next["show_light_gesture_feedback"] = lightGestureFeedbackOption->isChecked();
    next["show_opening_cover"] =
            !static_cast<SettingsPanel *>(general)->openingCoverOption->isChecked();
    next["dense_stats_chart"] = denseGraphOption->isChecked();
    next.remove(pageNavigation::barVisibleKey);
    next["minimal_annotation_mode"] = minimalAnnotationOption->isChecked();
    next["screen_refresh"] = kobalt::screenRefreshValue(screenRefreshMode);
    if (restoringPresets)
        textpresets::store(next, textpresets::restoreExamples(textpresets::load(next)));
    if (restoringDefaults) {
        next[kobalt::headerfooter::presetKey] = kobalt::headerfooter::presetValue(
                services.phoneLayout ? kobalt::headerfooter::Preset::Phone
                                     : kobalt::headerfooter::Preset::Default);
        next[kobalt::headerfooter::textKey] =
                kobalt::headerfooter::textSettings(kobalt::headerfooter::presetLayout(
                        services.phoneLayout ? kobalt::headerfooter::Preset::Phone
                                             : kobalt::headerfooter::Preset::Default));
    }
    const auto rotation = orientation->settings();
    for (auto it = rotation.begin(); it != rotation.end(); ++it)
        next[it.key()] = it.value();
    next = appearance::cleanPreferences(next);
    if (!writePreferences(next)) {
        bionicOption->setChecked(
                session->requestedAppearance().fields().value("bold_reading").toString("off")
                == "bold");
        minimalAnnotationOption->setChecked(minimalAnnotationMode());
        settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
        settingsError->show();
        appearanceEdit.cancelNavigation();
        refresh();
        return;
    }
    if (zenMode() != next.value("zen_mode").toBool(false))
        flashAfterZenSwitch = true;
    preferences = next;
    const bool hideBook = static_cast<SettingsPanel *>(general)->hideBookOnSleepOption->isChecked();
    if (hideBook != book->hideBookOnSleepScreen()) {
        book->setHideBookOnSleepScreen(hideBook);
        QJsonObject snapshot;
        book->checkpoint(&snapshot, true);
    }
    if (!features::enabled(preferences, features::Kosync)) {
        cancelKosync();
        kosyncClient->cancel();
        syncCoordinator->resetProgress();
        hideKosyncOpening();
    }
    if (!features::enabled(preferences, features::Words)) {
        ++savedLookupEpoch;
        lookupSnapshot = {};
        dictionaryPanel->setSaved(false, false);
        if (wordNotice)
            wordNotice->hide();
    }
    if (!features::enabled(preferences, features::Annotations) && annotationMode)
        leaveAnnotations();
    bookmarkList->setSortOrder(preferences.value("marks_sort").toString("position"));
    showMenuView();
    showNavigationBarButtons();
    ui::setHeaderFooterAddedWeight(settingsHeaderFooterWeight);
    applyInterfaceFonts();
    dictionaryPanel->setDefinitionFont(settingsDictionaryFontFace, settingsDictionaryFontSize);
    navigation->setStatusVisible(!hideNativeStatus());
    const auto chrome = kobalt::headerfooter::select(preferences, services.phoneLayout);
    chromePreset = chrome.preset;
    page->chrome = chrome.layout;
    syncChromeControls();
    applyEdgeLight();
    chapterGraph->setDense(preferences.value("dense_stats_chart").toBool(false));
    const auto requested = appearanceEdit.edited() && appearanceEdit.ready()
            ? appearanceEdit.preview()->requestedAppearance()
            : session->displayedAppearance().settings;
    QJsonObject settings;
    settings["default_font_face"] =
            settingsDefaultFontFace.isEmpty() ? QString(defaultFontFace) : settingsDefaultFontFace;
    for (const auto *key : appearance::fallbackFontKeys)
        settings[key] = preferences.value(key).toString();
    settings["dark_mode"] = darkOption->isChecked();
    settings["hide_nonlinear_fragments"] = hideNonlinearOption->isChecked();
    settings["inpage_footnotes"] = inpageFootnotesOption->isChecked();
    settings["vertical_justification"] = preferences.value("vertical_justification");
    settings["vertical_spacing_scope"] = preferences.value("vertical_spacing_scope");
    settings["columns"] = 1;
    settings["two_columns_landscape"] = preferences.value("two_columns_landscape");
    settings["column_spacing_percent"] = settingsColumnSpacing;
    settings["word_spacing"] = settingsWordSpacing;
    settings["paragraph_spacing"] = settingsParagraphSpacing;
    settings["paragraph_indent"] = settingsParagraphIndent;
    for (const auto *key : appearance::normalizationKeys)
        settings[key] = settingsBeautifier.value(key);
    settings["justification_tracking"] = settingsTracking;
    settings["hanging_punctuation"] = settingsHangingPunctuation;
    settings["widows_orphans"] = settingsWidowsOrphans;
    settings["bold_reading"] =
            bionicOption->isChecked() ? QStringLiteral("bold") : QStringLiteral("off");
    if (restoringDefaults) {
        const auto defaults = kobalt::appearance::typographyDefaults();
        for (auto it = defaults.begin(); it != defaults.end(); ++it)
            settings[it.key()] = it.value();
    }
    renderingMode::apply(preferences);
    const auto rendering = renderingMode::effective(renderingMode::fromPreferences(preferences));
    session->requestAppearance(requested.edited(appearance::Patch(settings)), bodySize(),
                               rendering == renderingMode::Mode::Accurate ? 0
                                       : rendering == renderingMode::Mode::Hybrid
                                       ? ReaderSession::hybridChapterMode
                                       : 1);
    applyingText = session->busy();
    if (appearanceEdit.destination() == AppearanceDestination::None)
        appearanceEdit.request(AppearanceDestination::Controls);
    appearanceEdit.beginApply(!session->busy() && appearanceEdit.edited() && appearanceEdit.ready()
                                      ? kobalt::reader::AppearanceEditSession::Retention::Reuse
                                      : kobalt::reader::AppearanceEditSession::Retention::Release);
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
    refresh();
}

void Reader::setDenseGraph(bool dense)
{
    if (savingButtonControls || savingDarkMode >= 0 || savingDenseGraph) {
        chapterGraph->setDense(preferences.value("dense_stats_chart").toBool(false));
        refresh();
        return;
    }
    auto next = appearance::globalPreferences(preferences);
    appearance::rememberTextLayout(next, session->state());
    appearance::rememberPageMargins(next, session->state());
    next["dense_stats_chart"] = dense;
    logSettingChanges(qt::object({ { "dense_stats_chart",
                                     preferences.value("dense_stats_chart").toBool(false) } }),
                      qt::object({ { "dense_stats_chart", dense } }));
    savingDenseGraph = true;
    if (!preferencesWriter)
        preferencesWriter = std::make_unique<storage::RecordWriter>();
    QPointer<Reader> guard(this);
    preferencesWriter->post(
            [path = book->paths().preferences(), next] {
                return storage::writePreferences(path, next);
            },
            [guard, dense](bool saved) {
                qt::invoke(
                        QCoreApplication::instance(),
                        [guard, dense, saved] {
                            if (!guard)
                                return;
                            guard->savingDenseGraph = false;
                            saves::settled();
                            if (saved) {
                                guard->preferences["dense_stats_chart"] = dense;
                                guard->denseGraphOption->setChecked(dense);
                            } else {
                                guard->chapterGraph->setDense(
                                        guard->preferences.value("dense_stats_chart")
                                                .toBool(false));
                                guard->showSettingsWriteError();
                            }
                            guard->refresh();
                        },
                        Qt::QueuedConnection);
            });
    refresh();
}

void Reader::toggleZenStats()
{
    // Do not overtake a pending dark mode or button save, as writePreferences
    // does. The button already toggled itself, so put it back.
    if (savingButtonControls || savingDarkMode >= 0 || savingDenseGraph) {
        syncNavigation();
        return;
    }
    // The panel changes at once. The choice is saved on the writer thread,
    // so the main thread never waits for storage while the menu is open.
    const bool shown = !preferences.value("zen_stats").toBool(false);
    logSettingChanges(qt::object({ { "zen_stats", !shown } }),
                      qt::object({ { "zen_stats", shown } }));
    preferences["zen_stats"] = shown;
    const auto write = ++preferencesWrites;
    auto next = appearance::globalPreferences(preferences);
    appearance::rememberTextLayout(next, session->state());
    appearance::rememberPageMargins(next, session->state());
    if (!preferencesWriter)
        preferencesWriter = std::make_unique<storage::RecordWriter>();
    QPointer<Reader> guard(this);
    preferencesWriter->post([path = book->paths().preferences(),
                             next] { return kobalt::storage::writePreferences(path, next); },
                            [guard, shown, write](bool saved) {
                                if (saved)
                                    return;
                                qt::invoke(
                                        QCoreApplication::instance(),
                                        [guard, shown, write] {
                                            if (!guard || guard->preferencesWrites != write)
                                                return;
                                            // A failed save undoes the toggle, as the
                                            // other settings do.
                                            guard->preferences["zen_stats"] = !shown;
                                            guard->showSettingsWriteError();
                                            guard->syncNavigation();
                                        },
                                        Qt::QueuedConnection);
                            });
    syncNavigation();
}

void Reader::applyDarkMode(bool enabled)
{
    if (savingButtonControls || savingDarkMode >= 0 || savingDenseGraph || pendingDarkMode >= 0
        || session->busy() || session->temporaryTypography()) {
        refresh();
        return;
    }
    auto next = appearance::globalPreferences(preferences);
    appearance::rememberTextLayout(next, session->state());
    appearance::rememberPageMargins(next, session->state());
    next["dark_mode"] = enabled;
    logSettingChanges(qt::object({ { "dark_mode", preferences.value("dark_mode").toBool(false) } }),
                      qt::object({ { "dark_mode", enabled } }));
    savingDarkMode = enabled;
    if (!preferencesWriter)
        preferencesWriter = std::make_unique<storage::RecordWriter>();
    QPointer<Reader> guard(this);
    preferencesWriter->post([path = book->paths().preferences(),
                             next] { return kobalt::storage::writePreferences(path, next); },
                            [guard, enabled](bool saved) {
                                qt::invoke(
                                        QCoreApplication::instance(),
                                        [guard, enabled, saved] {
                                            if (!guard)
                                                return;
                                            auto *reader = guard.data();
                                            reader->savingDarkMode = -1;
                                            if (!saved) {
                                                reader->showSettingsWriteError();
                                                reader->refresh();
                                                return;
                                            }
                                            reader->preferences["dark_mode"] = enabled;
                                            reader->settingsError->clear();
                                            reader->settingsError->hide();
                                            reader->pendingDarkMode = enabled;
                                            reader->session->setDarkMode(enabled);
                                            if (reader->appearanceEdit.preview())
                                                reader->appearanceEdit.preview()->setDarkMode(
                                                        enabled);
                                            reader->refresh();
                                        },
                                        Qt::QueuedConnection);
                            });
    refresh();
}

void Reader::applyBoldReading(bool enabled)
{
    if (session->temporaryTypography())
        return;
    const auto mode = enabled ? QStringLiteral("bold") : QStringLiteral("off");
    auto next = preferences;
    next["bold_reading"] = mode;
    if (!writePreferences(next)) {
        bionicOption->setChecked(
                session->requestedAppearance().fields().value("bold_reading").toString("off")
                == "bold");
        return;
    }
    preferences = next;
    settingsError->clear();
    settingsError->hide();
    session->editAppearance(appearance::Patch(qt::object({ { "bold_reading", mode } })));
    // Keep a pending typography draft in the same mode, including a preview
    // that has not finished opening yet.
    if (appearanceEdit.preview()) {
        mutateAppearance([mode](ReaderSession &preview) {
            preview.editAppearance(appearance::Patch(qt::object({ { "bold_reading", mode } })));
        });
    }
    refresh();
}

void Reader::applyAutomaticOpening(bool enabled)
{
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

void Reader::applyTheme(bool dark)
{
    if (themeReady && darkTheme == dark)
        return;
    // The first theme is part of showing the reader.
    const bool first = !themeReady;
    QElapsedTimer theming;
    theming.start();
    themeReady = true;
    darkTheme = dark;
    // Theme this window only. Nickel keeps its own palette when we close.
    const auto colors = ui::readerPalette(palette(), dark);
    const auto applyColors = [&colors](QWidget *widget) { ui::applyReaderPalette(widget, colors); };
    setProperty("reader_dark", dark);
    applyColors(this);
    textRefresh(this);
    for (auto *widget : findChildren<QWidget *>()) {
        // Native controls manage their own source colors and cached icons.
        bool nativeColors = false;
        for (auto *ancestor = widget; ancestor && ancestor != this;
             ancestor = ancestor->parentWidget())
            if (ancestor->property("reader_native_colors").toBool()) {
                nativeColors = true;
                break;
            }
        if (nativeColors)
            continue;
        applyColors(widget);
        textRefresh(widget);
        if (auto *button = qobject_cast<QPushButton *>(widget))
            kobalt::ui::refreshButtonIcon(button);
    }
    update();
    if (first)
        openingStages["theme_ms"] = theming.nsecsElapsed() / 1e6;
}

// Resolve the displayed book face and retain its bytes for GUI text. Publisher
// faces use the current text face, with the configured reading default as fallback.
QString Reader::bookFace()
{
    QString face = session->fontFace();
    if (face.isEmpty())
        face = session->state().value("text_font_face").toString();
    bool loadable = false;
    for (const auto &font : session->fontCatalog())
        loadable |= font.family == face;
    if (!loadable) {
        face = session->state().value("default_font_face").toString(kobalt::defaultFontFace);
        if (!session->fontFaces().contains(face))
            face = QString::fromLatin1(kobalt::defaultFontFace);
    }
    registerPickerFonts(face);
    return face;
}

void Reader::registerPickerFonts(const QString &family)
{
    // Private text uses these faces without adding them to Qt's font database. Adding
    // a font there and removing it when the reader closes makes Qt rebuild the
    // whole database on the next text it draws, Nickel's Home included. On the
    // Mac that rebuild was about 100 ms of a 125 ms close.
    const auto registerFont = [](const QString &path, const QString &family) {
        ui::registerPrivateInterfaceFont(path, family);
    };
    // Match the engine's source precedence so duplicate sideloads cannot
    // replace a bundled face in private text.
    // Qt 5.2 on firmware 4 keeps the first face it sees for a weight and
    // slant. A bold face whose PANOSE weight says "medium" is filed under
    // normal weight there, so register each family's regular face first.
    for (const auto &font : session->fontCatalog()) {
        if (!family.isEmpty() && font.family != family)
            continue;
        if (family.isEmpty() && font.origin == FontOrigin::Interface) {
            const auto requested = appearanceEdit.edited() && appearanceEdit.ready()
                    ? appearanceEdit.preview()->requestedAppearance()
                    : session->displayedAppearance().settings;
            if (font.family != supplementalFontFamily(session->state().value("language").toString())
                && font.family
                        != preferences.value("default_font_face").toString(kobalt::defaultFontFace)
                && font.family != requested.fields().value("font_face").toString())
                continue;
        }
        for (const auto &file : font.files)
            if (file.contains("Regular", Qt::CaseInsensitive))
                registerFont(file, font.family);
        for (const auto &file : font.files)
            if (!file.contains("Regular", Qt::CaseInsensitive))
                registerFont(file, font.family);
    }
}

void Reader::resizeText()
{
    if (!isVisible() || closing)
        return;
    const QSize viewport = bodySize();
    kobalt::diagnostic(
            "reader-resize-text",
            { { "viewport_width", viewport.width() },
              { "viewport_height", viewport.height() },
              { "target", appearanceEdit.preview() ? "preview" : "main" },
              { "main_busy", session->busy() },
              { "preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy() },
              { "updates_enabled", updatesEnabled() },
              { "resize_pending", resizePending } },
            false);
    // The chapter preview is the visible page while editing. Reflow the
    // whole book once when applying the final size and appearance.
    bool resizedPreview = false;
    if (appearanceEdit.panelOpen()) {
        if (viewport != appearanceViewport) {
            mutateAppearance([viewport](ReaderSession &preview) { preview.resize(viewport); });
            resizedPreview = true;
        }
    } else
        session->resize(viewport);
    applyingText = session->busy();
    if (!resizedPreview)
        refresh();
    kobalt::diagnostic(
            "reader-resize-refresh-return",
            { { "viewport_width", viewport.width() },
              { "viewport_height", viewport.height() },
              { "target", appearanceEdit.preview() ? "preview" : "main" },
              { "main_busy", session->busy() },
              { "preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy() },
              { "updates_enabled", updatesEnabled() },
              { "resize_pending", resizePending } },
            false);
}

void Reader::selectPanel(int index)
{
    session->cancelPendingTurn();
    lightPopup->hide();
    chapterGraph->hide();
    if (general->isVisible()) {
        appearanceEdit.request(index == 0 ? AppearanceDestination::Text
                                          : AppearanceDestination::Layout);
        applySettings();
    } else if (menu->isVisible()
               && (panels->currentIndex() == index
                   || (index == int(kobalt::reader::AppearancePage::Layout)
                       && panels->currentIndex()
                               != int(kobalt::reader::AppearancePage::Typography)))) {
        // The Layout tab closes the header and footer pages too.
        finishPreview();
    } else
        openPanel(index);
}

void Reader::openPanel(int index)
{
    if (session->temporaryTypography())
        return;
    if (session->busy())
        return;
    session->cancelPendingTurn();
    appearanceEdit.cancelDeferredApply();
    // Only the selected font is drawn here. Load the other faces when the
    // font picker opens, and do not read font files for the Layout panel.
    if (index == 0 && !session->fontFace().isEmpty())
        registerPickerFonts(session->fontFace());
    panels->setCurrentIndex(index);
    if (!menu->isVisible() && !appearanceEdit.preview()) {
        warmAttemptGeneration = 0;
    }
    appearanceEdit.setPanelOpen(true);
    appearanceViewport = bodySize();
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

void Reader::schedulePreviewWarm()
{
    if (!appearanceEdit.panelOpen() || !menu->isVisible() || general->isVisible()
        || appearanceEdit.preview() || session->busy() || lifecycleHidden || exitPending)
        return;
    const quint64 panelGeneration = appearanceEdit.panelGeneration();
    if (warmAttemptGeneration == panelGeneration || warmScheduledGeneration == panelGeneration)
        return;
    warmScheduledGeneration = panelGeneration;
    QTimer::singleShot(0, this, [this, panelGeneration] {
        if (warmScheduledGeneration == panelGeneration)
            warmScheduledGeneration = 0;
        if (appearanceEdit.panelGeneration() != panelGeneration || !appearanceEdit.panelOpen()
            || !menu->isVisible() || general->isVisible() || appearanceEdit.preview()
            || session->busy() || lifecycleHidden || exitPending)
            return;
        warmAttemptGeneration = panelGeneration;
        appearanceEdit.open(
                *session, bodySize(), [this] { refresh(); },
                [this] {
                    if (!appearanceEdit.edited() && !appearanceEdit.ready()) {
                        appearanceEdit.resetPreview();
                        appearanceEdit.clearError();
                        return;
                    }
                    if (exitPending || appearanceEdit.finishPending()
                        || appearanceEdit.externalDestination())
                        resumeAppearanceApply();
                    else
                        refresh();
                });
    });
}

void Reader::mutateAppearance(std::function<void(ReaderSession &)> operation)
{
    diagnostic("appearance-preview-request", {}, false);
    appearanceViewport = bodySize();
    appearanceEdit.mutate(
            *session, bodySize(), [this] { refresh(); },
            [this] {
                diagnostic("appearance-preview-opened",
                           { { "ready", appearanceEdit.ready() },
                             { "failed", !appearanceEdit.error().isEmpty() } },
                           false);
                if (exitPending || appearanceEdit.finishPending()
                    || appearanceEdit.externalDestination())
                    resumeAppearanceApply();
                else
                    refresh();
            },
            std::move(operation));
    refresh();
}

void Reader::finishPreview()
{
    if (session->temporaryTypography())
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
    const QSize viewport = bodySize();
    if (appearanceEdit.ready())
        session->requestAppearance(appearanceEdit.preview()->requestedAppearance(), viewport);
    else if (session->image().size() != viewport)
        session->requestAppearance(session->displayedAppearance().settings, viewport);
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
    if (appearanceEdit.destination() == AppearanceDestination::None)
        appearanceEdit.request(AppearanceDestination::Controls);
    applyingText = session->busy();
    appearanceEdit.beginApply();
    refresh();
}

// Continues a Settings or panel apply that waited for the preview to open.
void Reader::resumeAppearanceApply()
{
    if (general->isVisible())
        applySettings();
    else
        finishPreview();
}

// Both edges follow one setting. The right edge needs adjustable warmth.
void Reader::applyEdgeLight()
{
    page->brightnessEdge =
            preferences.value("edge_light_gestures").toBool(true) && services.lightAvailable();
    page->warmthEdge = page->brightnessEdge && warmthSupported;
    if (lightPopup) {
        lightPopup->setEdgeGesturesEnabled(page->brightnessEdge);
    }
}

void Reader::applyInterfaceFonts()
{
    for (const auto &face :
         { settingsUiFontFace, settingsUiMonoFontFace, settingsHeaderFooterFontFace })
        if (!face.isEmpty())
            registerPickerFonts(face);
    const auto previousUi = ui::interfaceFont().family(),
               previousStrip = ui::headerFooterFont().family(),
               previousMono = ui::chromeFont().family();
    ui::setInterfaceFontFamily(settingsUiFontFace);
    ui::setInterfaceMonoFontFamily(settingsUiMonoFontFace);
    ui::setHeaderFooterFontFamily(settingsHeaderFooterFontFace);
    if (previousUi == ui::interfaceFont().family()
        && previousStrip == ui::headerFooterFont().family()
        && previousMono == ui::chromeFont().family())
        return;
    setFont(ui::interfaceFont());
    const auto widgets = findChildren<QWidget *>();
    for (auto *widget : widgets) {
        const auto style = widget->property("reader_style").toString();
        if (!style.isEmpty())
            readerStyle(widget, style);
    }
    readerStyle(this, ui::styles::reader());
    page->setFont(ui::headerFooterFont());
    ui::refreshInterfaceText(this);
    fitSettings();
}

void Reader::showSettingsFonts()
{
    auto *panel = static_cast<SettingsPanel *>(general);
    panel->percentageFormatButton->setCaption(
            SettingsPanel::percentageFormatLabel(settingsPercentageFormat).toUpper());
    panel->timeRemainingFormatButton->setCaption(
            SettingsPanel::timeRemainingFormatLabel(settingsTimeRemainingFormat).toUpper());
    panel->renderingButton->setCaption(
            SettingsPanel::renderingModeLabel(settingsRenderingMode).toUpper());
    panel->headerFooterWeightButton->setCaption(
            (settingsHeaderFooterWeight ? strings::get("settings.header_footer_weight_heavier")
                                                  .arg(settingsHeaderFooterWeight)
                                        : strings::get("settings.header_footer_weight_lighter"))
                    .toUpper());
    panel->uiFontButton->setCaption(
            (settingsUiFontFace.isEmpty()
                     ? strings::get("settings.font_default_value").arg("IBM Plex Sans")
                     : fontDisplayName(settingsUiFontFace))
                    .toUpper());
    panel->uiMonoFontButton->setCaption(
            (settingsUiMonoFontFace.isEmpty()
                     ? strings::get("settings.font_default_value").arg("IBM Plex Mono")
                     : fontDisplayName(settingsUiMonoFontFace))
                    .toUpper());
    panel->headerFooterFontButton->setCaption(
            (settingsHeaderFooterFontFace.isEmpty()
                     ? strings::get("settings.font_default_value").arg("IBM Plex Mono")
                     : fontDisplayName(settingsHeaderFooterFontFace))
                    .toUpper());
    const auto sample = [this](ui::CaptionButton *button, const QString &family) {
        if (button->typefacePreview() != family)
            registerPickerFonts(family);
        button->setTypefacePreview(family);
    };
    sample(panel->uiFontButton,
           settingsUiFontFace.isEmpty() ? QStringLiteral("IBM Plex Sans") : settingsUiFontFace);
    sample(panel->uiMonoFontButton,
           settingsUiMonoFontFace.isEmpty() ? QStringLiteral("IBM Plex Mono")
                                            : settingsUiMonoFontFace);
    sample(panel->headerFooterFontButton,
           settingsHeaderFooterFontFace.isEmpty() ? QStringLiteral("IBM Plex Mono")
                                                  : settingsHeaderFooterFontFace);
    const auto dictionaryDefault = bookFace();
    const auto dictionaryFace =
            settingsDictionaryFontFace.isEmpty() ? dictionaryDefault : settingsDictionaryFontFace;
    panel->dictionaryFontButton->setCaption(
            (settingsDictionaryFontFace.isEmpty() ? strings::get("settings.font_default_value")
                                                            .arg(fontDisplayName(dictionaryDefault))
                                                  : fontDisplayName(settingsDictionaryFontFace))
                    .toUpper());
    sample(panel->dictionaryFontButton, dictionaryFace);
    panel->dictionaryFontSizeButton->setCaption(
            SettingsPanel::dictionaryFontSizeLabel(settingsDictionaryFontSize).toUpper());
    sample(panel->defaultFontButton, settingsDefaultFontFace);
    panel->defaultFontButton->setCaption((settingsDefaultFontFace.isEmpty()
                                                  ? strings::get("reader.publisher_fonts")
                                                  : fontDisplayName(settingsDefaultFontFace))
                                                 .toUpper());
    const auto defaults = fallbackFontDefaults(session->fontFaces());
    int index = 0;
    for (auto *button : { panel->serifFontButton, panel->sansFontButton, panel->monoFontButton }) {
        const auto family = settingsFontFaces.value(appearance::fallbackFontKeys[index]).toString();
        button->setCaption((family.isEmpty() ? strings::get("settings.font_default_value")
                                                       .arg(fontDisplayName(defaults[index]))
                                             : fontDisplayName(family))
                                   .toUpper());
        sample(button, family.isEmpty() ? defaults[index] : family);
        ++index;
    }
}

void Reader::openSettingsFont(const QString &key)
{
    const bool isDefault = key == "default_font_face";
    const bool isUi = key == "ui_font_face", isMono = key == "ui_mono_font_face",
               isHeaderFooter = key == "header_footer_font_face",
               isDictionary = key == "dictionary_font_face";
    int index = -1;
    for (int family = 0; family < int(appearance::fallbackFontKeys.size()); ++family)
        if (key == appearance::fallbackFontKeys[family])
            index = family;
    if ((!isDefault && !isUi && !isMono && !isHeaderFooter && !isDictionary && index < 0)
        || !general->isVisible())
        return;
    auto *panel = static_cast<SettingsPanel *>(general);
    const QStringList titles{ strings::get("settings.serif_font"),
                              strings::get("settings.sans_font"),
                              strings::get("settings.mono_font") };
    const auto selected = isDefault ? settingsDefaultFontFace
            : isUi                  ? settingsUiFontFace
            : isMono                ? settingsUiMonoFontFace
            : isHeaderFooter        ? settingsHeaderFooterFontFace
            : isDictionary          ? settingsDictionaryFontFace
                                    : settingsFontFaces.value(key).toString();
    auto fonts = session->fontCatalog();
    const auto nativeFamilies = services.fontFamilies();
    for (int row = fonts.size() - 1; row >= 0; --row)
        if (fonts[row].origin == FontOrigin::BuiltIn
            && !nativeFamilies.contains(fonts[row].family, Qt::CaseInsensitive))
            fonts.removeAt(row);
    fonts = orderedReadingFonts(std::move(fonts), { selected });
    fonts.prepend({ QString(), {}, FontOrigin::Recommended, {}, {} });
    panel->showFontChoices(
            isDefault                ? strings::get("reader.default_font")
                    : isUi           ? strings::get("settings.ui_font")
                    : isMono         ? strings::get("settings.ui_mono_font")
                    : isHeaderFooter ? strings::get("settings.header_footer_font")
                    : isDictionary   ? strings::get("settings.dictionary_font")
                                     : titles[index],
            fonts, selected,
            isDefault
                    ? strings::get("reader.publisher_fonts")
                    : strings::get("settings.font_default_value")
                              .arg(fontDisplayName(
                                      isUi ? QString("IBM Plex Sans")
                                              : (isMono || isHeaderFooter)
                                              ? QString("IBM Plex Mono")
                                              : isDictionary
                                              ? bookFace()
                                              : fallbackFontDefaults(session->fontFaces())[index])),
            [this, key, isDefault, isUi, isMono, isHeaderFooter,
             isDictionary](const QString &family) {
                if (isDefault)
                    settingsDefaultFontFace = family;
                else if (isUi)
                    settingsUiFontFace = family;
                else if (isMono)
                    settingsUiMonoFontFace = family;
                else if (isHeaderFooter)
                    settingsHeaderFooterFontFace = family;
                else if (isDictionary)
                    settingsDictionaryFontFace = family;
                else
                    settingsFontFaces[key] = family;
                showSettingsFonts();
                refresh();
            });
}
} // namespace kobalt::reader
