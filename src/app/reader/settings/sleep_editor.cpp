#include "sleep_editor.h"
#include "settings_panel.h"
#include "sleep_gallery.h"

#include "app/reader/views/sleep_screen.h"
#include "support/strings.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/options.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/scale.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace chrome = ui::metrics::chrome;
namespace metrics = ui::metrics;

namespace {
QString backdropLabel(sleep::Mode mode)
{
    switch (mode) {
    case sleep::Mode::Cover:
        return strings::get("sleep.backdrop_cover");
    case sleep::Mode::Page:
        return strings::get("sleep.backdrop_page");
    case sleep::Mode::Overlay:
        return strings::get("sleep.backdrop_overlay");
    case sleep::Mode::Wallpaper:
        return strings::get("sleep.backdrop_wallpaper");
    default:
        return strings::get("sleep.backdrop_plain");
    }
}

QString statusLabel(sleep::Design status)
{
    return status == sleep::Design::Cover ? strings::get("sleep.status_none")
                                          : SleepGallery::label(status);
}

QString placeLabel(sleep::Place place)
{
    return strings::get(QStringLiteral("sleep.place_%1")
                                .arg(sleep::value(place))
                                .replace('-', '_')
                                .toLatin1()
                                .constData());
}
} // namespace

// The large preview. It draws the screen as it will look and, over it, a
// dashed area for every other place the status can move to.
class SleepEditorPreview final : public QWidget
{
public:
    explicit SleepEditorPreview(SleepEditor *editor) : QWidget(editor), editor(editor)
    {
        setObjectName("rrSleepEditorPreview");
        setAttribute(Qt::WA_OpaquePaintEvent);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    // The screen's place in the widget, keeping the screen's shape.
    QRect screenArea() const
    {
        const int margin = ui::px(28);
        const QSize screen = editor->setup_.screen;
        const QSize room(qMax(1, width() - 2 * margin), qMax(1, height() - 2 * margin));
        const QSize fitted = screen.isEmpty() ? QSize() : screen.scaled(room, Qt::KeepAspectRatio);
        return { (width() - fitted.width()) / 2, (height() - fitted.height()) / 2, fitted.width(),
                 fitted.height() };
    }

    // The area each other place covers, in widget coordinates.
    QVector<QPair<sleep::Place, QRect>> targets() const
    {
        QVector<QPair<sleep::Place, QRect>> result;
        const auto area = screenArea();
        const QSize screen = editor->setup_.screen;
        if (area.isEmpty() || screen.isEmpty())
            return result;
        const auto look = editor->look();
        const auto status = sleep::statusDesign(look);
        const auto current = editor->place();
        const qreal scaleX = qreal(area.width()) / screen.width();
        const qreal scaleY = qreal(area.height()) / screen.height();
        for (const auto option : sleep::places(status)) {
            if (option == current)
                continue;
            QRect bounds;
            if (status == sleep::Design::Caption) {
                // The caption's repaint area is only its state line; offer
                // the whole half of the screen it fades over.
                const int half = qRound(screen.height() * 0.46);
                bounds = option == sleep::Place::Top
                        ? QRect(0, 0, screen.width(), half)
                        : QRect(0, screen.height() - half, screen.width(), half);
            } else {
                auto shot = editor->setup_.sample;
                shot.design = look.design;
                shot.mode = look.mode;
                shot.tile = look.tile;
                shot.place = option;
                for (const auto &part : sleepStatusAreas(screen, shot))
                    bounds |= part;
            }
            if (bounds.isEmpty())
                continue;
            result.append({ option,
                            QRectF(area.left() + bounds.left() * scaleX,
                                   area.top() + bounds.top() * scaleY, bounds.width() * scaleX,
                                   bounds.height() * scaleY)
                                    .toAlignedRect()
                                    .intersected(area) });
        }
        return result;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto colors = ui::themeColors(this);
        painter.fillRect(rect(), colors.paper);
        const auto area = screenArea();
        if (area.isEmpty())
            return;
        auto shot = editor->setup_.sample;
        const auto look = editor->look();
        shot.design = look.design;
        shot.mode = look.mode;
        shot.tile = look.tile;
        shot.place = editor->place();
        const bool off = editor->powerOff_ && !editor->setup_.arrange;
        if (off)
            shot.dark = true;
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        paintSleepPreview(painter, QRectF(area), editor->setup_.screen, editor->cover_,
                          editor->page_, editor->overlay_, shot, editor->wallpaper(),
                          off ? SleepStatus::PoweredOff : SleepStatus::Sleeping);
        // The same thin frame as an unselected gallery preview.
        const int gap = ui::px(8);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(ui::dividerColor(this), int(metrics::control::border)));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(area.adjusted(-gap, -gap, gap - 1, gap - 1));
        // Dashed areas: ink dashes over a light wash, with the place's name.
        const QFont font = ui::interfaceFont(metrics::text::secondary);
        for (const auto &target : targets()) {
            const QRect box = target.second.adjusted(ui::px(4), ui::px(4), -ui::px(4), -ui::px(4));
            QColor wash = colors.paper;
            wash.setAlpha(110);
            painter.fillRect(box, wash);
            QPen dash(colors.ink, ui::px(4));
            dash.setStyle(Qt::DashLine);
            painter.setPen(dash);
            painter.drawRect(box);
            const QString label = placeLabel(target.first);
            const int padding = ui::px(12);
            const int labelWidth = qMin(box.width() - 2 * padding,
                                        ui::interfaceTextWidth(label, font) + 2 * padding);
            const int labelHeight = ui::interfaceLineHeight(font) + padding;
            if (labelWidth <= 0 || labelHeight > box.height())
                continue;
            // Corner areas can overlap, such as the card's. Each label keeps to
            // its own outer side so none covers another.
            const auto place = target.first;
            const bool left = place == sleep::Place::TopLeft || place == sleep::Place::BottomLeft
                    || place == sleep::Place::Left;
            const bool right = place == sleep::Place::TopRight || place == sleep::Place::BottomRight
                    || place == sleep::Place::Right;
            const int x = left ? box.left() + padding
                    : right    ? box.right() - padding - labelWidth
                               : box.center().x() - labelWidth / 2;
            const QRect tag(x, box.center().y() - labelHeight / 2, labelWidth, labelHeight);
            painter.fillRect(tag, colors.paper);
            painter.setPen(QPen(colors.ink, int(metrics::control::border)));
            painter.drawRect(tag.adjusted(0, 0, -1, -1));
            painter.setFont(font);
            ui::drawSegmentPrivateText(painter, tag,
                                       ui::elideInterfaceText(label, font, tag.width() - padding),
                                       Qt::AlignCenter);
        }
    }

