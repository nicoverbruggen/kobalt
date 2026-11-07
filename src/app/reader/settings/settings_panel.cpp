#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/controls/options.h"
#include "ui/controls/panels.h"
#include "ui/controls/sliders.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "ui/panel_timing.h"
#include "settings_panel.h"
#include "app/about/about_page.h"
#include "reading/reading_density.h"
#include "app/reader/panels/dictionary_panel.h"
#include "reading/appearance_settings.h"
#include "support/strings.h"
#include "support/qt_font.h"
#include "assets/font_catalog.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/fonts.h"
#include "ui/style/styles.h"
#include <QApplication>
#include <memory>
#include <QtMath>
#include <QButtonGroup>
#include <QGridLayout>
#include <QPointer>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QHideEvent>
#include <QRegularExpression>
#include <QPainter>

namespace kobalt::reader {
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;
namespace styles = ui::styles;
using ui::makeButton;
using ui::readerStyle;
using ReaderOption = ui::DescribedOption;

namespace {
QPushButton *segmentButton(QWidget *parent, QHBoxLayout *layout, QButtonGroup *group,
                           const QString &name, const QString &text);

// Keep section titles on the Settings baseline. The private text bitmap
// includes each word's descenders, so centering that bitmap moves the baseline.
class SettingsTitle final : public ui::InterfaceLabel
{
public:
    using ui::InterfaceLabel::InterfaceLabel;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (!ui::needsPrivateText(text(), font())) {
            ui::InterfaceLabel::paintEvent(event);
            return;
        }
        const auto reference = ui::renderPrivateText(strings::get("settings.title"), font());
        const auto shown = ui::renderPrivateText(text(), font());
        const auto area = contentsRect();
        const int baseline =
                area.top() + (area.height() - reference.image.height()) / 2 - reference.origin.y();
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(QPalette::WindowText));
        ui::drawInterfaceText(
                painter,
                QRect(area.left(), baseline + shown.origin.y(), area.width(), shown.image.height()),
                text(), Qt::AlignLeft | Qt::AlignTop);
    }
};

// The chevron of a section row in the narrow Settings list, as other rows
// that open a page show it. It follows its row's size and pressed state.
class SectionChevron final : public QWidget
{
public:
    explicit SectionChevron(QPushButton *row) : QWidget(row), row(row)
    {
        setObjectName("rrSettingsSectionChevron");
        ui::readerStyle(this, "background:transparent;");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        row->installEventFilter(this);
        place();
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Resize)
            place();
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        ui::controlIcon(ui::ControlIcon::Next, row->palette().color(QPalette::WindowText))
                .paint(&painter, rect());
    }

private:
    QPushButton *row;

    void place()
    {
        const int size = ui::px(28);
        setGeometry(row->width() - chrome::chevronRight - size / 2, (row->height() - size) / 2,
                    size, size);
    }
};

// Rendered sample pages for the styling sheet. Page-break settings show a
// scaled spread. Spacing settings show the top of the first page at full
// resolution, because scaling would hide the difference. Page spacing adds a
// magnified bottom edge, where the spare space it moves is hatched.
class StylingPreview final : public QWidget
{
public:
    explicit StylingPreview(QWidget *parent) : QWidget(parent)
    {
        setObjectName("rrStylingPreview");
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

    void show(const StyleSample &sample, int choice)
    {
        pages = sample.spreads.value(choice);
        lines = sample.crops.value(choice);
        linesRatio = sample.cropRatio;
        gaps = sample.cropGaps.value(choice);
        cropReadingArea = sample.cropReadingArea;
        setProperty("paragraph_gap_count", gaps.size());
        readingArea = sample.readingArea;
        edge = sample.details.value(choice);
        edgeArea = sample.detailArea;
        spare = sample.spare.value(choice);
        update();
    }

    // Logical height of the magnified edge, below the spread.
    void setEdgeHeight(int pixels) { edgeHeight = pixels; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), ui::sectionColor(this));
        const auto ink = palette().color(QPalette::WindowText);
        const auto frame = [&painter, ink](const QRect &bounds) {
            painter.setPen(QPen(ink, int(metrics::control::border)));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(bounds).adjusted(.5, .5, -.5, -.5));
        };
        const auto dashed = [&painter, ink](const QRectF &bounds) {
            QPen pen(ink, 1, Qt::DashLine);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(bounds.adjusted(.5, .5, -.5, -.5));
        };
        const auto hatch = [&painter, ink](const QRectF &area) {
            painter.save();
            painter.setClipRect(area, Qt::IntersectClip);
            QPen pen(ink, 1);
            pen.setCosmetic(true);
            painter.setPen(pen);
            for (qreal x = area.left() - area.height(); x < area.right(); x += ui::px(10))
                painter.drawLine(QPointF(x, area.bottom()), QPointF(x + area.height(), area.top()));
            painter.restore();
        };
        const qreal ratio = qMax<qreal>(1, devicePixelRatio());
        const int padding = ui::px(20);
        if (!lines.isNull()) {
            // Show the sample at its own scale, unless the reading line is wider than
            // the sheet. Book samples then match the reading page. Paper in the
            // page's colour keeps the lines clear of the frame.
            const int paperHeight = ui::px(24);
            QSize size(qRound(lines.width() / linesRatio), qRound(lines.height() / linesRatio));
            const QSize space(qMax(1, width() - 2 * int(ui::px(24))),
                              qMax(1, height() - 2 * padding - 2 * paperHeight));
            if (size.width() > space.width() || size.height() > space.height()) {
                size.scale(space, Qt::KeepAspectRatio);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
            }
            const QRect target((width() - size.width()) / 2, (height() - size.height()) / 2,
                               size.width(), size.height());
            const auto sheet = target.adjusted(0, -paperHeight, 0, paperHeight);
            painter.fillRect(sheet, QColor::fromRgb(lines.pixel(0, 0)));
            painter.drawImage(target, lines);
            const qreal scale = qreal(target.width()) / lines.width();
            painter.save();
            painter.setClipRect(target);
            for (const auto &gap : gaps)
                hatch(QRectF(QPointF(target.topLeft()) + QPointF(gap.topLeft()) * scale,
                             QSizeF(gap.size()) * scale));
            if (!cropReadingArea.isEmpty()) {
                QPen guide(ink, 1, Qt::DashLine);
                guide.setCosmetic(true);
                painter.setPen(guide);
                for (int x : { cropReadingArea.left(), cropReadingArea.right() + 1 }) {
                    const qreal edge = target.left() + x * scale;
                    painter.drawLine(QPointF(edge, target.top()), QPointF(edge, target.bottom()));
                }
            }
            painter.restore();
            frame(sheet);
            return;
        }
        if (pages.isEmpty())
            return;
        const int gap = ui::px(16);
        // A card on a short screen gives the preview less than its preferred
        // height. The spread and the magnified edge then shrink together.
        const int preferred = property("preferred_height").toInt();
        const int edgeShown = preferred > height() ? edgeHeight * height() / preferred : edgeHeight;
        const int spreadHeight = height() - (edgeShown ? edgeShown + gap : 0);
        QSize size = pages.first().size() / ratio;
        const int pageSpace = qMax(1, spreadHeight - 2 * padding);
        if (size.height() > pageSpace)
            size = size.scaled(size.width(), pageSpace, Qt::KeepAspectRatio);
        int total = pages.size() * size.width() + (pages.size() - 1) * gap;
        int left = (width() - total) / 2;
        for (int index = 0; index < pages.size(); ++index) {
            const QRect target(left, (spreadHeight - size.height()) / 2, size.width(),
                               size.height());
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(target, pages[index]);
            if (!readingArea.isEmpty()) {
                const qreal scale = qreal(size.width()) / pages[index].width();
                dashed(QRectF(target.topLeft() + readingArea.topLeft() * scale,
                              readingArea.size() * scale));
            }
            frame(target);
            left += size.width() + gap;
        }
        if (!edgeShown || edge.isNull())
            return;
        // The bottom of the changed page, larger than in the spread. Hatching
        // marks height the text does not use with this option.
        QSizeF shown = QSizeF(edge.size()) / ratio;
        const QSizeF space(qMax(1, width() - 2 * int(ui::px(24))), qMax(1, edgeShown - padding));
        if (shown.width() > space.width() || shown.height() > space.height())
            shown.scale(space, Qt::KeepAspectRatio);
        const QRectF target((width() - shown.width()) / 2,
                            spreadHeight + (space.height() - shown.height()) / 2, shown.width(),
                            shown.height());
        painter.drawImage(target, edge);
        const qreal scale = shown.width() / edge.width();
        const auto mapped = [&target, scale](const QRect &rect) {
            return QRectF(target.topLeft() + QPointF(rect.topLeft()) * scale,
                          QSizeF(rect.size()) * scale);
        };
        painter.save();
        painter.setClipRect(target);
        for (const auto &rect : spare)
            hatch(mapped(rect));
        dashed(mapped(edgeArea));
        painter.restore();
        frame(target.toAlignedRect());
    }

private:
    QVector<QImage> pages;
    QImage lines, edge;
    qreal linesRatio = 1;
    QRect cropReadingArea;
    QRectF readingArea;
    QRect edgeArea;
    QVector<QRect> spare;
    QVector<QRect> gaps;
    int edgeHeight = 0;
};

QPushButton *actionButton(QWidget *parent, QBoxLayout *layout, const char *name,
                          const QString &text)
{
    auto *button = new ui::Button(text, parent);
    button->setObjectName(name);
    button->setFocusPolicy(Qt::NoFocus);
    if (layout)
        layout->addWidget(button);
    return button;
}
} // namespace

