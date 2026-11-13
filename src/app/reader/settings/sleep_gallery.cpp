#include "sleep_gallery.h"
#include "settings_panel.h"

#include "app/reader/views/sleep_screen.h"
#include "support/strings.h"
#include "ui/controls/buttons.h"
#include "ui/controls/paging.h"
#include "ui/dialogs/modals.h"
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
#include <QTimer>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace chrome = ui::metrics::chrome;

namespace {
constexpr int perPage = 4;
} // namespace

// The four cells of one page. Painted rather than built from widgets, so a
// page turn redraws one surface.
class SleepGalleryGrid final : public QWidget
{
public:
    explicit SleepGalleryGrid(SleepGallery *gallery) : QWidget(gallery), gallery(gallery)
    {
        setObjectName("rrSleepGalleryGrid");
        setAttribute(Qt::WA_OpaquePaintEvent);
        hold.setSingleShot(true);
        hold.setInterval(600);
        connect(&hold, &QTimer::timeout, this, [this] {
            // A long press on an entry that has options opens them instead of choosing.
            auto *owner = this->gallery;
            if (pressed < 0 || !owner->customize
                || owner->entries_[pressed].design != sleep::Design::Night)
                return;
            held = true;
            owner->customize(owner->entries_[pressed].design);
        });
    }

    QRect cell(int slot) const
    {
        const int half = width() / 2, middle = height() / 2;
        const int column = slot % 2, row = slot / 2;
        return { column * half, row * middle, column ? width() - half : half,
                 row ? height() - middle : middle };
    }

    // The preview's place inside a cell, keeping the screen's shape.
    QRect previewArea(int slot) const
    {
        const auto area = cell(slot);
        const int margin = ui::px(28), label = labelHeight();
        const QSize screen = gallery->screen_;
        const QSize room(area.width() - 2 * margin, area.height() - 2 * margin - label);
        const QSize fitted = screen.scaled(room, Qt::KeepAspectRatio);
        return { area.left() + (area.width() - fitted.width()) / 2, area.top() + margin,
                 fitted.width(), fitted.height() };
    }

    int labelHeight() const
    {
        return qMax<int>(int(ui::metrics::settings::checkboxSize),
                         ui::interfaceLineHeight(ui::interfaceFont(ui::metrics::text::choice)))
                + ui::px(16);
    }

    int entryAt(QPoint point) const
    {
        for (int slot = 0; slot < perPage; ++slot)
            if (cell(slot).contains(point)) {
                const int index = gallery->pageIndex_ * perPage + slot;
                return index < gallery->entries_.size() ? index : -1;
            }
        return -1;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto colors = ui::themeColors(this);
        const int rule = int(ui::metrics::control::border);
        painter.fillRect(rect(), colors.paper);
        const auto divider = ui::dividerColor(this);
        painter.fillRect(QRect(width() / 2, 0, rule, height()), divider);
        painter.fillRect(QRect(0, height() / 2, width(), rule), divider);
        painter.fillRect(QRect(0, height() - rule, width(), rule), divider);
        for (int slot = 0; slot < perPage; ++slot) {
            const int index = gallery->pageIndex_ * perPage + slot;
            if (index >= gallery->entries_.size())
                break;
            paintEntry(painter, slot, gallery->entries_[index], colors);
        }
    }