    void mousePressEvent(QMouseEvent *event) override { event->accept(); }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        for (const auto &target : targets())
            if (target.second.contains(event->pos())) {
                editor->move(target.first);
                return;
            }
    }

private:
    SleepEditor *editor;
};

SleepEditor::SleepEditor(QWidget *parent, Setup setup) : ui::Panel(parent), setup_(std::move(setup))
{
    setObjectName(setup_.arrange ? "rrSleepArrange" : "rrSleepEditor");
    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);
    // The same header as the gallery: a title in the brand face and a cell
    // on the right, here Done.
    auto *heading = new ui::Panel(this, Qt::BottomEdge);
    heading->setObjectName("rrSleepEditorHeader");
    heading->setEdgeWidth(chrome::heavyRule);
    heading->setFixedHeight(chrome::actionHeight);
    auto *headingRows = new QHBoxLayout(heading);
    headingRows->setContentsMargins(0, 0, 0, chrome::heavyRule);
    headingRows->setSpacing(0);
    const auto status = sleep::statusDesign(setup_.preset);
    auto *title = new SettingsTitle(
            setup_.arrange ? strings::get("sleep.arrange_title").arg(SleepGallery::label(status))
                           : strings::get("sleep.custom_title"),
            heading);
    title->setObjectName("rrSleepEditorTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    ui::readerStyle(title, ui::styles::brand(chrome::titleText) + "background:transparent;");
    headingRows->addWidget(title, 1);
    if (!setup_.arrange) {
        // Where wallpapers come from, explained on request rather than in a
        // line that crowds the controls.
        help_ = new ui::CellButton(ui::ControlIcon::Question, heading);
        help_->setObjectName("rrSleepEditorHelp");
        help_->setAccessibleName(strings::get("sleep.wallpaper_help_title"));
        help_->setRule(Qt::LeftEdge);
        help_->setFixedWidth(chrome::cellWidth);
        headingRows->addWidget(help_);
        connect(help_, &QPushButton::clicked, this, [this] {
            if (help)
                help();
        });
    }
    done_ = new ui::CellButton(ui::ControlIcon::Check, heading);
    done_->setObjectName("rrSleepEditorDone");
    done_->setAccessibleName(strings::get("common.done"));
    done_->setRule(Qt::LeftEdge);
    done_->setFixedWidth(chrome::cellWidth);
    headingRows->addWidget(done_);
    connect(done_, &QPushButton::clicked, this, [this] {
        if (done)
            done();
    });
    rows->addWidget(heading);

    auto *top = new QWidget(this);
    auto *topRows = new QVBoxLayout(top);
    topRows->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    topRows->setSpacing(0);
    rows->addWidget(top);
    if (!setup_.arrange) {
        // Which screen the controls change, shown only when the two differ.
        ui::SegmentedRow state(top, topRows, "rrSleepEditorState",
                               strings::get("sleep.custom_editing"));
        sleepingTab_ = state.add("rrSleepEditorSleeping", strings::get("sleep.sleeping"));
        poweredOffTab_ = state.add("rrSleepEditorPoweredOff", strings::get("sleep.powered_off"));
        for (auto *tab : { sleepingTab_, poweredOffTab_ })
            tab->setCheckable(true);
        sleepingTab_->setChecked(true);
        stateRow_ = ui::controlRowOf(sleepingTab_);
        connect(sleepingTab_, &QPushButton::clicked, this, [this] {
            powerOff_ = false;
            refresh();
        });
        connect(poweredOffTab_, &QPushButton::clicked, this, [this] {
            powerOff_ = true;
            refresh();
        });
    }

    preview_ = new SleepEditorPreview(this);
    rows->addWidget(preview_, 1);

    auto *controls = new QWidget(this);
    auto *controlRows = new QVBoxLayout(controls);
    controlRows->setContentsMargins(chrome::inset, 0, chrome::inset, ui::px(12));
    controlRows->setSpacing(0);
    rows->addWidget(controls);
    const auto secondary = [](ui::InterfaceLabel *label) {
        label->setSecondaryInk(true);
        ui::readerStyle(label, ui::styles::font(metrics::control::captionValueText));
    };
    // A row that steps through a list: ‹ value  n of m ›.
    const auto stepper = [&](const char *name, const QString &label, ui::InterfaceLabel *&value,
                             ui::InterfaceLabel *&count, std::function<void(int)> step) {
        auto *line = ui::labeledRow(controls, controlRows, label);
        auto *previous = new ui::StepButton(ui::ControlIcon::Previous, controls);
        auto *next = new ui::StepButton(ui::ControlIcon::Next, controls);
        previous->setObjectName(QString(name) + "Previous");
        next->setObjectName(QString(name) + "Next");
        previous->setAccessibleName(strings::get("controls.decrease").arg(label.toLower()));
        next->setAccessibleName(strings::get("controls.increase").arg(label.toLower()));
        value = new ui::InterfaceLabel(controls);
        value->setObjectName(name);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ui::readerStyle(value, ui::styles::font(metrics::text::secondary));
        count = new ui::InterfaceLabel(controls);
        count->setObjectName(QString(name) + "Count");
        count->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        secondary(count);
        line->addWidget(previous, 0, Qt::AlignVCenter);
        line->addWidget(value, 1);
        line->addSpacing(ui::px(16));
        line->addWidget(count, 1);
        line->addWidget(next, 0, Qt::AlignVCenter);
        connect(previous, &QPushButton::clicked, this, [step] { step(-1); });
        connect(next, &QPushButton::clicked, this, [step] { step(1); });
    };
    if (setup_.arrange) {
        auto *hint = new ui::InterfaceLabel(
                strings::get("sleep.arrange_hint").arg(SleepGallery::label(status).toLower()),
                controls);
        hint->setObjectName("rrSleepArrangeHint");
        hint->setAlignment(Qt::AlignCenter);
        hint->setWordWrap(true);
        secondary(hint);
        controlRows->addWidget(hint);
        controlRows->addSpacing(ui::px(16));
        bookOnly_ = checklistOption(this, "rrSleepArrangeBookOnly",
                                    strings::get("sleep.arrange_book_only"),
                                    strings::get("sleep.arrange_book_only_description"), true);
        bookOnly_->setChecked(setup_.bookOnly);
        connect(bookOnly_, &QCheckBox::toggled, this,
                [this](bool checked) { setup_.bookOnly = checked; });
        rows->addWidget(bookOnly_);
    } else {
        stepper("rrSleepEditorBackdrop", strings::get("sleep.custom_background"), backdropValue_,
                backdropCount_, [this](int direction) { stepBackdrop(direction); });
        stepper("rrSleepEditorStatus", strings::get("sleep.custom_status"), statusValue_,
                statusCount_, [this](int direction) { stepStatus(direction); });
        same_ = checklistOption(this, "rrSleepEditorSame", strings::get("sleep.custom_same"),
                                strings::get("sleep.custom_same_description"), true);
        same_->setChecked(setup_.custom.same);
        connect(same_, &QCheckBox::toggled, this, [this](bool checked) {
            setup_.custom.same = checked;
            // Turning it off starts power off from the sleeping screen.
            if (!checked)
                setup_.custom.off = setup_.custom.sleep;
            powerOff_ = !checked;
            refresh();
        });
        rows->addWidget(same_);
    }
    refresh();
}

