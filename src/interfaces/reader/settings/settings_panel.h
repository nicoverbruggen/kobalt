#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "dictionaries_page.h"
#include <QStackedWidget>
#include <QVector>
#include <QPointer>

namespace kobalt::reader {
enum SettingsPage {
    DisplaySettings,
    AdvancedSettings,
    DictionariesSettings,
    StatsSettings,
    AboutSettings,
    NativeUiSettings
};
struct SettingsPanelInputs {
    QString release, commit;
    int qtMajor = 0;
    bool commitVisible = false;
    bool animationsSupported = false, lightSupported = false, warmthSupported = false;
    bool debugProgressAvailable = false, desktopUi = false;
    bool downloadableDictionaries = true;
};
// Qt parents own every widget. Reader installs policy callbacks after construction.
class SettingsPanel final : public ui::Panel {
public:
    SettingsPanel(QWidget *parent, const SettingsPanelInputs &inputs);
    ~SettingsPanel() override;
    void dismissRestore();
    void hideDeviceOptions(bool hidden);
    void showDictionaries(bool visible);
    int fitContent(QSize availableSize);
    ui::DescribedOption *hideStatusOption = nullptr, *hideDeviceOptionsOption = nullptr;
    QStackedWidget *settingsPages = nullptr;
    QVector<QPushButton *> settingsTabButtons;
    ui::DescribedOption *darkOption = nullptr, *smoothOption = nullptr, *progressOption = nullptr,
                        *animationOption = nullptr;
    ui::DescribedOption *edgeLightOption = nullptr, *denseGraphOption = nullptr, *pinStatsOption = nullptr,
                        *heavierTextOption = nullptr;
    ui::DescribedOption *linkIndicatorOption = nullptr, *hideNonlinearOption = nullptr;
    ui::DescribedOption *bionicOption = nullptr;
    ui::CaptionButton *refreshButton = nullptr, *resetDefaults = nullptr;
    QPushButton *keepDefaults = nullptr, *confirmDefaults = nullptr;
    QPushButton *openAcknowledgments = nullptr, *debugProgress = nullptr;
    DictionariesPage *dictionaries = nullptr;
    QLabel *settingsError = nullptr;

protected:
    void hideEvent(QHideEvent *event) override;

private:
    QWidget *tabRail = nullptr;
    QLabel *aboutDescription = nullptr;
    QPointer<ui::ChoiceModal> restoreDialog;
    QVector<QWidget *> deviceOptions;
};
}