    // Accept the press so the release, which chooses, comes here too.
    void mousePressEvent(QMouseEvent *event) override
    {
        event->accept();
        pressed = entryAt(event->pos());
        held = false;
        hold.start();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        hold.stop();
        const int index = entryAt(event->pos());
        const bool wasHeld = held;
        held = false;
        pressed = -1;
        if (index < 0 || wasHeld)
            return;
        const auto entry = gallery->entries_[index];
        if (!entry.enabled) {
            if (gallery->unavailable)
                gallery->unavailable(entry.design);
            return;
        }
        gallery->current_ = entry.design;
        update();
        if (gallery->chosen)
            gallery->chosen(entry.design);
    }

private:
    void paintEntry(QPainter &painter, int slot, const SleepGallery::Entry &entry,
                    const ui::ThemeColors &colors)
    {
        const auto area = previewArea(slot);
        const bool selected = entry.design == gallery->current_;
        if (!entry.enabled)
            painter.setOpacity(.45);
        painter.drawImage(area.topLeft(), gallery->preview(entry, area.size()));
        painter.setRenderHint(QPainter::Antialiasing, false);
        const int gap = ui::px(8);
        if (selected) {
            const int rule = ui::px(8);
            const QRect outer = area.adjusted(-gap - rule, -gap - rule, gap + rule, gap + rule);
            painter.fillRect(QRect(outer.topLeft(), QSize(outer.width(), rule)), colors.ink);
            painter.fillRect(QRect(outer.left(), outer.bottom() - rule + 1, outer.width(), rule),
                             colors.ink);
            painter.fillRect(QRect(outer.topLeft(), QSize(rule, outer.height())), colors.ink);
            painter.fillRect(QRect(outer.right() - rule + 1, outer.top(), rule, outer.height()),
                             colors.ink);
        } else {
            painter.setPen(QPen(ui::dividerColor(this), int(ui::metrics::control::border)));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(area.adjusted(-gap, -gap, gap - 1, gap - 1));
        }
        // Centre the radio dot and name under the preview. Labels share a
        // baseline so descenders do not lift a name above its neighbours.
        const QFont name = ui::interfaceFont(ui::metrics::text::secondary);
        const QString label = SleepGallery::label(entry.design);
        const int dot = int(ui::metrics::settings::checkboxSize);
        const int spacing = ui::px(16);
        const auto cellArea = cell(slot);
        const int available = cellArea.width() - 2 * ui::px(12) - dot - spacing;
        const int nameWidth = qMin(ui::interfaceTextWidth(label, name), available);
        const int top = area.bottom() + gap + ui::px(20);
        const int lineHeight = labelHeight() - ui::px(16);
        int x = cellArea.left() + (cellArea.width() - dot - spacing - nameWidth) / 2;
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF track(x + 1, top + (lineHeight - dot) / 2.0 + 1, dot - 2, dot - 2);
        painter.setPen(QPen(colors.ink, ui::px(2)));
        painter.setBrush(colors.paper);
        painter.drawEllipse(track);
        if (selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.ink);
            const qreal inset = track.width() / 4;
            painter.drawEllipse(track.adjusted(inset, inset, -inset, -inset));
        }
        x += dot + spacing;
        painter.setPen(colors.ink);
        painter.setFont(name);
        ui::drawSegmentPrivateText(painter, QRect(x, top, nameWidth + 2, lineHeight),
                                   ui::elideInterfaceText(label, name, nameWidth + 2),
                                   Qt::AlignLeft);
        painter.setOpacity(1);
    }

    SleepGallery *gallery;
    QTimer hold;
    int pressed = -1;
    bool held = false;
};