SleepEditor::~SleepEditor() = default;

sleep::Layer &SleepEditor::layer()
{
    return powerOff_ && !setup_.custom.same ? setup_.custom.off : setup_.custom.sleep;
}

const sleep::Layer &SleepEditor::layer() const
{
    return powerOff_ && !setup_.custom.same ? setup_.custom.off : setup_.custom.sleep;
}

sleep::Look SleepEditor::look() const
{
    if (setup_.arrange)
        return setup_.preset;
    const auto &current = layer();
    return { current.status, current.backdrop, sleep::tileFor(current.status) };
}

sleep::Place SleepEditor::place() const
{
    const auto status = sleep::statusDesign(look());
    const auto saved = setup_.places.value(int(status), sleep::Place::Default);
    return saved == sleep::Place::Default ? sleep::defaultPlace(status) : saved;
}

void SleepEditor::move(sleep::Place place)
{
    const auto status = sleep::statusDesign(look());
    setup_.places[int(status)] = place;
    moved_[int(status)] = place;
    preview_->update();
}

void SleepEditor::stepBackdrop(int direction)
{
    // Every wallpaper file is its own stop, after a random one.
    struct Stop
    {
        sleep::Mode mode;
        QString wallpaper;
    };

    QVector<Stop> stops;
    for (const auto mode : sleep::customBackdrops) {
        if (mode != sleep::Mode::Wallpaper) {
            stops.append({ mode, {} });
            continue;
        }
        stops.append({ mode, {} });
        for (const auto &name : wallpapers_)
            stops.append({ mode, name });
    }
    auto &current = layer();
    int index = 0;
    for (int i = 0; i < stops.size(); ++i)
        if (stops[i].mode == current.backdrop
            && (current.backdrop != sleep::Mode::Wallpaper
                || stops[i].wallpaper == current.wallpaper))
            index = i;
    const auto &next = stops[(index + direction + stops.size()) % stops.size()];
    current.backdrop = next.mode;
    current.wallpaper = next.wallpaper;
    refresh();
}

