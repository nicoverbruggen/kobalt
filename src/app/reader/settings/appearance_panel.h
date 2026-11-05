#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/panels.h"
#include "ui/controls/sliders.h"
#include "header_footer_panel.h"
#include "ui/controls/options.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include <QStackedWidget>
#include <QVBoxLayout>

namespace kobalt::reader {
// The sheet's pages, in this order. Header and footer is a full-screen view;
// the reader supplies its rows in AppearanceLayout.
enum class AppearancePage { Typography, Layout };

struct AppearanceLayout
{
    QWidget *typography = nullptr, *pageLayout = nullptr, *headerFooter = nullptr;
    QVBoxLayout *typeRows = nullptr, *pageRows = nullptr, *headerFooterRows = nullptr;
};

struct AppearancePanelInputs
{
    bool phone = false;
    bool sideButtons = false, reverseLandscapeButtons = false;
    bool automaticRotationAvailable = false;
    bool systemRotation = false;
};

// Measure only the selected page. When it does not fit the available reader
// area, it pages between complete rows, as the Header and footer sheet does.
class AppearanceSheet final : public ui::Panel
{
public:
    explicit AppearanceSheet(QWidget *parent);
    AppearanceLayout controls() const;
    int fitHeight(int width, int availableHeight);
    QStackedWidget *pages;
    QLabel *message;

private:
    QWidget *clip;
    ui::PagerBar *pager;
    // The top and bottom of each page of rows, in the selected page's coordinates.
    QVector<QPair<int, int>> rowPages;
    int rowPage = 0, pagedIndex = -1;
    void showRows(int page);
};

class StripSizeControl final : public ui::ControlRow
{
public:
    StripSizeControl(QWidget *parent, bool footer);
    void setSize(int pixels, bool shown);
    ui::SettingSlider *slider;

private:
    void showValue(int value);
    QLabel *valueLabel;
};

// The existing pages own the controls. Reader keeps preview and apply policy.
class AppearancePanel
{
public:
    AppearancePanel(const AppearanceLayout &layout, const AppearancePanelInputs &inputs);
    ui::FontButton *fontButton = nullptr;
    ui::SettingSlider *opticalSizeSlider = nullptr, *sizeSlider = nullptr, *weightSlider = nullptr,
                      *variableWeightSlider = nullptr, *spacingSlider = nullptr,
                      *marginSlider = nullptr, *verticalMarginSlider = nullptr;
    QVector<QPushButton *> alignmentButtons, hyphenButtons, rotationButtons;
    QPushButton *autoRotateOption = nullptr;
    // Header and footer: the Layout row that opens its view, and the view's
    // presets and controls, built only when the layout has its rows.
    ui::DropdownButton *headerFooterRow = nullptr;
    SlotMap *slotMap = nullptr;
    QVector<PresetCard *> presetCards;
    StripSizeControl *headerSizeControl = nullptr, *footerSizeControl = nullptr;
    QVector<QPushButton *> progressBarButtons;
    QLabel *slotHint = nullptr;
};
} // namespace kobalt::reader