SettingsPanel::SettingsPanel(QWidget *parent, const SettingsPanelInputs &inputs)
    : ui::Panel(parent, Qt::BottomEdge)
{
    setObjectName("rrGeneralSettings");
    setSoftEdges(true);
    auto *generalRows = new QVBoxLayout(this);
    generalRows->setSizeConstraint(QLayout::SetNoConstraint);
    generalRows->setContentsMargins(0, 0, 0, 0);
    generalRows->setSpacing(0);
    auto *heading = new ui::Panel(this, Qt::BottomEdge);
    heading->setObjectName("rrSettingsHeader");
    heading->setEdgeWidth(chrome::heavyRule);
    heading->setFixedHeight(chrome::actionHeight);
    auto *headingRows = new QHBoxLayout(heading);
    headingRows->setContentsMargins(0, 0, 0, chrome::heavyRule);
    headingRows->setSpacing(0);
    closeButton = new ui::CellButton(ui::ControlIcon::Close, heading);
    closeButton->setObjectName("rrSettingsClose");
    closeButton->setAccessibleName(strings::get("settings.close"));
    closeButton->setRule(Qt::RightEdge);
    closeButton->setFixedWidth(chrome::cellWidth);
    headingRows->addWidget(closeButton);
    backButton = new ui::CellButton(ui::ControlIcon::Back, heading);
    backButton->setObjectName("rrSettingsBack");
    backButton->setAccessibleName(strings::get("common.back"));
    backButton->setRule(Qt::RightEdge);
    backButton->setFixedWidth(chrome::cellWidth);
    backButton->hide();
    headingRows->addWidget(backButton);
    connect(backButton, &QPushButton::clicked, this, [this] {
        sectionOpen = false;
        if (refitRequested)
            refitRequested();
    });
    settingsTitle = new SettingsTitle(strings::get("settings.title"), heading);
    settingsTitle->setObjectName("rrSettingsTitle");
    settingsTitle->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    readerStyle(settingsTitle, styles::brand(chrome::titleText) + "background:transparent;");
    headingRows->addWidget(settingsTitle, 1);
    generalRows->addWidget(heading);
    auto *sections = new QWidget(this);
    generalRows->addWidget(sections, 1);
    auto *sectionRows = new QBoxLayout(QBoxLayout::LeftToRight, sections);
    sectionRows->setSizeConstraint(QLayout::SetNoConstraint);
    sectionRows->setObjectName("settingsSectionLayout");
    sectionRows->setContentsMargins(0, 0, 0, 0);
    sectionRows->setSpacing(0);
    tabRail = new QWidget(sections);
    tabRail->setObjectName("rrSettingsRail");
    auto *settingsTabs = new QBoxLayout(QBoxLayout::TopToBottom, tabRail);
    settingsTabs->setContentsMargins(0, 0, 0, 0);
    settingsTabs->setSpacing(0);
    sectionRows->addWidget(tabRail);
    optionsColumn = new QWidget(sections);
    auto *optionsRows = new QVBoxLayout(optionsColumn);
    optionsRows->setContentsMargins(0, 0, 0, 0);
    optionsRows->setSpacing(0);
    optionsRows->setSizeConstraint(QLayout::SetNoConstraint);
    sectionRows->addWidget(optionsColumn, 1);
    optionsViewport = new ui::Panel(optionsColumn, Qt::LeftEdge);
    optionsViewport->setObjectName("rrSettingsViewport");
    optionsRows->addWidget(optionsViewport, 1);
    optionsClip = new QWidget(optionsViewport);
    optionsClip->setObjectName("rrSettingsPageClip");
    optionsPager = new ui::PagerBar(optionsColumn, "rrSettingsOptions");
    optionsPager->previous->setRule(ui::Edges(Qt::LeftEdge) | Qt::RightEdge);
    optionsPager->next->setRule(ui::Edges(Qt::LeftEdge) | Qt::RightEdge);
    optionsRows->addWidget(optionsPager);
    optionsPager->hide();
    connect(optionsPager->previous, &QPushButton::clicked, this,
            [this] { showOptionsPage(optionsPager->turnTarget(-1)); });
    connect(optionsPager->next, &QPushButton::clicked, this,
            [this] { showOptionsPage(optionsPager->turnTarget(1)); });
    settingsPages = new QStackedWidget(optionsClip);
    settingsPages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    settingsPages->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    settingsPages->setObjectName("rrSettingsPages");
    readerStyle(settingsPages,
                QString("QStackedWidget{border:0;border-left:%1px solid palette(window-text);}")
                        .arg(int(metrics::control::border)));
    auto *tabGroup = new QButtonGroup(this);
    const char *tabNames[] = { "rrSettingsDisplay", "rrSettingsControls",     "rrSettingsDefaults",
                               "rrSettingsStyling", "rrSettingsDictionaries", "rrSettingsSystem",
                               "rrSettingsAbout",   "rrSettingsKosync" };
    const ui::ControlIcon tabIcons[] = { ui::ControlIcon::Display,      ui::ControlIcon::Swipe,
                                         ui::ControlIcon::Undo,         ui::ControlIcon::Styling,
                                         ui::ControlIcon::Dictionaries, ui::ControlIcon::System,
                                         ui::ControlIcon::About,        ui::ControlIcon::Sync };
    const QStringList tabs = { strings::get("settings.display"),  strings::get("settings.controls"),
                               strings::get("settings.defaults"), strings::get("settings.styling"),
                               strings::get("settings.lookup"),   strings::get("settings.system"),
                               strings::get("settings.about"),    strings::get("kosync.title") };
    for (int index = 0; index < tabs.size(); ++index) {
        auto *button = makeButton(this, settingsTabs, tabNames[index], tabs[index], [this, index] {
            const bool same = settingsPages->currentIndex() == index;
            sectionOpen = listMode;
            settingsPages->setCurrentIndex(index);
            if (same && listMode && refitRequested)
                refitRequested();
        });
        if (index == int(SettingsPage::Dictionaries))
            button->setAccessibleName(strings::get("settings.dictionaries"));
        button->setProperty("reader_tab_label", tabs[index]);
        button->setProperty("reader_tab_icon", int(tabIcons[index]));
        button->setCheckable(true);
        tabGroup->addButton(button, index);
        settingsTabButtons.append(button);
        readerStyle(button, styles::tab(index == 0));
        // Qt 5.2 ignores a font set in the :checked rule. A property rule
        // applies on both Qt versions once the style is polished again.
        connect(button, &QPushButton::toggled, button, [button](bool on) {
            button->setProperty("kobalt_selected", on);
            button->style()->unpolish(button);
            button->style()->polish(button);
        });
        settingsTabs->setStretch(index, 1);
        if (index == int(SettingsPage::Dictionaries) && !inputs.downloadableDictionaries) {
            button->setEnabled(false);
            button->hide();
            settingsTabs->setStretch(index, 0);
        }
    }
    settingsTabs->removeWidget(settingsTabButtons[int(SettingsPage::Kosync)]);
    settingsTabs->insertWidget(int(SettingsPage::About),
                               settingsTabButtons[int(SettingsPage::Kosync)], 1);
    auto section = [](QWidget *parent, QBoxLayout *rows, const char *name, const QString &caption) {
        auto *header = new ui::ListHeader(parent);
        header->setObjectName(name);
        header->setShaded(true);
        header->setCaptions(caption.toUpper());
        header->setFixedHeight(header->sizeHint().height());
        rows->addWidget(header);
        return header;
    };
    auto *displayPage = new QWidget(settingsPages);
    displayPage->setObjectName("rrDisplayOptions");
    settingsPages->addWidget(displayPage);
    auto *generalOptions = new QVBoxLayout(displayPage);
    generalOptions->setContentsMargins(metrics::settings::displayMargins);
    generalOptions->setSpacing(0);
    auto check = [](QWidget *parent, const char *name, const QString &text,
                    const QString &description) {
        auto *box = new ReaderOption(text, description, parent);
        box->setObjectName(name);
        return box;
    };
    darkOption = check(displayPage, "rrDarkMode", strings::get("reader.dark_mode"),
                       strings::get("settings.dark_description"));
    lightDarkModeOption =
            check(displayPage, "rrLightDarkModeShortcut", strings::get("settings.light_dark_mode"),
                  strings::get("settings.light_dark_mode_description"));
    lightDarkModeOption->setVisible(inputs.lightSupported);
    const bool animationsSupported = pageAnimationsAvailable(inputs.animationSupport);
    const auto animationDescription =
            strings::get(inputs.animationSupport == PageAnimationSupport::Uncertain
                                 ? "settings.animations_uncertain_description"
                                 : animationsSupported ? "settings.animations_description"
                                                       : "settings.unsupported_description");
    animationOption = check(displayPage, "rrPageAnimations",
                            strings::get("reader.enable_page_animations"), animationDescription);
    animationOption->setEnabled(animationsSupported);
    bionicOption = check(displayPage, "rrBionicReading", strings::get("reader.bold_reading"),
                         strings::get("reader.bionic_description"));
    section(displayPage, generalOptions, "rrDisplayScreenSection",
            strings::get("settings.section_screen"));
    refreshButton = ui::makeCaptionButton(displayPage, nullptr, "rrScreenRefresh",
                                          strings::get("reader.refresh_the_screen"), [] { });
    generalOptions->addWidget(refreshButton);
    generalOptions->addWidget(animationOption);
    section(displayPage, generalOptions, "rrDisplayAppearanceSection",
            strings::get("settings.section_appearance"));
    for (auto *option : { darkOption, lightDarkModeOption, bionicOption })
        generalOptions->addWidget(option);
    section(displayPage, generalOptions, "rrDisplayColumnsSection", strings::get("reader.columns"));
    landscapeColumnsOption =
            check(displayPage, "rrLandscapeColumns", strings::get("settings.landscape_columns"),
                  strings::get("settings.landscape_columns_description"));
    generalOptions->addWidget(landscapeColumnsOption);
    columnSpacingButton =
            ui::makeCaptionButton(displayPage, generalOptions, "rrColumnSpacingSetting",
                                  strings::get("reader.column_spacing"), [] { });
    columnSpacingButton->setEnabled(false);
    connect(landscapeColumnsOption, &QCheckBox::toggled, columnSpacingButton, &QWidget::setEnabled);
    section(displayPage, generalOptions, "rrDisplayGraphsSection",
            strings::get("settings.section_graphs"));
    denseGraphOption = check(displayPage, "rrDenseGraph", strings::get("reader.dense_graph"),
                             strings::get("settings.dense_graph_description"));
    generalOptions->addWidget(denseGraphOption);
    generalOptions->addStretch(1);
    auto *controlsPage = new QWidget(settingsPages);
    controlsPage->setObjectName("rrControlsOptions");
    settingsPages->addWidget(controlsPage);
    auto *controlsRows = new QVBoxLayout(controlsPage);
    controlsRows->setContentsMargins(0, 0, 0, 0);
    controlsRows->setSpacing(0);
    section(controlsPage, controlsRows, "rrControlsNavigationSection",
            strings::get("settings.section_navigation"));
    pageNavigationButton = ui::makeCaptionButton(controlsPage, nullptr, "rrPageNavigation",
                                                 strings::get("settings.page_navigation"), [] { });
    controlsRows->addWidget(pageNavigationButton);
    tapZonesButton = ui::makeCaptionButton(controlsPage, nullptr, "rrTapZones",
                                           strings::get("settings.tap_zones"), [] { });
    controlsRows->addWidget(tapZonesButton);
    section(controlsPage, controlsRows, "rrControlsChapterListSection",
            strings::get("settings.section_chapter_list"));
    chapterGroupsButton = ui::makeCaptionButton(controlsPage, nullptr, "rrChapterGroups",
                                                strings::get("settings.chapter_groups"), [] { });
    controlsRows->addWidget(chapterGroupsButton);
    section(controlsPage, controlsRows, "rrControlsLinksSection",
            strings::get("settings.section_links"));
    tappableLinksButton = ui::makeCaptionButton(controlsPage, nullptr, "rrTappableLinks",
                                                strings::get("settings.tappable_links"), [] { });
    controlsRows->addWidget(tappableLinksButton);
    section(controlsPage, controlsRows, "rrControlsMenuSection",
            strings::get("settings.section_reading_menu"));
    menuViewButton = ui::makeCaptionButton(controlsPage, nullptr, "rrDefaultMenuView",
                                           strings::get("settings.default_menu_view"), [] { });
    controlsRows->addWidget(menuViewButton);
    section(controlsPage, controlsRows, "rrControlsNavigationBarSection",
            strings::get("settings.section_navigation_bar"));
    navigationBarButton =
            ui::makeCaptionButton(controlsPage, nullptr, "rrNavigationBarButtons",
                                  strings::get("settings.navigation_bar_buttons"), [] { });
    controlsRows->addWidget(navigationBarButton);
    minimalAnnotationOption = check(controlsPage, "rrMinimalAnnotationMode",
                                    strings::get("settings.minimal_annotation_mode"),
                                    strings::get("settings.minimal_annotation_mode_description"));
    minimalAnnotationOption->hide();
    section(controlsPage, controlsRows, "rrControlsLightSection",
            strings::get("settings.section_frontlight"));
    const bool lightSupported = inputs.lightSupported;
    edgeLightOption = check(controlsPage, "rrEdgeLight", strings::get("reader.edge_light"),
                            !lightSupported ? strings::get("settings.unsupported_description")
                                    : inputs.warmthSupported
                                    ? strings::get("settings.edge_light_description")
                                    : strings::get("settings.edge_brightness_description"));
    edgeLightOption->setEnabled(lightSupported);
    controlsRows->addWidget(edgeLightOption);
    lightGestureFeedbackOption = check(controlsPage, "rrLightGestureFeedback",
                                       strings::get("settings.light_gesture_feedback"),
                                       strings::get("settings.light_gesture_feedback_description"));
    lightGestureFeedbackOption->setEnabled(lightSupported);
    controlsRows->addWidget(lightGestureFeedbackOption);
    controlsRows->addStretch(1);
    auto *defaultsPage = new QWidget(settingsPages);
    defaultsPage->setObjectName("rrDefaultsOptions");
    settingsPages->addWidget(defaultsPage);
    auto *defaultsRows = new QVBoxLayout(defaultsPage);
    defaultsRows->setContentsMargins(metrics::settings::displayMargins);
    defaultsRows->setSpacing(0);
    section(defaultsPage, defaultsRows, "rrDefaultsNewBooksSection",
            strings::get("settings.section_new_books"));
    inheritTextLayoutOption =
            check(defaultsPage, "rrInheritTextLayout", strings::get("settings.inherit_text_layout"),
                  strings::get("settings.inherit_text_layout_description"));
    defaultsRows->addWidget(inheritTextLayoutOption);
    inheritTextLayoutOption->setChecked(true);
    section(defaultsPage, defaultsRows, "rrDefaultsGlobalFontSection",
            strings::get("settings.section_default_global_font"));
    defaultFontButton = ui::makeCaptionButton(defaultsPage, nullptr, "rrDefaultFont",
                                              strings::get("reader.default_font"), [] { });
    defaultsRows->addWidget(defaultFontButton);
    // Fallback fonts are global font defaults too, so they sit with the
    // default font rather than with the typography settings.
    section(defaultsPage, defaultsRows, "rrFontsFamiliesSection",
            strings::get("settings.font_families"));
    serifFontButton = ui::makeCaptionButton(defaultsPage, defaultsRows, "rrSerifFont",
                                            strings::get("settings.serif_font"), [] { });
    sansFontButton = ui::makeCaptionButton(defaultsPage, defaultsRows, "rrSansFont",
                                           strings::get("settings.sans_font"), [] { });
    monoFontButton = ui::makeCaptionButton(defaultsPage, defaultsRows, "rrMonoFont",
                                           strings::get("settings.mono_font"), [] { });
    section(defaultsPage, defaultsRows, "rrDefaultsRestoreSection",
            strings::get("reader.restore_defaults"));
    auto *resetRow = kobalt::ui::horizontalRow();
    defaultsRows->addLayout(resetRow);
    resetRow->addStretch(1);
    resetDefaults =
            ui::makeCaptionButton(defaultsPage, resetRow, "rrRestoreDefaults",
                                  strings::get("reader.restore_appearance_defaults"), [this] {
                                      settingsPages->setCurrentIndex(int(SettingsPage::Defaults));
                                      restoreDialog->fitCard();
                                      restoreDialog->show();
                                      restoreDialog->raise();
                                      restoreDialog->setFocus();
                                  });
    // A list action paints its own label; a text link's underline does not belong here.
    readerStyle(resetDefaults, styles::font(metrics::text::secondary));
    resetDefaults->setFixedHeight(metrics::about::actionHeight);
    resetRow->addStretch(1);
    defaultsRows->addStretch(1);
    auto *stylingPage = new QWidget(settingsPages);
    stylingPage->setObjectName("rrStylingOptions");
    settingsPages->addWidget(stylingPage);
    auto *stylingRows = new QVBoxLayout(stylingPage);
    stylingRows->setContentsMargins(0, 0, 0, 0);
    stylingRows->setSpacing(0);
    // Group line styling, paragraph layout and page spacing separately.
    section(stylingPage, stylingRows, "rrAdvancedTextSection",
            strings::get("settings.section_words_letters"));
    wordSpacingButton = ui::makeCaptionButton(stylingPage, stylingRows, "rrWordSpacing",
                                              strings::get("settings.word_spacing"), [] { });
    trackingButton = ui::makeCaptionButton(stylingPage, stylingRows, "rrJustificationTracking",
                                           strings::get("settings.justification_tracking"), [] { });
    hangingPunctuationButton =
            ui::makeCaptionButton(stylingPage, stylingRows, "rrHangingPunctuation",
                                  strings::get("settings.hanging_punctuation"), [] { });
    hangingPunctuationButton->setDescription(strings::get("settings.hanging_punctuation_summary"));
    section(stylingPage, stylingRows, "rrParagraphSpacingSection",
            strings::get("settings.section_paragraphs"));
    paragraphSpacingButton =
            ui::makeCaptionButton(stylingPage, stylingRows, "rrParagraphSpacing",
                                  strings::get("settings.paragraph_spacing"), [] { });
    paragraphSpacingButton->setDescription(strings::get("settings.paragraph_spacing_summary"));
    widowsOrphansButton = ui::makeCaptionButton(stylingPage, stylingRows, "rrWidowsOrphans",
                                                strings::get("settings.widows_orphans"), [] { });
    section(stylingPage, stylingRows, "rrVerticalSpacingSection",
            strings::get("settings.section_page_breaks"));
    verticalSpacingButton =
            ui::makeCaptionButton(stylingPage, stylingRows, "rrVerticalSpacing",
                                  strings::get("settings.vertical_spacing_mode"), [] { });
    verticalSpacingButton->setDescription(strings::get("settings.vertical_spacing_description"));
    section(stylingPage, stylingRows, "rrNormalizationSection",
            strings::get("reader.book_beautifier"));
    beautifierButton = ui::makeCaptionButton(stylingPage, stylingRows, "rrBeautifierSettings",
                                             strings::get("reader.book_beautifier"), [] { });
    beautifierButton->setDescription(strings::get("settings.beautifier_summary"));
    section(stylingPage, stylingRows, "rrAdvancedContentSection",
            strings::get("settings.section_book_content"));
    hideNonlinearOption = check(stylingPage, "rrHideNonlinearFragments",
                                strings::get("reader.hide_nonlinear_fragments"),
                                strings::get("settings.hide_nonlinear_fragments_description"));
    stylingRows->addWidget(hideNonlinearOption);
    for (const auto &[button, icon] :
         { std::pair{ beautifierButton, ui::ControlIcon::MagicWand },
           std::pair{ wordSpacingButton, ui::ControlIcon::SpreadHorizontal },
           std::pair{ paragraphSpacingButton, ui::ControlIcon::Paragraph },
           std::pair{ trackingButton, ui::ControlIcon::AlignJustify },
           std::pair{ hangingPunctuationButton, ui::ControlIcon::HangingPunctuation },
           std::pair{ widowsOrphansButton, ui::ControlIcon::WidowsOrphans },
           std::pair{ verticalSpacingButton, ui::ControlIcon::SpreadVertical } }) {
        button->setIconVisible(true);
        ui::buttonIcon(button, icon);
    }
    hideNonlinearOption->setLeadingIcon(ui::ControlIcon::Hidden);
    wordSpacingButton->setDescription(strings::get("settings.word_spacing_summary"));
    trackingButton->setDescription(strings::get("settings.justification_tracking_summary"));
    widowsOrphansButton->setDescription(strings::get("settings.widows_orphans_summary"));
    stylingRows->addStretch(1);

    auto *systemPage = new QWidget(settingsPages);
    systemPage->setObjectName("rrSystemOptions");
    settingsPages->addWidget(systemPage);
    auto *systemRows = new QVBoxLayout(systemPage);
    systemRows->setContentsMargins(0, 0, 0, 0);
    systemRows->setSpacing(0);
    section(systemPage, systemRows, "rrSystemReaderSection",
            strings::get("settings.section_reader"));
    automaticOpeningOption = check(
            systemPage, "rrAutomaticBookOpening", strings::get("settings.automatic_book_opening"),
            inputs.automaticBookOpeningSupported
                    ? strings::get("settings.automatic_book_opening_description")
                    : strings::get("settings.unsupported_description"));
    automaticOpeningOption->setEnabled(inputs.automaticBookOpeningSupported);
    systemRows->addWidget(automaticOpeningOption);
    timeRemainingFormatButton =
            ui::makeCaptionButton(systemPage, nullptr, "rrTimeRemainingFormat",
                                  strings::get("settings.time_remaining_format"), [] { });
    systemRows->addWidget(timeRemainingFormatButton);
    section(systemPage, systemRows, "rrSystemSleepSection", strings::get("settings.section_sleep"));
    sleepScreenButton = ui::makeCaptionButton(systemPage, nullptr, "rrSleepScreen",
                                              strings::get("settings.sleep_screen"), [] { });
    sleepScreenButton->setEnabled(inputs.sleepScreenSupported);
    if (!inputs.sleepScreenSupported)
        sleepScreenButton->setCaption(strings::get("settings.unsupported_caption"));
    systemRows->addWidget(sleepScreenButton);
    sleepTileButton = ui::makeCaptionButton(systemPage, nullptr, "rrSleepTile",
                                            strings::get("settings.sleep_tile"), [] { });
    sleepTileButton->setEnabled(inputs.sleepScreenSupported);
    if (!inputs.sleepScreenSupported)
        sleepTileButton->setCaption(strings::get("settings.unsupported_caption"));
    systemRows->addWidget(sleepTileButton);
    sessionTimeoutButton = ui::makeCaptionButton(systemPage, nullptr, "rrSessionTimeout",
                                                 strings::get("settings.session_timeout"), [] { });
    systemRows->addWidget(sessionTimeoutButton);
    if (inputs.wifiPowerSaverSupported) {
        powerSection = section(systemPage, systemRows, "rrSystemPowerSection",
                               strings::get("settings.section_power"));
        wifiPowerSaverOption =
                check(systemPage, "rrWifiPowerSaver", strings::get("settings.wifi_power_saver"),
                      strings::get("settings.wifi_power_saver_description"));
        systemRows->addWidget(wifiPowerSaverOption);
    }
    section(systemPage, systemRows, "rrSystemInterfaceSection",
            strings::get("settings.section_user_interface"));
    uiFontButton = ui::makeCaptionButton(systemPage, nullptr, "rrUiFont",
                                         strings::get("settings.ui_font"), [] { });
    uiMonoFontButton = ui::makeCaptionButton(systemPage, nullptr, "rrUiMonoFont",
                                             strings::get("settings.ui_mono_font"), [] { });
    headerFooterFontButton =
            ui::makeCaptionButton(systemPage, nullptr, "rrHeaderFooterFont",
                                  strings::get("settings.header_footer_font"), [] { });
    systemRows->addWidget(uiFontButton);
    systemRows->addWidget(uiMonoFontButton);
    systemRows->addWidget(headerFooterFontButton);
    headerFooterWeightButton =
            ui::makeCaptionButton(systemPage, nullptr, "rrHeaderFooterWeight",
                                  strings::get("settings.header_footer_weight"), [] { });
    systemRows->addWidget(headerFooterWeightButton);
    hideTabLabelsOption =
            check(systemPage, "rrHideTabLabels", strings::get("settings.hide_tab_labels"),
                  strings::get("settings.hide_tab_labels_description"));
    systemRows->addWidget(hideTabLabelsOption);
    section(systemPage, systemRows, "rrSystemDictionarySection",
            strings::get("settings.section_dictionary_ui"));
    dictionaryFontButton = ui::makeCaptionButton(systemPage, nullptr, "rrDictionaryFont",
                                                 strings::get("settings.dictionary_font"), [] { });
    dictionaryFontSizeButton =
            ui::makeCaptionButton(systemPage, nullptr, "rrDictionaryFontSize",
                                  strings::get("settings.dictionary_font_size"), [] { });
    systemRows->addWidget(dictionaryFontButton);
    systemRows->addWidget(dictionaryFontSizeButton);
    if (inputs.readerStandbySupported) {
        section(systemPage, systemRows, "rrSystemAdvancedSection",
                strings::get("settings.section_advanced"));
        disableStandbyOption = check(systemPage, "rrDisableReaderStandby",
                                     strings::get("settings.disable_reader_standby"),
                                     strings::get("settings.disable_reader_standby_description"));
        systemRows->addWidget(disableStandbyOption);
        standbyDialog = new ui::ChoiceModal(
                parent,
                { strings::get("settings.disable_reader_standby_warning"),
                  strings::get("settings.disable_reader_standby_title"),
                  {} },
                { strings::get("common.cancel"), "rrKeepReaderStandby" },
                { strings::get("settings.disable_reader_standby"), "rrConfirmDisableStandby" });
        standbyDialog->setObjectName("rrStandbyConfirmation");
        standbyDialog->setDimmed(true);
        connect(disableStandbyOption, &QCheckBox::clicked, this, [this](bool checked) {
            if (!checked)
                return;
            // An unanswered confirmation must never enable the preference.
            disableStandbyOption->setChecked(false);
            standbyDialog->fitCard();
            standbyDialog->show();
            standbyDialog->raise();
            standbyDialog->secondaryButton()->setFocus();
        });
        connect(standbyDialog->secondaryButton(), &QPushButton::clicked, this,
                [this] { dismissStandbyConfirmation(); });
        connect(standbyDialog->primaryButton(), &QPushButton::clicked, this, [this] {
            disableStandbyOption->setChecked(true);
            dismissStandbyConfirmation();
        });
        standbyDialog->dismissed = [this] { dismissStandbyConfirmation(); };
    }
    systemRows->addStretch(1);
    aboutPage = new AboutPage(settingsPages);
    settingsPages->addWidget(aboutPage);
    auto *aboutLayout = static_cast<QGridLayout *>(aboutPage->layout());
    openAcknowledgments = aboutPage->acknowledgmentsButton;
    if (inputs.debugProgressAvailable) {
        auto *debugRow = kobalt::ui::horizontalRow();
        aboutLayout->addLayout(debugRow, 0, 0, Qt::AlignRight | Qt::AlignBottom);
        debugProgress = actionButton(aboutPage, debugRow, "rrDebugProgress", {});
        const auto label = strings::get("reader.debug_progress");
        debugProgress->setAccessibleName(label);
        debugProgress->setToolTip(label);
        kobalt::ui::iconAction(debugProgress, ui::ControlIcon::Debug, label);
        debugProgress->setFixedWidth(metrics::control::iconButtonWidth);
        readerStyle(
                debugProgress,
                QString("padding:0;border:none;min-width:%1px;max-width:%1px;min-height:%1px;max-height:%1px;")
                        .arg(int(metrics::control::stepSize)));
        debugProgress->setFixedSize(metrics::control::stepSize, metrics::control::stepSize);
    }
    dictionaries = new DictionariesPage(settingsPages);
    settingsPages->insertWidget(int(SettingsPage::Dictionaries), dictionaries);
    kosync = new KosyncPage(settingsPages, parent, inputs.keyboard);
    settingsPages->addWidget(kosync);
    restoreDialog = new ui::ChoiceModal(
            parent,
            { strings::get(
                      "reader.your_reading_position_stays_the_same_the_defaults_apply_when_you_c"),
              strings::get("reader.restore_appearance_defaults_prompt"),
              {} },
            { strings::get("common.cancel"), "rrKeepDefaults" },
            { strings::get("reader.restore_defaults"), "rrConfirmDefaults" });
    restoreDialog->setObjectName("rrRestoreConfirmation");
    restoreDialog->setDimmed(true);
    restoreDialog->card()->findChild<QLabel *>("rrModalTitle")->setObjectName("rrRestoreTitle");
    restoreDialog->descriptionLabel()->setObjectName("rrRestoreDescription");
    keepDefaults = restoreDialog->secondaryButton();
    confirmDefaults = restoreDialog->primaryButton();
    connect(keepDefaults, &QPushButton::clicked, this, [this] { dismissRestore(); });
    connect(confirmDefaults, &QPushButton::clicked, this, [this] { dismissRestore(); });
    restoreDialog->dismissed = [this] { dismissRestore(); };
    settingsError = new ui::InterfaceLabel(this);
    settingsError->setWordWrap(true);
    settingsError->hide();
    generalRows->addWidget(settingsError);
    connect(settingsPages, &QStackedWidget::currentChanged, this, [this](int index) {
        settingsTabButtons[index]->setChecked(true);
        optionPage = 0;
    });
    settingsTabButtons.first()->setChecked(true);
    QVector<ui::DescribedOption *> options = { hideNonlinearOption,        darkOption,
                                               lightDarkModeOption,        bionicOption,
                                               landscapeColumnsOption,     animationOption,
                                               inheritTextLayoutOption,    edgeLightOption,
                                               lightGestureFeedbackOption, denseGraphOption,
                                               hideTabLabelsOption,        wifiPowerSaverOption,
                                               disableStandbyOption,       automaticOpeningOption,
                                               minimalAnnotationOption };
    for (auto *option : options) {
        if (!option)
            continue;
        auto presentation = option->presentation();
        presentation.indicator = ui::DescribedOption::Indicator::Square;
        presentation.pressedFill = true;
        presentation.dividers = Qt::BottomEdge;
        presentation.horizontalInset = chrome::inset;
        presentation.labelPixels = chrome::labelText;
        presentation.descriptionPixels = chrome::descriptionText;
        presentation.indicatorSize = { chrome::checkSize, chrome::checkSize };
        presentation.minimumRowHeight = chrome::optionHeight;
        option->setPresentation(presentation);
    }
    generalOptions->setContentsMargins(0, 0, 0, 0);
    defaultsRows->setContentsMargins(0, 0, 0, 0);
    resetRow->setStretch(0, 0);
    resetRow->setStretch(1, 1);
    resetRow->setStretch(2, 0);
    for (auto *button : { resetDefaults,
                          refreshButton,
                          columnSpacingButton,
                          sleepScreenButton,
                          sleepTileButton,
                          sessionTimeoutButton,
                          pageNavigationButton,
                          tapZonesButton,
                          chapterGroupsButton,
                          tappableLinksButton,
                          navigationBarButton,
                          menuViewButton,
                          uiFontButton,
                          uiMonoFontButton,
                          headerFooterFontButton,
                          headerFooterWeightButton,
                          dictionaryFontButton,
                          dictionaryFontSizeButton,
                          timeRemainingFormatButton,
                          defaultFontButton,
                          serifFontButton,
                          sansFontButton,
                          monoFontButton,
                          beautifierButton,
                          wordSpacingButton,
                          paragraphSpacingButton,
                          trackingButton,
                          hangingPunctuationButton,
                          widowsOrphansButton,
                          verticalSpacingButton }) {
        button->setRule(Qt::BottomEdge);
        // The refresh row's caption shows its current choice, set when Settings opens.
        if (button == resetDefaults)
            button->setCaption(strings::get("settings.asks_first"));
        ui::readerStyle(
                button,
                QString("QPushButton{font-size:%1px;min-height:%2px;max-height:%2px;border:0;border-radius:0;text-align:left;padding:0 %3px;background:palette(window);color:palette(window-text);} QPushButton:pressed{background:palette(midlight);}")
                        .arg(int(chrome::labelText))
                        .arg(int(chrome::listActionHeight))
                        .arg(int(chrome::inset)));
        if (button == beautifierButton || button == wordSpacingButton
            || button == paragraphSpacingButton || button == trackingButton
            || button == hangingPunctuationButton || button == widowsOrphansButton
            || button == verticalSpacingButton) {
            button->setMaximumHeight(QWIDGETSIZE_MAX);
            readerStyle(
                    button,
                    QString("QPushButton{font-size:%1px;border:0;border-radius:0;text-align:left;padding:0;background:palette(window);color:palette(window-text);}")
                            .arg(int(chrome::labelText)));
            button->setMinimumHeight(chrome::listActionHeight);
        } else {
            button->setFixedHeight(chrome::listActionHeight);
        }
    }
    settingsTabs->setAlignment(Qt::AlignTop);
    styleTabs(false);
    fitRail();
    hide();
}

