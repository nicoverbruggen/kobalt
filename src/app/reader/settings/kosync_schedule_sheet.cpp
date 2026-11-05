#include "kosync_schedule_sheet.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/options.h"
#include "ui/controls/paging.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include <QEvent>
#include <QAbstractButton>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>
#include <memory>

namespace kobalt::reader {
namespace metrics = ui::metrics;
namespace chrome = metrics::chrome;
using ui::px;

// A Wi-Fi choice drawn like a header and footer preset: a framed picture with
// its name below. A selected tile has a heavy frame and an inverted name.
class KosyncWifiTile final : public ui::Button
{
public:
    KosyncWifiTile(const char *name, QVector<ui::ControlIcon> icons, const QString &picture,
                   const QString &label, const QString &outcome, QWidget *parent)
        : ui::Button(parent),
          glyphs(std::move(icons)),
          pictureText(picture),
          labelText(label),
          outcomeText(outcome)
    {
        setObjectName(name);
        setCheckable(true);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(label);
        setAccessibleDescription(outcome);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    // Paired tiles share their picture and outcome heights, so both are
    // the same height whatever their text wraps to.
    void pairWith(KosyncWifiTile *other) { partner = other; }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        return pictureHeight(width) + int(metrics::headerFooter::nameGap)
                + int(metrics::headerFooter::nameHeight) + px(6) + outcomeHeight(width);
    }

    QSize sizeHint() const override { return { px(300), heightForWidth(px(300)) }; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto &colors = ui::themeColors(this);
        const bool enabled = isEnabled();
        const bool selected = isChecked() || (enabled && isDown());
        const auto ink = enabled ? colors.ink : colors.disabledInk;
        painter.fillRect(rect(), colors.paper);
        const QRect frame(0, 0, width(), pictureHeight(width()));
        const int edge = selected ? int(metrics::headerFooter::selectedBorder)
                                  : int(metrics::control::border);
        ui::paintEdges(painter, frame,
                       ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                       selected && enabled ? ink : colors.divider, edge);
        const int icon = chrome::cellIcon, arrow = metrics::choices::fontArrowSize, gap = px(10);
        int iconsWidth = 0;
        for (int index = 0; index < glyphs.size(); ++index)
            iconsWidth += icon + (index ? arrow + 2 * gap : 0);
        int x = (width() - iconsWidth) / 2;
        const int iconTop = frame.y() + px(28);
        for (int index = 0; index < glyphs.size(); ++index) {
            if (index) {
                ui::controlIcon(ui::ControlIcon::Next,
                                enabled ? colors.secondaryInk : colors.disabledInk)
                        .paint(&painter,
                               QRect(x + gap, iconTop + (icon - arrow) / 2, arrow, arrow));
                x += arrow + 2 * gap;
            }
            // Wi-Fi glyphs are status icons and paint through their own helper.
            if (glyphs[index] >= ui::ControlIcon::WifiNone
                && glyphs[index] <= ui::ControlIcon::WifiDisconnected)
                ui::paintWifiStatusIcon(painter, glyphs[index], QRectF(x, iconTop, icon, icon),
                                        ink);
            else
                ui::paintOpeningIcon(painter, glyphs[index], QRectF(x, iconTop, icon, icon), ink);
            x += icon;
        }
        painter.setPen(ink);
        painter.setFont(ui::interfaceFont(chrome::descriptionText));
        painter.drawText(frame.adjusted(px(16), iconTop + icon + px(14), -px(16), -px(12)),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, pictureText);
        const QFont label = ui::interfaceFont(metrics::text::secondary);
        painter.setFont(label);
        const int padding = metrics::headerFooter::namePadding;
        const int textWidth = qMin(ui::interfaceTextWidth(labelText, label), width() - 2 * padding);
        const QRect block((width() - textWidth - 2 * padding) / 2,
                          frame.bottom() + 1 + metrics::headerFooter::nameGap,
                          textWidth + 2 * padding, metrics::headerFooter::nameHeight);
        if (selected)
            painter.fillRect(block, ink);
        painter.setPen(selected ? colors.paper : ink);
        ui::drawInterfaceText(painter, block.adjusted(padding, 0, -padding, 0), labelText,
                              Qt::AlignCenter);
        painter.setPen(enabled ? colors.secondaryInk : colors.disabledInk);
        painter.setFont(ui::interfaceFont(metrics::control::captionValueText));
        painter.drawText(QRect(0, block.bottom() + 1 + px(6), width(), outcomeHeight(width())),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, outcomeText);
    }

private:
    int ownPictureHeight(int width) const
    {
        const QFontMetrics metrics(ui::interfaceFont(chrome::descriptionText));
        const auto text = metrics.boundingRect(QRect(0, 0, qMax(1, width - px(32)), 10000),
                                               Qt::AlignHCenter | Qt::TextWordWrap, pictureText);
        return px(28) + chrome::cellIcon + px(14) + text.height() + px(24);
    }