SleepGallery::SleepGallery(QWidget *parent, QVector<Entry> entries, sleep::Design current,
                           const sleep::Snapshot &sample, QSize screen, sleep::NativePreview native,
                           bool followReading)
    : ui::Panel(parent),
      entries_(std::move(entries)),
      current_(current),
      sample_(sample),
      native_(native),
      screen_(followReading ? screen
                            : QSize(qMin(screen.width(), screen.height()),
                                    qMax(screen.width(), screen.height()))),
      readingScreen_(screen)
{
    setObjectName("rrSleepGallery");
    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);
    // The header matches Settings: a title in the brand face, a heavy rule
    // under it, and a close cell, here on the right.
    auto *heading = new ui::Panel(this, Qt::BottomEdge);
    heading->setObjectName("rrSleepGalleryHeader");
    heading->setEdgeWidth(chrome::heavyRule);
    heading->setFixedHeight(chrome::actionHeight);
    auto *headingRows = new QHBoxLayout(heading);
    headingRows->setContentsMargins(0, 0, 0, chrome::heavyRule);
    headingRows->setSpacing(0);
    title_ = new SettingsTitle(strings::get("sleep.gallery_title"), heading);
    title_->setObjectName("rrSleepGalleryTitle");
    title_->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    ui::readerStyle(title_, ui::styles::brand(chrome::titleText) + "background:transparent;");
    headingRows->addWidget(title_, 1);
    followOrientation_ = new ui::CheckCellButton(ui::ControlIcon::DeviceRotate, heading);
    followOrientation_->setChecked(followReading);
    followOrientation_->setObjectName("rrSleepFollowOrientation");
    followOrientation_->setAccessibleName(strings::get("sleep.follow_orientation"));
    followOrientation_->setRule(Qt::LeftEdge);
    followOrientation_->setFixedWidth(chrome::cellWidth);
    headingRows->addWidget(followOrientation_);
    orientationNotice_ = new ui::ButtonFeedback(this);
    orientationNotice_->setObjectName("rrSleepOrientationNotice");
    connect(followOrientation_, &QPushButton::clicked, this, [this](bool checked) {
        if (followOrientationChanged && !followOrientationChanged(checked)) {
            followOrientation_->setChecked(!checked);
            return;
        }
        screen_ = checked ? readingScreen_
                          : QSize(qMin(readingScreen_.width(), readingScreen_.height()),
                                  qMax(readingScreen_.width(), readingScreen_.height()));
        previews_.clear();
        grid_->update();
        orientationNotice_->showFor(
                followOrientation_,
                strings::get(checked ? "sleep.rotation_enabled" : "sleep.rotation_disabled"),
                { ui::ButtonFeedback::AnchorSide::Below, {}, {} });
    });
    close_ = new ui::CellButton(ui::ControlIcon::Close, heading);
    close_->setObjectName("rrSleepGalleryClose");
    close_->setAccessibleName(strings::get("settings.close"));
    close_->setRule(Qt::LeftEdge);
    close_->setFixedWidth(chrome::cellWidth);
    headingRows->addWidget(close_);
    connect(close_, &QPushButton::clicked, this, [this] {
        if (closed)
            closed();
    });
    rows->addWidget(heading);
    grid_ = new SleepGalleryGrid(this);
    rows->addWidget(grid_, 1);
    pager_ = new ui::PagerBar(this, "rrSleepGallery");
    rows->addWidget(pager_);
    connect(pager_->previous, &QPushButton::clicked, this,
            [this] { showPage(pager_->turnTarget(-1)); });
    connect(pager_->next, &QPushButton::clicked, this, [this] { showPage(pager_->turnTarget(1)); });
    int start = 0;
    for (int index = 0; index < entries_.size(); ++index)
        if (entries_[index].design == current_)
            start = index / perPage;
    showPage(start);
}

SleepGallery::~SleepGallery() = default;

QString SleepGallery::label(sleep::Design design)
{
    return strings::get(
            QStringLiteral("sleep.design_%1").arg(sleep::value(design)).toLatin1().constData());
}

int SleepGallery::pageCount() const
{
    return qMax(1, int((entries_.size() + perPage - 1) / perPage));
}

void SleepGallery::showPage(int page)
{
    pageIndex_ = qBound(0, page, pageCount() - 1);
    pager_->setPage(pageIndex_, pageCount());
    grid_->update();
}

void SleepGallery::setCover(const QImage &cover)
{
    cover_ = cover;
    previews_.clear();
    grid_->update();
}

void SleepGallery::setPage(const QImage &page)
{
    pageImage_ = page;
    previews_.remove(int(sleep::Design::Page));
    previews_.remove(int(sleep::Design::Overlay));
    grid_->update();
}

void SleepGallery::setSample(const sleep::Snapshot &sample)
{
    sample_ = sample;
    previews_.clear();
    grid_->update();
}

void SleepGallery::setOverlay(const QImage &overlay)
{
    overlay_ = overlay;
    previews_.remove(int(sleep::Design::Overlay));
    grid_->update();
}

void SleepGallery::setEnabled(sleep::Design design, bool enabled)
{
    for (auto &entry : entries_)
        if (entry.design == design)
            entry.enabled = enabled;
    grid_->update();
}