void SettingsPanel::styleTabs(bool list)
{
    // Rail tabs stack an icon over an uppercase label. List rows put the icon
    // beside the label, as Settings rows do, and show no selection.
    const int height = list ? int(chrome::listActionHeight) : int(chrome::railRowHeight);
    for (auto *button : settingsTabButtons) {
        const auto label = button->property("reader_tab_label").toString();
        button->setText(list ? label : label.toUpper());
        button->setFixedHeight(height);
        if (auto *tab = dynamic_cast<ui::Button *>(button);
            tab && button->property("reader_tab_icon").isValid()) {
            ui::buttonIcon(button, ui::ControlIcon(button->property("reader_tab_icon").toInt()));
            button->setIconSize(QSize(chrome::tabIcon, chrome::tabIcon));
            const bool beside = list || shortRail;
            tab->setIconLabelGap(beside ? int(chrome::inset) : int(chrome::tabGap));
            tab->setIconAboveLabel(!beside);
            tab->setContentAlignment(beside ? Qt::AlignLeft : Qt::AlignHCenter);
        }
        // A list shows no selection; the rail marks the open section.
        button->setCheckable(!list);
        auto *chevron = button->findChild<QWidget *>("rrSettingsSectionChevron");
        if (!chevron && list)
            chevron = new SectionChevron(button);
        if (chevron)
            chevron->setVisible(list);
        const QString selected = list
                ? "QPushButton:pressed{background:palette(midlight);color:palette(window-text);}"
                : "QPushButton:checked,QPushButton:pressed{background:palette(window-text);color:palette(window);}";
        ui::readerStyle(
                button,
                QString("QPushButton{font-size:%1px;min-height:%2px;max-height:%2px;border:0;border-bottom:%5px solid %3;%6border-radius:0;text-align:left;padding:0 %4px;background:palette(window);color:palette(window-text);} %7")
                        .arg(list ? int(chrome::labelText) : int(chrome::tabText))
                        .arg(height)
                        .arg("palette(light)")
                        .arg(list ? int(chrome::inset) : int(chrome::railInset))
                        .arg(int(metrics::control::border))
                        .arg(list ? QString()
                                  : QString("border-right:%1px solid palette(window-text);")
                                             .arg(int(metrics::control::border)))
                        .arg(selected));
    }
    if (!list && settingsPages->currentIndex() >= 0)
        settingsTabButtons[settingsPages->currentIndex()]->setChecked(true);
    readerStyle(
            settingsPages,
            list ? QString("QStackedWidget{border:0;}")
                 : QString("QStackedWidget{border:0;border-left:%1px solid palette(window-text);}")
                            .arg(int(metrics::control::border)));
    static_cast<ui::Panel *>(optionsViewport)
            ->setEdges(list ? ui::Edges() : ui::Edges(Qt::LeftEdge));
}

