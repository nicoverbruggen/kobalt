#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/sliders.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "interfaces/reader/ui/style/theme.h"
#include "appearance_panel.h"
#include "support/strings.h"
#include "reading/appearance_settings.h"
#include "reading/reading_density.h"
#include "interfaces/reader/ui/style/styles.h"

#include <QEvent>
#include <QScrollBar>

namespace kobalt::reader {
namespace m = ui::metrics;
namespace styles = ui::styles;
using ui::readerStyle;
using ui::buttonIcon;
using ui::ControlIcon;
using ui::FontButton;
namespace {
class ChoiceLabels : public QObject {
public:
    ChoiceLabels(QWidget *frame, const QVector<QPushButton *> &buttons)
        : QObject(frame), frame(frame), buttons(buttons) {
        frame->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            int required = 0;
            for (auto *button : buttons) {
                const auto label = button->property("full_label").isValid() ? button->property("full_label").toString()
                                                                            : button->accessibleName();
                const int iconWidth = button->icon().isNull() ? 0 : button->iconSize().width() + ui::px(20);
                required = qMax(required, ui::interfaceTextWidth(label, button->font()) + iconWidth + ui::px(36));
            }
            const bool compact = frame->width() / buttons.size() < required;
            for (auto *button : buttons) {
                const auto label = button->property("full_label").isValid() ? button->property("full_label").toString()
                                                                            : button->accessibleName();
                button->setText(compact ? button->property("compact_label").toString() : label);
            }
        }
        return false;
    }

private:
    QWidget *frame;
    QVector<QPushButton *> buttons;
};
}