void SleepGallery::resizeEvent(QResizeEvent *event)
{
    previews_.clear();
    ui::Panel::resizeEvent(event);
}

QImage SleepGallery::preview(const Entry &entry, QSize size)
{
    const int key = int(entry.design);
    auto found = previews_.find(key);
    if (found != previews_.end() && found->size() == size)
        return *found;
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(ui::themeColors(this).paper);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (entry.design == sleep::Design::Stock && !sample_.privateBook) {
        // Kobo owns the real screen; these proportions represent its current cover options.
        painter.fillRect(image.rect(), Qt::white);
        painter.setPen(Qt::black);
        // Kobalt's interface font stands in for Kobo's. The bundled faces are
        // private on Kobo, so a family name alone would draw a fallback font.
        const auto font = [&](qreal share) {
            painter.setFont(ui::interfaceFont(qMax(1, qRound(size.width() * share))));
        };
        font(0.05);
        const auto draw = [&](const QRectF &area, Qt::Alignment alignment, const QString &value) {
            ui::drawSegmentPrivateText(painter, area.toAlignedRect(), value, alignment);
        };
        const QString status = sleepStatusText(SleepStatus::Sleeping);
        const bool showCover = native_.showCover && !cover_.isNull();
        if (showCover && native_.fullScreen) {
            painter.fillRect(image.rect(), Qt::black);
            painter.drawImage(sleep::coverArea(cover_.size(), size), cover_);
            if (native_.showInfo) {
                const QRectF panel(0, size.height() * 0.635, size.width() * 0.44,
                                   size.height() * 0.153);
                painter.fillRect(panel, Qt::white);
                painter.setPen(QPen(Qt::black, qMax<qreal>(0.5, size.width() / 1072.0)));
                const qreal inset = size.width() * 0.02;
                painter.drawRect(panel.adjusted(-1, inset, -inset, -inset));
                draw(panel.adjusted(size.width() * 0.06, inset, -inset, -panel.height() * 0.45),
                     Qt::AlignLeft | Qt::AlignVCenter, status);
                font(0.032);
                draw(panel.adjusted(size.width() * 0.06, panel.height() * 0.55, -inset, -inset),
                     Qt::AlignLeft | Qt::AlignVCenter, sample_.progress);
            }
        } else if (showCover) {
            draw(QRectF(0, size.height() * 0.025, size.width(), size.height() * 0.06),
                 Qt::AlignCenter, status);
            const QRect room(qRound(size.width() * 0.12), qRound(size.height() * 0.12),
                             qRound(size.width() * 0.76), qRound(size.height() * 0.79));
            const QSize fitted = cover_.size().scaled(room.size(), Qt::KeepAspectRatio);
            painter.drawImage(
                    QRect(room.center() - QPoint(fitted.width() / 2, fitted.height() / 2), fitted),
                    cover_);
            font(0.032);
            draw(QRectF(0, size.height() * 0.94, size.width(), size.height() * 0.04),
                 Qt::AlignCenter, sample_.progress);
        } else {
            draw(QRectF(size.width() * 0.11, size.height() * 0.82, size.width() * 0.78,
                        size.height() * 0.07),
                 Qt::AlignLeft | Qt::AlignVCenter, status);
        }
    } else {
        auto shot = sample_;
        shot.design = entry.design;
        shot.mode = sleep::backdrop(entry.design);
        if (!sleep::tiled(entry.design))
            shot.tile = sleep::Tile::None;
        else if (entry.design == sleep::Design::Bookmark)
            shot.tile = sleep::Tile::Bookmark;
        else if (entry.design == sleep::Design::Card)
            shot.tile = sleep::Tile::Card;
        else if (entry.design == sleep::Design::Cover)
            shot.tile = sleep::Tile::None;
        paintSleepPreview(painter, QRectF(QPointF(), QSizeF(size)), screen_, cover_, pageImage_,
                          overlay_, shot);
    }
    painter.end();
    previews_.insert(key, image);
    return image;
}
} // namespace kobalt::reader