int SettingsPanel::fitContent(QSize availableSize)
{
    const int headingHeight = chrome::actionHeight;
    const bool narrow = isNarrowWidth(availableSize.width(), ui::density());
    const bool shortScreen =
            !narrow && parentWidget() && isShortHeight(parentWidget()->height(), ui::density());
    if (shortScreen != shortRail) {
        shortRail = shortScreen;
        styleTabs(false);
    }
    if (narrow != listMode) {
        listMode = narrow;
        // Turning narrow while a section shows keeps that section open, so
        // a rotation does not close an open editor. Settings opens on the list.
        sectionOpen = listMode && isVisible();
        styleTabs(listMode);
    }
    const bool showList = listMode && !sectionOpen;
    tabRail->setVisible(!listMode || showList);
    optionsColumn->setVisible(!showList);
    backButton->setVisible(listMode && sectionOpen);
    closeButton->setVisible(!(listMode && sectionOpen));
    settingsTitle->setText(listMode && sectionOpen
                                   ? settingsTabButtons[settingsPages->currentIndex()]
                                             ->property("reader_tab_label")
                                             .toString()
                                   : strings::get("settings.title"));
    if (showList) {
        tabRail->setFixedWidth(availableSize.width());
        optionsPager->hide();
        return availableSize.height();
    }
    if (!listMode)
        fitRail(availableSize.height() - headingHeight);
    const int fullWidth = qMax(1, availableSize.width() - (listMode ? 0 : tabRail->width()));
    const int availableHeight = qMax(1, availableSize.height() - headingHeight);
    auto *content = settingsPages->currentWidget();
    content->ensurePolished();
    auto *rows = content->layout();
    const auto measure = [&](int width) {
        aboutPage->fitWidth(width);
        return qMax(rows->minimumSize().height(),
                    rows->hasHeightForWidth() ? rows->totalHeightForWidth(width)
                                              : rows->sizeHint().height());
    };
    const bool dictionaries = settingsPages->currentIndex() == int(SettingsPage::Dictionaries);
    settingsPages->ensurePolished();
    const auto frame = settingsPages->contentsMargins();
    const int contentWidth = qMax(1, fullWidth - frame.left() - frame.right());
    const int contentHeight = dictionaries ? availableHeight : measure(contentWidth);
    const bool overflow = contentHeight > availableHeight;
    const int panelHeight = availableHeight;
    pageHeight = qMax(1, panelHeight - (overflow ? optionsPager->height() : 0));
    optionsViewport->setFixedHeight(pageHeight);
    settingsPages->setFixedSize(fullWidth, qMax(contentHeight, pageHeight));
    settingsPages->layout()->activate();
    content->setGeometry(settingsPages->contentsRect());
    rows->setGeometry(content->rect());
    optionPages.clear();
    int start = 0, sectionStart = -1;
    bool occupied = false;
    if (overflow) {
        for (int index = 0; index < rows->count(); ++index) {
            auto *item = rows->itemAt(index);
            if (item->isEmpty())
                continue;
            auto *row = item->widget();
            const auto geometry = item->geometry().translated(content->pos());
            if (row && dynamic_cast<ui::ListHeader *>(row)) {
                if (sectionStart < 0)
                    sectionStart = geometry.top();
                continue;
            }
            const int top = sectionStart >= 0 ? sectionStart : geometry.top();
            const int bottom = geometry.bottom() + 1;
            // Keep section headings with their first option and avoid partial rows.
            if (bottom - start > pageHeight && top > start && occupied) {
                optionPages.append({ start, top });
                start = top;
            }
            // Unusually tall content still needs every part to be reachable.
            while (bottom - start > pageHeight) {
                optionPages.append({ start, start + pageHeight });
                start += pageHeight;
            }
            sectionStart = -1;
            occupied = true;
        }
    }
    optionPages.append({ start, overflow ? qMax(start + 1, contentHeight) : pageHeight });
    // Keep the About description off the logo/build page on short screens.
    // Tall screens retain the existing centered layout.
    if (overflow && settingsPages->currentIndex() == int(SettingsPage::About)) {
        const int split =
                aboutPage->descriptionTop() + aboutPage->y() - int(metrics::about::sectionGap);
        if (split > 0 && split <= pageHeight && contentHeight - split <= pageHeight)
            optionPages = { { 0, split }, { split, contentHeight } };
    }
    showOptionsPage(optionPage);
    return panelHeight + headingHeight;
}

