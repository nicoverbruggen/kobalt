#include "ui/controls/text.h"
#include "ui/controls/sliders.h"
#include "ui/controls/panels.h"
#include "ui/controls/options.h"
#include "ui/controls/rows.h"
#include "support/strings.h"
#include "light_panel.h"
#include "platform/api/light_controls.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include <QHideEvent>
#include <QHBoxLayout>
#include <QPainter>
#include "ui/controls/icons.h"
#include <utility>

namespace kobalt {
namespace metrics = ui::metrics;

namespace {
class SwipeBand : public QWidget
{
public:
    explicit SwipeBand(QWidget *parent) : QWidget(parent) { }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), ui::themeColors(this).lightHintPaper);
    }
};

class SwipeIcon : public QWidget
{
public:
    explicit SwipeIcon(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(metrics::text::heading, metrics::text::heading);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        ui::paintOpeningIcon(painter, ui::ControlIcon::Swipe, rect(),
                             palette().color(QPalette::WindowText));
    }
};
} // namespace

LightPanel::LightPanel(LightControls &light, QWidget *parent)
    : ui::Panel(parent, Qt::BottomEdge), light(light)
{
    setObjectName("rrLightPanel");
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, metrics::control::border, 0, 0);
    outer->setSpacing(0);
    auto *controls = new QWidget(this);
    auto *rows = new QVBoxLayout(controls);
    outer->addWidget(controls);
    rows->setContentsMargins(metrics::panel::contentMargins);
    rows->setSpacing(metrics::control::rowGap);
    automaticBrightness = new ui::DescribedOption(
            strings::get("light_panel.automatic_brightness"),
            strings::get("light_panel.automatic_brightness_description"), this);
    automaticBrightness->setObjectName("rrAutomaticBrightness");
    automaticWarmth =
            new ui::DescribedOption(strings::get("light_panel.automatic_warmth"),
                                    strings::get("light_panel.automatic_warmth_description"), this);
    automaticWarmth->setObjectName("rrAutomaticWarmth");
    darkMode = new ui::DescribedOption(strings::get("reader.dark_mode"),
                                       strings::get("settings.dark_description"), this);
    darkMode->setObjectName("rrLightDarkMode");
    darkMode->hide();
    connect(darkMode, &QCheckBox::clicked, this, [this](bool enabled) {
        if (darkModeChanged)
            darkModeChanged(enabled);
    });
    for (auto *option : { automaticBrightness, automaticWarmth, darkMode }) {
        auto presentation = option->presentation();
        presentation.indicator = ui::DescribedOption::Indicator::Square;
        presentation.labelPixels = metrics::chrome::labelText;
        presentation.descriptionPixels = metrics::chrome::descriptionText;
        presentation.indicatorSize = { metrics::chrome::checkSize, metrics::chrome::checkSize };
        presentation.leadingIndicator = true;
        presentation.minimumRowHeight = metrics::control::stepSize;
        // This compact panel must fit between the toolbar and navigation.
        presentation.descriptionPadding = 0;
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
    brightnessSlider = ui::sliderRow(this, rows, "rrBrightness", "rrBrightnessValue",
                                     strings::get("light_panel.brightness"), "rrBrightnessLess",
                                     "rrBrightnessMore", 0, 100, 1, {}, {}, "%");
    brightnessSlider->setMarkers({ 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 });
    warmthSlider = ui::sliderRow(this, rows, "rrWarmth", "rrWarmthValue",
                                 strings::get("light_panel.warmth"), "rrWarmthLess", "rrWarmthMore",
                                 0, 100, 10, {}, {}, "%");
    warmthSlider->setMarkers({ 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 },
                             ui::SettingSlider::MarkerMode::Stops);
    brightnessValue = findChild<QLabel *>("rrBrightnessValue");
    warmthValue = findChild<QLabel *>("rrWarmthValue");
    warmthRow = ui::controlRowOf(warmthSlider);
    automaticRows = new QWidget(controls);
    auto *automaticSection = new QVBoxLayout(automaticRows);
    automaticSection->setContentsMargins(0, metrics::control::rowGap, 0, 0);
    automaticSection->setSpacing(0);
    auto *automaticPanel = new ui::Panel(automaticRows);
    automaticPanel->setDividers(Qt::TopEdge);
    automaticSection->addWidget(automaticPanel);
    auto *automaticLayout = new QVBoxLayout(automaticPanel);
    automaticLayout->setContentsMargins(0, metrics::control::rowGap, 0, 0);
    automaticLayout->setSpacing(0);
    automaticLayout->addWidget(automaticBrightness);
    automaticLayout->addWidget(automaticWarmth);
    rows->addWidget(automaticRows);
    rows->addWidget(darkMode);
    swipeTip = new ui::InterfaceLabel(this);
    swipeTip->setObjectName("rrLightSwipeTip");
    swipeTip->setWordWrap(true);
    ui::readerStyle(
            swipeTip,
            ui::styles::font(metrics::text::secondary)
                    + "font-style:italic;background:transparent;color:palette(placeholder-text);");
    swipeRow = new SwipeBand(this);
    swipeRow->setObjectName("rrLightSwipeBand");
    ui::readerStyle(swipeRow, "QWidget{background:transparent;}");
    auto *tipLayout = new QHBoxLayout(swipeRow);
    tipLayout->setContentsMargins(
            metrics::panel::contentMargins.left(), metrics::panel::contentMargins.top() + ui::px(4),
            metrics::panel::contentMargins.right(), metrics::panel::contentMargins.bottom());
    tipLayout->setSpacing(metrics::spacing::row);
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
            const int minimum = this->light.warmthMinimum(),
                      range = this->light.warmthMaximum() - minimum;
            this->light.setWarmth(minimum + qRound(range * value / 100.0));
            automaticWarmth->setChecked(this->light.automaticWarmth());
        }
    });
    warmthSlider->committed = [this](int) {
        finishWarmth(true);
        refreshValues();
    };
    connect(&poll, &QTimer::timeout, this, [this] { refreshValues(); });
    poll.setInterval(metrics::light::pollMilliseconds);
    for (auto *widget : findChildren<QWidget *>())
        platform::textRefresh(widget);
    platform::textRefresh(this);
    hide();
}

