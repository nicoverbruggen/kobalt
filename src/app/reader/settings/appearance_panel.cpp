#include "ui/controls/text.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/rows.h"
#include "ui/controls/sliders.h"
#include "ui/style/private_text.h"
#include "ui/style/theme.h"
#include "appearance_panel.h"
#include "support/strings.h"
#include "reading/appearance_settings.h"
#include "reading/reading_density.h"
#include "ui/style/styles.h"

#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QRegion>
#include <QScrollBar>

namespace kobalt::reader {
namespace metrics = ui::metrics;
namespace styles = ui::styles;
using ui::buttonIcon;
using ui::ControlIcon;
using ui::FontButton;
using ui::readerStyle;

namespace {
class ChoiceLabels : public QObject
{
public:
    ChoiceLabels(QWidget *frame, const QVector<QPushButton *> &buttons)
        : QObject(frame), frame(frame), buttons(buttons)
    {
        frame->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            int required = 0;
            for (auto *button : buttons) {
                const auto label = button->property("full_label").isValid()
                        ? button->property("full_label").toString()
                        : button->accessibleName();
                const int iconWidth =
                        button->icon().isNull() ? 0 : button->iconSize().width() + ui::px(20);
                required = qMax(required,
                                ui::interfaceTextWidth(label, button->font()) + iconWidth
                                        + ui::px(36));
            }
            const bool compact = frame->width() / buttons.size() < required;
            for (auto *button : buttons) {
                const auto label = button->property("full_label").isValid()
                        ? button->property("full_label").toString()
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
} // namespace

StripSizeControl::StripSizeControl(QWidget *parent, bool footer) : ui::ControlRow(parent)
{
    const QString name = footer ? "rrFooterTextSize" : "rrHeaderTextSize";
    const auto title = strings::get(footer ? "reader.footer" : "reader.header");
    auto *line = controls();
    auto *caption = this->caption();
    auto *captionRows = new QVBoxLayout(caption);
    captionRows->setContentsMargins(0, 0, metrics::control::captionRight, 0);
    captionRows->setSpacing(0);
    setMarkerBand(metrics::control::sliderLabelHeight);
    auto *labels = new QVBoxLayout;
    labels->setSpacing(ui::px(2));
    auto *heading = new QHBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(ui::px(6));
    auto *titleLabel = new ui::InterfaceLabel(title, caption);
    readerStyle(titleLabel, styles::font(metrics::text::secondary));
    heading->addWidget(titleLabel, 1);
    labels->addLayout(heading);
    valueLabel = new ui::InterfaceLabel(caption);
    valueLabel->setObjectName(name + "Value");
    readerStyle(valueLabel,
                styles::font(metrics::control::captionValueText)
                        + "color:palette(placeholder-text);");
    labels->addWidget(valueLabel);
    captionRows->addLayout(labels);
    captionRows->setAlignment(labels, Qt::AlignVCenter);
    // Above the controls, the size sits beside the title, as slider values do,
    // so the caption takes one line.
    stackedChanged = [captionRows, labels, heading, value = valueLabel](bool stacked) {
        auto margins = captionRows->contentsMargins();
        margins.setRight(stacked ? 0 : int(metrics::control::captionRight));
        captionRows->setContentsMargins(margins);
        if (stacked) {
            labels->removeWidget(value);
            heading->addWidget(value, 0, Qt::AlignRight | Qt::AlignVCenter);
        } else {
            heading->removeWidget(value);
            labels->addWidget(value);
        }
    };
    stackedChanged(property("caption_stacked").toBool());
    auto *less = new ui::StepButton(ControlIcon::Minus, this);
    auto *more = new ui::StepButton(ControlIcon::Plus, this);
    less->setObjectName(name + "Less");
    more->setObjectName(name + "More");
    less->setAccessibleName(strings::get("controls.decrease").arg(title.toLower()));
    more->setAccessibleName(strings::get("controls.increase").arg(title.toLower()));
    slider = new ui::SettingSlider(this, headerfooter::offSize, headerfooter::maximumSize);
    slider->setObjectName(name);
    slider->setAccessibleName(title);
    slider->setMarkers({ headerfooter::offSize, headerfooter::smallSize, headerfooter::mediumSize,
                         headerfooter::largeSize, headerfooter::maximumSize },
                       ui::SettingSlider::MarkerMode::Checkpoints);
    slider->setMarkerLabels({ strings::get("common.off"), strings::get("common.size_small_short"),
                              strings::get("common.size_medium_short"),
                              strings::get("common.size_large_short"),
                              strings::get("common.size_extra_large_short") });
    const auto step = [line](QWidget *button) {
        auto *column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        column->addSpacing(metrics::control::sliderLabelHeight);
        column->addWidget(button, 0, Qt::AlignVCenter);
        line->addLayout(column);
    };
    step(less);
    line->addWidget(slider, 1);
    step(more);
    connect(less, &QPushButton::clicked, slider, [this] { slider->nudge(-1); });
    connect(more, &QPushButton::clicked, slider, [this] { slider->nudge(1); });
    connect(slider, &QSlider::valueChanged, this, [this](int value) { showValue(value); });
    setSize(headerfooter::smallSize, true);
}

void StripSizeControl::showValue(int value)
{
    QString text = strings::get("common.off");
    if (value != headerfooter::offSize) {
        text = strings::get("reader.text_size_value")
                       .arg(QString::number(value * 72.0 / 300.0, 'f', 1));
    }
    valueLabel->setText(text);
    slider->setAccessibleDescription(text);
}

void StripSizeControl::setSize(int pixels, bool shown)
{
    if (slider->interacting())
        return;
    slider->setValue(shown ? pixels : headerfooter::offSize);
    showValue(slider->value());
}

AppearanceSheet::AppearanceSheet(QWidget *parent) : ui::Panel(parent, Qt::BottomEdge)
{
    setObjectName("rrMenu");
    setFrameStyle(QFrame::Box);
    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(metrics::panel::margins);
    rows->setSpacing(metrics::panel::gap);
    rows->setSizeConstraint(QLayout::SetNoConstraint);
    // Rows that do not fit are shown a page at a time inside the clip.
    clip = new QWidget(this);
    clip->setObjectName("rrAppearanceClip");
    clip->hide();
    rows->addWidget(clip, 1);
    pages = new QStackedWidget(this);
    pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // QStackedLayout otherwise keeps the largest hidden page's minimum size.
    pages->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    rows->addWidget(pages, 1);
    for (int index = int(AppearancePage::Typography); index <= int(AppearancePage::Layout);
         ++index) {
        auto *page = new QWidget;
        auto *controls = new QVBoxLayout(page);
        controls->setContentsMargins(metrics::panel::rowsMargins);
        pages->addWidget(page);
    }
    pager = new ui::PagerBar(this, "rrAppearance");
    pager->setFixedHeight(metrics::chrome::actionHeight);
    pager->hide();
    rows->addWidget(pager);
    connect(pager->previous, &QPushButton::clicked, this,
            [this] { showRows(pager->turnTarget(-1)); });
    connect(pager->next, &QPushButton::clicked, this, [this] { showRows(pager->turnTarget(1)); });
    message = new ui::InterfaceLabel(this);
    message->setObjectName("rrPreviewLabel");
    message->setAlignment(Qt::AlignCenter);
    message->setWordWrap(true);
    rows->addWidget(message);
    message->hide();
}

AppearanceLayout AppearanceSheet::controls() const
{
    const auto rows = [this](int page) {
        return static_cast<QVBoxLayout *>(pages->widget(page)->layout());
    };
    return { pages->widget(int(AppearancePage::Typography)),
             pages->widget(int(AppearancePage::Layout)),
             nullptr,
             rows(int(AppearancePage::Typography)),
             rows(int(AppearancePage::Layout)),
             nullptr };
}

int AppearanceSheet::fitHeight(int width, int availableHeight)
{
    ensurePolished();
    const auto margins = layout()->contentsMargins();
    const int innerWidth = qMax(1, width - margins.left() - margins.right());
    auto *rows = pages->currentWidget()->layout();
    auto measure = [rows](int width) {
        return qMax(rows->minimumSize().height(),
                    rows->hasHeightForWidth() ? rows->totalHeightForWidth(width)
                                              : rows->sizeHint().height());
    };
    const int messageHeight =
            message->isHidden() ? 0 : qMax(0, message->heightForWidth(innerWidth));
    const int extra = margins.top() + margins.bottom()
            + (message->isHidden() ? 0 : messageHeight + layout()->spacing());
    const int contentHeight = measure(innerWidth);
    const bool overflow = contentHeight + extra > availableHeight;
    // Keep the ordinary hierarchy when everything fits, so a row that
    // updates its layout hint does not repaint rows on another page.
    if (overflow != (pages->parentWidget() == clip)) {
        QPointer<QWidget> focused = QApplication::focusWidget();
        auto *sheetRows = static_cast<QVBoxLayout *>(layout());
        if (overflow) {
            sheetRows->removeWidget(pages);
            pages->setParent(clip);
        } else {
            pages->clearMask();
            pages->setParent(this);
            sheetRows->insertWidget(0, pages, 1);
        }
        pages->show();
        clip->setVisible(overflow);
        pager->setVisible(overflow);
        if (focused && !focused->isVisible())
            pages->setFocus(Qt::OtherFocusReason);
    }
    if (pages->currentIndex() != pagedIndex) {
        pagedIndex = pages->currentIndex();
        rowPage = 0;
    }
    rowPages.clear();
    if (overflow) {
        // Lay out at the natural height, then split on complete row
        // boundaries. Squeezing the rows would overlap their controls.
        const int room = qMax(1,
                              availableHeight - extra - int(metrics::chrome::actionHeight)
                                      - layout()->spacing());
        pages->setGeometry(0, 0, innerWidth, contentHeight);
        pages->currentWidget()->setGeometry(0, 0, innerWidth, contentHeight);
        rows->setGeometry(QRect(0, 0, innerWidth, contentHeight));
        int start = 0;
        for (int index = 0; index < rows->count(); ++index) {
            auto *item = rows->itemAt(index);
            if (item->isEmpty() || !item->widget())
                continue;
            const auto bounds = item->geometry();
            if (bounds.bottom() + 1 - start > room && bounds.top() > start) {
                rowPages.append({ start, bounds.top() });
                start = bounds.top();
            }
        }
        rowPages.append({ start, contentHeight });
    }
    const int height = overflow ? qMax(1, availableHeight) : contentHeight + extra;
    resize(width, height);
    layout()->activate();
    if (overflow)
        showRows(rowPage);
    return height;
}

void AppearanceSheet::showRows(int page)
{
    if (rowPages.isEmpty())
        return;
    rowPage = qBound(0, page, int(rowPages.size()) - 1);
    const auto range = rowPages[rowPage];
    // Only this page's rows show and take touches; the next page's first
    // row must not peek out below them.
    pages->move(0, -range.first);
    pages->setMask(QRegion(0, range.first, pages->width(), range.second - range.first));
    pager->setPage(rowPage, rowPages.size());
    pager->setBlankArea({});
}

AppearancePanel::AppearancePanel(const AppearanceLayout &layout,
                                 const AppearancePanelInputs &inputs)
{
    auto *typography = layout.typography, *pageLayout = layout.pageLayout;
    auto *typeRows = layout.typeRows, *pageRows = layout.pageRows;
    fontButton = new FontButton(typography);
    fontButton->setObjectName("rrFont");
    readerStyle(fontButton,
                QString("min-height:%1px;max-height:%1px;")
                        .arg(metrics::choices::fontButtonHeight - 2 * metrics::control::padding));
    typeRows->addWidget(fontButton);
    typeRows->addSpacing(metrics::control::fontPickerGap);
    // Text size counts half points and margins count 300 ppi pixels, so both
    // look the same on every screen; the engine gets device pixels.
    sizeSlider = kobalt::ui::sliderRow(typography, typeRows, "rrTextSize", "rrTextSizeValue",
                                       strings::get("reader.text_size"), "rrSmaller", "rrLarger",
                                       kobalt::minimumHalfPoints, kobalt::maximumHalfPoints, 1, {});
    QObject::connect(sizeSlider, &QSlider::valueChanged, sizeSlider,
                     [slider = sizeSlider](int halfPoints) {
                         slider->setSingleStep(halfPoints < 64 ? 1 : 2);
                     });
    sizeSlider->setMarkers({ 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 48, 56, 64, 80, 96 },
                           ui::SettingSlider::MarkerMode::Stops);
    opticalSizeSlider = ui::sliderRow(typography, typeRows, "rrOpticalSize", "rrOpticalSizeValue",
                                      strings::get("reader.optical_size"), "rrOpticalSizeLess",
                                      "rrOpticalSizeMore", 6, 72, 1, {});
    ui::controlRowOf(opticalSizeSlider)->hide();
    weightSlider = kobalt::ui::sliderRow(typography, typeRows, "rrFontWeight", "rrFontWeightValue",
                                         strings::get("reader.font_thickness"), "rrLighter",
                                         "rrHeavier", 0, 12, 1, {}, "+");
    weightSlider->setMarkers({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 },
                             ui::SettingSlider::MarkerMode::Stops);
    variableWeightSlider =
            ui::sliderRow(typography, typeRows, "rrVariableFontWeight", "rrVariableFontWeightValue",
                          strings::get("reader.font_weight"), "rrVariableLighter",
                          "rrVariableHeavier", 300, 650, 10, {});
    ui::controlRowOf(variableWeightSlider)->hide();
    spacingSlider =
            kobalt::ui::sliderRow(typography, typeRows, "rrLineSpacing", "rrLineSpacingValue",
                                  strings::get("reader.line_spacing"), "rrTighter", "rrLooser", 100,
                                  maximumLineSpacing, 1, {}, {}, "%");
    spacingSlider->setMarkers({ 100, 110, 120, 130, 140, 150, 175, 200, 250, 300 },
                              ui::SettingSlider::MarkerMode::Stops);
    marginSlider = kobalt::ui::sliderRow(pageLayout, pageRows, "rrMargins", "rrMarginsValue",
                                         strings::get("reader.margins"), "rrNarrower", "rrWider", 0,
                                         120, 3, {});
    verticalMarginSlider =
            kobalt::ui::sliderRow(pageLayout, pageRows, "rrVerticalMargins",
                                  "rrVerticalMarginsValue", strings::get("reader.vertical_margins"),
                                  "rrVerticalMarginsLess", "rrVerticalMarginsMore", 0, 120, 3, {});
    QVector<int> marginMarks;
    for (int value = 0; value <= 120; value += 3) {
        marginMarks.append(value);
    }
    marginSlider->setMarkers(marginMarks);
    verticalMarginSlider->setMarkers(marginMarks);
    typeRows->addSpacing(metrics::control::sectionGap);
    kobalt::ui::SegmentedRow alignment(typography, typeRows, "rrAlignment",
                                       strings::get("reader.alignment"));
    auto alignmentButton = [&](const char *name, const char *value, const QString &label,
                               ControlIcon icon) {
        auto *button = alignment.add(name);
        button->setCheckable(true);
        button->setProperty("alignment", value);
        button->setAccessibleName(label);
        button->setToolTip(label);
        if (QString::fromLatin1(value) == "publisher")
            button->setText(strings::get("common.auto"));
        else {
            buttonIcon(button, icon);
            button->setIconSize(
                    QSize(metrics::control::chapterIconSize, metrics::control::chapterIconSize));
        }
        alignmentButtons.append(button);
    };
    alignmentButton("rrAlignLeft", "left", strings::get("reader.align_left"),
                    ControlIcon::AlignLeft);
    alignmentButton("rrAlignJustify", "justify", strings::get("reader.justify"),
                    ControlIcon::AlignJustify);
    alignmentButton("rrAlignPublisher", "publisher", strings::get("reader.auto_alignment"),
                    ControlIcon::Layout);
    kobalt::ui::SegmentedRow hyphenation(typography, typeRows, "rrHyphenation",
                                         strings::get("reader.hyphenation"));
    for (const auto *mode : { "off", "on", "auto" }) {
        const QString value = QString::fromLatin1(mode);
        const auto label = value == "off" ? strings::get("common.off")
                : value == "on"           ? strings::get("common.on")
                                          : strings::get("common.auto");
        auto *button = hyphenation.add("rrHyphenation_" + value, label);
        button->setProperty("hyphenation", value);
        button->setCheckable(true);
        hyphenButtons.append(button);
    }
    if (!inputs.systemRotation) {
        kobalt::ui::SegmentedRow rotation(pageLayout, pageRows, "rrRotation",
                                          strings::get("reader.rotation"));
        const QStringList rotationNames = { strings::get("reader.portrait"),
                                            strings::get("reader.landscape"),
                                            strings::get("reader.portrait_upside_down"),
                                            strings::get("reader.landscape_upside_down") };
        auto inlineIconLabel = [](QPushButton *button) {
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            button->setProperty("full_label", button->text());
            button->setProperty("compact_label", QString());
            static_cast<ui::Button *>(button)->setIconLabelGap(ui::px(20));
        };
        const bool sideButtons = inputs.sideButtons;
        for (int index = 0; index < (inputs.automaticRotationAvailable ? 2 : rotationNames.size());
             ++index) {
            const int target = !inputs.automaticRotationAvailable && inputs.reverseLandscapeButtons
                            && (index == 1 || index == 3)
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
                    { static_cast<ControlIcon>(int(ControlIcon::Portrait) + index),
                      ui::IconRendition::OriginalAsset, sideButtons });
            button->setIconSize(
                    inputs.automaticRotationAvailable
                            ? QSize(metrics::control::chapterIconSize,
                                    metrics::control::chapterIconSize)
                            : QSize(metrics::control::iconSize, metrics::control::iconSize));
        }
        autoRotateOption = rotation.add("rrAutoRotate", strings::get("reader.both"));
        if (inputs.automaticRotationAvailable) {
            buttonIcon(autoRotateOption, ControlIcon::RotationBoth);
            autoRotateOption->setIconSize(
                    QSize(metrics::control::chapterIconSize, metrics::control::chapterIconSize));
            inlineIconLabel(autoRotateOption);
            auto buttons = rotationButtons;
            buttons.append(autoRotateOption);
            new ChoiceLabels(autoRotateOption->parentWidget(), buttons);
        }
        autoRotateOption->setAccessibleName(strings::get("reader.automatic_rotation"));
        autoRotateOption->setVisible(inputs.automaticRotationAvailable);
    }
    // Header and footer moved to their own pages. Layout keeps one labeled
    // row whose button names the preset in force and opens the presets.
    auto *chromeRow = ui::labeledRow(pageLayout, pageRows, strings::get("header_footer.title"));
    headerFooterRow = new ui::DropdownButton(pageLayout);
    headerFooterRow->setObjectName("rrHeaderFooter");
    headerFooterRow->setFocusPolicy(Qt::NoFocus);
    headerFooterRow->setForward(true);
    // As tall as the segmented rows above it, with their label size. QSS
    // heights exclude the reader style's padding, as for the font button.
    const int rowHeight = metrics::control::segmentHeight
            + 2 * int(metrics::control::segmentVerticalPadding) + 2 * int(metrics::control::border);
    readerStyle(headerFooterRow,
                QString("min-height:%1px;max-height:%1px;")
                                .arg(rowHeight - 2 * metrics::control::padding)
                        + styles::font(metrics::text::secondary));
    chromeRow->addWidget(headerFooterRow, 1);

    // The rest lives in the Header and footer sheet, which supplies these
    // rows: the presets, the slots, the progress bar, then independent text sizes.
    if (!layout.headerFooter)
        return;
    auto *custom = layout.headerFooter;
    auto *customRows = layout.headerFooterRows;
    auto *cards = new QHBoxLayout;
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(0);
    for (const auto preset : headerfooter::presets) {
        auto *card = new PresetCard(preset, custom);
        card->setDivider(preset != headerfooter::Preset::Custom);
        card->setVisible(preset != headerfooter::Preset::Phone || inputs.phone);
        cards->addWidget(card, 1);
        presetCards.append(card);
    }
    customRows->addLayout(cards);
    // The presets are a choice of their own; keep them apart from the slots.
    customRows->addSpacing(metrics::spacing::section);
    slotMap = new SlotMap(custom, true);
    customRows->addWidget(slotMap);
    slotHint = new ui::InterfaceLabel(strings::get("header_footer.custom_hint"), custom);
    slotHint->setObjectName("rrHeaderFooterHint");
    slotHint->setWordWrap(true);
    // The same secondary text as the sliders' current values, so it follows the theme.
    readerStyle(slotHint,
                styles::font(metrics::control::captionValueText)
                        + "font-style:italic;color:palette(placeholder-text);");
    customRows->addWidget(slotHint);
    customRows->addSpacing(metrics::spacing::row);
    auto switchRow = [custom, customRows](const char *name, const QString &label,
                                          QVector<QPushButton *> &buttons) {
        ui::SegmentedRow row(custom, customRows, name, label,
                             strings::get("header_footer.progress_bar_detail"));
        for (const bool enabled : { false, true }) {
            auto *button = row.add(QString(name) + (enabled ? "_on" : "_off"),
                                   strings::get(enabled ? "common.on" : "common.off"));
            button->setProperty("switch_on", enabled);
            buttons.append(button);
        }
    };
    switchRow("rrHeaderFooterProgressBar", strings::get("header_footer.progress_bar"),
              progressBarButtons);
    customRows->addSpacing(metrics::spacing::row);
    auto *divider = new ui::Panel(custom);
    divider->setObjectName("rrHeaderFooterSizeDivider");
    divider->setFixedHeight(metrics::control::border);
    divider->setDividers(Qt::TopEdge);
    customRows->addWidget(divider);
    headerSizeControl = new StripSizeControl(custom, false);
    footerSizeControl = new StripSizeControl(custom, true);
    customRows->addWidget(headerSizeControl);
    customRows->addWidget(footerSizeControl);
}
} // namespace kobalt::reader