void SettingsPanel::showOptionsPage(int page)
{
    if (optionPages.isEmpty())
        return;
    optionPage = qBound(0, page, int(optionPages.size()) - 1);
    const auto range = optionPages[optionPage];
    optionsClip->setGeometry(0, 0, settingsPages->width(),
                             qMin(pageHeight, range.second - range.first));
    settingsPages->move(0, -range.first);
    optionsPager->setPage(optionPage, optionPages.size());
    const int used = optionsClip->height();
    const int gap = pageHeight - used;
    optionsPager->setBlankArea(
            { optionsViewport->mapTo(optionsPager->parentWidget(), QPoint(chrome::inset, used)),
              QSize(qMax(1, settingsPages->width() - 2 * chrome::inset), gap) });
}

SettingsPanel::~SettingsPanel()
{
    delete restoreDialog;
    delete standbyDialog;
    delete fontDialog;
}

void SettingsPanel::showFontChoices(const QString &title, const QVector<ReadingFont> &fonts,
                                    const QString &current, const QString &emptyLabel,
                                    std::function<void(const QString &)> chosen)
{
    if (fontDialog || fonts.isEmpty())
        return;
    fontDialog = new ui::ModalOverlay(parentWidget());
    fontDialog->setObjectName("rrSettingsFontPopup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    auto *heading = card->title(title, "rrSettingsFontTitle");
    heading->setContentsMargins(chrome::inset, 0, chrome::inset,
                                metrics::notice::cardMargins.bottom());
    auto *section = new ui::ListHeader(card);
    section->setObjectName("rrSettingsFontSection");
    section->setShaded(true);
    section->setFixedHeight(section->sizeHint().height());
    rows->addWidget(section);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrSettingsFontChoices");
    choices->setMinimumRowHeight(metrics::list::oneLine);
    choices->setTopDivider(true);
    int selected = 0;
    for (int index = 0; index < fonts.size(); ++index) {
        const auto &family = fonts[index].family;
        auto *option = choices->add(QString("rrSettingsFontChoice%1").arg(index),
                                    family.isEmpty() ? emptyLabel : fontDisplayName(family));
        option->setProperty("reader_font_family", family);
        if (family == current)
            selected = index;
    }
    choices->setCurrent(selected);
    rows->addWidget(choices);
    auto *pager = new ui::PagerBar(card, "rrSettingsFont");
    rows->addWidget(pager);
    const int cardWidth = qMin<int>(metrics::notice::cardWidth,
                                    parentWidget()->width() - 2 * metrics::notice::sideClearance);
    const int rowHeight = choices->rowHeight(cardWidth - 2 * metrics::control::border);
    heading->ensurePolished();
    const int overhead = rows->contentsMargins().top() + rows->contentsMargins().bottom()
            + heading->sizeHint().height() + section->height() + pager->height();
    const int perPage = qMax(
            1,
            (parentWidget()->height() - 2 * metrics::notice::sideClearance - overhead) / rowHeight);
    QVector<QPair<int, int>> pages;
    for (int first = 0; first < fonts.size();) {
        int end = first + 1;
        while (end < fonts.size() && end - first < perPage
               && fonts[end].origin == fonts[first].origin)
            ++end;
        pages.append({ first, end - first });
        first = end;
    }
    const auto showPage = [this, choices, pager, section, card, cardWidth, rowHeight, pages,
                           fonts](int page) {
        page = qBound(0, page, int(pages.size()) - 1);
        const auto range = pages[page];
        for (int index = range.first; index < range.first + range.second; ++index) {
            auto *option = choices->options()[index];
            option->setFontPreviewSource(
                    ui::fontPreviewSource(fonts[index].family, fonts[index].files));
            auto presentation = option->presentation();
            presentation.opticalLabel = !fonts[index].family.isEmpty();
            option->setPresentation(presentation);
        }
        QString caption;
        switch (fonts[range.first].origin) {
        case FontOrigin::Recommended:
            caption = strings::get("reader.recommended_fonts");
            break;
        case FontOrigin::BuiltIn:
            caption = strings::get("reader.built_in_fonts");
            break;
        case FontOrigin::Sideloaded:
            caption = strings::get("reader.sideloaded_fonts");
            break;
        case FontOrigin::Interface:
            caption = strings::get("reader.supplemental_fonts");
            break;
        }
        section->setCaptions(caption.toUpper());
        choices->showRows(range.first, range.second);
        choices->setFixedHeight(range.second * rowHeight);
        pager->setPage(page, pages.size());
        card->fitToWidth(cardWidth);
        fontDialog->centerCard();
    };
    connect(pager->previous, &QPushButton::clicked, fontDialog,
            [pager, showPage] { showPage(pager->turnTarget(-1)); });
    connect(pager->next, &QPushButton::clicked, fontDialog,
            [pager, showPage] { showPage(pager->turnTarget(1)); });
    choices->chosen = [this, fonts, chosen](int index) {
        const auto family = fonts[index].family;
        dismissFontChoices();
        chosen(family);
    };
    int initialPage = 0;
    for (int page = 0; page < pages.size(); ++page)
        if (selected >= pages[page].first && selected < pages[page].first + pages[page].second)
            initialPage = page;
    showPage(initialPage);
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
}

QString SettingsPanel::dictionaryFontSizeLabel(int percent)
{
    switch (percent) {
    case 80:
        return strings::get("settings.dictionary_font_small");
    case 120:
        return strings::get("settings.dictionary_font_large");
    case 140:
        return strings::get("settings.dictionary_font_extra_large");
    default:
        return strings::get("settings.dictionary_font_standard");
    }
}

void SettingsPanel::showDictionaryFontSizes(int current, const QString &family, bool customFont,
                                            std::function<void(int)> chosen)
{
    if (fontDialog)
        return;
    fontDialog = new ui::ModalOverlay(parentWidget());
    fontDialog->setObjectName("rrDictionaryFontSizePopup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    auto *heading = card->title(strings::get("settings.dictionary_font_size"));
    heading->setContentsMargins(chrome::inset, 0, chrome::inset,
                                metrics::notice::cardMargins.bottom());
    auto *choices = new ui::RadioList(card);
    choices->setMinimumRowHeight(metrics::list::oneLine);
    choices->setTopDivider(true);
    for (int percent : appearance::dictionaryFontSizes) {
        auto *option = choices->add(QString("rrDictionaryFontSize%1").arg(percent),
                                    dictionaryFontSizeLabel(percent));
        const auto font = DictionaryPanel::definitionFont(family, percent);
        const int lineHeight = DictionaryPanel::definitionLineHeight(percent, customFont);
        const auto sample = strings::get("settings.dictionary_font_sample");
        auto *preview = new QWidget(option);
        preview->setObjectName(QString("rrDictionaryFontPreview%1").arg(percent));
        const int padding = ui::px(12);
        auto *layout = new QHBoxLayout(preview);
        layout->setContentsMargins(padding, padding, padding, padding);
        auto *label = new ui::SampleLabel(preview);
        label->optical = true;
        label->setTextFormat(Qt::PlainText);
        label->setFont(font);
        label->setText(sample);
        auto face = font.family();
        face.replace('\\', "\\\\");
        face.replace('"', "\\\"");
        readerStyle(
                label,
                QString("font-family:\"%1\";font-size:%2px;font-weight:400;background:transparent;border:none;")
                        .arg(face)
                        .arg(font.pixelSize()));
        label->ensurePolished();
        layout->addWidget(label);
        const auto sampleFont = label->font();
        const int height =
                qCeil(ui::interfaceLineHeight(sampleFont) * qMax(100, lineHeight) / 100.0)
                + 2 * padding;
        const int textWidth = ui::interfaceTextWidth(sample, sampleFont);
        option->setPreview(preview, { textWidth + 2 * padding, height });
        if (percent == current)
            choices->setCurrent(choices->options().size() - 1);
    }
    choices->chosen = [this, chosen](int index) {
        dismissFontChoices();
        chosen(appearance::dictionaryFontSizes[index]);
    };
    rows->addWidget(choices);
    const int cardWidth = qMin<int>(metrics::notice::cardWidth,
                                    parentWidget()->width() - 2 * metrics::notice::sideClearance);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 2 * metrics::control::border));
    card->fitToWidth(cardWidth);
    fontDialog->centerCard();
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
}

