#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/style/theme.h"
#include "assets/assets.h"
#include "host_style.h"
#include "reading/reading_density.h"
#include "interfaces/reader/settings/settings_panel.h"
#include "interfaces/reader/settings/appearance_panel.h"
#include "interfaces/reader/views/opening.h"
#include "host_presentation.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/style/metrics.h"
#include <QDialog>
#include <QTabWidget>
#include "interfaces/reader/ui/style/fonts.h"
#include <QApplication>
#include <QFontInfo>
#include <QImage>
#include <QTest>
#include <QTimer>
#include <QDebug>

// This gallery exercises production controls. It does not emulate Nickel or
// claim reader/framebuffer parity. The full native reader is a separate target.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Kobalt native UI");
    kobalt::test::applyHostStyle();
    kobalt::AssetBundle assets(QStringLiteral(KOBALT_ASSET_ROOT), QStringLiteral(KOBALT_VERSION));
    if (!assets.available() || !kobalt::ui::initializeInterfaceFonts(assets)) {
        qCritical() << "Staged Kobalt assets failed verification";
        return 1;
    }
    namespace ui = kobalt::ui;
    ui::setDensity(300);
    ui::setInterfaceWeight(ui::heavierTextSteps);
    const QFont font = ui::interfaceFont(32);
    if (QFontInfo(font).family() != "Kobalt UI Sans" || QFontInfo(ui::chromeFont()).family() != "Kobalt UI Mono" ||
        QFontInfo(ui::brandFont(32)).family() != "Kobalt UI Serif") {
        qCritical() << "Unexpected font substitution:" << QFontInfo(font).family();
        return 1;
    }
    app.setFont(font);
    QWidget window;
    window.setWindowTitle("Kobalt · Native UI preview");
    window.resize(1264, 1680);
    QPalette palette;
    palette.setColor(QPalette::Window, Qt::white);
    palette.setColor(QPalette::WindowText, Qt::black);
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, Qt::black);
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, Qt::black);
    window.setPalette(palette);
    window.setAutoFillBackground(true);
    auto *windowRows = new QVBoxLayout(&window);
    auto *tabs = new QTabWidget(&window);
    windowRows->addWidget(tabs);
    auto *controls = new QWidget(tabs);
    tabs->addTab(controls, "Controls");
    auto *rows = new QVBoxLayout(controls);
    rows->setContentsMargins(28, 28, 28, 28);
    rows->setSpacing(18);
    auto *brand = new ui::BrandIcon(controls);
    brand->setFixedSize(56, 56);
    rows->addWidget(brand, 0, Qt::AlignHCenter);
    auto *title = new QLabel("Kobalt", controls);
    title->setFont(ui::brandFont(72));
    title->setAlignment(Qt::AlignCenter);
    rows->addWidget(title);
    auto *intro = new QLabel("Native Qt window with Kobalt's real controls and bundled fonts.", controls);
    intro->setWordWrap(true);
    rows->addWidget(intro);
    auto *option =
        new ui::DescribedOption("Page animations", "Preview control only. No device settings are changed.", controls);
    option->setObjectName("animation-option");
    rows->addWidget(option);
    auto *status = new QLabel("Animations off", controls);
    QObject::connect(option, &QCheckBox::toggled, status, [status](bool enabled) {
        status->setText(enabled ? "Animations on" : "Animations off");
    });
    rows->addWidget(status);
    int page = 1;
    auto *pageLabel = new QLabel("Page 1", controls);
    rows->addWidget(pageLabel);
    auto *next = ui::makeButton(controls, rows, "next-page", "Next page", [&] {
        pageLabel->setText(QString("Page %1").arg(++page));
    });
    ui::buttonIcon(next, ui::ControlIcon::Next);
    rows->addStretch();
    auto *note = new QLabel(
        "Component gallery with synthetic data. Use the native reader target for real EPUB rendering.", controls);
    note->setWordWrap(true);
    note->setFont(ui::captionFont(24));
    rows->addWidget(note);
    kobalt::reader::SettingsPanelInputs settingsInputs;
    settingsInputs.release = QStringLiteral(KOBALT_VERSION);
    settingsInputs.qtMajor = 6;
    settingsInputs.animationsSupported = true;
    settingsInputs.lightSupported = true;
    settingsInputs.warmthSupported = true;
    auto *settings = new kobalt::reader::SettingsPanel(tabs, settingsInputs);
    tabs->addTab(settings, "Settings");
    ui::readerStyle(settings, ui::styles::reader());
    auto *typography = new QWidget(tabs);
    auto *layout = new QWidget(tabs);
    auto *headerFooter = new QWidget(tabs);
    tabs->addTab(typography, "Typography");
    tabs->addTab(layout, "Page layout");
    tabs->addTab(headerFooter, "Header and footer");
    auto *typeRows = new QVBoxLayout(typography);
    auto *pageRows = new QVBoxLayout(layout);
    auto *headerFooterRows = new QVBoxLayout(headerFooter);
    for (auto *rows : {typeRows, pageRows, headerFooterRows}) {
        rows->setContentsMargins(ui::metrics::panel::contentMargins);
        rows->setSpacing(ui::metrics::control::rowGap);
    }
    kobalt::reader::AppearancePanelInputs appearanceInputs;
    kobalt::reader::AppearancePanel appearance({typography, layout, headerFooter, typeRows, pageRows, headerFooterRows},
                                               appearanceInputs);
    const int sampleHalfPoints = 21;
    appearance.sizeSlider->setValue(sampleHalfPoints);
    appearance.spacingSlider->setValue(120);
    appearance.marginSlider->setValue(48);
    auto *sample = new QLabel("Synthetic typography sample", typography);
    sample->setFont(ui::interfaceFont(kobalt::halfPointPixels(sampleHalfPoints)));
    sample->setStyleSheet(QString("font-size:%1px;").arg(kobalt::halfPointPixels(sampleHalfPoints)));
    typeRows->addWidget(sample);
    typeRows->addStretch();
    pageRows->addStretch();
    ui::readerStyle(typography, ui::styles::reader());
    ui::readerStyle(layout, ui::styles::reader());
    ui::readerStyle(headerFooter, ui::styles::reader());
    QObject::connect(appearance.sizeSlider, &QSlider::valueChanged, sample, [sample](int halfPoints) {
        sample->setFont(ui::interfaceFont(kobalt::halfPointPixels(halfPoints)));
        sample->setStyleSheet(QString("font-size:%1px;").arg(kobalt::halfPointPixels(halfPoints)));
    });
    std::unique_ptr<kobalt::ReaderOpeningView> opening;
    auto showOpening = [&](bool failure) {
        opening = kobalt::createOpening(&window, {}, kobalt::test::makeHostPresentation);
        if (failure)
            opening->showFallback(kobalt::OpeningFallback::Failure);
        opening->window()->show();
    };
    auto *openingRow = new QHBoxLayout;
    rows->addLayout(openingRow);
    ui::makeButton(controls, openingRow, "gallery-opening", "Opening", [&] {
        showOpening(false);
    });
    ui::makeButton(controls, openingRow, "gallery-failure", "Opening failure", [&] {
        showOpening(true);
    });
    window.show();
    if (app.arguments().contains("--smoke")) {
        QTimer::singleShot(100, &window, [&] {
            QTest::mouseClick(option, Qt::LeftButton, {}, option->rect().center());
            QTest::mouseClick(next, Qt::LeftButton, {}, next->rect().center());
            tabs->setCurrentWidget(settings);
            app.processEvents();
            QTest::mouseClick(settings->darkOption, Qt::LeftButton, {}, settings->darkOption->rect().center());
            const bool settingsPassed = settings->darkOption->isChecked();
            tabs->setCurrentWidget(typography);
            app.processEvents();
            auto *larger = typography->findChild<QPushButton *>("rrLarger");
            if (!larger) {
                app.exit(1);
                return;
            }
            QTest::mouseClick(larger, Qt::LeftButton, {}, larger->rect().center());
            const bool appearancePassed = appearance.sizeSlider->value() == sampleHalfPoints + 2 &&
                                          sample->font().pixelSize() == kobalt::halfPointPixels(sampleHalfPoints + 2);
            showOpening(true);
            app.processEvents();
            auto *back = opening->window()->findChild<QPushButton *>("rrOpeningBack");
            const bool failurePassed = back && back->isVisible();
            if (back)
                QTest::mouseClick(back, Qt::LeftButton, {}, back->rect().center());
            const bool dismissed = !opening->window()->isVisible();
            opening.reset();
            QImage frame(window.size(), QImage::Format_ARGB32_Premultiplied);
            frame.fill(Qt::transparent);
            window.render(&frame);
            const bool passed = option->isChecked() && status->text() == "Animations on" && page == 2 &&
                                pageLabel->text() == "Page 2" && controls->rect().contains(next->geometry()) &&
                                settingsPassed && appearancePassed && failurePassed && dismissed &&
                                frame.save("native-ui-smoke.png");
            qInfo() << "Native UI interaction and render:" << (passed ? "PASS" : "FAIL");
            app.exit(passed ? 0 : 1);
        });
    }
    return app.exec();
}