AppearanceSheet::AppearanceSheet(QWidget *parent) : ui::Panel(parent, Qt::BottomEdge) {
    setObjectName("rrMenu");
    setFrameStyle(QFrame::Box);
    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(m::panel::margins);
    rows->setSpacing(m::panel::gap);
    rows->setSizeConstraint(QLayout::SetNoConstraint);
    scroll = new QScrollArea(this);
    scroll->setObjectName("rrAppearanceScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rows->addWidget(scroll, 1);
    pages = new QStackedWidget(this);
    pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // QStackedLayout otherwise keeps the largest hidden page's minimum size.
    pages->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    scroll->hide();
    rows->addWidget(pages, 1);
    for (int index = 0; index < 2; ++index) {
        auto *page = new QWidget;
        auto *controls = new QVBoxLayout(page);
        controls->setContentsMargins(m::panel::rowsMargins);
        pages->addWidget(page);
    }
    message = new QLabel(this);
    message->setObjectName("rrPreviewLabel");
    message->setAlignment(Qt::AlignCenter);
    message->setWordWrap(true);
    rows->addWidget(message);
    message->hide();
}

AppearanceLayout AppearanceSheet::controls() const {
    return {pages->widget(0), pages->widget(1), static_cast<QVBoxLayout *>(pages->widget(0)->layout()),
            static_cast<QVBoxLayout *>(pages->widget(1)->layout())};
}

int AppearanceSheet::fitHeight(int width, int availableHeight) {
    ensurePolished();
    const auto margins = layout()->contentsMargins();
    const int innerWidth = qMax(1, width - margins.left() - margins.right());
    auto *rows = pages->currentWidget()->layout();
    auto measure = [rows](int width) {
        return qMax(rows->minimumSize().height(),
                    rows->hasHeightForWidth() ? rows->totalHeightForWidth(width) : rows->sizeHint().height());
    };
    const int messageHeight = message->isHidden() ? 0 : qMax(0, message->heightForWidth(innerWidth));
    const int extra =
        margins.top() + margins.bottom() + (message->isHidden() ? 0 : messageHeight + layout()->spacing());
    int contentHeight = measure(innerWidth);
    int contentWidth = innerWidth;
    if (contentHeight + extra > availableHeight) {
        contentWidth = qMax(1, innerWidth - scroll->verticalScrollBar()->sizeHint().width());
        contentHeight = measure(contentWidth);
    }
    const bool overflow = contentHeight + extra > availableHeight;
    // Keep the ordinary preview hierarchy unchanged. A scroll viewport can
    // repaint unrelated rows when a child updates its layout hint.
    if (overflow != (scroll->widget() != nullptr)) {
        if (overflow) {
            layout()->removeWidget(pages);
            scroll->setWidget(pages);
            scroll->show();
        } else {
            scroll->takeWidget();
            pages->setParent(this);
            static_cast<QVBoxLayout *>(layout())->insertWidget(0, pages, 1);
            scroll->hide();
        }
        pages->show();
    }
    if (overflow) {
        pages->resize(contentWidth, contentHeight);
    }
    const int height = qMin(contentHeight + extra, qMax(1, availableHeight));
    resize(width, height);
    layout()->activate();
    return height;
}

AppearancePanel::AppearancePanel(const AppearanceLayout &layout, const AppearancePanelInputs &inputs) {
    auto *typography = layout.typography, *pageLayout = layout.pageLayout;
    auto *typeRows = layout.typeRows, *pageRows = layout.pageRows;
    fontButton = new FontButton(typography);
    fontButton->setObjectName("rrFont");
    readerStyle(
        fontButton,
        QString("min-height:%1px;max-height:%1px;").arg(m::choices::fontButtonHeight - 2 * m::control::padding));
    typeRows->addWidget(fontButton);
    typeRows->addSpacing(m::control::fontPickerGap);
    // Text size counts half points and margins count 300 ppi pixels, so both
    // look the same on every screen; the engine gets device pixels.
    sizeSlider =
        kobalt::ui::sliderRow(typography, typeRows, "rrTextSize", "rrTextSizeValue", strings::get("reader.text_size"),
                              "rrSmaller", "rrLarger", kobalt::minimumHalfPoints, kobalt::maximumHalfPoints, 2, {});
    sizeSlider->setMarkers({14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 48, 56, 64, 80, 96},
                           ui::SettingSlider::MarkerMode::Stops);
    opticalSizeSlider =
        ui::sliderRow(typography, typeRows, "rrOpticalSize", "rrOpticalSizeValue", strings::get("reader.optical_size"),
                      "rrOpticalSizeLess", "rrOpticalSizeMore", 6, 72, 1, {});
    opticalSizeSlider->parentWidget()->hide();
    weightSlider =
        kobalt::ui::sliderRow(typography, typeRows, "rrFontWeight", "rrFontWeightValue",
                              strings::get("reader.font_thickness"), "rrLighter", "rrHeavier", 0, 12, 1, {}, "+");
    weightSlider->setMarkers({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, ui::SettingSlider::MarkerMode::Stops);
    variableWeightSlider =
        ui::sliderRow(typography, typeRows, "rrVariableFontWeight", "rrVariableFontWeightValue",
                      strings::get("reader.font_weight"), "rrVariableLighter", "rrVariableHeavier", 300, 650, 10, {});
    variableWeightSlider->parentWidget()->hide();
    spacingSlider = kobalt::ui::sliderRow(typography, typeRows, "rrLineSpacing", "rrLineSpacingValue",
                                          strings::get("reader.line_spacing"), "rrTighter", "rrLooser", 100,
                                          maximumLineSpacing, 1, {}, {}, "%");
    spacingSlider->setMarkers({100, 110, 120, 130, 140, 150, 175, 200, 250, 300}, ui::SettingSlider::MarkerMode::Stops);
    marginSlider = kobalt::ui::sliderRow(pageLayout, pageRows, "rrMargins", "rrMarginsValue",
                                         strings::get("reader.margins"), "rrNarrower", "rrWider", 0, 120, 3, {});
    verticalMarginSlider = kobalt::ui::sliderRow(pageLayout, pageRows, "rrVerticalMargins", "rrVerticalMarginsValue",
                                                 strings::get("reader.vertical_margins"), "rrVerticalMarginsLess",
                                                 "rrVerticalMarginsMore", 0, 120, 3, {});
    QVector<int> marginMarks;
    for (int value = 0; value <= 120; value += 3) {
        marginMarks.append(value);
    }
    marginSlider->setMarkers(marginMarks);
    verticalMarginSlider->setMarkers(marginMarks);
    typeRows->addSpacing(m::control::sectionGap);
    kobalt::ui::SegmentedRow alignment(typography, typeRows, "rrAlignment", strings::get("reader.alignment"));
    auto alignmentButton = [&](const char *name, const char *value, const QString &label, ControlIcon icon) {
        auto *button = alignment.add(name);
        button->setCheckable(true);
        button->setProperty("alignment", value);
        button->setAccessibleName(label);
        button->setToolTip(label);
        if (QString::fromLatin1(value) == "publisher")
            button->setText(strings::get("common.auto"));
        else {
            buttonIcon(button, icon);
            button->setIconSize(QSize(m::control::chapterIconSize, m::control::chapterIconSize));
        }
        alignmentButtons.append(button);
    };
    alignmentButton("rrAlignLeft", "left", strings::get("reader.align_left"), ControlIcon::AlignLeft);
    alignmentButton("rrAlignJustify", "justify", strings::get("reader.justify"), ControlIcon::AlignJustify);
    alignmentButton("rrAlignPublisher", "publisher", strings::get("reader.auto_alignment"), ControlIcon::Layout);
    kobalt::ui::SegmentedRow hyphenation(typography, typeRows, "rrHyphenation", strings::get("reader.hyphenation"));
    for (const auto *mode : {"off", "on", "auto"}) {
        const QString value = QString::fromLatin1(mode);
        const auto label = value == "off"  ? strings::get("common.off")
                           : value == "on" ? strings::get("common.on")
                                           : strings::get("common.auto");
        auto *button = hyphenation.add("rrHyphenation_" + value, label);
        button->setProperty("hyphenation", value);
        button->setCheckable(true);
        hyphenButtons.append(button);
    }
    if (!inputs.systemRotation) {
        kobalt::ui::SegmentedRow rotation(pageLayout, pageRows, "rrRotation", strings::get("reader.rotation"));
        const QStringList rotationNames = {strings::get("reader.portrait"), strings::get("reader.landscape"),
                                           strings::get("reader.portrait_upside_down"),
                                           strings::get("reader.landscape_upside_down")};
        auto inlineIconLabel = [](QPushButton *button) {
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            button->setProperty("full_label", button->text());
            button->setProperty("compact_label", QString());
            static_cast<ui::Button *>(button)->setIconLabelGap(ui::px(20));
        };
        const bool sideButtons = inputs.sideButtons;
        for (int index = 0; index < (inputs.automaticRotationAvailable ? 2 : rotationNames.size()); ++index) {
            const int target =
                !inputs.automaticRotationAvailable && inputs.reverseLandscapeButtons && (index == 1 || index == 3)
                    ? 4 - index
                    : index;
            const int value = 1 << target;
            auto *button = rotation.add(QString("rrOrientation%1").arg(value));
            button->setProperty("orientation", value);
            const auto label = inputs.automaticRotationAvailable
                                   ? strings::get(index == 0 ? "reader.lock_portrait" : "reader.lock_landscape")
                                   : rotationNames[index];
            button->setAccessibleName(label);
            button->setToolTip(label);
            if (inputs.automaticRotationAvailable) {
                button->setText(rotationNames[index]);
                inlineIconLabel(button);
            }
            rotationButtons.append(button);
            static_cast<ui::Button *>(button)->setIconConfiguration(
                {static_cast<ControlIcon>(int(ControlIcon::Portrait) + index), ui::IconRendition::OriginalAsset,
                 sideButtons});
            button->setIconSize(inputs.automaticRotationAvailable
                                    ? QSize(m::control::chapterIconSize, m::control::chapterIconSize)
                                    : QSize(m::control::iconSize, m::control::iconSize));
        }
        autoRotateOption = rotation.add("rrAutoRotate", strings::get("reader.both"));
        if (inputs.automaticRotationAvailable) {
            buttonIcon(autoRotateOption, ControlIcon::RotationBoth);
            autoRotateOption->setIconSize(QSize(m::control::chapterIconSize, m::control::chapterIconSize));
            inlineIconLabel(autoRotateOption);
            auto buttons = rotationButtons;
            buttons.append(autoRotateOption);
            new ChoiceLabels(autoRotateOption->parentWidget(), buttons);
        }
        autoRotateOption->setAccessibleName(strings::get("reader.automatic_rotation"));
        autoRotateOption->setVisible(inputs.automaticRotationAvailable);
    }
    auto chromeSizeRow = [this, pageLayout, pageRows, &inputs](bool footer) {
        const QString name = footer ? "rrFooterTextSize" : "rrHeaderTextSize";
        ui::SegmentedRow row(pageLayout, pageRows, name.toLatin1().constData(),
                             strings::get(footer ? "reader.footer" : "reader.header"));
        auto &buttons = footer ? footerSizeButtons : headerSizeButtons;
        struct SizeChoice {
            const char *value, *label, *compactLabel;
        };
        for (const auto &choice : {
                 SizeChoice{"off", strings::key("common.off"), strings::key("common.off")},
                 SizeChoice{"small", strings::key("common.size_small"), strings::key("common.size_small_short")},
                 SizeChoice{"medium", strings::key("common.size_medium"), strings::key("common.size_medium_short")},
                 SizeChoice{"large", strings::key("common.size_large"), strings::key("common.size_large_short")},
             }) {
            const QString value = QString::fromLatin1(choice.value);
            const QString label = strings::get(choice.label);
            auto *button = row.add(name + "_" + value, label);
            button->setProperty("text_size", value);
            button->setProperty("full_label", label);
            button->setProperty("compact_label", strings::get(choice.compactLabel));
            button->setAccessibleName(label);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            button->setChecked(value == (footer ? inputs.footerSize : inputs.headerSize));
            buttons.append(button);
        }
        new ChoiceLabels(buttons.first()->parentWidget(), buttons);
    };
    chromeSizeRow(false);
    chromeSizeRow(true);
    kobalt::ui::SegmentedRow clock(pageLayout, pageRows, "rrClock", strings::get("reader.clock"));
    clockOption = clock.add("rrShowClock", strings::get("common.on"));
    clockHideOption = clock.add("rrHideClock", strings::get("common.off"));
    (inputs.showClock ? clockOption : clockHideOption)->setChecked(true);
}
}