void SettingsPanel::showHeaderFooterWeights(int current, const QFont &sampleFont,
                                            std::function<void(int)> chosen)
{
    if (fontDialog)
        return;
    fontDialog = new ui::ModalOverlay(parentWidget());
    fontDialog->setObjectName("rrHeaderFooterWeightPopup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    auto *heading =
            card->title(strings::get("settings.header_footer_weight"), "rrHeaderFooterWeightTitle");
    heading->setContentsMargins(chrome::inset, 0, chrome::inset,
                                metrics::notice::cardMargins.bottom());
    auto *choices = new ui::RadioList(card);
    choices->setMinimumRowHeight(metrics::list::oneLine);
    choices->setTopDivider(true);
    for (int steps : { 0, 2, 4, 6, 8 }) {
        auto *option = choices->add(
                QString("rrHeaderFooterWeight%1").arg(steps),
                steps ? strings::get("settings.header_footer_weight_heavier").arg(steps)
                      : strings::get("settings.header_footer_weight_lighter"));
        ui::DescribedOption::ValueStyle sample;
        sample.sampleFont = sampleFont;
        sample.sampleWeight = ui::interfaceWeight() + steps;
        option->setValue(QStringLiteral("SAMPLE TEXT"), sample);
        if (steps == current)
            choices->setCurrent(choices->options().size() - 1);
    }
    choices->chosen = [this, chosen](int index) {
        dismissFontChoices();
        chosen(index * 2);
    };
    rows->addWidget(choices);
    const int cardWidth = qMin<int>(metrics::notice::cardWidth,
                                    parentWidget()->width() - 2 * metrics::notice::sideClearance);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 2 * metrics::control::border));
    card->fitToWidth(cardWidth);
    fontDialog->centerCard();
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
}

QString SettingsPanel::columnSpacingLabel(int percent)
{
    return percent == defaultColumnSpacingPercent
            ? strings::get("settings.column_spacing_default")
            : QString::number(percent / 100.0, 'f', 1) + QChar(0x00d7);
}

QString SettingsPanel::paragraphSpacingLabel(int percent)
{
    return percent < 0 ? strings::get("reader.publisher_spacing")
                       : strings::get("common.percent").arg(percent);
}

void SettingsPanel::showBeautifier(const QJsonObject &current,
                                   std::function<void(QJsonObject)> chosen)
{
    if (fontDialog)
        return;
    fontDialog = new ui::ModalOverlay(parentWidget());
    fontDialog->setObjectName("rrBeautifierPopup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    auto *heading = card->title(strings::get("reader.book_beautifier"));
    heading->setContentsMargins(chrome::inset, 0, chrome::inset,
                                metrics::notice::cardMargins.bottom());
    auto *hint = card->message(strings::get("settings.beautifier_description"));
    hint->setContentsMargins(chrome::inset, 0, chrome::inset,
                             metrics::notice::cardMargins.bottom());
    readerStyle(hint, styles::font(chrome::descriptionText));
    auto *presetRow = new QWidget(card);
    auto *presetLayout = new QHBoxLayout(presetRow);
    presetLayout->setContentsMargins(chrome::inset, 0, chrome::inset,
                                     metrics::notice::cardMargins.bottom());
    presetLayout->setSpacing(0);
    auto *presets = new QButtonGroup(card);
    const QStringList presetNames{ "recommended", "full", "custom" };
    const QStringList presetObjects{ "rrNormalizationRecommended", "rrNormalizationFull",
                                     "rrNormalizationCustom" };
    auto draft = std::make_shared<QJsonObject>(appearance::normalizationSettings(current));
    (*draft)[appearance::normalizationPresetKey] = appearance::normalizationPreset(current);
    for (int i = 0; i < presetNames.size(); ++i) {
        auto *button = segmentButton(
                presetRow, presetLayout, presets, presetObjects[i],
                strings::get(("settings.normalization_" + presetNames[i]).toUtf8().constData()));
        button->setChecked(draft->value(appearance::normalizationPresetKey) == presetNames[i]);
        presets->setId(button, i);
    }
    presets->buttons().last()->setProperty("segment_last", true);
    rows->addWidget(presetRow);
    QVector<ui::DescribedOption *> options;
    const QStringList names{ "rrNormalizationMargins", "rrNormalizationChapters",
                             "rrBeautifierInitials", "rrBeautifierSpacing", "rrBeautifierIndent" };
    const QStringList labels{ "settings.normalization_margins", "settings.normalization_chapters",
                              "settings.beautifier_initials", "settings.beautifier_spacing",
                              "settings.beautifier_indent" };
    for (int i = 0; i < int(appearance::normalizationKeys.size()); ++i) {
        const auto prefix = labels[i];
        auto *option = new ui::DescribedOption(
                strings::get(prefix.toUtf8().constData()),
                strings::get((prefix + "_description").toUtf8().constData()), card);
        option->setObjectName(names[i]);
        auto presentation = option->presentation();
        presentation.indicator = ui::DescribedOption::Indicator::Square;
        presentation.pressedFill = true;
        presentation.leadingIndicator = true;
        presentation.dividers = Qt::BottomEdge;
        if (i == 0)
            presentation.dividers |= Qt::TopEdge;
        presentation.horizontalInset = chrome::inset;
        presentation.labelPixels = chrome::labelText;
        presentation.descriptionPixels = chrome::descriptionText;
        presentation.indicatorSize = { chrome::checkSize, chrome::checkSize };
        presentation.minimumRowHeight = chrome::optionHeight;
        option->setPresentation(presentation);
        option->setChecked(draft->value(appearance::normalizationKeys[i]).toBool());
        connect(option, &QCheckBox::clicked, card, [draft, presets, i](bool checked) {
            (*draft)[appearance::normalizationKeys[i]] = checked;
            (*draft)[appearance::normalizationPresetKey] = QStringLiteral("custom");
            presets->button(2)->setChecked(true);
        });
        rows->addWidget(option);
        options.append(option);
    }
    for (int i = 0; i < presetNames.size(); ++i) {
        connect(presets->button(i), &QPushButton::clicked, card, [draft, options, presetNames, i] {
            if (i < 2)
                *draft = appearance::normalizationPresetSettings(presetNames[i]);
            else
                (*draft)[appearance::normalizationPresetKey] = QStringLiteral("custom");
            for (int j = 0; j < options.size(); ++j)
                options[j]->setChecked(draft->value(appearance::normalizationKeys[j]).toBool());
        });
    }
    auto *cancel = ui::makeModalButton(
            card, { strings::get("common.cancel"), "rrBeautifierCancel" }, false);
    auto *done =
            ui::makeModalButton(card, { strings::get("common.done"), "rrBeautifierDone" }, true);
    card->adaptiveActions({ cancel, done });
    connect(cancel, &QPushButton::clicked, this, [this] { dismissFontChoices(); });
    connect(done, &QPushButton::clicked, this, [this, draft, chosen] {
        const auto value = *draft;
        dismissFontChoices();
        chosen(value);
    });
    const int width =
            qMin<int>(ui::px(1064), parentWidget()->width() - 2 * metrics::notice::sideClearance);
    for (auto *option : options)
        option->setFixedHeight(option->heightForWidth(width - 2 * metrics::control::border));
    card->fitToWidth(width);
    fontDialog->centerCard();
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
}

QString SettingsPanel::timeRemainingFormatLabel(const QString &format)
{
    return strings::get(format == "approximate" ? "settings.time_remaining_approximate"
                                                : "settings.time_remaining_detailed");
}

void SettingsPanel::showTimeRemainingFormats(const QString &current,
                                             std::function<void(const QString &)> chosen)
{
    if (fontDialog)
        return;
    fontDialog = new ui::ModalOverlay(parentWidget());
    fontDialog->setObjectName("rrTimeRemainingFormatPopup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    auto *heading = card->title(strings::get("settings.time_remaining_format"));
    heading->setContentsMargins(chrome::inset, 0, chrome::inset,
                                metrics::notice::cardMargins.bottom());
    auto *hint = card->message(strings::get("settings.time_remaining_format_description"));
    hint->setContentsMargins(chrome::inset, 0, chrome::inset,
                             metrics::notice::cardMargins.bottom());
    readerStyle(hint, styles::font(chrome::descriptionText));
    auto *choices = new ui::RadioList(card);
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
    for (const auto *format : { "detailed", "approximate" }) {
        choices->add(QString("rrTimeRemainingFormat_%1").arg(format),
                     timeRemainingFormatLabel(format),
                     strings::get(QString("settings.time_remaining_%1_description")
                                          .arg(format)
                                          .toLatin1()
                                          .constData()));
        if (format == current)
            choices->setCurrent(choices->options().size() - 1);
    }
    choices->chosen = [this, chosen](int index) {
        dismissFontChoices();
        chosen(index == 1 ? QStringLiteral("approximate") : QStringLiteral("detailed"));
    };
    rows->addWidget(choices);
    const int cardWidth = qMin<int>(metrics::notice::cardWidth,
                                    parentWidget()->width() - 2 * metrics::notice::sideClearance);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 2 * metrics::control::border));
    card->fitToWidth(cardWidth);
    fontDialog->centerCard();
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
}

void SettingsPanel::showColumnSpacingChoices(int current, std::function<void(int)> chosen)
{
    if (fontDialog)
        return;
    fontDialog = new ui::ModalOverlay(parentWidget());
    fontDialog->setObjectName("rrColumnSpacingPopup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    auto *heading = card->title(strings::get("reader.column_spacing"));
    heading->setContentsMargins(chrome::inset, 0, chrome::inset,
                                metrics::notice::cardMargins.bottom());
    auto *hint = card->message(strings::get("settings.column_spacing_description"));
    hint->setContentsMargins(chrome::inset, 0, chrome::inset,
                             metrics::notice::cardMargins.bottom());
    readerStyle(hint, styles::font(chrome::descriptionText));
    auto *choices = new ui::RadioList(card);
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
    for (int percent : { 100, 150, 200, 250, 300 }) {
        choices->add(QString("rrColumnSpacing%1").arg(percent), columnSpacingLabel(percent));
        if (percent == current)
            choices->setCurrent(choices->options().size() - 1);
    }
    choices->chosen = [this, chosen](int index) {
        dismissFontChoices();
        chosen(100 + index * 50);
    };
    rows->addWidget(choices);
    const int cardWidth = qMin<int>(metrics::notice::cardWidth,
                                    parentWidget()->width() - 2 * metrics::notice::sideClearance);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 2 * metrics::control::border));
    card->fitToWidth(cardWidth);
    fontDialog->centerCard();
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
}

namespace {
// The styling sheet is wider than other choices so its sample pages are large
// enough to read. It leaves the usual clearance at the screen edges.
int stylingSheetWidth(const QWidget *parent)
{
    return qMax(1,
                qMin(int(ui::px(1264)), parent->width() - 2 * int(metrics::notice::sideClearance)));
}

// Width available to the sample inside the sheet's border and side padding.
int stylingSampleWidth(const QWidget *parent)
{
    return qMax(
            1, stylingSheetWidth(parent) - 2 * int(metrics::control::border) - 2 * int(ui::px(24)));
}

constexpr int stylingPreviewPadding = 20;

QPushButton *segmentButton(QWidget *parent, QHBoxLayout *layout, QButtonGroup *group,
                           const QString &name, const QString &text)
{
    auto *button = new ui::Button(text, parent);
    button->setObjectName(name);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setProperty("segment_first", group->buttons().isEmpty());
    readerStyle(button, styles::segment());
    group->addButton(button);
    layout->addWidget(button, 1);
    return button;
}
} // namespace

