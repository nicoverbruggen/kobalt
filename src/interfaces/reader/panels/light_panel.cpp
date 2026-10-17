#include "interfaces/reader/ui/controls/sliders.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/options.h"
#include "support/strings.h"
#include "light_panel.h"
#include "platform/api/light_controls.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include <QHideEvent>
#include <QHBoxLayout>
#include <QPainter>
#include "interfaces/reader/ui/controls/icons.h"
#include <utility>

namespace kobalt {
namespace m = ui::metrics;
namespace {
class SwipeBand : public QWidget {
public:
    explicit SwipeBand(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(ui::darkTheme(this) ? m::light::hintPaperDark : m::light::hintPaperLight));
    }
};
class SwipeIcon : public QWidget {
public:
    explicit SwipeIcon(QWidget *parent) : QWidget(parent) {
        setFixedSize(m::text::heading, m::text::heading);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        ui::paintOpeningIcon(painter, ui::ControlIcon::Swipe, rect(), palette().color(QPalette::WindowText));
    }
};
}
LightPanel::LightPanel(LightControls &light, QWidget *parent) : ui::Panel(parent, Qt::BottomEdge), light(light) {
    setObjectName("rrLightPanel");
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, m::control::border, 0, 0);
    outer->setSpacing(0);
    auto *controls = new QWidget(this);
    auto *rows = new QVBoxLayout(controls);
    outer->addWidget(controls);
    rows->setContentsMargins(m::panel::contentMargins);
    rows->setSpacing(m::control::rowGap);
    automaticBrightness = new ui::DescribedOption(strings::get("light_panel.automatic_brightness"),
                                                  strings::get("light_panel.automatic_brightness_description"), this);
    automaticBrightness->setObjectName("rrAutomaticBrightness");
    automaticWarmth = new ui::DescribedOption(strings::get("light_panel.automatic_warmth"),
                                              strings::get("light_panel.automatic_warmth_description"), this);
    automaticWarmth->setObjectName("rrAutomaticWarmth");
    for (auto *option : {automaticBrightness, automaticWarmth}) {
        auto presentation = option->presentation();
        presentation.indicator = ui::DescribedOption::Indicator::Square;
        presentation.labelPixels = m::chrome::labelText;
        presentation.descriptionPixels = m::chrome::descriptionText;
        presentation.indicatorSize = {m::chrome::checkSize, m::chrome::checkSize};
        presentation.leadingIndicator = true;
        presentation.minimumRowHeight = m::control::stepSize;
        option->setPresentation(presentation);
    }
    connect(automaticBrightness, &QCheckBox::clicked, this, [this](bool enabled) {
        this->light.setAutomaticBrightness(enabled);
        refreshValues();
    });
    connect(automaticWarmth, &QCheckBox::clicked, this, [this](bool enabled) {
        this->light.setAutomaticWarmth(enabled);
        refreshValues();
    });
    brightnessSlider =
        ui::sliderRow(this, rows, "rrBrightness", "rrBrightnessValue", strings::get("light_panel.brightness"),
                      "rrBrightnessLess", "rrBrightnessMore", 0, 100, 1, {}, {}, "%");
    brightnessSlider->setMarkers({0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100});
    warmthSlider = ui::sliderRow(this, rows, "rrWarmth", "rrWarmthValue", strings::get("light_panel.warmth"),
                                 "rrWarmthLess", "rrWarmthMore", 0, 100, 10, {}, {}, "%");
    warmthSlider->setMarkers({0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}, ui::SettingSlider::MarkerMode::Stops);
    brightnessValue = findChild<QLabel *>("rrBrightnessValue");
    warmthValue = findChild<QLabel *>("rrWarmthValue");
    warmthRow = warmthSlider->parentWidget();
    automaticRows = new QWidget(controls);
    auto *automaticSection = new QVBoxLayout(automaticRows);
    automaticSection->setContentsMargins(0, m::control::rowGap, 0, 0);
    automaticSection->setSpacing(0);
    auto *automaticPanel = new ui::Panel(automaticRows);
    automaticPanel->setDividers(Qt::TopEdge);
    automaticSection->addWidget(automaticPanel);
    auto *automaticLayout = new QVBoxLayout(automaticPanel);
    automaticLayout->setContentsMargins(0, m::control::rowGap, 0, 0);
    automaticLayout->setSpacing(0);
    automaticLayout->addWidget(automaticBrightness);
    automaticLayout->addWidget(automaticWarmth);
    rows->addWidget(automaticRows);
    swipeTip = new QLabel(this);
    swipeTip->setObjectName("rrLightSwipeTip");
    swipeTip->setWordWrap(true);
    ui::readerStyle(swipeTip, ui::styles::font(m::text::secondary) +
                                  "font-style:italic;background:transparent;color:palette(placeholder-text);");
    swipeRow = new SwipeBand(this);
    swipeRow->setObjectName("rrLightSwipeBand");
    ui::readerStyle(swipeRow, "QWidget{background:transparent;}");
    auto *tipLayout = new QHBoxLayout(swipeRow);
    tipLayout->setContentsMargins(m::panel::contentMargins.left(), m::panel::contentMargins.top() + ui::px(4),
                                  m::panel::contentMargins.right(), m::panel::contentMargins.bottom());
    tipLayout->setSpacing(m::spacing::row);
    tipLayout->addWidget(new SwipeIcon(swipeRow), 0, Qt::AlignTop);
    tipLayout->addWidget(swipeTip, 1);
    outer->insertWidget(0, swipeRow);
    // Both automatic settings belong to Nickel. Bedtime stays in Nickel's
    // settings. Moving a slider turns its automatic setting off.
    // Brightness changes reach the device while dragging. Native persistence
    // runs on release; interrupted drags restore the original level.
    connect(brightnessSlider, &QSlider::valueChanged, this, [this](int value) {
        brightnessValue->setText(QString::number(value) + '%');
        if (!updating && this->light.beginGesture()) {
            this->light.setGestureLevel(value);
            automaticBrightness->setChecked(this->light.automaticBrightness());
        }
    });
    brightnessSlider->committed = [this](int) {
        this->light.finishGesture(true);
        refreshValues();
    };
    connect(warmthSlider, &QSlider::valueChanged, this, [this](int value) {
        if (!updating) {
            if (warmthOriginal < 0) {
                warmthOriginal = this->light.warmth();
                warmthAutomatic = this->light.automaticWarmth();
            }
            const int minimum = this->light.warmthMinimum(), range = this->light.warmthMaximum() - minimum;
            this->light.setWarmth(minimum + qRound(range * value / 100.0));
            automaticWarmth->setChecked(this->light.automaticWarmth());
        }
    });
    warmthSlider->committed = [this](int) {
        finishWarmth(true);
        refreshValues();
    };
    connect(&poll, &QTimer::timeout, this, [this] {
        refreshValues();
    });
    poll.setInterval(m::light::pollMilliseconds);
    for (auto *widget : findChildren<QWidget *>())
        platform::textRefresh(widget);
    platform::textRefresh(this);
    hide();
}
LightPanel::~LightPanel() {
    poll.stop();
    light.finishGesture(false);
    finishWarmth(false);
}
bool LightPanel::open() {
    if (!light.initialize())
        return false;
    refreshValues();
    layoutPanel();
    show();
    raise();
    poll.start();
    if (changed)
        changed();
    return true;
}
void LightPanel::setEdgeGesturesEnabled(bool enabled) {
    if (edgeGesturesEnabled == enabled) {
        return;
    }
    edgeGesturesEnabled = enabled;
    swipeRow->setVisible(enabled);
    layoutPanel();
}
void LightPanel::layoutPanel() {
    layout()->activate();
    const int width = parentWidget()->width();
    const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : sizeHint().height();
    const QRect bounds = placement ? placement(height) : QRect(0, m::panel::toolbarHeight, width, height);
    const bool moved = geometry() != bounds;
    setGeometry(bounds);
    if (moved && isVisible() && changed)
        changed();
}
void LightPanel::refreshValues() {
    updating = true;
    const bool sensor = light.hasAutomaticBrightness();
    automaticBrightness->setVisible(sensor);
    if (!brightnessSlider->interacting() && light.gesturing())
        light.finishGesture(false);
    if (!warmthSlider->interacting())
        finishWarmth(false);
    automaticBrightness->setChecked(light.automaticBrightness());
    if (!brightnessSlider->interacting())
        brightnessSlider->setValue(light.brightness());
    brightnessValue->setText(QString::number(brightnessSlider->value()) + '%');
    const bool warmth = light.hasWarmth();
    warmthRow->setVisible(warmth);
    automaticWarmth->setVisible(warmth);
    automaticWarmth->setChecked(light.automaticWarmth());
    automaticRows->setVisible(sensor || warmth);
    swipeTip->setText(strings::get(warmth ? "light_panel.swipe_hint" : "light_panel.brightness_swipe_hint"));
    if (warmth) {
        if (!warmthSlider->interacting()) {
            const int minimum = light.warmthMinimum(), range = light.warmthMaximum() - minimum;
            const int percent = range > 0 ? qRound(10.0 * (light.warmth() - minimum) / range) * 10 : 0;
            warmthSlider->setValue(qBound(0, percent, 100));
        }
        // setValue does not emit when the current value is unchanged.
        warmthValue->setText(QString::number(warmthSlider->value()) + '%');
    }
    updating = false;
    layoutPanel();
}
void LightPanel::finishWarmth(bool save) {
    if (warmthOriginal < 0)
        return;
    const int original = std::exchange(warmthOriginal, -1);
    if (save)
        light.saveWarmth();
    else {
        // The native slider disables automatic warmth on manual adjustment.
        // A cancelled touch must restore both the light and that preference.
        light.setWarmth(original);
        if (warmthAutomatic)
            light.setAutomaticWarmth(true);
    }
}
void LightPanel::hideEvent(QHideEvent *event) {
    updating = true;
    poll.stop();
    light.finishGesture(false);
    finishWarmth(false);
    ui::Panel::hideEvent(event);
    if (changed)
        changed();
}
}