    int ownOutcomeHeight(int width) const
    {
        const QFontMetrics metrics(ui::interfaceFont(metrics::control::captionValueText));
        return metrics
                .boundingRect(QRect(0, 0, qMax(1, width), 10000),
                              Qt::AlignHCenter | Qt::TextWordWrap, outcomeText)
                .height();
    }

    int pictureHeight(int width) const
    {
        return qMax(ownPictureHeight(width), partner ? partner->ownPictureHeight(width) : 0);
    }

    int outcomeHeight(int width) const
    {
        return qMax(ownOutcomeHeight(width), partner ? partner->ownOutcomeHeight(width) : 0);
    }

    KosyncWifiTile *partner = nullptr;
    QVector<ui::ControlIcon> glyphs;
    QString pictureText, labelText, outcomeText;
};

// The square checkbox of the Settings rows, for a moment that can be off.
// Not a push button, so the theme's button heights cannot stretch it.
class MomentCheck final : public QAbstractButton
{
public:
    MomentCheck(const char *name, const QString &label, QWidget *parent) : QAbstractButton(parent)
    {
        setObjectName(name);
        setCheckable(true);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(label);
        setFixedSize(chrome::checkSize, chrome::checkSize);
    }

    QSize sizeHint() const override { return { chrome::checkSize, chrome::checkSize }; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto &colors = ui::themeColors(this);
        const auto ink = isEnabled() ? colors.ink : colors.disabledInk;
        painter.fillRect(rect(), colors.paper);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setPen(QPen(ink, px(2)));
        painter.setBrush(isChecked() ? ink : colors.paper);
        painter.drawRect(box);
        if (!isChecked())
            return;
        // The same tick as DescribedOption's square, drawn for a 48 pixel box.
        painter.setPen(QPen(colors.paper, px(4), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        QPolygonF check;
        check << QPointF(px(11), px(24)) << QPointF(px(21), px(34)) << QPointF(px(37), px(14));
        painter.drawPolyline(check);
    }
};

namespace {
// The icon of a moment in the list. It follows the row's enabled state.
class MomentIcon final : public QWidget
{
public:
    MomentIcon(ui::ControlIcon icon, QWidget *parent) : QWidget(parent), glyph(icon)
    {
        setFixedSize(chrome::cellIcon, chrome::cellIcon);
    }

protected:
    void changeEvent(QEvent *event) override
    {
        if (event->type() == QEvent::EnabledChange)
            update();
        QWidget::changeEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto &colors = ui::themeColors(this);
        ui::paintOpeningIcon(painter, glyph, rect(), isEnabled() ? colors.ink : colors.disabledInk);
    }

private:
    ui::ControlIcon glyph;
};

// One moment in the list. Its height comes from its own text, measured at the
// width it is given, so every row has the same padding around its text. A
// row with a checkbox toggles it wherever the row is tapped.
class MomentRow final : public QWidget
{
public:
    using QWidget::QWidget;
    QLabel *title = nullptr, *description = nullptr;
    QWidget *trailing = nullptr;
    MomentCheck *check = nullptr;
    bool stacked = false;

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        const auto margins = layout()->contentsMargins();
        int room =
                width - margins.left() - margins.right() - chrome::cellIcon - layout()->spacing();
        if (trailing && !stacked)
            room -= trailing->width() + layout()->spacing();
        const auto height = [room](QLabel *label) {
            label->ensurePolished();
            return label->heightForWidth(qMax(1, room));
        };
        int text = height(title) + px(2) + height(description);
        if (trailing && stacked)
            text += px(10) + trailing->sizeHint().height();
        const int content = qMax(qMax(text, int(chrome::cellIcon)),
                                 trailing && !stacked ? trailing->sizeHint().height() : 0);
        return margins.top() + content + margins.bottom();
    }

    QSize sizeHint() const override { return { width(), heightForWidth(qMax(1, width())) }; }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (check && isEnabled() && rect().contains(event->pos()))
            check->click();
        QWidget::mouseReleaseEvent(event);
    }
};

// Gray when the sheet's details are disabled, as disabled buttons are.
QLabel *sheetLabel(QWidget *parent, const QString &text, const char *name, int size, bool secondary)
{
    auto *label = new ui::InterfaceLabel(text, parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    ui::readerStyle(label,
                    QString("QLabel{color:%1;%2} QLabel:disabled{color:palette(mid);}")
                            .arg(secondary ? "palette(placeholder-text)" : "palette(window-text)",
                                 ui::styles::font(size)));
    return label;
}

QWidget *padded(QWidget *parent, QVBoxLayout *rows, const QMargins &margins, QVBoxLayout **inner)
{
    auto *area = new QWidget(parent);
    auto *areaRows = new QVBoxLayout(area);
    areaRows->setContentsMargins(margins);
    areaRows->setSpacing(px(6));
    rows->addWidget(area);
    *inner = areaRows;
    return area;
}

int sheetWidth(const QWidget *parent)
{
    return qMax(1, qMin(int(px(1264)), parent->width() - 2 * int(metrics::notice::sideClearance)));
}
} // namespace

QString KosyncScheduleSheet::intervalKey(int minutes)
{
    return minutes == kosync::neverWhileReading ? QStringLiteral("never")
                                                : QString::number(minutes);
}

QString KosyncScheduleSheet::intervalLabel(int minutes)
{
    return strings::get(("kosync.interval_" + intervalKey(minutes)).toLatin1().constData());
}

KosyncScheduleSheet::KosyncScheduleSheet(QWidget *parent, bool automatic,
                                         const kosync::Schedule &schedule, Features features)
    : ui::ModalOverlay(parent), automatic_(automatic), schedule_(schedule)
{
    setObjectName("rrKosyncAutomaticPopup");
    setDimmed(true);
    setGeometry(parent->rect());
    dismissed = [this] {
        if (closed)
            closed();
    };
    auto *card = this->card();
    auto *rows = card->rows();
    rows->setContentsMargins(metrics::control::border, metrics::control::border,
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);

    // The same header as the styling sheets: a bold title and a close cell.
    auto *header = new ui::Panel(card, Qt::BottomEdge);
    header->setFixedHeight(chrome::actionHeight);
    auto *headerRow = new QHBoxLayout(header);
    // Children stop above the header's bottom rule, so the close cell's
    // divider meets it.
    headerRow->setContentsMargins(0, 0, 0, metrics::control::border);
    headerRow->setSpacing(0);
    auto *title = new ui::InterfaceLabel(strings::get("kosync.automatic"), header);
    title->setObjectName("rrKosyncAutomaticTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    ui::readerStyle(title, ui::styles::font(chrome::labelText, true));
    headerRow->addWidget(title, 1);
    auto *close = new ui::CellButton(ui::ControlIcon::Close, header);
    close->setObjectName("rrKosyncAutomaticClose");
    close->setAccessibleName(strings::get("settings.close"));
    close->setRule(Qt::LeftEdge);
    close->setFixedSize(chrome::cellWidth, chrome::actionHeight - metrics::control::border);
    headerRow->addWidget(close);
    connect(close, &QPushButton::clicked, this, [this] {
        if (closed)
            closed();
    });
    rows->addWidget(header);

    // Every block below the header is its own card row, so a short screen
    // can page between them.
    QVBoxLayout *modeRows = nullptr;
    auto *mode = padded(card, rows, { 0, px(24), 0, 0 }, &modeRows);
    mode->setObjectName("rrKosyncAutomaticModeRow");
    // The segmented row keeps the sheet's side inset; the rule below spans the card.
    auto *choiceArea = new QWidget(mode);
    auto *choiceRows = new QVBoxLayout(choiceArea);
    choiceRows->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    choiceRows->setSpacing(0);
    modeRows->addWidget(choiceArea);
    ui::SegmentedRow modeChoice(choiceArea, choiceRows, "rrKosyncAutomaticMode",
                                strings::get("kosync.automatic"));
    off_ = modeChoice.add("rrKosyncAutomaticOff", strings::get("common.off"));
    on_ = modeChoice.add("rrKosyncAutomaticOn", strings::get("common.on"));
    connect(off_, &QPushButton::clicked, this, [this] {
        if (!automatic_)
            return;
        automatic_ = false;
        present();
        if (automaticChanged)
            automaticChanged(false);
    });
    connect(on_, &QPushButton::clicked, this, [this] {
        if (automatic_)
            return;
        automatic_ = true;
        present();
        if (automaticChanged)
            automaticChanged(true);
    });
    auto *modeRule = new ui::Panel(mode);
    modeRule->setDividers(Qt::BottomEdge);
    modeRule->setFixedHeight(metrics::control::border);
    modeRows->addSpacing(px(24));
    modeRows->addWidget(modeRule);

    QVBoxLayout *headingRows = nullptr;
    details_.append(
            padded(card, rows, { chrome::inset, px(32), chrome::inset, px(8) }, &headingRows));
    headingRows->addWidget(sheetLabel(details_.last(), strings::get("kosync.when_heading"),
                                      "rrKosyncWhenHeading", chrome::labelText, false));
    // Each moment: an icon, what happens, and for reading its interval.
    const auto moment = [&](ui::ControlIcon icon, const char *name, const char *titleKey,
                            const char *descriptionKey, QWidget *trailing) {
        auto *row = new MomentRow(card);
        row->setObjectName(name);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(chrome::inset, px(16), chrome::inset, px(16));
        line->setSpacing(px(20));
        line->addWidget(new MomentIcon(icon, row), 0, Qt::AlignTop);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(px(2));
        row->title = sheetLabel(row, strings::get(titleKey), "", metrics::text::secondary, false);
        row->description = sheetLabel(row, strings::get(descriptionKey), "",
                                      metrics::control::captionValueText, true);
        texts->addWidget(row->title);
        texts->addWidget(row->description);
        // Extra row height, from a taller drop-down, goes below the text.
        texts->addStretch(1);
        line->addLayout(texts, 1);
        // A narrow phone card keeps the drop-down under its text, so the
        // moment's title does not wrap word by word.
        if (trailing) {
            trailing->setParent(row);
            row->trailing = trailing;
            row->stacked = trailing == interval_ && sheetWidth(parent) < px(900);
            if (row->stacked) {
                texts->addSpacing(px(10));
                texts->addWidget(trailing, 0, Qt::AlignLeft);
            } else {
                line->addWidget(trailing, 0, Qt::AlignVCenter);
            }
        }
        rows->addWidget(row);
        details_.append(row);
        return row;
    };
    // A moment with its own switch: tapping the row toggles it.
    const auto switchable = [&](ui::ControlIcon icon, const char *name, const char *titleKey,
                                const char *descriptionKey, bool kosync::Schedule::*field) {
        auto *check = new MomentCheck(name, strings::get(titleKey), card);
        auto *row = moment(icon, QByteArray(name).append("Row").constData(), titleKey,
                           descriptionKey, check);
        row->check = check;
        connect(check, &QAbstractButton::clicked, this, [this, field](bool checked) {
            if (schedule_.*field == checked)
                return;
            schedule_.*field = checked;
            if (scheduleChanged)
                scheduleChanged(schedule_);
        });
        checks_.append({ check, field });
    };
    // While you read comes first: its interval is the choice on this list.
    interval_ = new ui::DropdownButton(card);
    interval_->setObjectName("rrKosyncInterval");
    interval_->setFocusPolicy(Qt::NoFocus);
    const int dropdownHeight = metrics::control::segmentHeight
            + 2 * int(metrics::control::segmentVerticalPadding) + 2 * int(metrics::control::border);
    ui::readerStyle(interval_,
                    QString("min-height:%1px;max-height:%1px;")
                                    .arg(dropdownHeight - 2 * metrics::control::padding)
                            + ui::styles::font(metrics::text::secondary));
    int intervalWidth = 0;
    for (const int minutes : kosync::scheduleMinutes())
        intervalWidth = qMax(intervalWidth,
                             ui::interfaceTextWidth(intervalLabel(minutes),
                                                    ui::interfaceFont(metrics::text::secondary)));
    // DropdownButton keeps fontButtonWidthReserve for its padding and arrow.
    interval_->setFixedWidth(intervalWidth + int(metrics::choices::fontButtonWidthReserve) + px(8));
    connect(interval_, &QPushButton::clicked, this, [this] { showIntervals(); });
    moment(ui::ControlIcon::Clock, "rrKosyncWhenReading", "kosync.when_reading",
           "kosync.when_reading_description", interval_);
    switchable(ui::ControlIcon::Dictionary, "rrKosyncOnOpen", "kosync.when_open",
               "kosync.when_open_description", &kosync::Schedule::onOpen);
    // Only a device with a sleep and wake lifecycle offers this moment.
    if (features.sleepWake)
        switchable(ui::ControlIcon::Moon, "rrKosyncOnSleepWake", "kosync.when_sleep_wake",
                   "kosync.when_sleep_wake_description", &kosync::Schedule::onSleepWake);
    switchable(ui::ControlIcon::Door, "rrKosyncOnClose", "kosync.when_close",
               "kosync.when_close_description", &kosync::Schedule::onClose);
    // The gap before the Wi-Fi divider belongs to the list, so a page that
    // starts with the Wi-Fi section has its divider right under the header.
    details_.last()->layout()->setContentsMargins(chrome::inset, px(16), chrome::inset,
                                                  features.wifi ? px(40) : px(32));

    // Only a host that can turn on Wi-Fi for a sync offers the Wi-Fi choice.
    if (features.wifi) {
        wifi_ = new QWidget(card);
        wifi_->setObjectName("rrKosyncWifiRow");
        auto *wifiBlock = new QVBoxLayout(wifi_);
        wifiBlock->setContentsMargins(0, 0, 0, 0);
        wifiBlock->setSpacing(0);
        rows->addWidget(wifi_);
        details_.append(wifi_);
        auto *divider = new ui::Panel(wifi_);
        divider->setDividers(Qt::TopEdge);
        divider->setFixedHeight(metrics::control::border);
        wifiBlock->addWidget(divider);
        QVBoxLayout *wifiRows = nullptr;
        padded(wifi_, wifiBlock, { chrome::inset, px(32), chrome::inset, px(36) }, &wifiRows);
        wifiRows->setSpacing(px(20));
        wifiRows->addWidget(sheetLabel(wifi_, strings::get("kosync.wifi_heading"),
                                       "rrKosyncWifiHeading", chrome::labelText, false));
        auto *tiles = new QHBoxLayout;
        tiles->setSpacing(px(32));
        wait_ = new KosyncWifiTile(
                "rrKosyncWifiWait", { ui::ControlIcon::WifiDisconnected, ui::ControlIcon::Clock },
                strings::get("kosync.wifi_wait_picture"), strings::get("kosync.wifi_wait"),
                strings::get("kosync.wifi_wait_outcome"), wifi_);
        wake_ = new KosyncWifiTile("rrKosyncWifiWake",
                                   { ui::ControlIcon::WifiHigh, ui::ControlIcon::Sync,
                                     ui::ControlIcon::WifiDisconnected },
                                   strings::get("kosync.wifi_wake_picture"),
                                   strings::get("kosync.wifi_wake"),
                                   strings::get("kosync.wifi_wake_outcome"), wifi_);
        wait_->pairWith(wake_);
        wake_->pairWith(wait_);
        tiles->addWidget(wait_, 1, Qt::AlignTop);
        tiles->addWidget(wake_, 1, Qt::AlignTop);
        wifiRows->addLayout(tiles);
        const auto chooseWifi = [this](bool wake) {
            wait_->setChecked(!wake);
            wake_->setChecked(wake);
            if (schedule_.wakeWifi == wake)
                return;
            schedule_.wakeWifi = wake;
            if (scheduleChanged)
                scheduleChanged(schedule_);
        };
        connect(wait_, &QPushButton::clicked, this, [chooseWifi] { chooseWifi(false); });
        connect(wake_, &QPushButton::clicked, this, [chooseWifi] { chooseWifi(true); });
    }
    present();
}

KosyncScheduleSheet::~KosyncScheduleSheet()
{
    dismissIntervals();
}

void KosyncScheduleSheet::setChoices(bool automatic, const kosync::Schedule &schedule)
{
    automatic_ = automatic;
    schedule_ = schedule;
    present();
}

void KosyncScheduleSheet::present()
{
    off_->setChecked(!automatic_);
    on_->setChecked(automatic_);
    for (auto *row : details_)
        row->setEnabled(automatic_);
    interval_->setText(intervalLabel(schedule_.minutes));
    for (const auto &check : checks_)
        check.first->setChecked(schedule_.*check.second);
    if (wifi_) {
        // The Wi-Fi choice applies only to syncs while reading.
        wifi_->setEnabled(automatic_ && kosync::syncsWhileReading(schedule_));
        wait_->setChecked(!schedule_.wakeWifi);
        wake_->setChecked(schedule_.wakeWifi);
    }
    if (isVisible())
        fit();
}

void KosyncScheduleSheet::fit()
{
    auto *card = this->card();
    card->fitToWidth(sheetWidth(parentWidget()));
    card->fitToHeight(parentWidget()->height() - 2 * int(metrics::notice::sideClearance));
    centerCard();
}

void KosyncScheduleSheet::showIntervals()
{
    if (intervals_ || !automatic_)
        return;
    // The list opens over the sheet, like the other Settings lists. Choosing
    // closes it and returns to the sheet.
    auto *list = new ui::ModalOverlay(parentWidget());
    intervals_ = list;
    list->setObjectName("rrKosyncIntervalPopup");
    list->setDimmed(true);
    list->setGeometry(parentWidget()->rect());
    list->dismissed = [this] { dismissIntervals(); };
    auto *card = list->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(metrics::notice::cardMargins.top());
    auto *heading = card->title(strings::get("kosync.when_reading"), "rrKosyncIntervalTitle");
    heading->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::spacing::row);
    auto *hint = card->message(strings::get("kosync.interval_description"), "rrKosyncIntervalHint");
    ui::readerStyle(hint, ui::styles::font(chrome::descriptionText));
    hint->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::notice::cardMargins.bottom());
    const int cardWidth = qMin<int>(metrics::notice::cardWidth,
                                    parentWidget()->width() - 2 * metrics::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrKosyncIntervalChoices");
    choices->setUniformRows(false);
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
    const auto &values = kosync::scheduleMinutes();
    for (const int minutes : values)
        choices->add("rrKosyncInterval_" + intervalKey(minutes), intervalLabel(minutes));
    choices->setCurrent(qMax(0, values.indexOf(schedule_.minutes)));
    choices->chosen = [this, values](int index) {
        const int minutes = values.value(index, schedule_.minutes);
        dismissIntervals();
        if (minutes == schedule_.minutes)
            return;
        schedule_.minutes = minutes;
        present();
        if (scheduleChanged)
            scheduleChanged(schedule_);
    };
    card->rows()->addWidget(choices);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    // A phone in landscape pages the rows, as Reader::openSleepChoice does.
    const int available = parentWidget()->height() - 2 * metrics::notice::sideClearance;
    if (card->height() > available) {
        auto *pager = new ui::PagerBar(card, "rrKosyncIntervalPopup");
        card->rows()->addWidget(pager);
        const int fixed = card->height() - choices->height();
        const int room = qMax(1, available - fixed - int(chrome::actionHeight));
        QVector<int> starts{ 0 };
        int used = 0;
        for (int row = 0; row < choices->options().size(); ++row) {
            const int rowHeight = choices->rowHeight(row, cardWidth - 4);
            if (used && used + rowHeight > room) {
                starts.append(row);
                used = 0;
            }
            used += rowHeight;
        }
        auto page = std::make_shared<int>(0);
        const int current = qMax(0, values.indexOf(schedule_.minutes));
        for (int index = 0; index < starts.size(); ++index)
            if (starts[index] <= current)
                *page = index;
        const auto showPage = [list, choices, pager, card, cardWidth, starts, room, page] {
            const int first = starts[*page];
            const int last =
                    *page + 1 < starts.size() ? starts[*page + 1] : int(choices->options().size());
            choices->showRows(first, last - first);
            choices->setFixedHeight(room);
            pager->setPage(*page, starts.size());
            card->fitToWidth(cardWidth);
            list->centerCard();
        };
        connect(pager->previous, &QPushButton::clicked, list, [page, pager, showPage] {
            *page = pager->turnTarget(-1);
            showPage();
        });
        connect(pager->next, &QPushButton::clicked, list, [page, pager, showPage] {
            *page = pager->turnTarget(1);
            showPage();
        });
        showPage();
    }
    list->show();
    list->raise();
    list->fitCard();
}

void KosyncScheduleSheet::dismissIntervals()
{
    if (!intervals_)
        return;
    intervals_->hide();
    intervals_->deleteLater();
    intervals_ = nullptr;
}
} // namespace kobalt::reader