LightPanel::~LightPanel()
{
    poll.stop();
    light.finishGesture(false);
    finishWarmth(false);
}

bool LightPanel::open()
{
    if (!light.initialize())
        return false;
    refreshValues();
    layoutPanel();
    show();
    layoutPanel();
    raise();
    poll.start();
    if (changed)
        changed();
    return true;
}

void LightPanel::setEdgeGesturesEnabled(bool enabled)
{
    if (edgeGesturesEnabled == enabled) {
        return;
    }
    edgeGesturesEnabled = enabled;
    layoutPanel();
}

void LightPanel::setDarkModeShortcut(bool visible, bool checked, bool enabled)
{
    const bool visibilityChanged = darkMode->isHidden() == visible;
    darkMode->setVisible(visible);
    darkMode->setChecked(checked);
    darkMode->setEnabled(enabled);
    if (visibilityChanged && isVisible())
        layoutPanel();
}

void LightPanel::layoutPanel()
{
    ensurePolished();
    for (auto *widget : findChildren<QWidget *>())
        widget->ensurePolished();
    const int width = parentWidget()->width();
    // Hidden options can retain a minimum height measured at their initial
    // narrow width. Fit them to the actual row before measuring the panel.
    const int rowWidth = qMax(1,
                              width - metrics::panel::contentMargins.left()
                                      - metrics::panel::contentMargins.right());
    for (auto *option : { automaticBrightness, automaticWarmth, darkMode })
        option->setMinimumHeight(option->heightForWidth(rowWidth));
    int automaticHeight = 2 * int(metrics::control::rowGap);
    for (auto *option : { automaticBrightness, automaticWarmth })
        if (!option->isHidden())
            automaticHeight += option->heightForWidth(rowWidth);
    automaticRows->setMinimumHeight(automaticHeight);
    const auto widgets = findChildren<QWidget *>();
    for (int index = int(widgets.size()) - 1; index >= 0; --index) {
        auto *widget = widgets.at(index);
        if (widget->layout())
            widget->layout()->invalidate();
        widget->updateGeometry();
    }
    layout()->invalidate();
    layout()->activate();
    const auto measuredHeight = [this, width] {
        return qMax(layout()->minimumSize().height(),
                    layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width)
                                                  : sizeHint().height());
    };
    int height = measuredHeight();
    const int hintHeight = swipeRow->layout()->hasHeightForWidth()
            ? swipeRow->layout()->totalHeightForWidth(width)
            : swipeRow->sizeHint().height();
    const int controlsHeight = height - (swipeRow->isHidden() ? 0 : hintHeight);
    const bool hintVisible = edgeGesturesEnabled
            && (!availableHeight || controlsHeight + hintHeight <= availableHeight());
    if (hintVisible == swipeRow->isHidden()) {
        // Keep the controls in a short viewport. The optional gesture hint
        // returns when there is room, without changing the gesture setting.
        swipeRow->setVisible(hintVisible);
        layout()->invalidate();
        height = measuredHeight();
    }
    const QRect bounds =
            placement ? placement(height) : QRect(0, metrics::panel::toolbarHeight, width, height);
    const bool moved = geometry() != bounds;
    setGeometry(bounds);
    if (moved && isVisible() && changed)
        changed();
}

void LightPanel::refreshValues()
{
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
    automaticWarmth->setVisible(light.hasAutomaticWarmth());
    automaticWarmth->setChecked(light.automaticWarmth());
    automaticRows->setVisible(sensor || light.hasAutomaticWarmth());
    swipeTip->setText(
            strings::get(warmth ? "light_panel.swipe_hint" : "light_panel.brightness_swipe_hint"));
    if (warmth) {
        if (!warmthSlider->interacting()) {
            const int minimum = light.warmthMinimum(), range = light.warmthMaximum() - minimum;
            const int percent =
                    range > 0 ? qRound(10.0 * (light.warmth() - minimum) / range) * 10 : 0;
            warmthSlider->setValue(qBound(0, percent, 100));
        }
        // setValue does not emit when the current value is unchanged.
        warmthValue->setText(QString::number(warmthSlider->value()) + '%');
    }
    updating = false;
    layoutPanel();
}

void LightPanel::finishWarmth(bool save)
{
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

void LightPanel::hideEvent(QHideEvent *event)
{
    updating = true;
    poll.stop();
    light.finishGesture(false);
    finishWarmth(false);
    ui::Panel::hideEvent(event);
    if (changed)
        changed();
}
} // namespace kobalt
