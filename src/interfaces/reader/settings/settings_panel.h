#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "dictionaries_page.h"
#include "kosync_page.h"
#include "assets/font_catalog.h"
#include "reading/style_sample.h"
#include <QHash>
#include <QStackedWidget>
#include <QVector>
#include <QPointer>

namespace kobalt::reader {
enum SettingsPage {
    DisplaySettings,
    ControlsSettings,
    DefaultsSettings,
    StylingSettings,
    DictionariesSettings,
    SystemSettings,
    AboutSettings,
    KosyncSettings
};
struct SettingsPanelInputs {
    KeyboardFactory keyboard;
    QString release, commit;
    int qtMajor = 0;
    bool commitVisible = false;
    bool animationsSupported = false, lightSupported = false, warmthSupported = false;
    bool debugProgressAvailable = false;
    bool downloadableDictionaries = true;
    bool sleepScreenSupported = false;
    bool automaticBookOpeningSupported = false;
};
// Qt parents own every widget. Reader installs policy callbacks after construction.
class SettingsPanel final : public ui::Panel {
public:
    SettingsPanel(QWidget *parent, const SettingsPanelInputs &inputs);
    ~SettingsPanel() override;
    void dismissRestore();
    void showFontChoices(const QString &title, const QVector<ReadingFont> &fonts, const QString &current,
                         const QString &emptyLabel, std::function<void(const QString &)> chosen,
                         std::function<void(const QStringList &)> prepareFonts = {});
    void dismissFontChoices();
    void showHeaderFooterWeights(int current, const QFont &sampleFont, std::function<void(int)> chosen);
    void showDictionaryFontSizes(int current, const QString &family, bool customFont, std::function<void(int)> chosen);
    static QString dictionaryFontSizeLabel(int percent);
    void showColumnSpacingChoices(int current, std::function<void(int)> chosen);
    static QString columnSpacingLabel(int percent);
    enum class AdvancedText { WordSpacing, Tracking, Pagination, VerticalSpacing, VerticalSpacingScope, ParagraphSpacing };
    static QString paragraphSpacingLabel(int percent);
    void showBeautifier(const QJsonObject &current, std::function<void(QJsonObject)> chosen);
    void showParagraphAdjustments(int spacing, int indent, std::function<void(int, int)> chosen);
    // The sheet stays open while choosing, so each choice can be compared in
    // its preview.
    void showAdvancedTextChoices(AdvancedText feature, int current, std::function<void(int)> chosen,
                                 const QString &alignment = {});
    // Rendered sample pages for every option of one setting, shown when its
    // sheet is open.
    void setStylingSample(AdvancedText feature, const StyleSample &sample);
    void clearStylingSamples();
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
    void showDictionaries(bool visible);
    int fitContent(QSize availableSize);
    ui::DescribedOption *hideTabLabelsOption = nullptr;
    QStackedWidget *settingsPages = nullptr;
    QVector<QPushButton *> settingsTabButtons;
    ui::DescribedOption *darkOption = nullptr, *inheritTextLayoutOption = nullptr, *animationOption = nullptr;
    ui::DescribedOption *edgeLightOption = nullptr, *lightGestureFeedbackOption = nullptr, *denseGraphOption = nullptr;
    ui::DescribedOption *linkIndicatorOption = nullptr, *hideNonlinearOption = nullptr,
                        *avoidWastefulStylingOption = nullptr;
    ui::DescribedOption *automaticOpeningOption = nullptr, *minimalAnnotationOption = nullptr;
    ui::CaptionButton *defaultFontButton = nullptr, *uiFontButton = nullptr, *uiMonoFontButton = nullptr,
                      *headerFooterFontButton = nullptr, *headerFooterWeightButton = nullptr;
    ui::CaptionButton *serifFontButton = nullptr, *sansFontButton = nullptr, *monoFontButton = nullptr;
    ui::CaptionButton *dictionaryFontButton = nullptr, *dictionaryFontSizeButton = nullptr;
    ui::DescribedOption *landscapeColumnsOption = nullptr;
    ui::CaptionButton *columnSpacingButton = nullptr;
    ui::CaptionButton *verticalSpacingButton = nullptr, *verticalSpacingScopeButton = nullptr;
    ui::CaptionButton *wordSpacingButton = nullptr, *trackingButton = nullptr, *widowsOrphansButton = nullptr;
    ui::CaptionButton *paragraphSpacingButton = nullptr, *beautifierButton = nullptr;
    ui::CaptionButton *sleepScreenButton = nullptr, *sessionTimeoutButton = nullptr, *sleepTileButton = nullptr, *menuViewButton = nullptr;
    ui::CaptionButton *pageNavigationButton = nullptr, *tapZonesButton = nullptr, *navigationBarButton = nullptr,
                      *tappableLinksButton = nullptr;
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
                            const QString &alignment, int indent, std::function<void(int, int)> paragraphChosen);
    QWidget *tabRail = nullptr;
    QWidget *optionsViewport = nullptr, *optionsClip = nullptr;
    ui::PagerBar *optionsPager = nullptr;
    QVector<QPair<int, int>> optionPages;
    int optionPage = 0, pageHeight = 0;
    void showOptionsPage(int page);
    void fitRail(int availableHeight = 0);
    QLabel *aboutDescription = nullptr;
    QPointer<ui::ChoiceModal> restoreDialog;
    QHash<int, StyleSample> stylingSamples;
    // The open styling sheet, if any, and the option its preview shows.
    QPointer<QWidget> stylingPreview;
    std::function<void()> stylingSampleReady;
    AdvancedText stylingFeature = AdvancedText::WordSpacing;
    int stylingChoice = 0;
    void showStylingPreview();
};
}
