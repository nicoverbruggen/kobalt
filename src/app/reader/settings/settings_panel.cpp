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
#include "ui/dialogs/choice_sheet.h"
#include "support/qt_compat.h"
#include "reading/text_presets.h"
#include "reading/features.h"
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
#include <algorithm>
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
#include <QTimer>

namespace kobalt::reader {
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;
namespace styles = ui::styles;
using ui::makeButton;
using ui::readerStyle;

namespace {

struct FeatureOption
{
    features::Feature feature;
    const char *label;
    ui::ControlIcon icon;
};

constexpr FeatureOption featureOptions[] = {
    { features::Navigate, "features.navigate", ui::ControlIcon::Compass },
    { features::Stats, "features.stats", ui::ControlIcon::Graph },
    { features::Activity, "features.activity", ui::ControlIcon::Calendar },
    { features::Kosync, "features.kosync", ui::ControlIcon::Sync },
    { features::Annotations, "features.annotations", ui::ControlIcon::Annotate },
    { features::Bookmarks, "features.bookmarks", ui::ControlIcon::Bookmark },
    { features::HighlightLinks, "features.highlight_links", ui::ControlIcon::Link },
    { features::Words, "features.words", ui::ControlIcon::Note },
};
static_assert(sizeof(featureOptions) / sizeof(*featureOptions)
                      == sizeof(features::keys) / sizeof(*features::keys),
              "A feature without a checklist row");

// A retained confirmation might have a new font. If Qt has a resize to
// deliver on show, that resize already fits it with the current font.
void showConfirmation(ui::ChoiceModal *dialog)
{
    if (!dialog->testAttribute(Qt::WA_PendingResizeEvent))
        dialog->fitCard();
    dialog->show();
    dialog->raise();
}

ui::DescribedOption *settingsOption(QWidget *parent, const QString &name, const QString &label,
                                    const QString &description, bool first = false,
                                    bool leading = false)
{
    auto *option = new ui::DescribedOption(label, description, parent);
    option->setObjectName(name);
    auto presentation = option->presentation();
    presentation.indicator = ui::DescribedOption::Indicator::Square;
    presentation.pressedFill = true;
    presentation.leadingIndicator = leading;
    presentation.dividers = Qt::BottomEdge;
    if (first)
        presentation.dividers |= Qt::TopEdge;
    presentation.horizontalInset = chrome::inset;
    presentation.labelPixels = chrome::labelText;
    presentation.descriptionPixels = chrome::descriptionText;
    presentation.indicatorSize = { chrome::checkSize, chrome::checkSize };
    presentation.minimumRowHeight = chrome::optionHeight;
    option->setPresentation(presentation);
    return option;
}

} // namespace

ui::DescribedOption *checklistOption(QWidget *parent, const QString &name, const QString &label,
                                     const QString &description, bool first)
{
    return settingsOption(parent, name, label, description, first, true);
}

namespace {
// Leaves Zen mode only after a held press. While held, it inverts and fills
// three steps, one update each, so an e-ink panel sees no animation. Letting
// go early clears the steps. The step length is half a second; the
// `reader_hold_step_ms` property shortens it for tests.
class HoldButton final : public QPushButton
{
public:
    HoldButton(const QString &label, const QString &holding, QWidget *parent)
        : QPushButton(label, parent), idleLabel(label), holdingLabel(holding)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(label);
        timer.setSingleShot(false);
        connect(&timer, &QTimer::timeout, this, [this] { advance(); });
        connect(this, &QPushButton::pressed, this, [this] {
            const int interval = property("reader_hold_step_ms").toInt();
            setStep(0);
            timer.start(interval > 0 ? interval : 500);
            update();
        });
        connect(this, &QPushButton::released, this, [this] { cancel(); });
    }

    static constexpr int steps = 3;
    std::function<void()> held;

    // In the heading the button is a cell like the close cell: a rule on its
    // left instead of a heavy frame.
    void setCell(bool value)
    {
        cell = value;
        // Qt reapplies the sheet on a theme change. Keep the height here
        // so the button still leaves room for the heading's bottom rule.
        const int height = chrome::actionHeight - (cell ? int(chrome::heavyRule) : 0);
        readerStyle(this,
                    QString("QPushButton{min-height:%1px;max-height:%1px;border:0;padding:0;}")
                            .arg(height));
        update();
    }

    int cellWidth() const
    {
        const auto font = ui::interfaceFont(chrome::labelText);
        return qMax(ui::interfaceTextWidth(idleLabel, font),
                    ui::interfaceTextWidth(holdingLabel, font))
                + chrome::cellIcon + ui::px(16) + 2 * chrome::inset;
    }

    // Called when the steps change, so the note below can follow.
    std::function<void(int)> stepChanged;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool down = isDown() || step > 0;
        const auto paper = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::WindowText);
        painter.fillRect(rect(), down ? ink : paper);
        if (cell)
            ui::paintEdges(painter, rect(), Qt::LeftEdge, palette().color(QPalette::Mid));
        else
            ui::paintEdges(painter, rect(),
                           ui::Edges(Qt::LeftEdge) | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge,
                           ink, chrome::heavyRule);
        const auto color = down ? paper : ink;
        const auto font = ui::interfaceFont(chrome::labelText);
        const QString label = down ? holdingLabel : idleLabel;
        const int icon = chrome::cellIcon, gap = ui::px(16);
        const int textWidth = ui::interfaceTextWidth(label, font);
        const int stepWidth = ui::px(64), stepHeight = ui::px(10), stepGap = ui::px(10);
        const int labelHeight = qMax(icon, QFontMetrics(font).height());
        const int block = labelHeight + ui::px(14) + stepHeight;
        const int top = (height() - block) / 2;
        int x = (width() - icon - gap - textWidth) / 2;
        ui::controlIcon(ui::ControlIcon::Lock, color)
                .paint(&painter, QRect(x, top + (labelHeight - icon) / 2, icon, icon));
        painter.setPen(color);
        painter.setFont(font);
        ui::drawInterfaceText(painter,
                              QRect(x + icon + gap, top, textWidth + ui::px(4), labelHeight), label,
                              Qt::AlignLeft | Qt::AlignVCenter);
        const int stepsLeft = (width() - steps * stepWidth - (steps - 1) * stepGap) / 2;
        const int stepsTop = top + labelHeight + ui::px(14);
        for (int index = 0; index < steps; ++index) {
            const QRect box(stepsLeft + index * (stepWidth + stepGap), stepsTop, stepWidth,
                            stepHeight);
            if (index < step)
                painter.fillRect(box, color);
            else
                ui::paintEdges(painter, box,
                               ui::Edges(Qt::LeftEdge) | Qt::TopEdge | Qt::RightEdge
                                       | Qt::BottomEdge,
                               color);
        }
    }

    // A press that ends without a release, because Settings was hidden or
    // the button moved between the heading and the bar, must not leave Zen
    // mode later.
    void hideEvent(QHideEvent *event) override
    {
        cancel();
        QPushButton::hideEvent(event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ParentChange)
            cancel();
        return QPushButton::event(event);
    }

private:
    void cancel()
    {
        timer.stop();
        setDown(false);
        setStep(0);
        update();
    }

    void setStep(int next)
    {
        if (step == next)
            return;
        step = next;
        setProperty("reader_hold_step", step);
        if (stepChanged)
            stepChanged(step);
        update();
    }

    void advance()
    {
        setStep(step + 1);
        if (step < steps)
            return;
        timer.stop();
        if (held)
            held();
        // The next time Zen mode opens Settings, the button starts empty.
        setStep(0);
    }

    QString idleLabel, holdingLabel;
    QTimer timer;
    int step = 0;
    bool cell = false;
};

QPushButton *segmentButton(QWidget *parent, QHBoxLayout *layout, QButtonGroup *group,
                           const QString &name, const QString &text);

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

QString themeName(const QString &id)
{
    return strings::get(("settings.theme_" + id).toUtf8().constData());
}

ui::ListHeader *section(QWidget *parent, QBoxLayout *rows, const char *name, const QString &caption)
{
    auto *header = new ui::ListHeader(parent);
    header->setObjectName(name);
    header->setShaded(true);
    header->setCaptions(caption.toUpper());
    header->setFixedHeight(header->sizeHint().height());
    rows->addWidget(header);
    return header;
}

ui::CaptionButton *listAction(QWidget *parent, QBoxLayout *rows, const char *name,
                              const QString &label, std::function<void()> action, bool wrap = false)
{
    auto *button = ui::makeCaptionButton(parent, rows, name, label, std::move(action));
    button->setRule(Qt::BottomEdge);
    if (wrap) {
        readerStyle(
                button,
                QString("QPushButton{font-size:%1px;border:0;border-radius:0;text-align:left;padding:0;background:palette(window);color:palette(window-text);}")
                        .arg(int(chrome::labelText)));
        button->setMinimumHeight(chrome::listActionHeight);
    } else {
        ui::readerStyle(
                button,
                QString("QPushButton{font-size:%1px;min-height:%2px;max-height:%2px;border:0;border-radius:0;text-align:left;padding:0 %3px;background:palette(window);color:palette(window-text);} QPushButton:pressed{background:palette(midlight);}")
                        .arg(int(chrome::labelText))
                        .arg(int(chrome::listActionHeight))
                        .arg(int(chrome::inset)));
        button->setFixedHeight(chrome::listActionHeight);
    }
    return button;
}

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

void SettingsTitle::paintEvent(QPaintEvent *event)
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

