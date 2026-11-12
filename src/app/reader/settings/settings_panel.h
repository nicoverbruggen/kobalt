#pragma once
#include "platform/api/page_animation_support.h"
#include "platform/api/page_buttons.h"
#include "ui/controls/buttons.h"
#include "ui/controls/options.h"
#include "ui/controls/panels.h"
#include "ui/controls/paging.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/dialogs/modals.h"
#include "dictionaries_page.h"
#include "kosync_page.h"
#include "assets/font_catalog.h"
#include "reading/style_sample.h"
#include <QHash>
#include <QStackedWidget>
#include <QVector>
#include <QPointer>

namespace kobalt {
class AboutPage;
}

namespace kobalt::reader {
// The brand-face title of a full-screen settings page. It sits where the
// word "Settings" sits, so every page title shares one baseline.
class SettingsTitle final : public ui::InterfaceLabel
{
public:
    using ui::InterfaceLabel::InterfaceLabel;

protected:
    void paintEvent(QPaintEvent *event) override;
};

// Settings tabs, in the order of the stacked pages.
enum class SettingsPage {
    Display,
    Controls,
    Defaults,
    Styling,
    Dictionaries,
    System,
    About,
    Kosync,
    // Zen mode's own page. It has no tab: the rail is hidden in Zen mode.
    Zen
};

struct SettingsPanelInputs
{
    KeyboardFactory keyboard;
    PageAnimationSupport animationSupport = PageAnimationSupport::Unavailable;
    bool lightSupported = false, warmthSupported = false;
    bool debugProgressAvailable = false;
    bool downloadableDictionaries = true;
    bool sleepScreenSupported = false;
    bool automaticBookOpeningSupported = false;
    // The host can turn Wi-Fi off when a book opens, or previews a host that can.
    bool wifiPowerSaverSupported = false;
    bool readerStandbySupported = false;
};

// Qt parents own every widget. Reader installs policy callbacks after construction.
class SettingsPanel final : public ui::Panel
{
public:
    SettingsPanel(QWidget *parent, const SettingsPanelInputs &inputs);
    ~SettingsPanel() override;
    void dismissRestore();
    void dismissStandbyConfirmation();
    void dismissAutomaticOpeningConfirmation();
    // Receives a confirmed choice for Open books automatically in Kobalt.
    // Turning it off asks first; turning it on applies at once.
    std::function<void(bool)> automaticOpeningChosen;
    void showFontChoices(const QString &title, const QVector<ReadingFont> &fonts,
                         const QString &current, const QString &emptyLabel,
                         std::function<void(const QString &)> chosen);
    void dismissSheet();
    // Shows, raises and focuses the open sheet, then reports it through
    // sheetShown().
    void presentSheet();
    ui::RadioList *openChoiceSheet(const QString &name, const QString &title,
                                   const QString &hint = {}, const QString &titleName = {});
    void showHeaderFooterWeights(int current, const QFont &sampleFont,
                                 std::function<void(int)> chosen);
    void showDictionaryFontSizes(int current, const QString &family, bool customFont,
                                 std::function<void(int)> chosen);
    static QString dictionaryFontSizeLabel(int percent, bool compact = false);
    void showTimeRemainingFormats(const QString &current,
                                  std::function<void(const QString &)> chosen);
    static QString timeRemainingFormatLabel(const QString &format);
    void showPercentageFormats(const QString &current, std::function<void(const QString &)> chosen);
    static QString percentageFormatLabel(const QString &format);
    void showRenderingModes(const QString &current, std::function<void(const QString &)> chosen);
    static QString renderingModeLabel(const QString &mode);
    void showColumnSpacingChoices(int current, std::function<void(int)> chosen);
    static QString columnSpacingLabel(int percent);
    enum class AdvancedText {
        WordSpacing,
        Tracking,
        HangingPunctuation,
        Pagination,
        VerticalSpacing,
        ParagraphSpacing
    };
    static QString paragraphSpacingLabel(int percent);
    void updateButtonControls(const pageButtons::Buttons &buttons, const QJsonObject &preferences,
                              bool saving);
    void showButtonControls(const pageButtons::Buttons &buttons, const QJsonObject &current,
                            std::function<void(QJsonObject)> chosen);
    void showBeautifier(const QJsonObject &current, std::function<void(QJsonObject)> chosen);
    void showPageSpacingChoices(int mode, int scope, std::function<void(int, int)> chosen);
    void showParagraphAdjustments(int spacing, int indent, std::function<void(int, int)> chosen);
    // The sheet stays open while choosing, so each choice can be compared in
    // its preview.
    void showAdvancedTextChoices(AdvancedText feature, int current, std::function<void(int)> chosen,
                                 const QString &alignment = {});
    // Rendered sample pages for every option of one setting, shown when its
    // sheet is open.
    void setStylingSample(AdvancedText feature, const StyleSample &sample);
    void invalidateStylingSamples(int unchanged = -1);
    std::function<void(AdvancedText)> stylingRequested;
    // Called when the panel hides, so pending sample renders can stop.
    std::function<void()> stylingHidden;
    // Sizes the sample is drawn at, in device pixels, for a reading page.
    // `edge` leaves room for the magnified bottom edge of page spacing.
    QSize stylingPageSize(QSize readingPage, bool edge = false) const;
    int stylingEdgeHeight() const;
    // The page region, in device pixels, that fills the magnified edge.
    QSize stylingEdgeSize(QSize readingPage) const;
    QSize stylingCropSize(QSize readingPage, int horizontalMargin) const;
    QSize stylingTextPageSize(QSize readingPage, int horizontalMargin) const;
    QPointer<ui::ModalOverlay> sheet;
    // Called each time a sheet appears, so the reader can set its e-ink
    // refresh and keep it above other pop-ups.
    std::function<void(ui::ModalOverlay *)> sheetShown;
    ui::CellButton *closeButton = nullptr;
    // Asks the reader to fit the panel again, for changes that do not switch
    // the page, such as returning to the section list.
    std::function<void()> refitRequested;
    void showDictionaries(bool visible);
    void showPowerManagement(bool visible);
    int fitContent(QSize availableSize);
    ui::DescribedOption *hideTabLabelsOption = nullptr, *saveWordsOption = nullptr;
    ui::DescribedOption *wifiPowerSaverOption = nullptr;
    ui::DescribedOption *disableStandbyOption = nullptr;
    ui::DescribedOption *hideBookOnSleepOption = nullptr;
    QStackedWidget *settingsPages = nullptr;
    QVector<QPushButton *> settingsTabButtons;
    ui::CaptionButton *disabledFeaturesButton = nullptr;
    QJsonObject featurePreferences;
    void showDisabledFeatures();
    void updateFeatureOptions();
    ui::DescribedOption *darkOption = nullptr, *animationOption = nullptr,
                        *openingCoverOption = nullptr;
    // Defaults: what new books start with. The reader opens the preset grid.
    ui::CaptionButton *textForNewBooksButton = nullptr;
    ui::DescribedOption *edgeLightOption = nullptr, *lightGestureFeedbackOption = nullptr,
                        *denseGraphOption = nullptr;
    ui::DescribedOption *hideNonlinearOption = nullptr;
    ui::DescribedOption *inpageFootnotesOption = nullptr;
    ui::DescribedOption *zenModeOption = nullptr;
    // Zen mode shows one short page instead of the tabs: Dark mode, Light
    // when no edge swipe changes it, Sync when it is set up, About, and a bar
    // that leaves Zen mode only after a held press. Leaving turns Zen mode
    // off in the draft and shows all settings; closing Settings applies it.
    void setZenLayout(bool zen);

