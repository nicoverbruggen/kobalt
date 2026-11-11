#pragma once
#include "platform/api/page_animation_support.h"
#include "platform/api/page_buttons.h"
#include "ui/controls/buttons.h"
#include "ui/controls/options.h"
#include "ui/controls/panels.h"
#include "ui/controls/paging.h"
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
// Settings tabs, in the order of the stacked pages.
enum class SettingsPage {
    Display,
    Controls,
    Defaults,
    Styling,
    Dictionaries,
    System,
    About,
    Kosync
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
    void showFontChoices(const QString &title, const QVector<ReadingFont> &fonts,
                         const QString &current, const QString &emptyLabel,
                         std::function<void(const QString &)> chosen);
    void dismissFontChoices();
    void showHeaderFooterWeights(int current, const QFont &sampleFont,
                                 std::function<void(int)> chosen);
    void showDictionaryFontSizes(int current, const QString &family, bool customFont,
                                 std::function<void(int)> chosen);
    static QString dictionaryFontSizeLabel(int percent, bool compact = false);
    void showTimeRemainingFormats(const QString &current,
                                  std::function<void(const QString &)> chosen);
    static QString timeRemainingFormatLabel(const QString &format);
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
    QPointer<ui::ModalOverlay> fontDialog;
    ui::CellButton *closeButton = nullptr;
    // Asks the reader to fit the panel again, for changes that do not switch
    // the page, such as returning to the section list.
    std::function<void()> refitRequested;
    void showDictionaries(bool visible);
    void showPowerManagement(bool visible);
    int fitContent(QSize availableSize);
    ui::DescribedOption *hideTabLabelsOption = nullptr;
    QWidget *powerSection = nullptr;
    ui::DescribedOption *wifiPowerSaverOption = nullptr;
    ui::DescribedOption *disableStandbyOption = nullptr;
    QStackedWidget *settingsPages = nullptr;
    QVector<QPushButton *> settingsTabButtons;
    ui::CaptionButton *disabledFeaturesButton = nullptr;
    QJsonObject featurePreferences;
    void showDisabledFeatures();
    void updateFeatureOptions();
    ui::DescribedOption *darkOption = nullptr, *inheritTextLayoutOption = nullptr,
                        *animationOption = nullptr;
    ui::DescribedOption *edgeLightOption = nullptr, *lightGestureFeedbackOption = nullptr,
                        *denseGraphOption = nullptr;
    ui::DescribedOption *hideNonlinearOption = nullptr;
    ui::DescribedOption *automaticOpeningOption = nullptr, *minimalAnnotationOption = nullptr;
    ui::CaptionButton *defaultFontButton = nullptr, *uiFontButton = nullptr,
                      *uiMonoFontButton = nullptr, *headerFooterFontButton = nullptr,
                      *headerFooterWeightButton = nullptr;
    ui::CaptionButton *serifFontButton = nullptr, *sansFontButton = nullptr,
                      *monoFontButton = nullptr;
    ui::CaptionButton *dictionaryFontButton = nullptr, *dictionaryFontSizeButton = nullptr;
    ui::DescribedOption *landscapeColumnsOption = nullptr;
    ui::CaptionButton *columnSpacingButton = nullptr, *timeRemainingFormatButton = nullptr;
    ui::CaptionButton *verticalSpacingButton = nullptr;
    ui::CaptionButton *wordSpacingButton = nullptr, *trackingButton = nullptr,
                      *hangingPunctuationButton = nullptr, *widowsOrphansButton = nullptr;
    ui::CaptionButton *paragraphSpacingButton = nullptr, *beautifierButton = nullptr;
    ui::CaptionButton *sleepScreenButton = nullptr, *sessionTimeoutButton = nullptr,
                      *sleepTileButton = nullptr, *menuViewButton = nullptr;
    ui::CaptionButton *buttonControlsButton = nullptr;
    ui::CaptionButton *pageNavigationButton = nullptr, *tapZonesButton = nullptr,
                      *navigationBarButton = nullptr, *tappableLinksButton = nullptr,
                      *chapterGroupsButton = nullptr;
    ui::DescribedOption *bionicOption = nullptr;
    ui::CaptionButton *refreshButton = nullptr, *resetDefaults = nullptr;
    QPushButton *keepDefaults = nullptr, *confirmDefaults = nullptr;
    QPushButton *openAcknowledgments = nullptr, *debugProgress = nullptr;
    DictionariesPage *dictionaries = nullptr;
    KosyncPage *kosync = nullptr;
    QLabel *settingsError = nullptr;

protected:
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void showStylingChoices(AdvancedText feature, int current, std::function<void(int)> chosen,
                            const QString &alignment, int secondary,
                            std::function<void(int, int)> pairChosen);
    QWidget *tabRail = nullptr, *optionsColumn = nullptr;
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
    int optionPage = 0, pageHeight = 0;
    void showOptionsPage(int page);
    void fitRail(int availableHeight = 0);
    AboutPage *aboutPage = nullptr;
    QPointer<ui::ChoiceModal> restoreDialog;
    QPointer<ui::ChoiceModal> standbyDialog;
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