SettingsPanel::SettingsPanel(QWidget *parent, const SettingsPanelInputs &inputs)
    : ui::Panel(parent, Qt::BottomEdge), inputs(inputs)
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
    headingLayout = headingRows;
    headingRows->setContentsMargins(0, 0, 0, chrome::heavyRule);
    headingRows->setSpacing(0);
    backButton = new ui::CellButton(ui::ControlIcon::Back, heading);
    backButton->setObjectName("rrSettingsBack");
    backButton->setAccessibleName(strings::get("common.back"));
    backButton->setRule(Qt::RightEdge);
    backButton->setFixedWidth(chrome::cellWidth);
    backButton->hide();
    headingRows->addWidget(backButton);
    connect(backButton, &QPushButton::clicked, this, [this] {
        if (settingsPages->currentWidget() == themingPage) {
            settingsPages->setCurrentIndex(int(SettingsPage::System));
            return;
        }
        sectionOpen = false;
        // In Zen mode, back returns from About to the Zen page.
        if (zen)
            settingsPages->setCurrentIndex(int(SettingsPage::Zen));
        if (refitRequested)
            refitRequested();
    });
    settingsTitle = new SettingsTitle(strings::get("settings.title"), heading);
    settingsTitle->setObjectName("rrSettingsTitle");
    settingsTitle->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    readerStyle(settingsTitle, styles::brand(chrome::titleText) + "background:transparent;");
    headingRows->addWidget(settingsTitle, 1);
    // The close cell sits on the right, as in Text Presets. An open section
    // replaces it with the back cell on the left.
    closeButton = new ui::CellButton(ui::ControlIcon::Close, heading);
    closeButton->setObjectName("rrSettingsClose");
    closeButton->setAccessibleName(strings::get("settings.close"));
    closeButton->setRule(Qt::LeftEdge);
    closeButton->setFixedWidth(chrome::cellWidth);
    headingRows->addWidget(closeButton);
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
                               "rrSettingsAbout",   "rrSettingsKosync",       "rrSettingsDebug" };
    const ui::ControlIcon tabIcons[] = { ui::ControlIcon::Display,      ui::ControlIcon::Swipe,
                                         ui::ControlIcon::Undo,         ui::ControlIcon::Styling,
                                         ui::ControlIcon::Dictionaries, ui::ControlIcon::System,
                                         ui::ControlIcon::About,        ui::ControlIcon::Sync,
                                         ui::ControlIcon::Debug };
    const QStringList tabs = { strings::get("settings.display"),  strings::get("settings.controls"),
                               strings::get("settings.defaults"), strings::get("settings.styling"),
                               strings::get("settings.lookup"),   strings::get("settings.system"),
                               strings::get("settings.about"),    strings::get("kosync.title"),
                               strings::get("settings.debug") };
    for (int index = 0; index < tabs.size(); ++index) {
        auto *button = makeButton(this, settingsTabs, tabNames[index], tabs[index], [this, index] {
            const bool same = settingsPages->currentIndex() == index;
            sectionOpen = listMode;
            ensurePage(SettingsPage(index));
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
        // Debug mode shows it; see showDebug().
        if (index == int(SettingsPage::Debug)) {
            button->setEnabled(false);
            button->hide();
        }
    }
    settingsTabs->removeWidget(settingsTabButtons[int(SettingsPage::Kosync)]);
    settingsTabs->insertWidget(int(SettingsPage::About),
                               settingsTabButtons[int(SettingsPage::Kosync)], 1);
    for (int index = 0; index <= int(SettingsPage::Theming); ++index)
        settingsPages->addWidget(new QWidget(settingsPages));
    themingPage = settingsPages->widget(int(SettingsPage::Theming));
    settingsError = new ui::InterfaceLabel(this);
    settingsError->setWordWrap(true);
    settingsError->hide();
    generalRows->addWidget(settingsError);
    connect(settingsPages, &QStackedWidget::currentChanged, this, [this](int index) {
        if (isVisible() && (!listMode || sectionOpen || zen || index == int(SettingsPage::Theming)))
            ensurePage(SettingsPage(index));
        // Zen and Theming have no tab.
        if (index < settingsTabButtons.size())
            settingsTabButtons[index]->setChecked(true);
        optionPage = 0;
    });
    settingsTabButtons.first()->setChecked(true);
    settingsTabs->setAlignment(Qt::AlignTop);
    styleTabs(false);
    fitRail();
    hide();
}

bool SettingsPanel::pageIsBuilt(SettingsPage page) const
{
    return settingsPages->widget(int(page))->layout() != nullptr;
}

void SettingsPanel::ensurePage(SettingsPage page)
{
    const int index = int(page);
    if (pageIsBuilt(page))
        return;
    auto *placeholder = settingsPages->widget(index);
    // Qt queues showing rows added to an already visible layout. Build the
    // selected section while hidden so its first fit measures all visible rows.
    const bool visible = placeholder->isVisible();
    if (visible)
        placeholder->hide();
    QWidget *content = placeholder;
    switch (page) {
    case SettingsPage::Display:
        buildDisplay(content);
        break;
    case SettingsPage::Controls:
        buildControls(content);
        break;
    case SettingsPage::Defaults:
        buildDefaults(content);
        break;
    case SettingsPage::Styling:
        buildStyling(content);
        break;
    case SettingsPage::Dictionaries:
        content = dictionaries = new DictionariesPage(settingsPages);
        break;
    case SettingsPage::System:
        buildSystem(content);
        break;
    case SettingsPage::About:
        content = aboutPage = new AboutPage(settingsPages);
        buildAbout();
        break;
    case SettingsPage::Kosync:
        content = kosync =
                new KosyncPage(settingsPages, inputs.window ? inputs.window : parentWidget(),
                               inputs.keyboard, kosyncSchedule);
        break;
    case SettingsPage::Debug:
        buildDebug(content);
        break;
    case SettingsPage::Zen:
        zenPage = content;
        buildZen();
        break;
    case SettingsPage::Theming:
        buildTheming();
        break;
    }
    if (content != placeholder) {
        // Preserve the selected section and send only its original currentChanged.
        const qt::SignalBlocker block(settingsPages);
        const bool selected = settingsPages->currentWidget() == placeholder;
        settingsPages->insertWidget(index, content);
        if (selected)
            settingsPages->setCurrentWidget(content);
        settingsPages->removeWidget(placeholder);
        delete placeholder;
    } else if (visible) {
        content->show();
    }
    content->installEventFilter(this);
    if (page == SettingsPage::Controls || page == SettingsPage::System)
        updateFeatureOptions();
    if (pageBuilt)
        pageBuilt(page, content);
}

void SettingsPanel::buildDisplay(QWidget *displayPage)
{
    displayPage->setObjectName("rrDisplayOptions");
    auto *generalOptions = new QVBoxLayout(displayPage);
    generalOptions->setContentsMargins(0, 0, 0, 0);
    generalOptions->setSpacing(0);
    darkOption = settingsOption(displayPage, "rrDarkMode", strings::get("settings.dark_mode"),
                                strings::get("settings.night_mode_description"));
    darkOption->setLeadingIcon(ui::ControlIcon::Moon);
    const bool animationsSupported = pageAnimationsAvailable(inputs.animationSupport);
    const auto animationDescription =
            strings::get(inputs.animationSupport == PageAnimationSupport::Uncertain
                                 ? "settings.animations_uncertain_description"
                                 : animationsSupported ? "settings.animations_description"
                                                       : "settings.unsupported_description");
    animationOption =
            settingsOption(displayPage, "rrPageAnimations",
                           strings::get("reader.enable_page_animations"), animationDescription);
    animationOption->setLeadingIcon(ui::ControlIcon::Note);
    animationOption->setEnabled(animationsSupported);
    bionicOption =
            settingsOption(displayPage, "rrBionicReading", strings::get("reader.bold_reading"),
                           strings::get("reader.bionic_description"));
    bionicOption->setLeadingIcon(ui::ControlIcon::TextB);
    section(displayPage, generalOptions, "rrDisplayModesSection",
            strings::get("settings.section_modes"));
    zenModeOption = settingsOption(displayPage, "rrZenMode", strings::get("settings.zen_mode"),
                                   strings::get("settings.zen_mode_description"));
    zenModeOption->setLeadingIcon(ui::ControlIcon::Lotus);
    auto *displayMark = ui::makeZenMark(zenModeOption, "rrDisplayZenMark");
    displayMark->setText(zenModeOption->text().toUpper());
    zenModeOption->setTitleLabel(displayMark);
    generalOptions->addWidget(zenModeOption);
    generalOptions->addWidget(darkOption);
    generalOptions->addWidget(bionicOption);
    section(displayPage, generalOptions, "rrDisplayLayoutSection",
            strings::get("reader.page_layout"));
    columnsButton = listAction(displayPage, generalOptions, "rrColumnsSettings",
                               strings::get("settings.landscape_column_layout"), [] { });
    columnsButton->setIconVisible(true);
    ui::buttonIcon(columnsButton, ui::ControlIcon::TwoColumns);
    section(displayPage, generalOptions, "rrDisplayScreenSection",
            strings::get("settings.section_screen"));
    refreshButton = listAction(displayPage, nullptr, "rrScreenRefresh",
                               strings::get("reader.refresh_the_screen"), [] { });
    refreshButton->setIconVisible(true);
    ui::buttonIcon(refreshButton, ui::ControlIcon::Refresh);
    generalOptions->addWidget(refreshButton);
    generalOptions->addWidget(animationOption);
    generalOptions->addStretch(1);
}

void SettingsPanel::buildControls(QWidget *controlsPage)
{
    controlsPage->setObjectName("rrControlsOptions");
    auto *controlsRows = new QVBoxLayout(controlsPage);
    controlsRows->setContentsMargins(0, 0, 0, 0);
    controlsRows->setSpacing(0);
    section(controlsPage, controlsRows, "rrControlsNavigationSection",
            strings::get("settings.section_navigation"));
    pageNavigationButton = listAction(controlsPage, nullptr, "rrPageNavigation",
                                      strings::get("settings.page_navigation"), [] { });
    controlsRows->addWidget(pageNavigationButton);
    buttonControlsButton = listAction(controlsPage, nullptr, "rrButtonControls",
                                      strings::get("buttons.hardware_title"), [] { });
    controlsRows->addWidget(buttonControlsButton);
    tapZonesButton = listAction(controlsPage, nullptr, "rrTapZones",
                                strings::get("settings.tap_zones"), [] { });
    controlsRows->addWidget(tapZonesButton);
    section(controlsPage, controlsRows, "rrControlsChapterListSection",
            strings::get("settings.section_chapter_list"));
    chapterGroupsButton = listAction(controlsPage, nullptr, "rrChapterGroups",
                                     strings::get("settings.chapter_groups"), [] { });
    controlsRows->addWidget(chapterGroupsButton);
    section(controlsPage, controlsRows, "rrControlsLinksSection",
            strings::get("settings.section_links"));
    tappableLinksButton = listAction(controlsPage, nullptr, "rrTappableLinks",
                                     strings::get("settings.tappable_links"), [] { });
    controlsRows->addWidget(tappableLinksButton);
    section(controlsPage, controlsRows, "rrControlsAnnotationsSection",
            strings::get("settings.section_annotations"));
    annotationInteractionButton =
            listAction(controlsPage, nullptr, "rrAnnotationInteraction",
                       strings::get("settings.annotation_interaction"), [] { });
    controlsRows->addWidget(annotationInteractionButton);
    section(controlsPage, controlsRows, "rrControlsMenuSection",
            strings::get("settings.section_reading_menu"));
    menuViewButton = listAction(controlsPage, nullptr, "rrDefaultMenuView",
                                strings::get("settings.default_menu_view"), [] { });
    controlsRows->addWidget(menuViewButton);
    section(controlsPage, controlsRows, "rrControlsNavigationBarSection",
            strings::get("settings.section_navigation_bar"));
    navigationBarButton = listAction(controlsPage, nullptr, "rrNavigationBarButtons",
                                     strings::get("settings.navigation_bar_buttons"), [] { });
    controlsRows->addWidget(navigationBarButton);
    minimalAnnotationOption =
            settingsOption(controlsPage, "rrMinimalAnnotationMode",
                           strings::get("settings.minimal_annotation_mode"),
                           strings::get("settings.minimal_annotation_mode_description"));
    minimalAnnotationOption->hide();
    section(controlsPage, controlsRows, "rrControlsLightSection",
            strings::get(inputs.screenBrightness ? "light_panel.brightness"
                                                 : "settings.section_frontlight"));
    const bool lightSupported = inputs.lightSupported;
    edgeLightOption = settingsOption(
            controlsPage, "rrEdgeLight",
            strings::get(inputs.screenBrightness ? "reader.edge_brightness" : "reader.edge_light"),
            !lightSupported ? strings::get("settings.unsupported_description")
                    : inputs.warmthSupported
                    ? strings::get("settings.edge_light_description")
                    : strings::get("settings.edge_brightness_description"));
    edgeLightOption->setEnabled(lightSupported);
    controlsRows->addWidget(edgeLightOption);
    lightGestureFeedbackOption = settingsOption(
            controlsPage, "rrLightGestureFeedback",
            strings::get(inputs.screenBrightness ? "settings.brightness_gesture_feedback"
                                                 : "settings.light_gesture_feedback"),
            strings::get("settings.light_gesture_feedback_description"));
    lightGestureFeedbackOption->setEnabled(lightSupported);
    controlsRows->addWidget(lightGestureFeedbackOption);
    for (const auto &[button, icon] :
         { std::pair{ pageNavigationButton, ui::ControlIcon::SpreadHorizontal },
           std::pair{ buttonControlsButton, ui::ControlIcon::HardwareButtons },
           std::pair{ tapZonesButton, ui::ControlIcon::HandTap },
           std::pair{ chapterGroupsButton, ui::ControlIcon::ChapterTree },
           std::pair{ tappableLinksButton, ui::ControlIcon::Link },
           std::pair{ annotationInteractionButton, ui::ControlIcon::Annotate },
           std::pair{ menuViewButton, ui::ControlIcon::Layout },
           std::pair{ navigationBarButton, ui::ControlIcon::SearchAfter } }) {
        button->setIconVisible(true);
        ui::buttonIcon(button, icon);
    }
    edgeLightOption->setLeadingIcon(ui::ControlIcon::Light);
    lightGestureFeedbackOption->setLeadingIcon(ui::ControlIcon::Gauge);
    controlsRows->addStretch(1);
}

void SettingsPanel::buildDefaults(QWidget *defaultsPage)
{
    defaultsPage->setObjectName("rrDefaultsOptions");
    auto *defaultsRows = new QVBoxLayout(defaultsPage);
    defaultsRows->setContentsMargins(0, 0, 0, 0);
    defaultsRows->setSpacing(0);
    section(defaultsPage, defaultsRows, "rrDefaultsNewBooksSection",
            strings::get("settings.section_new_books"));
    textForNewBooksButton = listAction(defaultsPage, defaultsRows, "rrTextForNewBooks",
                                       strings::get("settings.text_for_new_books"), [] { });
    section(defaultsPage, defaultsRows, "rrDefaultsGlobalFontSection",
            strings::get("settings.section_default_global_font"));
    defaultFontButton = listAction(defaultsPage, nullptr, "rrDefaultFont",
                                   strings::get("reader.default_font"), [] { });
    defaultsRows->addWidget(defaultFontButton);
    // Fallback fonts are global font defaults too, so they sit with the
    // default font rather than with the typography settings.
    section(defaultsPage, defaultsRows, "rrFontsFamiliesSection",
            strings::get("settings.font_families"));
    serifFontButton = listAction(defaultsPage, defaultsRows, "rrSerifFont",
                                 strings::get("settings.serif_font"), [] { });
    sansFontButton = listAction(defaultsPage, defaultsRows, "rrSansFont",
                                strings::get("settings.sans_font"), [] { });
    monoFontButton = listAction(defaultsPage, defaultsRows, "rrMonoFont",
                                strings::get("settings.mono_font"), [] { });
    section(defaultsPage, defaultsRows, "rrDefaultsRestoreSection",
            strings::get("reader.restore_defaults"));
    auto *resetRow = kobalt::ui::horizontalRow();
    defaultsRows->addLayout(resetRow);
    resetRow->addStretch(1);
    resetDefaults = listAction(defaultsPage, resetRow, "rrRestoreDefaults",
                               strings::get("reader.restore_appearance_defaults"), [this] {
                                   settingsPages->setCurrentIndex(int(SettingsPage::Defaults));
                                   auto *dialog = restoreConfirmation();
                                   showConfirmation(dialog);
                                   dialog->setFocus();
                               });
    resetDefaults->setCaption(strings::get("settings.asks_first"));
    resetRow->addStretch(1);
    // Text presets are not appearance defaults: a restore of either leaves
    // the other alone.
    auto *presetsRow = kobalt::ui::horizontalRow();
    defaultsRows->addLayout(presetsRow);
    presetsRow->addStretch(1);
    resetPresets = listAction(defaultsPage, presetsRow, "rrRestorePresets",
                              strings::get("text_presets.restore"), [this] {
                                  settingsPages->setCurrentIndex(int(SettingsPage::Defaults));
                                  auto *dialog = restorePresetsConfirmation();
                                  showConfirmation(dialog);
                                  dialog->setFocus();
                              });
    resetPresets->setCaption(strings::get("settings.asks_first"));
    presetsRow->addStretch(1);
    defaultsRows->addStretch(1);
    for (auto *row : { resetRow, presetsRow }) {
        row->setStretch(0, 0);
        row->setStretch(1, 1);
        row->setStretch(2, 0);
    }
}

void SettingsPanel::buildStyling(QWidget *stylingPage)
{
    stylingPage->setObjectName("rrStylingOptions");
    auto *stylingRows = new QVBoxLayout(stylingPage);
    stylingRows->setContentsMargins(0, 0, 0, 0);
    stylingRows->setSpacing(0);
    // Group line styling, paragraph layout and page spacing separately.
    section(stylingPage, stylingRows, "rrAdvancedTextSection",
            strings::get("settings.section_words_letters"));
    wordSpacingButton = listAction(
            stylingPage, stylingRows, "rrWordSpacing", strings::get("settings.word_spacing"),
            [] { }, true);
    trackingButton = listAction(
            stylingPage, stylingRows, "rrJustificationTracking",
            strings::get("settings.justification_tracking"), [] { }, true);
    hangingPunctuationButton = listAction(
            stylingPage, stylingRows, "rrHangingPunctuation",
            strings::get("settings.hanging_punctuation"), [] { }, true);
    hangingPunctuationButton->setDescription(strings::get("settings.hanging_punctuation_summary"));
    section(stylingPage, stylingRows, "rrParagraphSpacingSection",
            strings::get("settings.section_paragraphs"));
    paragraphSpacingButton = listAction(
            stylingPage, stylingRows, "rrParagraphSpacing",
            strings::get("settings.paragraph_spacing"), [] { }, true);
    paragraphSpacingButton->setDescription(strings::get("settings.paragraph_spacing_summary"));
    widowsOrphansButton = listAction(
            stylingPage, stylingRows, "rrWidowsOrphans", strings::get("settings.widows_orphans"),
            [] { }, true);
    section(stylingPage, stylingRows, "rrVerticalSpacingSection",
            strings::get("settings.section_page_breaks"));
    verticalSpacingButton = listAction(
            stylingPage, stylingRows, "rrVerticalSpacing",
            strings::get("settings.vertical_spacing_mode"), [] { }, true);
    verticalSpacingButton->setDescription(strings::get("settings.vertical_spacing_description"));
    section(stylingPage, stylingRows, "rrNormalizationSection",
            strings::get("reader.book_beautifier"));
    beautifierButton = listAction(
            stylingPage, stylingRows, "rrBeautifierSettings",
            strings::get("reader.book_beautifier"), [] { }, true);
    beautifierButton->setDescription(strings::get("settings.beautifier_summary"));
    section(stylingPage, stylingRows, "rrAdvancedContentSection",
            strings::get("settings.section_book_content"));
    hideNonlinearOption =
            settingsOption(stylingPage, "rrHideNonlinearFragments",
                           strings::get("reader.hide_nonlinear_fragments"),
                           strings::get("settings.hide_nonlinear_fragments_description"));
    stylingRows->addWidget(hideNonlinearOption);
    inpageFootnotesOption = settingsOption(stylingPage, "rrInpageFootnotes",
                                           strings::get("settings.inpage_footnotes"),
                                           strings::get("settings.inpage_footnotes_description"));
    stylingRows->addWidget(inpageFootnotesOption);
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
    inpageFootnotesOption->setLeadingIcon(ui::ControlIcon::Link);
    wordSpacingButton->setDescription(strings::get("settings.word_spacing_summary"));
    trackingButton->setDescription(strings::get("settings.justification_tracking_summary"));
    widowsOrphansButton->setDescription(strings::get("settings.widows_orphans_summary"));
    stylingRows->addStretch(1);
}

void SettingsPanel::buildSystem(QWidget *systemPage)
{
    systemPage->setObjectName("rrSystemOptions");
    auto *systemRows = new QVBoxLayout(systemPage);
    systemRows->setContentsMargins(0, 0, 0, 0);
    systemRows->setSpacing(0);
    if (inputs.showNativeReaderOptions)
        section(systemPage, systemRows, "rrSystemIntegrationSection",
                strings::get("settings.section_system_integration"));
    automaticOpeningOption = settingsOption(
            systemPage, "rrAutomaticBookOpening", strings::get("settings.automatic_book_opening"),
            inputs.automaticBookOpeningSupported
                    ? strings::get("settings.automatic_book_opening_description")
                    : strings::get("settings.unsupported_description"));
    automaticOpeningOption->setEnabled(inputs.automaticBookOpeningSupported);
    automaticOpeningOption->setVisible(inputs.showNativeReaderOptions);
    automaticOpeningOption->setLeadingIcon(ui::ControlIcon::Dictionary);
    systemRows->addWidget(automaticOpeningOption);
    if (inputs.automaticBookOpeningSupported) {
        // Turning automatic opening off sends every later book to Kobo's
        // reader, so ask first.
        connect(automaticOpeningOption, &QCheckBox::clicked, this, [this](bool checked) {
            if (checked) {
                if (automaticOpeningChosen)
                    automaticOpeningChosen(true);
                return;
            }
            // An unanswered confirmation must never turn the choice off.
            automaticOpeningOption->setChecked(true);
            auto *dialog = automaticOpeningConfirmation();
            showConfirmation(dialog);
            dialog->secondaryButton()->setFocus();
        });
    }
    section(systemPage, systemRows, "rrSystemVisibilitySection",
            strings::get("settings.section_feature_visibility"));
    disabledFeaturesButton = listAction(systemPage, systemRows, "rrDisabledFeatures",
                                        strings::get("settings.hidden_features"),
                                        [this] { showDisabledFeatures(); });
    section(systemPage, systemRows, "rrSystemSleepSection", strings::get("settings.section_sleep"));
    hideBookOnSleepOption =
            settingsOption(systemPage, "rrHideBookOnSleepScreen",
                           strings::get("settings.hide_book_on_sleep_screen"),
                           strings::get("settings.hide_book_on_sleep_screen_description"));
    hideBookOnSleepOption->setEnabled(inputs.sleepScreenSupported);
    hideBookOnSleepOption->setLeadingIcon(ui::ControlIcon::EyeClosed);
    systemRows->addWidget(hideBookOnSleepOption);
    sleepScreenButton = listAction(systemPage, nullptr, "rrSleepScreen",
                                   strings::get("settings.sleep_screen"), [] { });
    sleepScreenButton->setEnabled(inputs.sleepScreenSupported);
    if (!inputs.sleepScreenSupported)
        sleepScreenButton->setCaption(strings::get("settings.unsupported_caption"));
    systemRows->addWidget(sleepScreenButton);
    sessionTimeoutButton = listAction(systemPage, nullptr, "rrSessionTimeout",
                                      strings::get("settings.session_timeout"), [] { });
    systemRows->addWidget(sessionTimeoutButton);
    if (inputs.readerStandbySupported) {
        disableStandbyOption =
                settingsOption(systemPage, "rrDisableReaderStandby",
                               strings::get("settings.disable_reader_standby"),
                               strings::get("settings.disable_reader_standby_description"));
        disableStandbyOption->setLeadingIcon(ui::ControlIcon::Coffee);
        systemRows->addWidget(disableStandbyOption);
        connect(disableStandbyOption, &QCheckBox::clicked, this, [this](bool checked) {
            if (!checked)
                return;
            // An unanswered confirmation must never enable the preference.
            disableStandbyOption->setChecked(false);
            auto *dialog = standbyConfirmation();
            showConfirmation(dialog);
            dialog->secondaryButton()->setFocus();
        });
    }
    if (inputs.wifiPowerSaverSupported) {
        wifiPowerSaverOption = settingsOption(
                systemPage, "rrWifiPowerSaver", strings::get("settings.wifi_power_saver"),
                strings::get("settings.wifi_power_saver_description"));
        wifiPowerSaverOption->setLeadingIcon(ui::ControlIcon::WifiHigh);
        systemRows->addWidget(wifiPowerSaverOption);
    }
    section(systemPage, systemRows, "rrSystemNitpicksSection",
            strings::get("settings.section_nitpicks"));
    timeRemainingFormatButton = listAction(systemPage, nullptr, "rrTimeRemainingFormat",
                                           strings::get("settings.time_remaining_format"), [] { });
    systemRows->addWidget(timeRemainingFormatButton);
    percentageFormatButton = listAction(systemPage, nullptr, "rrPercentageFormat",
                                        strings::get("settings.percentage_format"), [] { });
    systemRows->addWidget(percentageFormatButton);
    openingCoverOption =
            settingsOption(systemPage, "rrHideOpeningCover", strings::get("settings.opening_cover"),
                           strings::get("settings.opening_cover_description"));
    openingCoverOption->setLeadingIcon(ui::ControlIcon::Image);
    systemRows->addWidget(openingCoverOption);
    section(systemPage, systemRows, "rrSystemInterfaceSection",
            strings::get("settings.section_user_interface"));
    hideTabLabelsOption =
            settingsOption(systemPage, "rrHideTabLabels", strings::get("settings.hide_tab_labels"),
                           strings::get("settings.hide_tab_labels_description"));
    hideTabLabelsOption->setLeadingIcon(ui::ControlIcon::TextTSlash);
    systemRows->addWidget(hideTabLabelsOption);
    themingButton =
            listAction(systemPage, systemRows, "rrTheming", strings::get("settings.theming"),
                       [this] { settingsPages->setCurrentIndex(int(SettingsPage::Theming)); });
    if (!selectedTheme.isEmpty())
        themingButton->setCaption(themeName(selectedTheme).toUpper());
    section(systemPage, systemRows, "rrSystemDictionarySection",
            strings::get("settings.section_dictionary_ui"));
    saveWordsOption = settingsOption(systemPage, "rrSaveWords", strings::get("settings.save_words"),
                                     strings::get("settings.save_words_description"));
    saveWordsOption->setLeadingIcon(ui::ControlIcon::Bookmark);
    systemRows->addWidget(saveWordsOption);
    dictionaryFontButton = listAction(systemPage, nullptr, "rrDictionaryFont",
                                      strings::get("settings.dictionary_font"), [] { });
    dictionaryFontSizeButton = listAction(systemPage, nullptr, "rrDictionaryFontSize",
                                          strings::get("settings.dictionary_font_size"), [] { });
    systemRows->addWidget(dictionaryFontButton);
    systemRows->addWidget(dictionaryFontSizeButton);
    section(systemPage, systemRows, "rrSystemEngineSection",
            strings::get("settings.section_engine"));
    renderingButton = listAction(
            systemPage, systemRows, "rrRendering", strings::get("settings.rendering"), [] { },
            true);
    renderingButton->setDescription(strings::get("settings.rendering_summary"));
    section(systemPage, systemRows, "rrSystemDeveloperSection",
            strings::get("settings.section_developer"));
    debugModeOption = settingsOption(systemPage, "rrDebugMode", strings::get("settings.debug_mode"),
                                     strings::get("settings.debug_mode_description"));
    debugModeOption->setLeadingIcon(ui::ControlIcon::Debug);
    systemRows->addWidget(debugModeOption);
    connect(debugModeOption, &QCheckBox::toggled, this, [this](bool on) { showDebug(on); });
    for (const auto &[button, icon] :
         { std::pair{ disabledFeaturesButton, ui::ControlIcon::Hidden },
           std::pair{ sleepScreenButton, ui::ControlIcon::MoonStars },
           std::pair{ sessionTimeoutButton, ui::ControlIcon::ClockCountdown },
           std::pair{ timeRemainingFormatButton, ui::ControlIcon::Clock },
           std::pair{ percentageFormatButton, ui::ControlIcon::Percent },
           std::pair{ themingButton, ui::ControlIcon::Palette },
           std::pair{ dictionaryFontButton, ui::ControlIcon::TextT },
           std::pair{ dictionaryFontSizeButton, ui::ControlIcon::Typography },
           std::pair{ renderingButton, ui::ControlIcon::Cpu } }) {
        button->setIconVisible(true);
        ui::buttonIcon(button, icon);
    }
    systemRows->addStretch(1);
}

void SettingsPanel::buildTheming()
{
    auto *themeRows = new QVBoxLayout(themingPage);
    themeRows->setContentsMargins(0, 0, 0, 0);
    themeRows->setSpacing(0);
    section(themingPage, themeRows, "rrThemeSelectionSection",
            strings::get("settings.theme_selection"));
    presetButton = listAction(themingPage, themeRows, "rrThemePreset",
                              strings::get("settings.theme_preset"), [] { });
    section(themingPage, themeRows, "rrThemeDetailsSection",
            strings::get("settings.theme_details"));
    iconThemeButton = listAction(themingPage, themeRows, "rrIconTheme",
                                 strings::get("settings.icon_theme"), [] { });
    paletteButton = listAction(themingPage, themeRows, "rrColorTheme",
                               strings::get("settings.palette"), [] { });
    uiFontButton =
            listAction(themingPage, nullptr, "rrUiFont", strings::get("settings.ui_font"), [] { });
    uiMonoFontButton = listAction(themingPage, nullptr, "rrUiMonoFont",
                                  strings::get("settings.ui_mono_font"), [] { });
    headerFooterFontButton = listAction(themingPage, nullptr, "rrHeaderFooterFont",
                                        strings::get("settings.header_footer_font"), [] { });
    section(themingPage, themeRows, "rrPrimaryTypographySection",
            strings::get("settings.primary_typography"));
    themeRows->addWidget(uiFontButton);
    themeRows->addWidget(uiMonoFontButton);
    section(themingPage, themeRows, "rrSecondaryTypographySection",
            strings::get("settings.secondary_typography"));
    themeRows->addWidget(headerFooterFontButton);
    headerFooterWeightButton = listAction(themingPage, nullptr, "rrHeaderFooterWeight",
                                          strings::get("settings.header_footer_weight"), [] { });
    themeRows->addWidget(headerFooterWeightButton);
    themeRows->addStretch(1);
}

void SettingsPanel::buildAbout()
{
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
}

void SettingsPanel::buildDebug(QWidget *debugPage)
{
    debugPage->setObjectName("rrDebugOptions");
    auto *debugRows = new QVBoxLayout(debugPage);
    debugRows->setContentsMargins(0, 0, 0, 0);
    debugRows->setSpacing(0);
    section(debugPage, debugRows, "rrDebugCacheSection", strings::get("debug.section_caches"));
    evictBookCacheButton = listAction(
            debugPage, debugRows, "rrDebugEvictCache", strings::get("debug.evict_cache"), [] { },
            true);
    evictBookCacheButton->setDescription(strings::get("debug.evict_cache_description"));
    clearCachesButton = listAction(
            debugPage, debugRows, "rrDebugClearCaches", strings::get("debug.clear_caches"), [] { },
            true);
    clearCachesButton->setDescription(strings::get("debug.clear_caches_description"));
    section(debugPage, debugRows, "rrDebugDiagnosticsSection",
            strings::get("debug.section_diagnostics"));
    detailedDiagnosticsOption = settingsOption(
            debugPage, "rrDebugDetailedDiagnostics", strings::get("debug.detailed_diagnostics"),
            strings::get("debug.detailed_diagnostics_description"));
    debugRows->addWidget(detailedDiagnosticsOption);
    clearDiagnosticLogButton = listAction(debugPage, debugRows, "rrDebugClearLog",
                                          strings::get("debug.clear_log"), [] { });
    section(debugPage, debugRows, "rrDebugDeviceSection", strings::get("debug.section_device"));
    debugVersionButton = listAction(debugPage, debugRows, "rrDebugVersion",
                                    strings::get("debug.version"), [] { });
    debugQtButton = listAction(debugPage, debugRows, "rrDebugQt", strings::get("debug.qt"), [] { });
    debugFirmwareButton = listAction(debugPage, debugRows, "rrDebugFirmware",
                                     strings::get("debug.firmware"), [] { });
    debugCoresButton =
            listAction(debugPage, debugRows, "rrDebugCores", strings::get("debug.cores"), [] { });
    debugStorageButton = listAction(debugPage, debugRows, "rrDebugStorage",
                                    strings::get("debug.storage"), [] { });
    // The facts open nothing: a tap reads them again.
    for (auto *fact : { debugVersionButton, debugQtButton, debugFirmwareButton, debugCoresButton,
                        debugStorageButton })
        fact->setChevron(false);
    // As Restore defaults: the action asks before it closes the book.
    evictBookCacheButton->setCaption(strings::get("settings.asks_first"));
    clearCachesButton->setCaption(strings::get("settings.asks_first"));
    debugRows->addStretch(1);
}

void SettingsPanel::buildZen()
{
    // Zen mode marks its settings page as the reading menu marks its status
    // line: the section heading caption, inverted to paper on an ink block.
    zenMarkBox = new QWidget(closeButton->parentWidget());
    auto *zenMarkRows = new QHBoxLayout(zenMarkBox);
    zenMarkRows->setContentsMargins(0, 0, chrome::inset, 0);
    auto *zenMark = ui::makeZenMark(zenMarkBox, "rrSettingsZenMark");
    zenMarkRows->addWidget(zenMark, 0, Qt::AlignVCenter);
    zenMarkBox->hide();
    headingLayout->insertWidget(headingLayout->indexOf(closeButton), zenMarkBox);
    // The way out of Zen mode sits on the bottom edge, where the tabs are.
    auto *leaveBar = new ui::Panel(optionsColumn, Qt::TopEdge);
    zenLeaveBar = leaveBar;
    leaveBar->setObjectName("rrZenLeaveBar");
    leaveBar->setEdgeWidth(chrome::heavyRule);
    auto *leaveRows = new QVBoxLayout(leaveBar);
    leaveLayout = leaveRows;
    leaveRows->setContentsMargins(chrome::inset, chrome::heavyRule + ui::px(24), chrome::inset,
                                  ui::px(24));
    leaveRows->setSpacing(ui::px(12));
    auto *leave = new HoldButton(strings::get("settings.zen_leave"),
                                 strings::get("settings.zen_leave_holding"), leaveBar);
    leave->setObjectName("rrZenLeave");
    leave->setCell(false);
    leaveRows->addWidget(leave);
    zenLeave = leave;
    auto *leaveNote = new ui::InterfaceLabel(strings::get("settings.zen_leave_note"), leaveBar);
    leaveNote->setObjectName("rrZenLeaveNote");
    leaveNote->setAlignment(Qt::AlignCenter);
    leaveNote->setWordWrap(true);
    readerStyle(leaveNote,
                QString("font-size:%1px;font-style:italic;background:transparent;")
                        .arg(int(chrome::descriptionText)));
    leaveRows->addWidget(leaveNote);
    leave->stepChanged = [leaveNote](int step) {
        leaveNote->setText(
                strings::get(step > 0 ? "settings.zen_leave_release" : "settings.zen_leave_note"));
    };
    leave->held = [this] {
        if (zenModeOption)
            zenModeOption->setChecked(false);
        if (zenLeft)
            zenLeft();
        setZenLayout(false);
        if (refitRequested)
            refitRequested();
    };
    optionsColumn->layout()->addWidget(leaveBar);
    leaveBar->hide();
    if (widgetCreated) {
        widgetCreated(zenMarkBox);
        widgetCreated(zenLeaveBar);
    }
    zenPage->setObjectName("rrZenSettings");
    auto *zenRows = new QVBoxLayout(zenPage);
    zenRows->setContentsMargins(0, 0, 0, 0);
    zenRows->setSpacing(0);
    section(zenPage, zenRows, "rrZenDisplaySection", strings::get("settings.display"));
    zenDarkOption = settingsOption(zenPage, "rrZenDarkMode", strings::get("reader.dark_mode"),
                                   strings::get("settings.dark_description"));
    // The rows keep their height; the stretch below takes the spare space.
    zenDarkOption->setSizePolicy(zenDarkOption->sizePolicy().horizontalPolicy(),
                                 QSizePolicy::Maximum);
    zenRows->addWidget(zenDarkOption);
    zenLightButton =
            listAction(zenPage, zenRows, "rrZenLight", strings::get("settings.zen_light"), [this] {
                if (zenLightRequested)
                    zenLightRequested();
            });
    zenLightButton->setCaption(strings::get(inputs.warmthSupported
                                                    ? "settings.zen_light_caption"
                                                    : "settings.zen_light_brightness"));
    zenSyncSection = section(zenPage, zenRows, "rrZenSyncSection", strings::get("kosync.title"));
    zenSyncButton = listAction(
            zenPage, zenRows, "rrZenSyncNow", strings::get("kosync.sync_now"),
            [this] {
                if (zenSyncRequested)
                    zenSyncRequested();
            },
            true);
    zenAutomaticButton = listAction(zenPage, zenRows, "rrZenAutomaticSync",
                                    strings::get("kosync.automatic"), [this] {
                                        if (zenScheduleRequested)
                                            zenScheduleRequested();
                                    });
    section(zenPage, zenRows, "rrZenAboutSection", strings::get("settings.about"));
    zenAboutButton = listAction(
            zenPage, zenRows, "rrZenAbout", strings::get("settings.zen_about"),
            [this] {
                settingsPages->setCurrentIndex(int(SettingsPage::About));
                if (refitRequested)
                    refitRequested();
            },
            true);
    // The version reads as a description, not as a chosen value.
    zenAboutButton->setDescription(AboutPage::versionText());
    zenRows->addStretch(1);
}

namespace {
QString iconStyleName(const QString &id)
{
    return strings::get(("settings.icon_" + id).toUtf8().constData());
}

class ThemeSample final : public QWidget
{
public:
    using Kind = SettingsPanel::ThemeChoice;

    ThemeSample(QWidget *parent, const InterfaceStyle &initial, Kind kind)
        : QWidget(parent), kind(kind)
    {
        setAppearance(initial);
    }

    void setAppearance(const InterfaceStyle &next)
    {
        setFixedSize((kind == Kind::Preset ? 4 : 2) * chrome::tabIcon + chrome::tabGap,
                     chrome::tabIcon);
        if (style.mainFace == next.mainFace && style.icons == next.icons
            && style.palette == next.palette)
            return;
        style = next;
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto &colors = ui::themeColors(ui::darkTheme(this), style.palette);
        if (kind == Kind::Palette) {
            int x = 0;
            for (const auto &ink : { colors.pressedPaper, colors.divider, colors.ink }) {
                painter.fillRect(x, height() / 5, width() / 3, height() * 3 / 5, ink);
                x += width() / 3;
            }
            return;
        }
        int x = 0;
        const int side = height();
        const auto ink = kind == Kind::Preset ? colors.ink : ui::labelInk(this);
        if (kind == Kind::Preset)
            painter.fillRect(rect(), colors.pressedPaper);
        for (auto icon : { ui::ControlIcon::Compass, ui::ControlIcon::Bookmark }) {
            ui::paintStyledIcon(painter, style.icons, icon, QRect(x, 0, side, side), ink);
            x += side + chrome::tabGap;
        }
        if (kind == Kind::Preset) {
            auto font = ui::interfaceFont(chrome::labelText);
            font.setFamily(style.mainFace.isEmpty() ? QStringLiteral("IBM Plex Sans")
                                                    : style.mainFace);
            painter.setFont(font);
            painter.setPen(ink);
            ui::drawOpticalText(painter, QRect(x, 0, width() - x, height()), QStringLiteral("Aa"),
                                true);
        }
    }

private:
    InterfaceStyle style;
    Kind kind;
};

QString paletteName(const QString &id)
{
    return strings::get(("settings.palette_" + id).toUtf8().constData());
}
} // namespace

void SettingsPanel::selectTheme(const QString &theme, const InterfaceStyle &style)
{
    selectedTheme = theme;
    const auto caption = themeName(theme).toUpper();
    if (themingButton)
        themingButton->setCaption(caption);
    if (!presetButton)
        return;
    const auto setSample = [&style](ui::CaptionButton *button, ThemeChoice kind) {
        if (auto *sample = static_cast<ThemeSample *>(button->sample()))
            sample->setAppearance(style);
        else
            button->setSample(new ThemeSample(button, style, kind));
    };
    presetButton->setCaption(caption);
    setSample(presetButton, ThemeChoice::Preset);
    paletteButton->setCaption(paletteName(style.palette).toUpper());
    setSample(paletteButton, ThemeChoice::Palette);
    iconThemeButton->setCaption(iconStyleName(ui::iconStyleId(style.icons)).toUpper());
    iconThemeButton->setIconStylePreview(style.icons);
    themingPage->update();
}

void SettingsPanel::showThemeChoices(ThemeChoice kind, const InterfaceStyle &style,
                                     std::function<void(const QString &)> chosen)
{
    if (sheet)
        return;
    const bool icons = kind == ThemeChoice::Icons, palette = kind == ThemeChoice::Palette;
    const QString name = icons ? "rrIconTheme" : palette ? "rrColorTheme" : "rrTheme";
    auto *choices = openChoiceSheet(name,
                                    strings::get(icons             ? "settings.icon_theme"
                                                         : palette ? "settings.palette"
                                                                   : "settings.theme_preset"));
    const QStringList ids = icons ? QStringList{ "monochrome", "color", "pastel", "rings" }
            : palette             ? QStringList{ "grayscale", "blue", "sage", "sepia" }
                                  : QStringList{ "classic", "playful", "notebook", "custom" };
    const QString current = icons ? ui::iconStyleId(style.icons)
            : palette             ? style.palette
                                  : selectedTheme;
    const bool buildRows = choices->options().isEmpty();
    for (int index = 0; index < ids.size(); ++index) {
        const auto &id = ids[index];
        auto *option = buildRows ? choices->add(name + "_" + id,
                                                icons             ? iconStyleName(id)
                                                        : palette ? paletteName(id)
                                                                  : themeName(id))
                                 : choices->options()[index];
        auto preview = kind == ThemeChoice::Preset && id != "custom" ? themePreset(id) : style;
        if (icons)
            preview.icons = ui::iconStyleFromId(id);
        if (palette)
            preview.palette = id;
        if (buildRows) {
            auto *sample = new ThemeSample(option, preview, kind);
            option->setPreview(sample, sample->size());
        } else {
            static_cast<ThemeSample *>(option->previewWidget())->setAppearance(preview);
        }
    }
    presentChoices(ids.indexOf(current), [ids, chosen](int index) { chosen(ids[index]); });
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
    const int current = settingsPages->currentIndex();
    if (!list && current >= 0 && current < settingsTabButtons.size())
        settingsTabButtons[current]->setChecked(true);
    // Without the rail beside it, as in a list or Zen mode, the page has no
    // left rule.
    const bool railless = list || zen;
    readerStyle(
            settingsPages,
            railless
                    ? QString("QStackedWidget{border:0;}")
                    : QString("QStackedWidget{border:0;border-left:%1px solid palette(window-text);}")
                              .arg(int(metrics::control::border)));
    static_cast<ui::Panel *>(optionsViewport)
            ->setEdges(railless ? ui::Edges() : ui::Edges(Qt::LeftEdge));
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
    // Zen mode has no rail or section list: its page fills the width, and
    // About, opened from it, has a back cell, as an opened section has.
    const bool zenPageShown = zen && settingsPages->currentIndex() == int(SettingsPage::Zen);
    const bool sectionShown = zen ? !zenPageShown : listMode && sectionOpen;
    const bool theming = settingsPages->currentWidget() == themingPage;
    const bool showList = !theming && !zen && listMode && !sectionOpen;
    const bool rail = !theming && !zen && !listMode;
    tabRail->setVisible(rail || showList);
    optionsColumn->setVisible(!showList);
    backButton->setVisible(sectionShown || theming);
    closeButton->setVisible(!sectionShown || theming);
    settingsTitle->setText(theming ? strings::get("settings.theming")
                                   : sectionShown
                                   ? settingsTabButtons[settingsPages->currentIndex()]
                                             ->property("reader_tab_label")
                                             .toString()
                                   : strings::get("settings.title"));
    if (!showList)
        ensurePage(SettingsPage(settingsPages->currentIndex()));
    if (zenMarkBox)
        zenMarkBox->setVisible(zen);
    if (showList) {
        fitZenLeave(false, false);
        tabRail->setFixedWidth(availableSize.width());
        optionsPager->hide();
        return availableSize.height();
    }
    if (rail)
        fitRail(availableSize.height() - headingHeight);
    const int fullWidth = qMax(1, availableSize.width() - (rail ? tabRail->width() : 0));
    const int leaveHeight =
            fitZenLeave(zenPageShown, availableSize.width() > availableSize.height());
    const int availableHeight = qMax(1, availableSize.height() - headingHeight - leaveHeight);
    auto *content = settingsPages->currentWidget();
    content->ensurePolished();
    auto *rows = content->layout();
    const auto measure = [&](int width) {
        // These two pages wrap their text to the width. The others measure
        // their rows on their own. A page is fitted again when it opens.
        if (content == aboutPage)
            aboutPage->fitWidth(width);
        else if (content == kosync)
            kosync->fitWidth(width);
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
    pagedRows = pageRows();
    showOptionsPage(optionPage);
    return panelHeight + headingHeight + leaveHeight;
}

int SettingsPanel::fitZenLeave(bool shown, bool leaveInHeading)
{
    // A landscape screen is short, so the hold button moves into the
    // heading as a cell, and the bar with its note goes.
    if (!zenLeave)
        return 0;
    auto *leave = static_cast<HoldButton *>(zenLeave);
    if (leaveInHeading != (leave->parentWidget() != zenLeaveBar)) {
        if (leaveInHeading) {
            // The hold cell goes before the close cell.
            headingLayout->insertWidget(headingLayout->indexOf(closeButton), leave);
            leave->setFixedWidth(leave->cellWidth());
        } else {
            leaveLayout->insertWidget(0, leave);
            leave->setMinimumWidth(0);
            leave->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        leave->setCell(leaveInHeading);
        // Reparenting makes Qt resolve the button palette against the new row.
        if (widgetCreated)
            widgetCreated(leave);
    }
    leave->setVisible(shown);
    zenLeaveBar->setVisible(shown && !leaveInHeading);
    return shown && !leaveInHeading ? zenLeaveBar->sizeHint().height() : 0;
}

QVector<QRect> SettingsPanel::pageRows() const
{
    QVector<QRect> result;
    auto *rows =
            settingsPages->currentWidget() ? settingsPages->currentWidget()->layout() : nullptr;
    if (!rows)
        return result;
    for (int index = 0; index < rows->count(); ++index)
        if (!rows->itemAt(index)->isEmpty())
            result.append(rows->itemAt(index)->geometry());
    return result;
}

bool SettingsPanel::eventFilter(QObject *watched, QEvent *event)
{
    // The page's layout handles the request after this filter. Compare the
    // rows once it has, and refit only when one moved or changed height.
    if (event->type() == QEvent::LayoutRequest && watched == settingsPages->currentWidget()
        && isVisible() && optionPages.size() > 1 && !repaginateQueued && refitRequested) {
        repaginateQueued = true;
        qt::later(0, this, [this] {
            repaginateQueued = false;
            if (isVisible() && pageRows() != pagedRows && refitRequested)
                refitRequested();
        });
    }
    return ui::Panel::eventFilter(watched, event);
}

void SettingsPanel::setZenLayout(bool value)
{
    if (zen != value) {
        zen = value;
        // Zen mode opens on its page; leaving it shows Display first.
        settingsPages->setCurrentIndex(int(zen ? SettingsPage::Zen : SettingsPage::Display));
        styleTabs(listMode);
    } else if (zen) {
        settingsPages->setCurrentIndex(int(SettingsPage::Zen));
    }
}

void SettingsPanel::setZenLightAvailable(bool available)
{
    if (!zenLightButton)
        return;
    const bool changed = zenLightButton->isVisibleTo(zenPage) != available;
    zenLightButton->setVisible(available);
    if (changed && zen && isVisible() && refitRequested)
        refitRequested();
}

void SettingsPanel::setZenSync(bool ready, bool enabled, const QString &last,
                               const QString &automatic)
{
    if (!zenSyncSection)
        return;
    const bool show = features::enabled(featurePreferences, features::Kosync) && ready;
    const bool changed = zenSyncSection->isVisibleTo(zenPage) != show;
    for (QWidget *row : { zenSyncSection, static_cast<QWidget *>(zenSyncButton),
                          static_cast<QWidget *>(zenAutomaticButton) })
        row->setVisible(show);
    zenSyncButton->setDescription(last);
    zenSyncButton->setEnabled(enabled);
    zenAutomaticButton->setCaption(automatic);
    zenAutomaticButton->setEnabled(enabled);
    if (changed && zen && isVisible() && refitRequested)
        refitRequested();
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
    delete restorePresetsDialog;
    delete standbyDialog;
    delete automaticOpeningDialog;
    delete sheet;
    for (const auto &saved : choiceSheets)
        delete saved.overlay;
    delete fontSheet;
}

namespace {
void retireChoiceSheet(ui::ModalOverlay *overlay)
{
    // A selection callback can reopen the chooser before its click returns.
    // Only the replacement should be found by name while the old sheet waits.
    for (auto *widget : overlay->findChildren<QWidget *>())
        widget->setObjectName({});
    overlay->setObjectName({});
    overlay->deleteLater();
}

QVariantList choiceSheetAppearance(QWidget *parent)
{
    return { ui::density(),       ui::textScale(),  ui::iconPx(100),
             ui::interfaceFont(), ui::chromeFont(), parent->palette() };
}

class SettingsFontSheet final : public ui::ModalOverlay
{
public:
    explicit SettingsFontSheet(QWidget *parent)
        : ui::ModalOverlay(parent), appearance(choiceSheetAppearance(parent))
    {
        setDimmed(true);
        auto *rows = card()->rows();
        heading = card()->heading({}, {}, "rrSettingsFontTitle").title;
        section = new ui::ListHeader(card());
        section->setObjectName("rrSettingsFontSection");
        section->setShaded(true);
        section->setFixedHeight(section->sizeHint().height());
        rows->addWidget(section);
        choices = new ui::RadioList(card());
        choices->setObjectName("rrSettingsFontChoices");
        choices->setMinimumRowHeight(metrics::list::oneLine);
        rows->addWidget(choices);
        pager = new ui::PagerBar(card(), "rrSettingsFont");
        rows->addWidget(pager);
        connect(pager->previous, &QPushButton::clicked, this,
                [this] { showPage(pager->turnTarget(-1)); });
        connect(pager->next, &QPushButton::clicked, this,
                [this] { showPage(pager->turnTarget(1)); });
    }

    bool accepts(int count) const
    {
        return choices->options().size() == count
                && appearance == choiceSheetAppearance(parentWidget());
    }

    void configure(const QString &title, const QVector<ReadingFont> &catalog,
                   const QString &current, const QString &emptyLabel,
                   std::function<void(const QString &)> chosen)
    {
        setGeometry(parentWidget()->rect());
        heading->setText(title);
        fonts = catalog;
        int selected = 0;
        for (int index = 0; index < fonts.size(); ++index) {
            const auto &family = fonts[index].family;
            const auto label = family.isEmpty() ? emptyLabel : fontDisplayName(family);
            auto *option = index < choices->options().size()
                    ? choices->options()[index]
                    : choices->add(QString("rrSettingsFontChoice%1").arg(index), label);
            option->setText(label);
            option->setProperty("reader_font_family", family);
            if (family == current)
                selected = index;
        }
        choices->setCurrent(selected);
        choices->chosen = [this, chosen = std::move(chosen)](int index) {
            const auto family = fonts[index].family;
            dismissed();
            chosen(family);
        };
        fit(selected);
    }

    void release()
    {
        choices->chosen = {};
        fonts.clear();
        pages.clear();
    }

private:
    void resizeEvent(QResizeEvent *event) override
    {
        if (!fonts.isEmpty() && size() != fittedSize)
            fit(firstVisible);
        ui::ModalOverlay::resizeEvent(event);
    }

    void fit(int row)
    {
        fittedSize = size();
        cardWidth =
                qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
        rowHeight = choices->rowHeight(cardWidth - 2 * metrics::control::border);
        auto *header = heading->parentWidget();
        header->ensurePolished();
        const auto margins = card()->rows()->contentsMargins();
        const int overhead = margins.top() + margins.bottom() + header->sizeHint().height()
                + section->height() + pager->height();
        const int perPage =
                qMax(1, (height() - 2 * metrics::notice::sideClearance - overhead) / rowHeight);
        pages.clear();
        int initialPage = 0;
        for (int first = 0; first < fonts.size();) {
            int end = first + 1;
            while (end < fonts.size() && end - first < perPage
                   && fonts[end].origin == fonts[first].origin)
                ++end;
            if (first <= row && row < end)
                initialPage = pages.size();
            pages.append({ first, end - first });
            first = end;
        }
        showPage(initialPage);
    }

    void showPage(int page)
    {
        if (pages.isEmpty())
            return;
        page = qBound(0, page, int(pages.size()) - 1);
        const auto range = pages[page];
        firstVisible = range.first;
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
        card()->fitToWidth(cardWidth);
        centerCard();
    }

    QLabel *heading;
    ui::ListHeader *section;
    ui::RadioList *choices;
    ui::PagerBar *pager;
    QVector<ReadingFont> fonts;
    QVector<QPair<int, int>> pages;
    int firstVisible = 0, cardWidth = 0, rowHeight = 0;
    QSize fittedSize;
    const QVariantList appearance;
};
} // namespace

void SettingsPanel::showFontChoices(const QString &title, const QVector<ReadingFont> &fonts,
                                    const QString &current, const QString &emptyLabel,
                                    std::function<void(const QString &)> chosen)
{
    if (sheet || fonts.isEmpty())
        return;
    auto *popup = static_cast<SettingsFontSheet *>(fontSheet.data());
    if (popup && !popup->accepts(fonts.size())) {
        retireChoiceSheet(popup);
        popup = nullptr;
    }
    if (!popup) {
        popup = new SettingsFontSheet(parentWidget());
        fontSheet = popup;
        popup->dismissed = [panel = QPointer<SettingsPanel>(this), popup] {
            if (panel && panel->sheet == popup)
                panel->dismissSheet();
        };
    }
    sheet = popup;
    popup->setObjectName("rrSettingsFontPopup");
    popup->configure(title, fonts, current, emptyLabel, std::move(chosen));
    presentSheet();
}

void SettingsPanel::releaseFontChoice()
{
    static_cast<SettingsFontSheet *>(fontSheet.data())->release();
}

QString SettingsPanel::dictionaryFontSizeLabel(int percent, bool compact)
{
    switch (percent) {
    case 80:
        return strings::get(compact ? "settings.dictionary_font_small_short"
                                    : "settings.dictionary_font_small");
    case 120:
        return strings::get(compact ? "settings.dictionary_font_large_short"
                                    : "settings.dictionary_font_large");
    case 140:
        return strings::get(compact ? "settings.dictionary_font_extra_large_short"
                                    : "settings.dictionary_font_extra_large");
    default:
        return strings::get(compact ? "settings.dictionary_font_standard_short"
                                    : "settings.dictionary_font_standard");
    }
}

ui::RadioList *SettingsPanel::openChoiceSheet(const QString &name, const QString &title,
                                              const QString &hint, const QString &titleName,
                                              QVariantList inputs)
{
    inputs << title;
    inputs.append(choiceSheetAppearance(parentWidget()));
    auto &saved = choiceSheets[name];
    if (!saved.overlay || saved.inputs != inputs) {
        if (saved.overlay)
            retireChoiceSheet(saved.overlay);
        const auto made = ui::makeChoiceSheet(parentWidget(), name, title, hint, titleName);
        saved.overlay = made.overlay;
        saved.list = made.list;
        saved.inputs = std::move(inputs);
        made.overlay->dismissed = [panel = QPointer<SettingsPanel>(this), overlay = made.overlay] {
            if (panel && panel->sheet == overlay)
                panel->dismissSheet();
        };
    }
    sheet = saved.overlay;
    sheet->setGeometry(parentWidget()->rect());
    activeChoices = saved.list;
    return activeChoices;
}

void SettingsPanel::presentChoices(int current, std::function<void(int)> chosen)
{
    activeChoices->setCurrent(current);
    activeChoices->chosen = [this, chosen = std::move(chosen)](int index) {
        dismissSheet();
        chosen(index);
    };
    ui::fitChoiceSheet(sheet, activeChoices);
    presentSheet();
}

void SettingsPanel::showDictionaryFontSizes(int current, const QString &family, bool customFont,
                                            std::function<void(int)> chosen)
{
    if (sheet)
        return;
    const bool compact = isPhoneScreen(parentWidget()->size(), ui::density());
    auto *choices = openChoiceSheet(
            "rrDictionaryFontSize", strings::get("settings.dictionary_font_size"), {}, {},
            { family, customFont, compact,
              ui::privateLayoutKey(DictionaryPanel::definitionFont(family, 100)) });
    if (choices->options().isEmpty()) {
        choices->setMinimumRowHeight(metrics::list::oneLine);
        for (int percent : appearance::dictionaryFontSizes) {
            auto *option = choices->add(
                    QString("rrDictionaryFontSize%1").arg(percent),
                    ui::textScale() >= 1.5 ? QString() : dictionaryFontSizeLabel(percent, compact));
            option->setAccessibleName(dictionaryFontSizeLabel(percent));
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
        }
    }
    const auto &sizes = appearance::dictionaryFontSizes;
    presentChoices(int(std::find(sizes.begin(), sizes.end(), current) - sizes.begin()),
                   [chosen](int index) { chosen(appearance::dictionaryFontSizes[index]); });
}

void SettingsPanel::showHeaderFooterWeights(int current, const QFont &sampleFont,
                                            std::function<void(int)> chosen)
{
    if (sheet)
        return;
    auto *choices =
            openChoiceSheet("rrHeaderFooterWeight", strings::get("settings.header_footer_weight"),
                            {}, "rrHeaderFooterWeightTitle", { sampleFont, ui::interfaceWeight() });
    const QVector<int> weights{ 0, 2, 4, 6, 8 };
    if (choices->options().isEmpty()) {
        choices->setMinimumRowHeight(metrics::list::oneLine);
        for (int steps : weights) {
            auto *option = choices->add(
                    QString("rrHeaderFooterWeight%1").arg(steps),
                    steps ? strings::get("settings.header_footer_weight_heavier").arg(steps)
                          : strings::get("settings.header_footer_weight_lighter"));
            ui::DescribedOption::ValueStyle sample;
            sample.sampleFont = sampleFont;
            sample.sampleWeight = ui::interfaceWeight() + steps;
            option->setValue(QStringLiteral("SAMPLE TEXT"), sample);
        }
    }
    presentChoices(weights.indexOf(current),
                   [weights, chosen](int index) { chosen(weights[index]); });
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
    if (sheet)
        return;
    sheet = new ui::ModalOverlay(parentWidget());
    sheet->setObjectName("rrBeautifierPopup");
    sheet->setDimmed(true);
    sheet->setGeometry(parentWidget()->rect());
    sheet->dismissed = [this] { dismissSheet(); };
    auto *card = sheet->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    card->heading(strings::get("reader.book_beautifier"),
                  strings::get("settings.beautifier_description"), {}, {});
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
                             "rrBeautifierInitials",   "rrBeautifierSpacing",
                             "rrBeautifierIndent",     "rrNormalizationTextColors" };
    const QStringList labels{
        "settings.normalization_margins", "settings.normalization_chapters",
        "settings.beautifier_initials",   "settings.beautifier_spacing",
        "settings.beautifier_indent",     "settings.normalization_text_colors"
    };
    for (int i = 0; i < int(appearance::normalizationKeys.size()); ++i) {
        const auto prefix = labels[i];
        auto *option = checklistOption(card, names[i], strings::get(prefix.toUtf8().constData()),
                                       strings::get((prefix + "_description").toUtf8().constData()),
                                       i == 0);
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
    connect(cancel, &QPushButton::clicked, this, [this] { dismissSheet(); });
    connect(done, &QPushButton::clicked, this, [this, draft, chosen] {
        const auto value = *draft;
        dismissSheet();
        chosen(value);
    });
    const int width =
            qMin<int>(ui::px(1064), parentWidget()->width() - 2 * metrics::notice::sideClearance);
    for (auto *option : options)
        option->setFixedHeight(option->heightForWidth(width - 2 * metrics::control::border));
    card->fitToWidth(width);
    sheet->centerCard();
    presentSheet();
}

QString SettingsPanel::timeRemainingFormatLabel(const QString &format)
{
    return strings::get(format == "approximate" ? "settings.time_remaining_approximate"
                                                : "settings.time_remaining_detailed");
}

void SettingsPanel::showTimeRemainingFormats(const QString &current,
                                             std::function<void(const QString &)> chosen)
{
    if (sheet)
        return;
    auto *choices =
            openChoiceSheet("rrTimeRemainingFormat", strings::get("settings.time_remaining_format"),
                            strings::get("settings.time_remaining_format_description"));
    const QStringList formats{ "detailed", "approximate" };
    if (choices->options().isEmpty())
        for (const auto &format : formats)
            choices->add(QString("rrTimeRemainingFormat_%1").arg(format),
                         timeRemainingFormatLabel(format),
                         strings::get(QString("settings.time_remaining_%1_description")
                                              .arg(format)
                                              .toLatin1()
                                              .constData()));
    presentChoices(formats.indexOf(current),
                   [chosen, formats](int index) { chosen(formats[index]); });
}

QString SettingsPanel::percentageFormatLabel(const QString &format)
{
    return strings::get(format == "decimal" ? "settings.percentage_decimal"
                                            : "settings.percentage_whole");
}

void SettingsPanel::showPercentageFormats(const QString &current,
                                          std::function<void(const QString &)> chosen)
{
    if (sheet)
        return;
    auto *choices =
            openChoiceSheet("rrPercentageFormat", strings::get("settings.percentage_format"),
                            strings::get("settings.percentage_format_description"));
    const QStringList formats{ "whole", "decimal" };
    if (choices->options().isEmpty())
        for (const auto &format : formats)
            choices->add(QString("rrPercentageFormat_%1").arg(format),
                         percentageFormatLabel(format),
                         strings::get(QString("settings.percentage_%1_description")
                                              .arg(format)
                                              .toLatin1()
                                              .constData()));
    presentChoices(formats.indexOf(current),
                   [chosen, formats](int index) { chosen(formats[index]); });
}

QString SettingsPanel::renderingModeLabel(const QString &mode)
{
    return strings::get(mode == "accurate"       ? "settings.rendering_accurate"
                                : mode == "fast" ? "settings.rendering_fast"
                                                 : "settings.rendering_hybrid");
}

void SettingsPanel::showRenderingModes(const QString &current,
                                       std::function<void(const QString &)> chosen)
{
    if (sheet)
        return;
    // Hybrid replaces Fast. Debug mode still offers it.
    QStringList modes{ "hybrid", "accurate" };
    if (debugModeOption && debugModeOption->isChecked())
        modes.insert(1, "fast");
    auto *choices = openChoiceSheet("rrRendering", strings::get("settings.rendering"),
                                    strings::get("settings.rendering_description"), {}, { modes });
    if (choices->options().isEmpty())
        for (const auto &mode : modes)
            choices->add(QString("rrRendering_%1").arg(mode), renderingModeLabel(mode),
                         strings::get(mode == "accurate" ? "settings.rendering_accurate_description"
                                              : mode == "fast"
                                              ? "settings.rendering_fast_description"
                                              : "settings.rendering_hybrid_description"));
    presentChoices(modes.indexOf(current), [chosen, modes](int index) { chosen(modes[index]); });
}

void SettingsPanel::showColumns(bool landscape, bool alignChapters, int spacing,
                                std::function<void(bool, bool, int)> chosen)
{
    if (sheet)
        return;
    sheet = new ui::ModalOverlay(parentWidget());
    sheet->setObjectName("rrColumnsPopup");
    sheet->setDimmed(true);
    sheet->setGeometry(parentWidget()->rect());
    sheet->dismissed = [this] { dismissSheet(); };
    auto *card = sheet->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    card->heading(strings::get("reader.columns"), strings::get("settings.columns_description"));

    auto *columns =
            checklistOption(card, "rrLandscapeColumns", strings::get("settings.landscape_columns"),
                            strings::get("settings.landscape_columns_description"), true);
    auto *chapters =
            checklistOption(card, "rrChapterStartsSpread", strings::get("settings.chapter_spread"),
                            strings::get("settings.chapter_spread_description"), false);
    rows->addWidget(columns);
    rows->addWidget(chapters);
    columns->setChecked(landscape);
    chapters->setChecked(alignChapters);
    chapters->setEnabled(landscape);
    connect(columns, &QCheckBox::toggled, chapters, &QWidget::setEnabled);

    auto *area = new QWidget(card);
    auto *controls = new QVBoxLayout(area);
    controls->setContentsMargins(chrome::inset, ui::px(32), chrome::inset, ui::px(32));
    rows->addWidget(area);
    auto *gap = ui::sliderRow(area, controls, "rrColumnSpacingSetting", "rrColumnSpacingValue",
                              strings::get("reader.column_spacing"), "rrColumnSpacingLess",
                              "rrColumnSpacingMore", 100, 300, 50, {});
    gap->setMarkers({ 100, 150, 200, 250, 300 }, ui::SettingSlider::MarkerMode::Stops);
    const auto showValue = [gap](int value) {
        const auto label = columnSpacingLabel(value);
        gap->valueLabel()->setText(label);
        gap->setAccessibleDescription(label);
    };
    connect(gap, &QSlider::valueChanged, gap, showValue);
    gap->setValue(spacing);
    showValue(gap->value());
    area->setEnabled(landscape);
    connect(columns, &QCheckBox::toggled, area, &QWidget::setEnabled);

    auto *cancel =
            ui::makeModalButton(card, { strings::get("common.cancel"), "rrColumnsCancel" }, false);
    auto *done = ui::makeModalButton(card, { strings::get("common.done"), "rrColumnsDone" }, true);
    card->adaptiveActions({ cancel, done });
    connect(cancel, &QPushButton::clicked, this, [this] { dismissSheet(); });
    connect(done, &QPushButton::clicked, this, [this, columns, chapters, gap, chosen] {
        const bool enabled = columns->isChecked();
        const bool aligned = chapters->isChecked();
        const int percent = gap->value();
        dismissSheet();
        chosen(enabled, aligned, percent);
    });

    const int width =
            qMin<int>(ui::px(1064), parentWidget()->width() - 2 * metrics::notice::sideClearance);
    for (auto *option : { columns, chapters })
        option->setFixedHeight(option->heightForWidth(width - 2 * metrics::control::border));
    card->fitToWidth(width);
    sheet->centerCard();
    presentSheet();
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
    readingPage.setWidth(
            qMin(readingPage.width(),
                 stylingSampleWidth(parentWidget()) + 2 * horizontalMargin - 2 * side));
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
    if (sheet)
        return;
    ui::PanelTiming timing("styling");
    timing.field("feature", int(feature));
    sheet = new ui::ModalOverlay(parentWidget());
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
    sheet->setObjectName(prefix + "Popup");
    sheet->setDimmed(true);
    sheet->setGeometry(parentWidget()->rect());
    sheet->dismissed = [this] { dismissSheet(); };
    auto *card = sheet->card();
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
    connect(close, &QPushButton::clicked, this, [this] { dismissSheet(); });
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
    presentSheet();
    showStylingPreview();
    if (!stylingSamples.value(int(feature)).valid() && stylingRequested)
        stylingRequested(feature);
}

void SettingsPanel::fitStylingCard()
{
    if (!sheet || !stylingPreview)
        return;
    auto *card = sheet->card();
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
    sheet->centerCard();
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
    if (auto *same = sheet ? sheet->findChild<QWidget *>("rrStylingSameNote") : nullptr) {
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

void SettingsPanel::dismissSheet()
{
    if (!sheet)
        return;
    // The closed Styling sheet is deleted later. Until then, only the next sheet's
    // preview may answer to the shared name.
    if (stylingPreview)
        stylingPreview->setObjectName({});
    stylingPreview.clear();
    stylingSampleReady = {};
    const auto closing = sheet;
    sheet.clear();
    const bool retained = activeChoices || closing == fontSheet;
    if (activeChoices) {
        activeChoices->chosen = {};
        activeChoices = nullptr;
    } else if (closing == fontSheet) {
        releaseFontChoice();
    }
    closing->hide();
    if (!retained && closing)
        closing->deleteLater();
}

void SettingsPanel::presentSheet()
{
    sheet->show();
    sheet->raise();
    sheet->setFocus();
    if (sheetShown)
        sheetShown(sheet);
}

void SettingsPanel::dialogCreated(ui::ChoiceModal *dialog)
{
    dialog->setDimmed(true);
    dialog->hide();
    if (widgetCreated)
        widgetCreated(dialog);
}

ui::ChoiceModal *SettingsPanel::restoreConfirmation()
{
    if (restoreDialog)
        return restoreDialog;
    restoreDialog = new ui::ChoiceModal(
            parentWidget(),
            { strings::get(
                      "reader.your_reading_position_stays_the_same_the_defaults_apply_when_you_c"),
              strings::get("reader.restore_appearance_defaults_prompt"),
              {},
              "rrRestoreTitle" },
            { strings::get("common.cancel"), "rrKeepDefaults" },
            { strings::get("reader.restore_defaults"), "rrConfirmDefaults" });
    restoreDialog->setObjectName("rrRestoreConfirmation");
    restoreDialog->descriptionLabel()->setObjectName("rrRestoreDescription");
    connect(restoreDialog->secondaryButton(), &QPushButton::clicked, this,
            [this] { dismissRestore(); });
    connect(restoreDialog->primaryButton(), &QPushButton::clicked, this, [this] {
        dismissRestore();
        if (restoreDefaultsConfirmed)
            restoreDefaultsConfirmed();
    });
    restoreDialog->dismissed = [this] { dismissRestore(); };
    dialogCreated(restoreDialog);
    return restoreDialog;
}

ui::ChoiceModal *SettingsPanel::restorePresetsConfirmation()
{
    if (restorePresetsDialog)
        return restorePresetsDialog;
    restorePresetsDialog = new ui::ChoiceModal(
            parentWidget(),
            { strings::get("text_presets.restore_detail"),
              strings::get("text_presets.restore_prompt"),
              {} },
            { strings::get("common.cancel"), "rrKeepPresets" },
            { strings::get("text_presets.restore_confirm"), "rrConfirmPresets" });
    restorePresetsDialog->setObjectName("rrRestorePresetsConfirmation");
    connect(restorePresetsDialog->secondaryButton(), &QPushButton::clicked, this,
            [this] { dismissRestore(); });
    connect(restorePresetsDialog->primaryButton(), &QPushButton::clicked, this, [this] {
        dismissRestore();
        if (restorePresetsConfirmed)
            restorePresetsConfirmed();
    });
    restorePresetsDialog->dismissed = [this] { dismissRestore(); };
    dialogCreated(restorePresetsDialog);
    return restorePresetsDialog;
}

ui::ChoiceModal *SettingsPanel::automaticOpeningConfirmation()
{
    if (automaticOpeningDialog)
        return automaticOpeningDialog;
    // Turning automatic opening off sends every later book to Kobo's
    // reader, so ask first.
    automaticOpeningDialog =
            new ui::ChoiceModal(parentWidget(),
                                { strings::get("settings.automatic_book_opening_off_description"),
                                  strings::get("settings.automatic_book_opening_off_title"),
                                  {} },
                                { strings::get("common.cancel"), "rrKeepAutomaticBookOpening" },
                                { strings::get("settings.automatic_book_opening_off"),
                                  "rrConfirmAutomaticBookOpeningOff" });
    automaticOpeningDialog->setObjectName("rrAutomaticBookOpeningConfirmation");
    connect(automaticOpeningDialog->secondaryButton(), &QPushButton::clicked, this,
            [this] { dismissAutomaticOpeningConfirmation(); });
    connect(automaticOpeningDialog->primaryButton(), &QPushButton::clicked, this, [this] {
        dismissAutomaticOpeningConfirmation();
        if (automaticOpeningChosen)
            automaticOpeningChosen(false);
    });
    automaticOpeningDialog->dismissed = [this] { dismissAutomaticOpeningConfirmation(); };
    dialogCreated(automaticOpeningDialog);
    return automaticOpeningDialog;
}

ui::ChoiceModal *SettingsPanel::standbyConfirmation()
{
    if (standbyDialog)
        return standbyDialog;
    standbyDialog = new ui::ChoiceModal(
            parentWidget(),
            { strings::get("settings.disable_reader_standby_warning"),
              strings::get("settings.disable_reader_standby_title"),
              {} },
            { strings::get("common.cancel"), "rrKeepReaderStandby" },
            { strings::get("settings.disable_reader_standby"), "rrConfirmDisableStandby" });
    standbyDialog->setObjectName("rrStandbyConfirmation");
    connect(standbyDialog->secondaryButton(), &QPushButton::clicked, this,
            [this] { dismissStandbyConfirmation(); });
    connect(standbyDialog->primaryButton(), &QPushButton::clicked, this, [this] {
        disableStandbyOption->setChecked(true);
        dismissStandbyConfirmation();
    });
    standbyDialog->dismissed = [this] { dismissStandbyConfirmation(); };
    dialogCreated(standbyDialog);
    return standbyDialog;
}

void SettingsPanel::dismissRestore()
{
    if (restoreDialog && !restoreDialog->isHidden()) {
        restoreDialog->hide();
        resetDefaults->setFocus();
    }
    if (restorePresetsDialog && !restorePresetsDialog->isHidden()) {
        restorePresetsDialog->hide();
        resetPresets->setFocus();
    }
}

void SettingsPanel::dismissAutomaticOpeningConfirmation()
{
    if (!automaticOpeningDialog || automaticOpeningDialog->isHidden())
        return;
    automaticOpeningDialog->hide();
    automaticOpeningOption->setFocus();
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
    kosyncSchedule.dismiss();
    // The next opening starts on the section list.
    sectionOpen = false;
    dismissRestore();
    dismissStandbyConfirmation();
    dismissAutomaticOpeningConfirmation();
    dismissSheet();
    if (stylingHidden)
        stylingHidden();
    stylingSamples.clear();
    ui::Panel::hideEvent(event);
}

void SettingsPanel::resizeEvent(QResizeEvent *event)
{
    // A rotation can leave too few rows for the current font page. Reopen at the new size.
    dismissSheet();
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

void SettingsPanel::showDebug(bool visible)
{
    auto *button = settingsTabButtons[int(SettingsPage::Debug)];
    if (button->isVisibleTo(this) == visible && button->isEnabled() == visible)
        return;
    button->setVisible(visible);
    button->setEnabled(visible);
    if (!visible && settingsPages->currentIndex() == int(SettingsPage::Debug))
        settingsPages->setCurrentIndex(int(SettingsPage::System));
    fitRail();
    // The checkbox changes the tabs while Settings is open: fit the rail to
    // the panel again, as Hidden features does.
    if (isVisible() && refitRequested)
        refitRequested();
}

void SettingsPanel::updateFeatureOptions()
{
    QStringList hidden;
    for (const auto &entry : featureOptions)
        if (!features::enabled(featurePreferences, entry.feature))
            hidden.append(strings::get(entry.label));
    const bool navigate = features::enabled(featurePreferences, features::Navigate);
    const bool stats = features::enabled(featurePreferences, features::Stats);
    if (disabledFeaturesButton) {
        disabledFeaturesButton->setCaption(hidden.isEmpty()
                                                   ? strings::get("settings.hidden_features_none")
                                                   : hidden.join(", "));
        saveWordsOption->setVisible(features::enabled(featurePreferences, features::Words));
    }
    if (navigationBarButton) {
        const bool annotations = features::enabled(featurePreferences, features::Annotations);
        navigationBarButton->setVisible(navigate);
        annotationInteractionButton->setVisible(annotations);
        findChild<QWidget *>("rrControlsAnnotationsSection")->setVisible(annotations);
        menuViewButton->setVisible(navigate || stats);
    }
    auto *syncTab = settingsTabButtons[int(SettingsPage::Kosync)];
    syncTab->setVisible(features::enabled(featurePreferences, features::Kosync));
    if (syncTab->isHidden() && settingsPages->currentIndex() == int(SettingsPage::Kosync))
        settingsPages->setCurrentIndex(int(SettingsPage::System));
}

void SettingsPanel::showDisabledFeatures()
{
    if (sheet)
        return;
    sheet = new ui::ModalOverlay(parentWidget());
    sheet->setObjectName("rrDisabledFeaturesPopup");
    sheet->setDimmed(true);
    sheet->setGeometry(parentWidget()->rect());
    sheet->dismissed = [this] { dismissSheet(); };
    auto *card = sheet->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::notice::cardMargins.top(),
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    card->heading(strings::get("settings.disable_features"),
                  strings::get("settings.disable_features_description"));
    auto draft = std::make_shared<QJsonObject>(featurePreferences);
    QVector<ui::DescribedOption *> options;
    ui::DescribedOption *stats = nullptr, *activity = nullptr;
    for (const auto &entry : featureOptions) {
        const auto *key = features::keys[entry.feature];
        auto *option = checklistOption(
                card, QString("rrDisableFeature_%1").arg(key), strings::get(entry.label),
                strings::get((QByteArray(entry.label) + "_description").constData()),
                options.isEmpty());
        auto presentation = option->presentation();
        presentation.leadingIndicator = false;
        option->setPresentation(presentation);
        using Branch = ui::DescribedOption::IconBranch;
        auto branch = Branch::None;
        if (entry.feature == features::Stats) {
            stats = option;
            stats->setProperty("keepWithNext", true);
            branch = Branch::Parent;
        } else if (entry.feature == features::Activity) {
            activity = option;
            branch = Branch::Child;
        }
        option->setLeadingIcon(entry.icon, branch);
        option->setChecked(draft->value(key).toBool(false));
        rows->addWidget(option);
        options.append(option);
        connect(option, &QCheckBox::clicked, card,
                [draft, key](bool hidden) { (*draft)[key] = hidden; });
    }
    const auto updateActivity = [activity, draft] {
        activity->setEnabled(features::enabled(*draft, features::Stats));
        activity->setChecked(!features::enabled(*draft, features::Activity));
    };
    connect(stats, &QCheckBox::clicked, card, updateActivity);
    updateActivity();
    auto *cancel = ui::makeModalButton(
            card, { strings::get("common.cancel"), "rrDisabledFeaturesCancel" }, false);
    auto *done = ui::makeModalButton(
            card, { strings::get("common.done"), "rrDisabledFeaturesDone" }, true);
    card->adaptiveActions({ cancel, done });
    connect(cancel, &QPushButton::clicked, this, [this] { dismissSheet(); });
    connect(done, &QPushButton::clicked, this, [this, draft] {
        featurePreferences = *draft;
        dismissSheet();
        updateFeatureOptions();
        if (refitRequested)
            refitRequested();
    });
    const int width =
            qMin<int>(ui::px(1064), parentWidget()->width() - 2 * metrics::notice::sideClearance);
    for (auto *option : options)
        option->setFixedHeight(option->heightForWidth(width - 2 * metrics::control::border));
    card->fitToWidth(width);
    sheet->centerCard();
    presentSheet();
}

void SettingsPanel::showPowerManagement(bool visible)
{
    if (wifiPowerSaverOption)
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