    std::function<void()> zenLightRequested, zenLeft;
    ui::DescribedOption *automaticOpeningOption = nullptr, *minimalAnnotationOption = nullptr;
    ui::CaptionButton *defaultFontButton = nullptr, *uiFontButton = nullptr,
                      *uiMonoFontButton = nullptr, *headerFooterFontButton = nullptr,
                      *headerFooterWeightButton = nullptr;
    ui::CaptionButton *serifFontButton = nullptr, *sansFontButton = nullptr,
                      *monoFontButton = nullptr;
    ui::CaptionButton *dictionaryFontButton = nullptr, *dictionaryFontSizeButton = nullptr;
    ui::DescribedOption *landscapeColumnsOption = nullptr;
    ui::CaptionButton *columnSpacingButton = nullptr, *timeRemainingFormatButton = nullptr,
                      *percentageFormatButton = nullptr;
    ui::CaptionButton *renderingButton = nullptr;
    ui::CaptionButton *verticalSpacingButton = nullptr;
    ui::CaptionButton *wordSpacingButton = nullptr, *trackingButton = nullptr,
                      *hangingPunctuationButton = nullptr, *widowsOrphansButton = nullptr;
    ui::CaptionButton *paragraphSpacingButton = nullptr, *beautifierButton = nullptr;
    ui::CaptionButton *sleepScreenButton = nullptr, *sessionTimeoutButton = nullptr,
                      *menuViewButton = nullptr;
    ui::CaptionButton *buttonControlsButton = nullptr;
    ui::CaptionButton *pageNavigationButton = nullptr, *tapZonesButton = nullptr,
                      *navigationBarButton = nullptr, *tappableLinksButton = nullptr,
                      *chapterGroupsButton = nullptr;
    ui::DescribedOption *bionicOption = nullptr;
    ui::CaptionButton *refreshButton = nullptr, *resetDefaults = nullptr, *resetPresets = nullptr;
    QPushButton *keepDefaults = nullptr, *confirmDefaults = nullptr;
    QPushButton *keepPresets = nullptr, *confirmPresets = nullptr;
    QPushButton *openAcknowledgments = nullptr, *debugProgress = nullptr;
    DictionariesPage *dictionaries = nullptr;
    KosyncPage *kosync = nullptr;
    QLabel *settingsError = nullptr;

protected:
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void showStylingChoices(AdvancedText feature, int current, std::function<void(int)> chosen,
                            const QString &alignment, int secondary,
                            std::function<void(int, int)> pairChosen);
    QWidget *tabRail = nullptr, *optionsColumn = nullptr;
    bool zen = false, lightSupported = false;
    void updateZenPage();
    QWidget *zenPage = nullptr, *zenMarkBox = nullptr, *zenLeaveBar = nullptr,
            *zenSyncSection = nullptr;
    // The hold button sits in the bar below the page, or in the heading on a
    // landscape screen, where the bar would take too much height.
    QPushButton *zenLeave = nullptr;
    QBoxLayout *headingLayout = nullptr, *leaveLayout = nullptr;
    ui::DescribedOption *zenDarkOption = nullptr;
    ui::CaptionButton *zenLightButton = nullptr, *zenSyncButton = nullptr,
                      *zenAutomaticButton = nullptr, *zenAboutButton = nullptr;
    QLabel *settingsTitle = nullptr;
    ui::CellButton *backButton = nullptr;
    // A narrow screen shows the sections as a list. Choosing one opens its
    // page at full width, with a back cell in place of the close cell.
    bool listMode = false, sectionOpen = false;
    // A short screen keeps the rail but puts each icon beside its label, so
    // eight sections fit its height.
    bool shortRail = false;
    void styleTabs(bool list);
    QWidget *optionsViewport = nullptr, *optionsClip = nullptr;
    ui::PagerBar *optionsPager = nullptr;
    QVector<QPair<int, int>> optionPages;
    // The row positions the pages were cut from. A row that changes its
    // height afterwards, for example when its description wraps anew, moves
    // the rows below it, so the pages are cut again.
    QVector<QRect> pagedRows;
    QVector<QRect> pageRows() const;
    bool repaginateQueued = false;
    int optionPage = 0, pageHeight = 0;
    void showOptionsPage(int page);
    void fitRail(int availableHeight = 0);
    AboutPage *aboutPage = nullptr;
    QPointer<ui::ChoiceModal> restoreDialog, restorePresetsDialog;
    static void styleListAction(ui::CaptionButton *button);
    QPointer<ui::ChoiceModal> standbyDialog;
    QPointer<ui::ChoiceModal> automaticOpeningDialog;
    QHash<int, StyleSample> stylingSamples;
    // The open styling sheet, if any, and the option its preview shows.
    QPointer<QWidget> stylingPreview;
    std::function<void()> stylingSampleReady;
    AdvancedText stylingFeature = AdvancedText::WordSpacing;
    int stylingChoice = 0;
    void showStylingPreview();
    void fitStylingCard();
};
} // namespace kobalt::reader