int SettingsPanel::stylingEdgeHeight() const
{
    return ui::px(200);
}

QSize SettingsPanel::stylingEdgeSize(QSize readingPage) const
{
    // The page's full width, scaled to the sheet, shows this much height.
    const qreal scale = qMin<qreal>(1,
                                    qreal(stylingSampleWidth(parentWidget()))
                                            * qMax<qreal>(1, devicePixelRatio())
                                            / qMax(1, readingPage.width()));
    return { readingPage.width(),
             qRound(stylingEdgeHeight() * qMax<qreal>(1, devicePixelRatio()) / scale) };
}

QSize SettingsPanel::stylingPageSize(QSize readingPage, bool edge) const
{
    if (readingPage.isEmpty())
        return {};
    // Two pages side by side, no taller than about two fifths of the screen,
    // or three tenths when the magnified edge shares the sheet.
    const qreal ratio = qMax<qreal>(1, devicePixelRatio());
    const int width = (stylingSampleWidth(parentWidget()) - ui::px(16)) / 2;
    QSize page(width, qRound(qreal(width) * readingPage.height() / readingPage.width()));
    const int tallest = edge ? parentWidget()->height() * 3 / 10 : parentWidget()->height() * 2 / 5;
    if (page.height() > tallest)
        page = QSize(qRound(qreal(tallest) * readingPage.width() / readingPage.height()), tallest);
    return page * ratio;
}

QSize SettingsPanel::stylingCropSize(QSize readingPage, int horizontalMargin) const
{
    // Whole lines, so the spaces at both ends of a line stay visible. The
    // preview scales them down only when the line is wider than the sheet. The reading
    // page's pixels are interface pixels at every screen scale.
    const int text = qMax(1, readingPage.width() - 2 * horizontalMargin);
    const qreal scale = qMin<qreal>(1, qreal(stylingSampleWidth(parentWidget())) / text);
    return { text, qMin(readingPage.height(), qRound(ui::px(300) / scale)) };
}

QSize SettingsPanel::stylingTextPageSize(QSize readingPage, int horizontalMargin) const
{
    // Reflow wide reading lines for the capped modal instead of shrinking their
    // glyphs. Keep the book's margins and the paper beside the cropped text.
    if (readingPage.width() <= ui::px(1264))
        return readingPage;
    const int side = qMin(horizontalMargin, int(ui::px(16)));
    readingPage.setWidth(qMin(readingPage.width(),
                              stylingSampleWidth(parentWidget()) + 2 * horizontalMargin
                                      - 2 * side));
    return readingPage;
}

void SettingsPanel::showAdvancedTextChoices(AdvancedText feature, int current,
                                            std::function<void(int)> chosen,
                                            const QString &alignment)
{
    showStylingChoices(feature, current, std::move(chosen), alignment, -1, {});
}

void SettingsPanel::showPageSpacingChoices(int mode, int scope,
                                           std::function<void(int, int)> chosen)
{
    showStylingChoices(AdvancedText::VerticalSpacing, mode, {}, {}, scope, std::move(chosen));
}

void SettingsPanel::showParagraphAdjustments(int spacing, int indent,
                                             std::function<void(int, int)> chosen)
{
    showStylingChoices(AdvancedText::ParagraphSpacing, spacing, {}, {}, indent, std::move(chosen));
}

void SettingsPanel::showStylingChoices(AdvancedText feature, int current,
                                       std::function<void(int)> chosen, const QString &alignment,
                                       int secondary, std::function<void(int, int)> pairChosen)
{
    if (fontDialog)
        return;
    ui::PanelTiming timing("styling");
    timing.field("feature", int(feature));
    fontDialog = new ui::ModalOverlay(parentWidget());
    const bool tracking = feature == AdvancedText::Tracking;
    const bool paragraph = feature == AdvancedText::ParagraphSpacing;
    const bool spacing = feature == AdvancedText::WordSpacing || tracking
            || feature == AdvancedText::HangingPunctuation;
    const bool vertical = feature == AdvancedText::VerticalSpacing;
    QString key, prefix;
    QStringList values;
    switch (feature) {
    case AdvancedText::ParagraphSpacing:
        key = "settings.paragraph_spacing";
        prefix = "rrParagraphSpacing";
        break;
    case AdvancedText::WordSpacing:
        key = "settings.word_spacing";
        prefix = "rrWordSpacing";
        values = { "narrow", "normal", "wide" };
        break;
    case AdvancedText::Tracking:
        key = "settings.justification_tracking";
        prefix = "rrTracking";
        values = { "off", "subtle", "more" };
        break;
    case AdvancedText::HangingPunctuation:
        key = "settings.hanging_punctuation";
        prefix = "rrHangingPunctuation";
        values = { "off", "on" };
        break;
    case AdvancedText::Pagination:
        key = "settings.widows_orphans";
        prefix = "rrWidowsOrphans";
        values = { "publisher", "avoid", "allow" };
        break;
    case AdvancedText::VerticalSpacing:
        // Centering is the recommended adjustment, so it comes before Expand.
        key = "settings.vertical_spacing";
        prefix = "rrVerticalSpacing";
        values = { "off", "center", "expand" };
        break;
    }
    const auto text = [key](const QString &suffix) {
        return strings::get((key + suffix).toUtf8().constData());
    };
    fontDialog->setObjectName(prefix + "Popup");
    fontDialog->setDimmed(true);
    fontDialog->setGeometry(parentWidget()->rect());
    fontDialog->dismissed = [this] { dismissFontChoices(); };
    auto *card = fontDialog->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::control::border,
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);

    // The sheet stays open while choosing. Close is the only way out besides
    // tapping outside, so it sits where Settings keeps its own close button.
    auto *header = new ui::Panel(card, Qt::BottomEdge);
    header->setFixedHeight(chrome::actionHeight);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(0);
    auto *title = new ui::InterfaceLabel(text(""), header);
    title->setObjectName("rrAdvancedTextTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    readerStyle(title, styles::font(chrome::labelText, true));
    headerRow->addWidget(title, 1);
    auto *close = new ui::CellButton(ui::ControlIcon::Close, header);
    close->setObjectName(prefix + "Close");
    close->setAccessibleName(strings::get("settings.close"));
    close->setRule(Qt::LeftEdge);
    close->setFixedWidth(chrome::cellWidth);
    headerRow->addWidget(close);
    connect(close, &QPushButton::clicked, this, [this] { dismissFontChoices(); });
    rows->addWidget(header);

    auto *preview = new StylingPreview(card);
    const qreal ratio = qMax<qreal>(1, devicePixelRatio());
    const int edgeHeight = vertical ? stylingEdgeHeight() : 0;
    preview->setEdgeHeight(edgeHeight);
    const int sampleHeight = paragraph
            ? qMin(int(ui::px(580) + 2 * ui::px(24)), parentWidget()->height() * 2 / 5)
            : spacing ? ui::px(300) + 2 * ui::px(24)
                      : qRound(stylingPageSize(styleSamplePageSize(), vertical).height() / ratio)
                    + (edgeHeight ? edgeHeight + ui::px(16) : 0);
    preview->setFixedHeight(sampleHeight + 2 * ui::px(stylingPreviewPadding));
    preview->setProperty("preferred_height", preview->height());
    rows->addWidget(preview);
    stylingPreview = preview;
    stylingFeature = feature;
    stylingChoice = paragraph ? 0 : current;
    stylingSampleReady = [timing, preview = QPointer<QWidget>(preview)]() mutable {
        timing.stage("sample");
        timing.finish(preview);
    };
    // Wrapped labels measure their height without contents margins, so their
    // inset comes from a container's layout.
    const auto padded = [card, rows](const QMargins &margins) {
        auto *area = new QWidget(card);
        auto *areaRows = new QVBoxLayout(area);
        areaRows->setContentsMargins(margins);
        areaRows->setSpacing(ui::px(12));
        rows->addWidget(area);
        return std::make_pair(area, areaRows);
    };
    const auto [same, sameRows] = padded({ chrome::inset, ui::px(12), chrome::inset, 0 });
    same->setObjectName("rrStylingSameNote");
    auto *sameText = new ui::InterfaceLabel(strings::get("settings.styling_preview_same"), same);
    sameText->setAlignment(Qt::AlignCenter);
    sameText->setWordWrap(true);
    readerStyle(sameText,
                styles::font(chrome::descriptionText)
                        + QString("color:%1;").arg(ui::noteColor(sameText).name()));
    sameRows->addWidget(sameText);
    same->hide();

    auto *choices = new QWidget(card);
    auto *choiceRow = new QHBoxLayout(choices);
    choiceRow->setContentsMargins(chrome::inset, ui::px(24), chrome::inset, 0);
    choiceRow->setSpacing(0);
    auto *group = new QButtonGroup(choices);
    QVector<QPushButton *> buttons;
    for (const auto &value : values)
        buttons.append(
                segmentButton(choices, choiceRow, group, prefix + "_" + value, text("_" + value)));
    rows->addWidget(choices);
    const auto [texts, textRows] = padded({ chrome::inset, ui::px(16), chrome::inset, ui::px(24) });
    auto *description = new ui::InterfaceLabel(texts);
    description->setObjectName("rrAdvancedTextDescription");
    description->setWordWrap(true);
    readerStyle(description, styles::font(chrome::descriptionText));
    textRows->addWidget(description);
    // Tracking depends on the alignment chosen in Text, so its note explains
    // whether the current text uses it.
    QString note;
    if (tracking) {
        const auto suffix = alignment == "left" ? "left"
                : alignment == "justify"        ? "justified"
                : alignment == "publisher"      ? "publisher"
                                                : "other";
        note = strings::get((QString("settings.justification_tracking_current_") + suffix)
                                    .toUtf8()
                                    .constData());
    } else {
        note = text("_description");
    }
    auto *hint = new ui::InterfaceLabel(note, texts);
    hint->setObjectName(tracking ? "rrTrackingAlignmentNotice" : "rrAdvancedTextHint");
    hint->setWordWrap(true);
    readerStyle(hint,
                styles::font(chrome::descriptionText)
                        + QString("color:%1;").arg(ui::noteColor(hint).name()));
    textRows->addWidget(hint);

    const auto describe = [description, buttons, text, values](int index) {
        for (int other = 0; other < buttons.size(); ++other)
            buttons[other]->setChecked(other == index);
        description->setText(text("_" + values[index]) + ". "
                             + text("_" + values[index] + "_description"));
    };
    if (paragraph) {
        const auto [area, controls] =
                padded({ chrome::inset, ui::px(12), chrome::inset, ui::px(16) });
        area->setObjectName("rrParagraphControls");
        rows->removeWidget(area);
        rows->insertWidget(rows->indexOf(choices), area);
        choices->hide();
        auto *slider = ui::sliderRow(
                area, controls, "rrParagraphSpacingSlider", "rrParagraphSpacingValue",
                strings::get("settings.paragraph_gap"), "rrParagraphSpacingLess",
                "rrParagraphSpacingMore", -1, 200, 1, {}, {}, {},
                { strings::get("reader.publisher_spacing"), "0", "50%", "100%", "150%", "200%" });
        slider->setMarkers({ -1, 0, 50, 100, 150, 200 }, ui::SettingSlider::MarkerMode::Stops);
        slider->setAdditionalStops({ 25, 75, 125, 175 });
        description->setText(strings::get("settings.paragraph_choices_description"));
        area->findChild<QLabel *>("rrParagraphSpacingSliderCurrentPrefix")->hide();
        const auto showValue = [slider, area](int value) {
            area->findChild<QLabel *>("rrParagraphSpacingValue")
                    ->setText(strings::get("controls.currently") + ' '
                              + paragraphSpacingLabel(value));
            slider->setAccessibleDescription(paragraphSpacingLabel(value));
        };
        connect(slider, &QSlider::valueChanged, slider, showValue);
        slider->setValue(current);
        showValue(current);
        auto *indentSlider = ui::sliderRow(
                area, controls, "rrParagraphIndentSlider", "rrParagraphIndentValue",
                strings::get("settings.paragraph_indent"), "rrParagraphIndentLess",
                "rrParagraphIndentMore", -1, 300, 1, {}, {}, {},
                { strings::get("reader.publisher_spacing"), "0", "100%", "200%", "300%" });
        indentSlider->setMarkers({ -1, 0, 100, 200, 300 }, ui::SettingSlider::MarkerMode::Stops);
        indentSlider->setAdditionalStops({ 25, 50, 75, 125, 150, 175 });
        area->findChild<QLabel *>("rrParagraphIndentSliderCurrentPrefix")->hide();
        const auto showIndent = [indentSlider, area](int value) {
            area->findChild<QLabel *>("rrParagraphIndentValue")
                    ->setText(strings::get("controls.currently") + ' '
                              + paragraphSpacingLabel(value));
            indentSlider->setAccessibleDescription(paragraphSpacingLabel(value));
        };
        connect(indentSlider, &QSlider::valueChanged, indentSlider, showIndent);
        slider->committed = [indentSlider, pairChosen](int value) {
            pairChosen(value, indentSlider->value());
        };
        indentSlider->committed = [slider, pairChosen](int value) {
            pairChosen(slider->value(), value);
        };
        indentSlider->setValue(secondary);
        showIndent(secondary);
    } else {
        describe(current);
    }
    if (vertical && pairChosen) {
        auto selection = std::make_shared<std::pair<int, int>>(current, secondary);
        auto *area = new QWidget(card);
        area->setObjectName("rrVerticalSpacingScopeControls");
        auto *rows = new QVBoxLayout(area);
        rows->setContentsMargins(chrome::inset, 0, chrome::inset, ui::px(24));
        rows->setSpacing(ui::px(16));
        auto *label = new ui::InterfaceLabel(strings::get("settings.vertical_spacing_scope"), area);
        readerStyle(label, styles::font(chrome::labelText));
        rows->addWidget(label);
        auto *segments = new QHBoxLayout;
        segments->setSpacing(0);
        rows->addLayout(segments);
        auto *group = new QButtonGroup(area);
        const QStringList values{ "all", "protection" };
        const int segmentWidth = (stylingSheetWidth(parentWidget())
                                  - 2 * int(metrics::control::border) - 2 * int(chrome::inset))
                / 2;
        for (int index = 0; index < values.size(); ++index) {
            const QString key = "settings.vertical_spacing_scope_" + values[index];
            auto *button =
                    segmentButton(area, segments, group, "rrVerticalSpacingScope_" + values[index],
                                  strings::get(key.toUtf8().constData()));
            button->setAccessibleName(button->text());
            button->setAccessibleDescription(
                    strings::get((key + "_description").toUtf8().constData()));
            button->ensurePolished();
            if (ui::interfaceTextWidth(button->text(), button->font()) + ui::px(36) > segmentWidth)
                button->setText(strings::get((key + "_short").toUtf8().constData()));
            button->setChecked(index == secondary);
            connect(button, &QPushButton::clicked, this, [selection, pairChosen, index] {
                selection->second = index;
                pairChosen(selection->first, index);
            });
        }
        area->setEnabled(current != 0);
        card->rows()->addWidget(area);
        hint->hide();
        chosen = [area, selection, pairChosen](int value) {
            selection->first = value;
            area->setEnabled(value != 0);
            pairChosen(value, selection->second);
        };
    }
    for (int index = 0; index < buttons.size(); ++index)
        connect(buttons[index], &QPushButton::clicked, this, [this, index, describe, chosen] {
            stylingChoice = index;
            describe(index);
            showStylingPreview();
            chosen(index);
        });
    const int cardWidth = stylingSheetWidth(parentWidget());
    // Reserve the height of the longest description, so choosing an option
    // does not move the controls below it.
    const auto reserve = [cardWidth](QLabel *label, const QStringList &texts) {
        label->ensurePolished();
        const int width =
                qMax(1, cardWidth - 2 * int(metrics::control::border) - 2 * int(chrome::inset));
        int tallest = 0;
        for (const auto &text : texts)
            tallest = qMax(tallest, ui::wrappedInterfaceTextHeight(text, label->font(), width));
        label->setMinimumHeight(tallest);
    };
    QStringList descriptions;
    for (const auto &value : values)
        descriptions.append(text("_" + value) + ". " + text("_" + value + "_description"));
    if (paragraph)
        descriptions = { strings::get("settings.paragraph_choices_description") };
    reserve(description, descriptions);
    // Long labels do not fit their segments on a narrow card. Choices with a
    // short label use it there; the description below names the full choice.
    const int segmentWidth =
            (cardWidth - 2 * int(metrics::control::border) - 2 * int(chrome::inset))
            / qMax(1, int(buttons.size()));
    bool labelsFit = true;
    for (auto *button : buttons)
        labelsFit &=
                ui::interfaceTextWidth(button->text(), button->font()) + ui::px(36) <= segmentWidth;
    for (int index = 0; !labelsFit && index < buttons.size(); ++index) {
        const auto shortKey = key + "_" + values[index] + "_short";
        const auto label = strings::get(shortKey.toUtf8().constData());
        if (label != shortKey) {
            buttons[index]->setAccessibleName(buttons[index]->text());
            buttons[index]->setText(label);
        }
    }
    card->fitToWidth(cardWidth);
    fitStylingCard();
    fontDialog->show();
    fontDialog->raise();
    fontDialog->setFocus();
    showStylingPreview();
    if (!stylingSamples.value(int(feature)).valid() && stylingRequested)
        stylingRequested(feature);
}