void SleepEditor::stepStatus(int direction)
{
    const int count = int(sizeof(sleep::customStatuses) / sizeof(sleep::customStatuses[0]));
    auto &current = layer();
    int index = 0;
    for (int i = 0; i < count; ++i)
        if (sleep::customStatuses[i] == current.status)
            index = i;
    current.status = sleep::customStatuses[(index + direction + count) % count];
    refresh();
}

QImage SleepEditor::wallpaper()
{
    const auto &current = layer();
    if (current.backdrop != sleep::Mode::Wallpaper || setup_.arrange)
        return {};
    if (const auto *image = wallpaperImages_.object(current.wallpaper))
        return *image;
    if (!wallpaperRequests_.contains(current.wallpaper) && wallpaperWanted) {
        wallpaperRequests_.insert(current.wallpaper);
        wallpaperWanted(current.wallpaper);
    }
    return {};
}

void SleepEditor::refresh()
{
    if (!setup_.arrange) {
        stateRow_->setVisible(!setup_.custom.same);
        sleepingTab_->setChecked(!powerOff_);
        poweredOffTab_->setChecked(powerOff_);
        const auto &current = layer();
        // Count the stops as stepBackdrop() lays them out.
        int stops = 0, index = 0;
        for (const auto mode : sleep::customBackdrops) {
            if (mode == current.backdrop && mode != sleep::Mode::Wallpaper)
                index = stops;
            ++stops;
            if (mode != sleep::Mode::Wallpaper)
                continue;
            if (current.backdrop == mode && current.wallpaper.isEmpty())
                index = stops - 1;
            for (const auto &name : wallpapers_) {
                if (current.backdrop == mode && current.wallpaper == name)
                    index = stops;
                ++stops;
            }
        }
        QString backdrop = backdropLabel(current.backdrop);
        if (current.backdrop == sleep::Mode::Wallpaper)
            backdrop = current.wallpaper.isEmpty() ? strings::get("sleep.wallpaper_random")
                                                   : current.wallpaper;
        backdropValue_->setText(ui::elideInterfaceText(
                backdrop, ui::interfaceFont(metrics::text::secondary), ui::px(360)));
        backdropCount_->setText(strings::get("sleep.step_count").arg(index + 1).arg(stops));
        const int statuses = int(sizeof(sleep::customStatuses) / sizeof(sleep::customStatuses[0]));
        int status = 0;
        for (int i = 0; i < statuses; ++i)
            if (sleep::customStatuses[i] == current.status)
                status = i;
        statusValue_->setText(statusLabel(current.status));
        statusCount_->setText(strings::get("sleep.step_count").arg(status + 1).arg(statuses));
    }
    preview_->update();
}

void SleepEditor::setImages(const QImage &cover, const QImage &page, const QImage &overlay)
{
    cover_ = cover;
    page_ = page;
    overlay_ = overlay;
    preview_->update();
}

void SleepEditor::setCover(const QImage &cover)
{
    cover_ = cover;
    preview_->update();
}

void SleepEditor::setWallpapers(const QStringList &names)
{
    if (names == wallpapers_)
        return;
    wallpapers_ = names;
    refresh();
}

void SleepEditor::setWallpaper(const QString &name, const QImage &image)
{
    wallpaperRequests_.remove(name);
    wallpaperImages_.insert(name, new QImage(image));
    preview_->update();
}
} // namespace kobalt::reader
