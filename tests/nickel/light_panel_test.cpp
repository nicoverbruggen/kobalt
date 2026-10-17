#include "interfaces/reader/panels/light_panel.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "platform/api/light_controls.h"

#include <QCheckBox>
#include <QFontMetrics>
#include <QImage>
#include "interfaces/reader/ui/controls/buttons.h"
#include <QLabel>
#include <QSlider>
#include <QStyle>
#include <QTest>

namespace kobalt::platform {
void textRefresh(QWidget *) {}
}
class Light final : public kobalt::LightControls {
public:
    bool supported = true, automatic = true, saveAllowed = true;
    int changes = 0, level = 50, original = -1;
    bool originalAutomatic = false, warmthEnabled = false;
    int warmthLevel = 0, warmthMin = 0, warmthMax = 100, warmthIncrement = 1;
    int roundWarmthTo = 1, roundBrightnessTo = 1;
    bool warmthAutomatic = true;
    int warmthChanges = 0, warmthSaves = 0;
    bool initialize() override {
        return true;
    }
    bool hasAutomaticBrightness() const override {
        return supported;
    }
    bool automaticBrightness() const override {
        return automatic;
    }
    bool setAutomaticBrightness(bool value) override {
        ++changes;
        if (!saveAllowed) {
            return false;
        }
        automatic = value;
        return true;
    }
    bool hasWarmth() const override {
        return warmthEnabled;
    }
    int warmth() const override {
        return warmthLevel;
    }
    int warmthMinimum() const override {
        return warmthMin;
    }
    int warmthMaximum() const override {
        return warmthMax;
    }
    int warmthStep() const override {
        return warmthIncrement;
    }
    bool automaticWarmth() const override {
        return warmthAutomatic;
    }
    int setWarmth(int value) override {
        if (warmthAutomatic && !setAutomaticWarmth(false)) {
            return -1;
        }
        return warmthLevel = qBound(
                   warmthMin, warmthMin + qRound(double(value - warmthMin) / roundWarmthTo) * roundWarmthTo, warmthMax);
    }
    bool saveWarmth() override {
        ++warmthSaves;
        return saveAllowed;
    }
    bool setAutomaticWarmth(bool value) override {
        ++warmthChanges;
        if (!saveAllowed) {
            return false;
        }
        warmthAutomatic = value;
        return true;
    }
    bool beginGesture() override {
        if (original < 0) {
            original = level;
            originalAutomatic = automatic;
        }
        return true;
    }
    int setGestureLevel(int value) override {
        if (automatic && !setAutomaticBrightness(false)) {
            return -1;
        }
        return level = qRound(double(value) / roundBrightnessTo) * roundBrightnessTo;
    }
    void finishGesture(bool save) override {
        if (original < 0) {
            return;
        }
        if (!save) {
            level = original;
            automatic = originalAutomatic;
        }
        original = -1;
    }
    int brightness() const override {
        return level;
    }
    bool gesturing() const override {
        return original >= 0;
    }
};
class LightPanelTest : public QObject {
    Q_OBJECT
private slots:
    void roundedNativeLevelsDoNotAddErrorRow() {
        QWidget parent;
        parent.resize(1072, 1448);
        parent.show();
        Light light;
        light.warmthEnabled = true;
        light.roundWarmthTo = light.roundBrightnessTo = 5;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        const QRect before = panel.geometry();
        panel.findChild<QSlider *>("rrWarmth")->setValue(32);
        QCOMPARE(light.warmthLevel, 30);
        QVERIFY(!panel.findChild<QLabel *>("rrLightError"));
        QCOMPARE(panel.geometry(), before);
        panel.findChild<QSlider *>("rrBrightness")->setValue(53);
        QCOMPARE(light.level, 55);
        QVERIFY(!panel.findChild<QLabel *>("rrLightError"));
        QCOMPARE(panel.geometry(), before);
    }
    void lightMarkersUsePercentageStops() {
        QWidget parent;
        parent.resize(1072, 1448);
        parent.show();
        Light light;
        light.warmthEnabled = true;
        light.warmthMin = light.warmthLevel = 1000;
        light.warmthMax = 6000;
        light.warmthIncrement = 500;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *warmth = dynamic_cast<kobalt::ui::SettingSlider *>(panel.findChild<QSlider *>("rrWarmth"));
        auto *brightness = dynamic_cast<kobalt::ui::SettingSlider *>(panel.findChild<QSlider *>("rrBrightness"));
        QVERIFY(warmth && brightness);
        QCOMPARE(warmth->minimum(), 0);
        QCOMPARE(warmth->maximum(), 100);
        QCOMPARE(warmth->singleStep(), 10);
        const auto verifyMarkers = [](kobalt::ui::SettingSlider *slider) {
            const auto image = slider->grab().toImage();
            const int inset = kobalt::ui::metrics::control::sliderInset;
            for (int index = 0; index <= 10; ++index) {
                const int x = inset + QStyle::sliderPositionFromValue(0, 10, index, slider->width() - 2 * inset);
                QCOMPARE(image.pixelColor(x, kobalt::ui::metrics::control::sliderMarkerTop + 1),
                         slider->palette().color(QPalette::WindowText));
            }
        };
        verifyMarkers(warmth);
        verifyMarkers(brightness);
        warmth->nudge(1);
        QCOMPARE(light.warmthLevel, 1500);
        QCOMPARE(panel.findChild<QLabel *>("rrWarmthValue")->text(), QString("10%"));
        brightness->nudge(1);
        QCOMPARE(light.level, 51);
    }
    void warmthDragKeepsTenPercentStopsAndStableGeometry() {
        QWidget parent;
        parent.resize(1072, 1448);
        parent.show();
        Light light;
        light.warmthEnabled = true;
        light.warmthMin = light.warmthLevel = 1000;
        light.warmthMax = 6000;
        light.roundWarmthTo = 200;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *slider = panel.findChild<QSlider *>("rrWarmth");
        const QRect bounds = panel.geometry();
        const int inset = kobalt::ui::metrics::control::sliderInset, span = slider->width() - 2 * inset;
        const auto point = [&](int percent) {
            return QPoint(inset + qRound(span * percent / 100.0), slider->height() / 2);
        };
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, point(34));
        QCOMPARE(slider->value(), 30);
        QCOMPARE(light.warmthLevel, 2600);
        QCOMPARE(panel.findChild<QLabel *>("rrWarmthValue")->text(), QString("30%"));
        QVERIFY(!panel.findChild<QLabel *>("rrLightError"));
        QCOMPARE(panel.geometry(), bounds);
        QTest::mouseMove(slider, point(46));
        QCOMPARE(slider->value(), 50);
        QCOMPARE(light.warmthLevel, 3600);
        QCOMPARE(panel.findChild<QLabel *>("rrWarmthValue")->text(), QString("50%"));
        QCOMPARE(panel.geometry(), bounds);
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, point(46));
        QCOMPARE(slider->value(), 50);
        QCOMPARE(panel.geometry(), bounds);
    }
    void thinBordersFollowDensity_data() {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<int>("border");
        QTest::newRow("167-ppi") << 167 << 1;
        QTest::newRow("200-ppi") << 200 << 1;
        QTest::newRow("nia") << 212 << 1;
        QTest::newRow("below-rounding-boundary") << 224 << 1;
        QTest::newRow("rounding-boundary") << 225 << 2;
        QTest::newRow("elipsa") << 227 << 2;
        QTest::newRow("aura-hd") << 265 << 2;
        QTest::newRow("invalid-density-fallback") << 0 << 2;
        QTest::newRow("300-ppi") << 300 << 2;
    }
    void thinBordersFollowDensity() {
        QFETCH(int, ppi);
        QFETCH(int, border);
        const int previous = kobalt::ui::density();
        struct RestoreDensity {
            int value;
            ~RestoreDensity() {
                kobalt::ui::setDensity(value);
            }
        } restore{previous};
        kobalt::ui::setDensity(ppi == 300 ? 212 : 300);
        kobalt::ui::Panel panel(nullptr,
                                kobalt::ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge);
        kobalt::ui::setDensity(ppi);
        QCOMPARE(int(kobalt::ui::metrics::control::border), border);
        panel.resize(40, 30);
        auto colors = panel.palette();
        colors.setColor(QPalette::Window, Qt::white);
        colors.setColor(QPalette::WindowText, Qt::black);
        panel.setPalette(colors);
        const auto image = panel.grab().toImage();
        for (int edge = 0; edge < border; ++edge) {
            QCOMPARE(image.pixelColor(20, edge), QColor(Qt::black));
            QCOMPARE(image.pixelColor(20, 29 - edge), QColor(Qt::black));
            QCOMPARE(image.pixelColor(edge, 15), QColor(Qt::black));
            QCOMPARE(image.pixelColor(39 - edge, 15), QColor(Qt::black));
        }
        QCOMPARE(image.pixelColor(20, border), QColor(Qt::white));
        kobalt::ui::LightSwipeNotice notice(nullptr);
        notice.setPalette(colors);
        notice.resize(kobalt::ui::metrics::lightSwipe::width, kobalt::ui::metrics::lightSwipe::height);
        const auto noticeImage = notice.grab().toImage();
        const int noticeBorder = kobalt::ui::metrics::chrome::heavyRule;
        for (int edge = 0; edge < noticeBorder; ++edge) {
            QCOMPARE(noticeImage.pixelColor(notice.width() / 2, edge), QColor(Qt::black));
            QCOMPARE(noticeImage.pixelColor(edge, notice.height() / 2), QColor(Qt::black));
        }
        QCOMPARE(noticeImage.pixelColor(notice.width() / 2, noticeBorder), QColor(Qt::white));
        kobalt::ui::StepButton step(kobalt::ui::ControlIcon::Plus, nullptr);
        step.setPalette(colors);
        const auto buttonImage = step.grab().toImage();
        for (int edge = 0; edge < border; ++edge)
            QCOMPARE(buttonImage.pixelColor(step.width() / 2, edge), QColor(Qt::black));
        QCOMPARE(buttonImage.pixelColor(step.width() / 2, border), QColor(Qt::white));
    }
    void automaticControlFitsDensity_data() {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<int>("width");
        QTest::newRow("300-ppi") << 300 << 1072;
        QTest::newRow("212-ppi") << 212 << 758;
    }
    void automaticControlFitsDensity() {
        QFETCH(int, ppi);
        QFETCH(int, width);
        const int previousDensity = kobalt::ui::density();
        struct RestoreDensity {
            int value;
            ~RestoreDensity() {
                kobalt::ui::setDensity(value);
            }
        } restore{previousDensity};
        kobalt::ui::setDensity(ppi);
        QWidget parent;
        parent.resize(width, 1448);
        parent.show();
        Light light;
        light.warmthEnabled = true;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        for (const auto *name : {"rrAutomaticBrightness", "rrAutomaticWarmth"}) {
            auto *option = dynamic_cast<kobalt::ui::DescribedOption *>(panel.findChild<QCheckBox *>(name));
            QVERIFY(option && option->isVisible());
            QCOMPARE(option->presentation().indicator, kobalt::ui::DescribedOption::Indicator::Square);
            QVERIFY(option->presentation().leadingIndicator);
            QCOMPARE(option->presentation().labelPixels,
                     panel.findChild<QLabel *>("rrBrightnessCaption")->font().pixelSize());
            QCOMPARE(option->presentation().labelPixels, int(kobalt::ui::metrics::chrome::labelText));
            QVERIFY(panel.rect().contains(QRect(option->mapTo(&panel, QPoint()), option->size())));
            QVERIFY(option->height() >= option->heightForWidth(option->width()));
        }
        const auto *brightness = panel.findChild<QCheckBox *>("rrAutomaticBrightness");
        const auto *warmth = panel.findChild<QCheckBox *>("rrAutomaticWarmth");
        QCOMPARE(warmth->mapTo(&panel, QPoint()).y(), brightness->mapTo(&panel, QPoint()).y() + brightness->height());
        auto *tip = panel.findChild<QLabel *>("rrLightSwipeTip");
        QVERIFY(tip->wordWrap());
        QVERIFY(tip->font().italic());
        QCOMPARE(tip->font().pixelSize(), int(kobalt::ui::metrics::text::secondary));
        QVERIFY(tip->height() >= tip->heightForWidth(tip->width()));
        QVERIFY(panel.rect().contains(QRect(tip->mapTo(&panel, QPoint()), tip->size())));
    }
    void swipeHintFitsSixInch_data() {
        QTest::addColumn<int>("ppi");
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("warmth");
        QTest::newRow("300-ppi-both-edges") << 300 << 1072 << true;
        QTest::newRow("300-ppi-left-edge") << 300 << 1072 << false;
        QTest::newRow("212-ppi-both-edges") << 212 << 758 << true;
        QTest::newRow("212-ppi-left-edge") << 212 << 758 << false;
    }
    void swipeHintFitsSixInch() {
        QFETCH(int, ppi);
        QFETCH(int, width);
        QFETCH(bool, warmth);
        struct RestoreDensity {
            int value = kobalt::ui::density();
            ~RestoreDensity() {
                kobalt::ui::setDensity(value);
            }
        } restore;
        kobalt::ui::setDensity(ppi);
        QWidget parent;
        parent.resize(width, 1448);
        parent.show();
        Light light;
        light.warmthEnabled = warmth;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        const auto *tip = panel.findChild<QLabel *>("rrLightSwipeTip");
        QVERIFY(tip);
        QVERIFY2(QFontMetrics(tip->font()).horizontalAdvance(tip->text()) <= tip->width(), qPrintable(tip->text()));
        QCOMPARE(tip->text().contains("left edge"), !warmth);
        QCOMPARE(tip->text().contains("edges"), warmth);
    }
    void externalSettingUpdatesWithoutWriting() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *option = panel.findChild<QCheckBox *>("rrAutomaticBrightness");
        light.automatic = false;
        QTRY_VERIFY(!option->isChecked());
        QCOMPARE(light.changes, 0);
    }
    void swipeHintPlacementAndVisibility() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *tip = panel.findChild<QLabel *>("rrLightSwipeTip");
        auto *brightness = panel.findChild<QSlider *>("rrBrightness");
        QVERIFY(tip->isVisible());
        QVERIFY(tip->mapTo(&panel, tip->rect().bottomLeft()).y() <
                brightness->parentWidget()->mapTo(&panel, QPoint()).y());
        panel.setEdgeGesturesEnabled(false);
        QVERIFY(!tip->isVisible());
        panel.hide();
        QVERIFY(panel.open());
        QVERIFY(!tip->isVisible());
        panel.setEdgeGesturesEnabled(true);
        QVERIFY(tip->isVisible());
    }
    void capabilityAndNativeState() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *option = panel.findChild<QCheckBox *>("rrAutomaticBrightness");
        QVERIFY(option && option->isVisible() && option->isChecked());
        QCOMPARE(light.changes, 0);
        option->click();
        QVERIFY(!light.automatic && !option->isChecked());
        option->click();
        QVERIFY(light.automatic && option->isChecked());
        QCOMPARE(light.changes, 2);
        panel.hide();
        light.supported = false;
        QVERIFY(panel.open());
        QVERIFY(!option->isVisible());
    }
    void failedToggleRetainsStateWithoutErrorRow() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        light.saveAllowed = false;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *option = panel.findChild<QCheckBox *>("rrAutomaticBrightness");
        option->click();
        QVERIFY(option->isChecked());
        QVERIFY(!panel.findChild<QLabel *>("rrLightError"));
        QCOMPARE(light.level, 50);
    }
    void manualSliderAndCancellation() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        panel.findChild<QSlider *>("rrBrightness")->setValue(70);
        QVERIFY(!light.automatic);
        QVERIFY(!panel.findChild<QCheckBox *>("rrAutomaticBrightness")->isChecked());
        QCOMPARE(light.level, 70);
        panel.hide();
        QVERIFY(light.automatic);
        QCOMPARE(light.level, 50);
        QVERIFY(panel.open());
        QVERIFY(panel.findChild<QCheckBox *>("rrAutomaticBrightness")->isChecked());
    }
    void warmthSettingIsInheritedOnFirstOpen_data() {
        QTest::addColumn<bool>("automatic");
        QTest::newRow("native-schedule-off") << false;
        QTest::newRow("native-schedule-on") << true;
    }
    void warmthSettingIsInheritedOnFirstOpen() {
        QFETCH(bool, automatic);
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        light.supported = false;
        light.warmthEnabled = true;
        light.warmthAutomatic = automatic;
        light.warmthLevel = 40;
        {
            kobalt::LightPanel panel(light, &parent);
            QCOMPARE(light.warmthChanges, 0);
            QVERIFY(panel.open());
            auto *option = panel.findChild<QCheckBox *>("rrAutomaticWarmth");
            QVERIFY(option && option->isVisible());
            QCOMPARE(option->isChecked(), automatic);
            QVERIFY(!panel.findChild<QCheckBox *>("rrAutomaticBrightness")->isVisible());
            QCOMPARE(light.warmthChanges, 0);
        }
        QCOMPARE(light.warmthAutomatic, automatic);
        QCOMPARE(light.warmthLevel, 40);
        QCOMPARE(light.warmthChanges, 0);
        QCOMPARE(light.warmthSaves, 0);
    }
    void warmthTogglePersistsAcrossPanels() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        light.warmthEnabled = true;
        {
            kobalt::LightPanel panel(light, &parent);
            QVERIFY(panel.open());
            auto *option = panel.findChild<QCheckBox *>("rrAutomaticWarmth");
            option->click();
            QVERIFY(!light.warmthAutomatic && !option->isChecked());
            option->click();
            QVERIFY(light.warmthAutomatic && option->isChecked());
            option->click();
        }
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        QVERIFY(!panel.findChild<QCheckBox *>("rrAutomaticWarmth")->isChecked());
        QCOMPARE(light.warmthChanges, 3);
        QCOMPARE(light.changes, 0);
        QVERIFY(light.automatic);
        QCOMPARE(light.warmthSaves, 0);
    }
    void warmthCapabilityAndExternalChanges() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *option = panel.findChild<QCheckBox *>("rrAutomaticWarmth");
        QVERIFY(option && !option->isVisible());
        light.warmthEnabled = true;
        QTRY_VERIFY(option->isVisible());
        QVERIFY(option->isChecked());
        light.warmthAutomatic = false;
        QTRY_VERIFY(!option->isChecked());
        light.warmthAutomatic = true;
        light.warmthLevel = 60;
        QTRY_VERIFY(option->isChecked());
        QTRY_COMPARE(panel.findChild<QSlider *>("rrWarmth")->value(), 60);
        QCOMPARE(light.warmthChanges, 0);
        QCOMPARE(light.warmthSaves, 0);
    }
    void failedWarmthToggleRetainsNativeState() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        light.warmthEnabled = true;
        light.saveAllowed = false;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *option = panel.findChild<QCheckBox *>("rrAutomaticWarmth");
        option->click();
        QVERIFY(option->isChecked());
        QVERIFY(light.warmthAutomatic);
        QCOMPARE(light.warmthChanges, 1);
        QCOMPARE(light.warmthSaves, 0);
        QCOMPARE(light.changes, 0);
    }
    void manualWarmthDisablesScheduleAndCancellationRestoresIt() {
        QWidget parent;
        parent.resize(768, 1024);
        parent.show();
        Light light;
        light.warmthEnabled = true;
        light.warmthLevel = 40;
        kobalt::LightPanel panel(light, &parent);
        QVERIFY(panel.open());
        auto *option = panel.findChild<QCheckBox *>("rrAutomaticWarmth");
        auto *slider = dynamic_cast<kobalt::ui::SettingSlider *>(panel.findChild<QSlider *>("rrWarmth"));
        QVERIFY(slider);
        slider->setValue(70);
        QVERIFY(!option->isChecked());
        QVERIFY(!light.warmthAutomatic);
        QCOMPARE(light.warmthLevel, 70);
        panel.hide();
        QVERIFY(light.warmthAutomatic);
        QCOMPARE(light.warmthLevel, 40);
        QCOMPARE(light.warmthSaves, 0);
        QVERIFY(panel.open());
        QVERIFY(option->isChecked());
        slider->setValue(70);
        slider->committed(70);
        panel.hide();
        QVERIFY(!light.warmthAutomatic);
        QCOMPARE(light.warmthLevel, 70);
        QCOMPARE(light.warmthSaves, 1);
        QVERIFY(panel.open());
        QVERIFY(!option->isChecked());
        QVERIFY(light.automatic);
        option->click();
        panel.findChild<QSlider *>("rrBrightness")->setValue(60);
        QCOMPARE(light.warmthChanges, 4);
        QVERIFY(light.warmthAutomatic);
        QVERIFY(!light.automatic);
    }
};
QTEST_MAIN(LightPanelTest)
#include "light_panel_test.moc"