void SettingsPanel::fitStylingCard()
{
    if (!fontDialog || !stylingPreview)
        return;
    auto *card = fontDialog->card();
    auto *preview = stylingPreview.data();
    preview->setFixedHeight(preview->property("preferred_height").toInt());
    card->fitToWidth(card->width());
    const int available = parentWidget()->height() - 2 * metrics::notice::sideClearance;
    if (card->height() > available) {
        // Keep the text and controls at their normal size. The visual sample
        // uses the space left above them on a phone or a landscape screen.
        preview->setFixedHeight(
                qMax(ui::px(120), preview->height() - (card->height() - available)));
        card->fitToWidth(card->width());
    }
    fontDialog->centerCard();
}

void SettingsPanel::showStylingPreview()
{
    auto *preview = static_cast<StylingPreview *>(stylingPreview.data());
    if (!preview)
        return;
    const auto sample = stylingSamples.value(int(stylingFeature));
    preview->show(sample, stylingChoice);
    preview->setProperty("sample_ready", sample.valid());
    if (sample.valid() && stylingSampleReady) {
        auto ready = stylingSampleReady;
        stylingSampleReady = {};
        ready();
    }
    if (auto *same = fontDialog ? fontDialog->findChild<QWidget *>("rrStylingSameNote") : nullptr) {
        const bool shown = sample.valid() && !sample.differs;
        if (same->isVisible() != shown) {
            same->setVisible(shown);
            fitStylingCard();
        }
    }
}

void SettingsPanel::setStylingSample(AdvancedText feature, const StyleSample &sample)
{
    stylingSamples[int(feature)] = sample;
    if (stylingPreview && stylingFeature == feature)
        showStylingPreview();
}

void SettingsPanel::invalidateStylingSamples(int unchanged)
{
    const auto kept = stylingSamples.value(unchanged);
    stylingSamples.clear();
    if (kept.valid())
        stylingSamples.insert(unchanged, kept);
    if (stylingPreview && int(stylingFeature) != unchanged) {
        showStylingPreview();
        if (stylingRequested)
            stylingRequested(stylingFeature);
    }
}

void SettingsPanel::dismissFontChoices()
{
    if (!fontDialog)
        return;
    // The closed sheet is deleted later. Until then, only the next sheet's
    // preview may answer to the shared name.
    if (stylingPreview)
        stylingPreview->setObjectName({});
    stylingPreview.clear();
    stylingSampleReady = {};
    fontDialog->hide();
    fontDialog->deleteLater();
    fontDialog.clear();
}

void SettingsPanel::dismissRestore()
{
    if (!restoreDialog || restoreDialog->isHidden())
        return;
    restoreDialog->hide();
    resetDefaults->setFocus();
}

void SettingsPanel::dismissStandbyConfirmation()
{
    if (!standbyDialog || standbyDialog->isHidden())
        return;
    standbyDialog->hide();
    disableStandbyOption->setFocus();
}

void SettingsPanel::hideEvent(QHideEvent *event)
{
    // The next opening starts on the section list.
    sectionOpen = false;
    dismissRestore();
    dismissStandbyConfirmation();
    dismissFontChoices();
    if (stylingHidden)
        stylingHidden();
    ui::Panel::hideEvent(event);
}

void SettingsPanel::resizeEvent(QResizeEvent *event)
{
    // A rotation can leave too few rows for the current font page. Reopen at the new size.
    dismissFontChoices();
    ui::Panel::resizeEvent(event);
}

void SettingsPanel::showDictionaries(bool visible)
{
    auto *button = settingsTabButtons[int(SettingsPage::Dictionaries)];
    button->setVisible(visible);
    button->setEnabled(visible);
    if (!visible && settingsPages->currentIndex() == int(SettingsPage::Dictionaries))
        settingsPages->setCurrentIndex(int(SettingsPage::Display));
    fitRail();
}

void SettingsPanel::showPowerManagement(bool visible)
{
    if (!powerSection)
        return;
    powerSection->setVisible(visible);
    wifiPowerSaverOption->setVisible(visible);
}

void SettingsPanel::fitRail(int availableHeight)
{
    // As wide as the widest visible label, or the icon above it, with the
    // rail's margin on each side. Without Dictionaries the rail narrows.
    int visible = 0;
    for (auto *button : settingsTabButtons)
        if (!button->isHidden())
            ++visible;
    // The rail uses the full panel height, independently of the list pager.
    const int rowHeight = availableHeight > 0
            ? qMin<int>(chrome::railRowHeight, qMax(1, availableHeight) / qMax(1, visible))
            : int(chrome::railRowHeight);
    int widest = chrome::tabIcon;
    for (auto *button : settingsTabButtons)
        if (!button->isHidden()) {
            if (button->height() != rowHeight) {
                auto style = button->property("reader_style").toString();
                // Style minimums must agree with the fitted height on Qt 5.2 too.
                style.replace(QRegularExpression("(min-height|max-height):[0-9]+px"),
                              "\\1:" + QString::number(rowHeight) + "px");
                ui::readerStyle(button, style);
                button->setFixedHeight(rowHeight);
                button->setIconSize(QSize(
                        chrome::tabIcon,
                        qMax(1,
                             qMin<int>(chrome::tabIcon,
                                       rowHeight - chrome::tabText - chrome::tabGap - ui::px(8)))));
            }
            button->ensurePolished();
            widest = qMax(widest,
                          ui::interfaceTextWidth(button->text(), button->font())
                                  + (shortRail ? int(chrome::tabIcon) + int(chrome::inset) : 0));
        }
    tabRail->setFixedWidth(widest + 2 * chrome::railInset);
}

} // namespace kobalt::reader
