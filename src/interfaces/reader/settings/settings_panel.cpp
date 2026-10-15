#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "settings_panel.h"
#include "support/strings.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/styles.h"
#include <QButtonGroup>
#include <QGridLayout>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QHideEvent>

namespace kobalt::reader {
namespace m = ui::metrics;
namespace c = ui::metrics::chrome;
namespace styles = ui::styles;
using ui::readerStyle;
using ui::makeButton;
using ReaderOption = ui::DescribedOption;
namespace {
QPushButton *actionButton(QWidget *parent, QBoxLayout *layout, const char *name, const QString &text) {
    auto *button = new ui::Button(text, parent);
    button->setObjectName(name);
    button->setFocusPolicy(Qt::NoFocus);
    if (layout)
        layout->addWidget(button);
    return button;
}
}
SettingsPanel::SettingsPanel(QWidget *parent, const SettingsPanelInputs &inputs) : ui::Panel(parent, Qt::BottomEdge) {
    setObjectName("rrGeneralSettings");
    setSoftEdges(true);
    auto *generalRows = new QVBoxLayout(this);
    generalRows->setContentsMargins(0, 0, 0, 0);
    generalRows->setSpacing(m::panel::gap);
    auto *sections = new QWidget(this);
    generalRows->addWidget(sections, 1);
    auto *sectionRows = new QBoxLayout(QBoxLayout::LeftToRight, sections);
    sectionRows->setObjectName("settingsSectionLayout");
    sectionRows->setContentsMargins(0, 0, 0, 0);
    sectionRows->setSpacing(0);
    tabRail = new QWidget(sections);
    tabRail->setObjectName("rrSettingsRail");
    auto *settingsTabs = new QBoxLayout(QBoxLayout::TopToBottom, tabRail);
    settingsTabs->setContentsMargins(0, 0, 0, 0);
    settingsTabs->setSpacing(0);
    sectionRows->addWidget(tabRail);
    settingsPages = new QStackedWidget(sections);
    settingsPages->setObjectName("rrSettingsPages");
    readerStyle(settingsPages, styles::tabFrame());
    auto *tabGroup = new QButtonGroup(this);
    const char *tabNames[] = {"rrSettingsDisplay", "rrSettingsAdvanced", "rrSettingsDictionaries", "rrSettingsStats",
                              "rrSettingsAbout"};
    const QStringList tabs = {strings::get("settings.display"), strings::get("settings.advanced"),
                              strings::get("settings.dictionaries"), strings::get("settings.stats"),
                              strings::get("settings.about")};
    for (int index = 0; index < tabs.size(); ++index) {
        auto *button = makeButton(this, settingsTabs, tabNames[index], tabs[index], [this, index] {
            settingsPages->setCurrentIndex(index);
        });
        button->setProperty("reader_tab_label", tabs[index]);
        button->setCheckable(true);
        tabGroup->addButton(button, index);
        settingsTabButtons.append(button);
        readerStyle(button, styles::tab(index == 0));
        // Qt 5.2 ignores a font set in the :checked rule. A property rule
        // applies on both Qt versions once the style is polished again.
        connect(button, &QPushButton::toggled, button, [button](bool on) {
            button->setProperty("kobalt_selected", on);
            button->style()->unpolish(button);
            button->style()->polish(button);
        });
        settingsTabs->setStretch(index, 1);
        if (index == DictionariesSettings && !inputs.downloadableDictionaries) {
            button->setEnabled(false);
            button->hide();
            settingsTabs->setStretch(index, 0);
        }
    }
    sectionRows->addWidget(settingsPages, 1);
    auto *displayPage = new QWidget(settingsPages);
    displayPage->setObjectName("rrDisplayOptions");
    settingsPages->addWidget(displayPage);
    auto *generalOptions = new QGridLayout(displayPage);
    generalOptions->setContentsMargins(m::settings::displayMargins);
    generalOptions->setHorizontalSpacing(m::settings::columnGap);
    generalOptions->setVerticalSpacing(0);
    auto check = [](QWidget *parent, const char *name, const QString &text, const QString &description) {
        auto *box = new ReaderOption(text, description, parent);
        box->setObjectName(name);
        return box;
    };
    darkOption =
        check(displayPage, "rrDarkMode", strings::get("reader.dark_mode"), strings::get("settings.dark_description"));
    progressOption = check(displayPage, "rrShowProgress", strings::get("reader.book_progress_bar"),
                           strings::get("settings.progress_description"));
    const bool animationsSupported = inputs.animationsSupported;
    animationOption = check(displayPage, "rrPageAnimations", strings::get("reader.enable_page_animations"),
                            animationsSupported ? strings::get("settings.animations_description")
                                                : strings::get("settings.unsupported_description"));
    animationOption->setEnabled(animationsSupported);
    bionicOption = check(displayPage, "rrBionicReading", strings::get("reader.bold_reading"),
                         strings::get("reader.bionic_description"));
    int optionIndex = 0;
    for (auto *option : {darkOption, progressOption, bionicOption, animationOption}) {
        option->setMinimumHeight(m::settings::displayOptionHeight);
        generalOptions->addWidget(option, optionIndex++, 0);
    }
    // Refresh the screen shows its choice under the title and picks from a pop-up.
    refreshButton = ui::makeCaptionButton(displayPage, nullptr, "rrScreenRefresh",
                                          strings::get("reader.refresh_the_screen"), [] {});
    generalOptions->addWidget(refreshButton, optionIndex++, 0);
    deviceOptions << animationOption << refreshButton;
    generalOptions->setRowStretch(optionIndex, 1);
    auto *advancedPage = new QWidget(settingsPages);
    advancedPage->setObjectName("rrAdvancedOptions");
    settingsPages->addWidget(advancedPage);
    auto *advancedRows = new QVBoxLayout(advancedPage);
    advancedRows->setContentsMargins(m::settings::displayMargins);
    advancedRows->setSpacing(0);
    smoothOption = check(advancedPage, "rrSmoothImages", strings::get("reader.smooth_images"),
                         strings::get("settings.smoothing_description"));
    advancedRows->addWidget(smoothOption);
    linkIndicatorOption = check(advancedPage, "rrShowLinkIndicator", strings::get("reader.show_link_indicator"),
                                strings::get("reader.link_indicator_description"));
    advancedRows->addWidget(linkIndicatorOption);
    hideNonlinearOption =
        check(advancedPage, "rrHideNonlinearFragments", strings::get("reader.hide_nonlinear_fragments"),
              strings::get("settings.hide_nonlinear_fragments_description"));
    advancedRows->addWidget(hideNonlinearOption);
    const bool lightSupported = inputs.lightSupported;
    edgeLightOption = check(advancedPage, "rrEdgeLight", strings::get("reader.edge_light"),
                            !lightSupported          ? strings::get("settings.unsupported_description")
                            : inputs.warmthSupported ? strings::get("settings.edge_light_description")
                                                     : strings::get("settings.edge_brightness_description"));
    edgeLightOption->setEnabled(lightSupported);
    advancedRows->addWidget(edgeLightOption);
    deviceOptions << edgeLightOption;
    heavierTextOption = check(advancedPage, "rrHeavierText", strings::get("reader.heavier_interface_text"),
                              strings::get("settings.heavier_interface_text_description"));
    advancedRows->addWidget(heavierTextOption);
    auto *resetRow = kobalt::ui::horizontalRow();
    advancedRows->addLayout(resetRow);
    resetRow->addStretch(1);
    resetDefaults = ui::makeCaptionButton(advancedPage, resetRow, "rrRestoreDefaults",
                                          strings::get("reader.restore_appearance_defaults"), [this] {
                                              settingsPages->setCurrentIndex(AdvancedSettings);
                                              restoreDialog->fitCard();
                                              restoreDialog->show();
                                              restoreDialog->raise();
                                              restoreDialog->setFocus();
                                          });
    readerStyle(resetDefaults, styles::textAction());
    resetDefaults->setFixedHeight(m::about::actionHeight);
    resetRow->addStretch(1);
    advancedRows->addStretch(1);
    auto *statsPage = new QWidget(settingsPages);
    statsPage->setObjectName("rrStatsOptions");
    settingsPages->addWidget(statsPage);
    auto *statsRows = new QVBoxLayout(statsPage);
    statsRows->setContentsMargins(m::settings::displayMargins);
    statsRows->setSpacing(0);
    denseGraphOption = check(statsPage, "rrDenseGraph", strings::get("reader.dense_graph"),
                             strings::get("settings.dense_graph_description"));
    statsRows->addWidget(denseGraphOption);
    pinStatsOption = check(statsPage, "rrPinStats", strings::get("reader.pin_stats"),
                           strings::get("settings.pin_stats_description"));
    statsRows->addWidget(pinStatsOption);
    statsRows->addStretch(1);
    auto *aboutPage = new QWidget(settingsPages);
    aboutPage->setObjectName("rrAbout");
    settingsPages->addWidget(aboutPage);
    auto *aboutLayout = new QGridLayout(aboutPage);
    aboutLayout->setContentsMargins(m::about::margins);
    auto *aboutContent = new QWidget(aboutPage);
    aboutContent->setObjectName("rrAboutContent");
    aboutLayout->addWidget(aboutContent, 0, 0, Qt::AlignCenter);
    auto *aboutRows = new QVBoxLayout(aboutContent);
    aboutRows->setContentsMargins(0, 0, 0, 0);
    aboutRows->setSpacing(m::about::gap);
    auto *logo = new kobalt::ui::BrandIcon(aboutPage);
    logo->setObjectName("rrAboutLogo");
    logo->setFixedSize(m::about::logoSize, m::about::logoSize);
    aboutRows->addWidget(logo, 0, Qt::AlignHCenter);
    auto *wordmark = new QLabel(strings::get("app.wordmark"), aboutPage);
    wordmark->setObjectName("rrAboutWordmark");
    wordmark->setAlignment(Qt::AlignCenter);
    readerStyle(wordmark, styles::brand(m::about::wordmarkText));
    aboutRows->addWidget(wordmark);
    auto *build = new QVBoxLayout;
    build->setSpacing(m::about::buildGap);
    aboutRows->addLayout(build);
    // "Version 0.4 (Qt6)", then the short commit with "*" for uncommitted changes.
    const auto release = inputs.release;
    auto *version = new QLabel(strings::get("about.version").arg(release).arg(inputs.qtMajor), aboutPage);
    version->setObjectName("rrAboutVersion");
    version->setAlignment(Qt::AlignCenter);
    readerStyle(version, styles::font(m::text::secondary));
    build->addWidget(version);
    auto *commit = new QLabel(inputs.commit, aboutPage);
    commit->setObjectName("rrAboutCommit");
    commit->setAlignment(Qt::AlignCenter);
    readerStyle(commit, styles::font(m::about::commitText));
    build->addWidget(commit);
    commit->setVisible(inputs.commitVisible);
    auto *description = new QLabel(strings::get("about.description"), aboutPage);
    aboutDescription = description;
    description->setObjectName("rrAboutDescription");
    description->setWordWrap(true);
    description->setAlignment(Qt::AlignCenter);
    description->setFixedWidth(m::about::textWidth);
    readerStyle(description, styles::font(m::text::body));
    description->ensurePolished();
    // Measure at the displayed width so Qt cannot clip wrapped lines.
    description->setFixedHeight(description->heightForWidth(m::about::textWidth));
    aboutRows->addWidget(description, 0, Qt::AlignHCenter);
    aboutRows->addSpacing(m::about::actionGap / 2);
    auto *aboutActions = new QGridLayout;
    aboutActions->setSpacing(m::about::actionGap);
    aboutRows->addLayout(aboutActions);
    aboutActions->setColumnStretch(0, 1);
    aboutActions->setColumnStretch(2, 1);
    int actionRow = 0;
    auto textAction = [&](const char *name, const QString &label) {
        auto *row = kobalt::ui::horizontalRow();
        aboutActions->addLayout(row, actionRow++, 1);
        row->addStretch(1);
        auto *button = actionButton(aboutPage, row, name, label);
        readerStyle(button, styles::textAction() + styles::font(m::text::body));
        button->setFixedHeight(m::about::actionHeight);
        row->addStretch(1);
        return button;
    };
    openAcknowledgments = textAction("rrOpenAcknowledgments", strings::get("acknowledgments.acknowledgments"));
    if (inputs.debugProgressAvailable) {
        auto *debugRow = kobalt::ui::horizontalRow();
        aboutLayout->addLayout(debugRow, 0, 0, Qt::AlignRight | Qt::AlignBottom);
        debugProgress = actionButton(aboutPage, debugRow, "rrDebugProgress", {});
        const auto label = strings::get("reader.debug_progress");
        debugProgress->setAccessibleName(label);
        debugProgress->setToolTip(label);
        kobalt::ui::iconAction(debugProgress, ui::ControlIcon::Debug, label);
        debugProgress->setFixedWidth(m::control::iconButtonWidth);
        readerStyle(debugProgress,
                    QString("padding:0;border:none;min-width:%1px;max-width:%1px;min-height:%1px;max-height:%1px;")
                        .arg(m::control::stepSize));
        debugProgress->setFixedSize(m::control::stepSize, m::control::stepSize);
    }
    dictionaries = new DictionariesPage(settingsPages);
    settingsPages->insertWidget(DictionariesSettings, dictionaries);
    restoreDialog =
        new ui::ChoiceModal(parent,
                            {strings::get("reader.your_reading_position_stays_the_same_the_defaults_apply_when_you_c"),
                             strings::get("reader.restore_appearance_defaults_prompt"),
                             {}},
                            {strings::get("common.cancel"), "rrKeepDefaults"},
                            {strings::get("reader.restore_defaults"), "rrConfirmDefaults"});
    restoreDialog->setObjectName("rrRestoreConfirmation");
    restoreDialog->card()->findChild<QLabel *>("rrModalTitle")->setObjectName("rrRestoreTitle");
    restoreDialog->descriptionLabel()->setObjectName("rrRestoreDescription");
    keepDefaults = restoreDialog->secondaryButton();
    confirmDefaults = restoreDialog->primaryButton();
    connect(keepDefaults, &QPushButton::clicked, this, [this] {
        dismissRestore();
    });
    connect(confirmDefaults, &QPushButton::clicked, this, [this] {
        dismissRestore();
    });
    restoreDialog->dismissed = [this] {
        dismissRestore();
    };
    if (inputs.desktopUi) {
        auto *nativePage = new QWidget(settingsPages);
        nativePage->setObjectName("rrNativeUiOptions");
        settingsPages->addWidget(nativePage);
        auto *rows = new QVBoxLayout(nativePage);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        hideStatusOption = check(nativePage, "rrHideStatusBar", strings::get("settings.hide_status_bar"),
                                 strings::get("settings.hide_status_bar_description"));
        hideDeviceOptionsOption = check(nativePage, "rrHideDeviceOptions", strings::get("settings.hide_device_options"),
                                        strings::get("settings.hide_device_options_description"));
        rows->addWidget(hideStatusOption);
        rows->addWidget(hideDeviceOptionsOption);
        rows->addStretch(1);
        auto *button = makeButton(this, nullptr, "rrSettingsUI", strings::get("settings.ui"), [this] {
            settingsPages->setCurrentIndex(NativeUiSettings);
        });
        button->setProperty("reader_tab_label", strings::get("settings.ui"));
        button->setCheckable(true);
        tabGroup->addButton(button);
        settingsTabButtons.append(button);
        settingsTabs->insertWidget(2, button);
        readerStyle(button, styles::tab(false));
        connect(button, &QPushButton::toggled, button, [button](bool on) {
            button->setProperty("kobalt_selected", on);
            button->style()->unpolish(button);
            button->style()->polish(button);
        });
        connect(hideDeviceOptionsOption, &QCheckBox::toggled, this, [this](bool hidden) {
            hideDeviceOptions(hidden);
        });
    }
    settingsError = new QLabel(this);
    settingsError->setWordWrap(true);
    settingsError->hide();
    generalRows->addWidget(settingsError);
    connect(settingsPages, &QStackedWidget::currentChanged, this, [this](int index) {
        settingsTabButtons[index == NativeUiSettings ? 5 : index]->setChecked(true);
    });
    settingsTabButtons.first()->setChecked(true);
    QVector<ui::DescribedOption *> options = {hideNonlinearOption, linkIndicatorOption, darkOption,    progressOption,
                                              bionicOption,        animationOption,     smoothOption,  edgeLightOption,
                                              heavierTextOption,   denseGraphOption,    pinStatsOption};
    if (inputs.desktopUi)
        options << hideStatusOption << hideDeviceOptionsOption;
    for (auto *option : options) {
        auto presentation = option->presentation();
        presentation.indicator = ui::DescribedOption::Indicator::Square;
        presentation.dividers = Qt::BottomEdge;
        presentation.horizontalInset = c::inset;
        presentation.labelPixels = c::labelText;
        presentation.descriptionPixels = c::descriptionText;
        presentation.indicatorSize = {c::checkSize, c::checkSize};
        presentation.minimumRowHeight = option == smoothOption ? c::listActionHeight : c::optionHeight;
        option->setPresentation(presentation);
    }
    generalOptions->setContentsMargins(0, 0, 0, 0);
    statsRows->setContentsMargins(0, 0, 0, 0);
    advancedRows->setContentsMargins(0, 0, 0, 0);
    resetRow->setStretch(0, 0);
    resetRow->setStretch(1, 1);
    resetRow->setStretch(2, 0);
    for (auto *button : {resetDefaults, refreshButton}) {
        button->setRule(Qt::BottomEdge);
        // The refresh row's caption shows its current choice, set when Settings opens.
        if (button == resetDefaults)
            button->setCaption(strings::get("settings.asks_first"));
        ui::readerStyle(
            button,
            QString(
                "QPushButton{font-size:%1px;min-height:%2px;max-height:%2px;border:0;border-radius:0;text-align:left;padding:0 %3px;background:palette(window);color:palette(window-text);} QPushButton:pressed{background:palette(midlight);}")
                .arg(c::labelText)
                .arg(c::listActionHeight)
                .arg(c::inset));
        button->setFixedHeight(c::listActionHeight);
    }
    tabRail->setFixedWidth(c::railWidth);
    settingsTabs->setAlignment(Qt::AlignTop);
    for (auto *button : settingsTabButtons) {
        button->setText(button->property("reader_tab_label").toString().toUpper());
        button->setFixedHeight(c::railRowHeight);
        ui::readerStyle(
            button,
            QString(
                "QPushButton{font-size:%1px;min-height:%2px;max-height:%2px;border:0;border-bottom:%5px solid %3;border-right:%5px solid palette(window-text);border-radius:0;text-align:left;padding:0 %4px;background:palette(window);color:palette(window-text);} QPushButton:checked{background:palette(window-text);color:palette(window);}")
                .arg(c::railText)
                .arg(c::railRowHeight)
                .arg(c::dividerLight)
                .arg(c::railInset)
                .arg(m::control::border));
    }
    ui::readerStyle(
        settingsPages,
        QString("QStackedWidget{border:0;border-left:%1px solid palette(window-text);}").arg(m::control::border));
    ui::readerStyle(description, ui::styles::font(c::labelText));
    ui::readerStyle(version, ui::styles::font(c::descriptionText));
    ui::readerStyle(commit, ui::styles::font(ui::px(21)));
    ui::readerStyle(openAcknowledgments, ui::styles::textAction() + ui::styles::font(c::labelText));
    hide();
}
int SettingsPanel::fitContent(QSize availableSize) {
    const int contentWidth = qMax(1, availableSize.width() - tabRail->width());
    auto *description = aboutDescription;
    description->setFixedWidth(qMin<int>(m::about::textWidth, contentWidth - ui::px(32)));
    description->setMinimumHeight(0);
    description->setMaximumHeight(QWIDGETSIZE_MAX);
    description->setFixedHeight(description->heightForWidth(description->width()));
    auto *content = settingsPages->currentWidget();
    content->ensurePolished();
    auto *rows = content->layout();
    const int contentHeight =
        qMax(rows->minimumSize().height(),
             rows->hasHeightForWidth() ? rows->totalHeightForWidth(contentWidth) : rows->sizeHint().height());
    // A short page still leaves room for every tab in the rail beside it.
    const int panelHeight = settingsPages->currentIndex() == DictionariesSettings
                                ? qMax(1, availableSize.height())
                                : qMax(contentHeight, tabRail->sizeHint().height());
    settingsPages->setFixedHeight(panelHeight);
    return panelHeight;
}
SettingsPanel::~SettingsPanel() {
    delete restoreDialog;
}
void SettingsPanel::dismissRestore() {
    if (!restoreDialog || restoreDialog->isHidden())
        return;
    restoreDialog->hide();
    resetDefaults->setFocus();
}
void SettingsPanel::hideEvent(QHideEvent *event) {
    dismissRestore();
    ui::Panel::hideEvent(event);
}
void SettingsPanel::showDictionaries(bool visible) {
    auto *button = settingsTabButtons[DictionariesSettings];
    button->setVisible(visible);
    button->setEnabled(visible);
    if (!visible && settingsPages->currentIndex() == DictionariesSettings)
        settingsPages->setCurrentIndex(DisplaySettings);
}

void SettingsPanel::hideDeviceOptions(bool hidden) {
    for (auto *widget : deviceOptions)
        widget->setVisible(!hidden);
}
}
