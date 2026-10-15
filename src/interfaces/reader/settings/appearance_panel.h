#pragma once
#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/sliders.h"
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace kobalt::reader {
struct AppearanceLayout {
    QWidget *typography = nullptr, *pageLayout = nullptr;
    QVBoxLayout *typeRows = nullptr, *pageRows = nullptr;
};
struct AppearancePanelInputs {
    QString headerSize = "small", footerSize = "small";
    bool sideButtons = false, reverseLandscapeButtons = false;
    bool automaticRotationAvailable = false, showClock = true;
    bool systemRotation = false;
};
// Measure only the selected page. Overflow scrolls inside the available reader area.
class AppearanceSheet final : public ui::Panel {
public:
    explicit AppearanceSheet(QWidget *parent);
    AppearanceLayout controls() const;
    int fitHeight(int width, int availableHeight);
    QStackedWidget *pages;
    QLabel *message;

private:
    QScrollArea *scroll;
};
// The existing pages own the controls. Reader keeps preview and apply policy.
class AppearancePanel {
public:
    AppearancePanel(const AppearanceLayout &layout, const AppearancePanelInputs &inputs);
    ui::FontButton *fontButton = nullptr;
    ui::SettingSlider *opticalSizeSlider = nullptr, *sizeSlider = nullptr, *weightSlider = nullptr,
                      *variableWeightSlider = nullptr, *spacingSlider = nullptr, *marginSlider = nullptr,
                      *verticalMarginSlider = nullptr;
    QVector<QPushButton *> alignmentButtons, hyphenButtons, rotationButtons, headerSizeButtons, footerSizeButtons;
    QPushButton *autoRotateOption = nullptr, *clockOption = nullptr, *clockHideOption = nullptr;
};
}
