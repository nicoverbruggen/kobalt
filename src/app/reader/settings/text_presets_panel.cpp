#include "text_presets_panel.h"
#include "settings_panel.h"

#include "reading/reading_density.h"
#include "support/strings.h"
#include "ui/controls/icons.h"
#include "ui/controls/paging.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/scale.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace kobalt::reader {
namespace metrics = ui::metrics;
namespace chrome = metrics::chrome;

namespace {
constexpr int holdMilliseconds = 600;
constexpr int galleryRows = 3;

int cardHeight()
{
    return ui::px(150);
}

int actionsHeight()
{
    return ui::px(84);
}

void drawPill(QPainter &painter, const QWidget *widget, const QRect &bounds, const QString &text,
              bool inverted)
{
    const auto ink = widget->palette().color(QPalette::WindowText),
               paper = widget->palette().color(QPalette::Window);
    const QFont font = ui::interfaceFont(metrics::text::secondary);
    painter.setFont(font);
    // A narrow card, as on a phone, gives the padding to the name first.
    const int padding = qBound<int>(
            ui::px(4), (bounds.width() - ui::interfaceTextWidth(text, font)) / 2, ui::px(14));
    const int room = qMax(0, bounds.width() - 2 * padding);
    const auto shown = ui::elideInterfaceText(text, font, room);
    const int width = ui::interfaceTextWidth(shown, font) + 2 * padding;
    const QRect block(bounds.x() + (bounds.width() - width) / 2, bounds.y(), width,
                      bounds.height());
    if (inverted)
        painter.fillRect(block, ink);
    painter.setPen(inverted ? paper : ink);
    // Centre the capitals in the block, not the line box, so the name looks
    // centred in the inverted pill.
    const int capitals = ui::interfaceInkBounds(QStringLiteral("H"), font).height();
    const int baseline = block.top() + (block.height() + capitals) / 2;
    const int ascent = ui::interfaceLineAscent(font);
    ui::drawInterfaceText(painter,
                          QRect(block.left() + padding, baseline - ascent,
                                block.width() - 2 * padding, ui::interfaceLineHeight(font)),
                          shown, Qt::AlignLeft | Qt::AlignTop);
}

// Update and Save as new: an icon and a label centred together, as one
// group, filling with ink while pressed like a toolbar cell.
class ChangeButton final : public ui::Button
{
public:
    ChangeButton(ui::ControlIcon icon, QWidget *parent) : ui::Button(parent), glyph(icon)
    {
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }

    static int iconGap() { return ui::px(16); }

    // The width of the icon and the whole label, with the usual side insets.
    static int fullWidth(const QString &label)
    {
        return 2 * chrome::inset + metrics::control::iconSize + iconGap()
                + ui::interfaceTextWidth(label, ui::interfaceFont(metrics::text::secondary));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto colors = ui::themeColors(this);
        const bool pressed = isDown();
        painter.fillRect(rect(), pressed ? colors.ink : colors.paper);
        const auto color = pressed ? colors.paper : colors.ink;
        const QFont font = ui::interfaceFont(metrics::text::secondary);
        painter.setFont(font);
        painter.setPen(color);
        const int icon = metrics::control::iconSize, gap = iconGap();
        // A narrow bar, as on a phone, gives the side insets to the label first.
        const int room = qMax(0, width() - 2 * ui::px(8) - icon - gap);
        const auto label = ui::elideInterfaceText(text(), font, room);
        const int labelWidth = ui::interfaceTextWidth(label, font);
        const int left = (width() - icon - gap - labelWidth) / 2;
        ui::controlIcon(glyph, color)
                .paint(&painter,
                       QRect(left, (height() - icon) / 2 + ui::iconInkOffset(glyph, icon), icon,
                             icon));
        ui::drawOpticalText(painter, QRectF(left + icon + gap, 0, labelWidth + 2, height()), label);
    }

private:
    ui::ControlIcon glyph;
};
} // namespace

HoldTimer::HoldTimer(QWidget *owner, std::function<void()> held) : QObject(owner)
{
    timer.setSingleShot(true);
    timer.setInterval(holdMilliseconds);
    connect(&timer, &QTimer::timeout, this, [this, held = std::move(held)] {
        fired = true;
        held();
    });
    owner->installEventFilter(this);
}

bool HoldTimer::eventFilter(QObject *, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        fired = false;
        timer.start();
        break;
    case QEvent::MouseButtonRelease:
        timer.stop();
        // A long press already acted; the release must not also click.
        if (fired) {
            fired = false;
            if (auto *button = qobject_cast<QAbstractButton *>(parent()))
                button->setDown(false);
            return true;
        }
        break;
    case QEvent::Hide:
        timer.stop();
        break;
    default:
        break;
    }
    return false;
}

TextPresetToggle::TextPresetToggle(QWidget *parent) : ui::Button(parent)
{
    setObjectName("rrTextPresetToggle");
    setFocusPolicy(Qt::NoFocus);
    // The shared button style sets a minimum height taller than the band.
    ui::readerStyle(this, "QPushButton{border:none;padding:0;min-height:0;}");
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void TextPresetToggle::setState(const QString &text, bool expanded)
{
    if (label == text && open == expanded)
        return;
    label = text;
    open = expanded;
    setAccessibleName(strings::get(open ? "text_presets.hide" : "text_presets.show"));
    setAccessibleDescription(label);
    updateGeometry();
    update();
}

QSize TextPresetToggle::sizeHint() const
{
    const QFont font = ui::captionFont(chrome::captionText);
    const int text = ui::interfaceTextWidth(label.toUpper(), font);
    const int width = 2 * chrome::inset + text + ui::px(16) + chrome::selectorIcon;
    return { qMax<int>(chrome::cellWidth, width), int(metrics::list::headerHeight) };
}

void TextPresetToggle::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto colors = ui::themeColors(this);
    const bool pressed = isDown();
    painter.fillRect(rect(), pressed ? colors.ink : ui::sectionColor(this));
    const int rule = metrics::control::border;
    // A full-height rule on the left, and the band's divider along the bottom.
    // The band's colour: a button's own palette draws dividers paler.
    const auto divider = ui::dividerColor(parentWidget() ? parentWidget() : this);
    painter.fillRect(0, 0, rule, height(), divider);
    painter.fillRect(0, height() - rule, width(), rule, divider);
    const auto color = pressed ? colors.paper : colors.ink;
    const QFont font = ui::captionFont(chrome::captionText);
    painter.setFont(font);
    painter.setPen(color);
    // A changed preset's star is set apart and raised, as a mark, not a letter.
    auto name = label.toUpper();
    const bool starred = name.endsWith(QLatin1Char('*'));
    if (starred)
        name.chop(1);
    const QString star = starred ? QStringLiteral("*") : QString();
    const int starWidth = starred ? ui::interfaceTextWidth(star, font) : 0;
    const int icon = chrome::selectorIcon;
    const int gap = ui::px(16);
    const int room = qMax(0, width() - 2 * chrome::inset - gap - icon - starWidth);
    const auto shown = ui::elideInterfaceText(name, font, room);
    const int textWidth = ui::interfaceTextWidth(shown, font);
    const int left = (width() - textWidth - starWidth - gap - icon) / 2;
    const int body = height() - rule;
    const int capitals = ui::interfaceInkBounds(QStringLiteral("H"), font).height();
    // Draw the value as the heading draws its caption: the same function, a
    // box of the same height and vertical centring, so both share a baseline.
    QRect box(left, 0, textWidth + 2, body);
    if (const auto *header = dynamic_cast<const ui::ListHeader *>(parentWidget())) {
        const auto *caption = header->leftLabel();
        const auto area = caption->contentsRect();
        box = QRect(left, caption->y() + area.top() - y(), textWidth + 2, area.height());
        painter.setFont(caption->font());
    }
    ui::drawInterfaceText(painter, box, shown, Qt::AlignLeft | Qt::AlignVCenter);
    if (starred)
        ui::drawInterfaceText(
                painter,
                QRect(left + textWidth, box.top() - capitals / 3, starWidth + 2, box.height()),
                star, Qt::AlignLeft | Qt::AlignVCenter);
    const auto glyph = open ? ui::ControlIcon::Down : ui::ControlIcon::Up;
    ui::controlIcon(glyph, color)
            .paint(&painter,
                   QRect(left + textWidth + starWidth + gap,
                         (body - icon) / 2 + ui::iconInkOffset(glyph, icon), icon, icon));
}

TextPresetCard::TextPresetCard(QWidget *parent) : ui::Button(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void TextPresetCard::setPreset(const TextPresetView &view, bool isSelected, bool isModified)
{
    // The reader syncs the cards after each settled render. An unchanged
    // card must not repaint: on e-ink each repaint is a screen update.
    if (!action && id == view.id && name == view.name && font.family == view.font.family
        && font.files == view.font.files && font.identity == view.font.identity
        && thickness == view.thickness && axisWeight == view.axisWeight && selected == isSelected
        && modified == isModified && accessibleDescription() == view.detail)
        return;
    action = false;
    id = view.id;
    name = view.name;
    font = view.font;
    thickness = view.thickness;
    axisWeight = view.axisWeight;
    selected = isSelected;
    modified = isModified;
    setObjectName("rrTextPreset_" + view.id);
    setAccessibleName(modified ? strings::get("text_presets.modified_name").arg(name) : name);
    setAccessibleDescription(view.detail);
    update();
}

void TextPresetCard::setAction(ui::ControlIcon glyph, const QString &label)
{
    action = true;
    id.clear();
    icon = glyph;
    name = label;
    selected = modified = false;
    setAccessibleName(label);
    update();
}

void TextPresetCard::setDivider(bool shown)
{
    if (divider == shown)
        return;
    divider = shown;
    update();
}

QSize TextPresetCard::sizeHint() const
{
    return { ui::px(200), cardHeight() };
}

QSize TextPresetCard::minimumSizeHint() const
{
    return { ui::px(96), cardHeight() };
}

void TextPresetCard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto colors = ui::themeColors(this);
    const bool held = isDown();
    painter.fillRect(rect(), held && action ? colors.pressedPaper : colors.paper);
    if (divider)
        painter.fillRect(width() - metrics::control::border, 0, metrics::control::border, height(),
                         ui::dividerColor(this));
    const bool marked = selected || (held && !action);
    if (marked) {
        // A heavy frame, as the header and footer preset cards draw around their page.
        const int frame = ui::px(5);
        ui::paintEdges(painter, rect().adjusted(0, 0, divider ? -metrics::control::border : 0, 0),
                       ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                       colors.ink, frame);
    }
    const int nameHeight =
            ui::interfaceLineHeight(ui::interfaceFont(metrics::text::secondary)) + ui::px(4);
    const int gap = ui::px(10);
    const int sample = ui::px(60);
    const int top = (height() - sample - gap - nameHeight) / 2;
    const QRect sampleArea(0, top, width(), sample);
    if (action) {
        // The size of the panel tab icons, so the strokes are equally thick.
        const int iconSize = metrics::control::iconSize;
        ui::controlIcon(icon, colors.ink)
                .paint(&painter,
                       QRect((width() - iconSize) / 2, sampleArea.center().y() - iconSize / 2,
                             iconSize, iconSize));
    } else {
        painter.setPen(colors.ink);
        if (!ui::drawFontPreview(painter, sampleArea, QStringLiteral("Aa"), font, ui::px(44),
                                 thickness, false, true, false, axisWeight)) {
            painter.setFont(ui::interfaceFont(sample * 3 / 4));
            ui::drawInterfaceText(painter, sampleArea, QStringLiteral("Aa"), Qt::AlignCenter);
        }
    }
    const auto label = modified ? strings::get("text_presets.modified").arg(name) : name;
    drawPill(painter, this,
             QRect(ui::px(6), sampleArea.bottom() + 1 + gap, width() - ui::px(12), nameHeight),
             label, marked);
}

TextPresetSection::TextPresetSection(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrTextPresets");
    header = new ui::ListHeader(this);
    header->setObjectName("rrTextPresetsHeader");
    header->setShaded(true);
    header->setCaptions(strings::get("text_presets.title").toUpper());
    toggle = new TextPresetToggle(header);
    connect(toggle, &QPushButton::clicked, this, [this] {
        if (toggled)
            toggled();
    });
    row = new QWidget(this);
    row->setObjectName("rrTextPresetCards");
    auto *cardsLayout = new QHBoxLayout(row);
    cardsLayout->setContentsMargins(0, 0, 0, 0);
    cardsLayout->setSpacing(0);
    for (int index = 0; index < cardCount; ++index) {
        auto *card = new TextPresetCard(row);
        cardsLayout->addWidget(card, 1);
        cards.append(card);
        new HoldTimer(card, [this, card] {
            if (held && !card->presetId().isEmpty())
                held(card->presetId());
        });
        connect(card, &QPushButton::clicked, this, [this, card] {
            if (chosen && !card->presetId().isEmpty())
                chosen(card->presetId());
        });
    }
    allCard = new TextPresetCard(row);
    allCard->setObjectName("rrTextPresetAll");
    allCard->setAction(ui::ControlIcon::Grid, strings::get("text_presets.all"));
    allCard->setDivider(false);
    // A narrow cell, as wide as a toolbar cell, so four presets fit beside it.
    // The cards ignore their width hint; a fixed cell must not, or the layout
    // gives the whole row to the cards.
    allCard->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    allCard->setFixedWidth(chrome::cellWidth);
    cardsLayout->addWidget(allCard);
    connect(allCard, &QPushButton::clicked, this, [this] {
        if (allRequested)
            allRequested();
    });
    // The section draws the divider above the bar, as it does under the
    // cards. A panel edge here would add a second, heavy rule.
    actions = new ui::Panel(this);
    actions->setObjectName("rrTextPresetChangeBar");
    auto *actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(0);
    const auto actionButton = [this](const char *name, ui::ControlIcon icon) {
        auto *button = new ChangeButton(icon, actions);
        button->setObjectName(name);
        return button;
    };
    updateButton = actionButton("rrTextPresetUpdate", ui::ControlIcon::Check);
    saveAsButton = actionButton("rrTextPresetSaveAs", ui::ControlIcon::Plus);
    actionsLayout->addWidget(updateButton, 1);
    auto *divider = new ui::Panel(actions);
    divider->setObjectName("rrTextPresetChangeDivider");
    divider->setFixedWidth(metrics::control::border);
    divider->setDividers(Qt::LeftEdge);
    actionsLayout->addWidget(divider);
    actionsLayout->addWidget(saveAsButton, 1);
    updateButton->setText(strings::get("text_presets.update"));
    saveAsButton->setText(strings::get("text_presets.save_as_new"));
    connect(updateButton, &QPushButton::clicked, this, [this] {
        if (updateRequested)
            updateRequested();
    });
    connect(saveAsButton, &QPushButton::clicked, this, [this] {
        if (saveAsRequested)
            saveAsRequested();
    });
    setState({}, {}, {}, false, false);
}

void TextPresetSection::setState(const QVector<TextPresetView> &views, const QString &selected,
                                 const QString &selectedName, bool isModified, bool isOpen)
{
    const bool resized = open != isOpen || modified != isModified;
    open = isOpen;
    modified = isModified;
    const auto label = selectedName.isEmpty() ? strings::get("text_presets.none")
            : modified ? strings::get("text_presets.modified").arg(selectedName)
                       : selectedName;
    toggle->setState(label, open);
    for (int index = 0; index < cards.size(); ++index) {
        auto *card = cards[index];
        card->setVisible(index < views.size());
        if (index < views.size())
            card->setPreset(views[index], views[index].id == selected,
                            views[index].id == selected && modified);
    }
    // Saving goes through this bar: Update and Save as new for a changed
    // preset, and Save as new alone when the text matches no preset.
    const bool none = selectedName.isEmpty();
    updateButton->setVisible(!none);
    if (auto *divider = actions->findChild<QWidget *>("rrTextPresetChangeDivider"))
        divider->setVisible(!none);
    const bool showActions = open && (modified || none);
    const bool rearranged = resized || row->isHidden() == open || actions->isHidden() == showActions
            || toggle->width() != qMin(toggle->sizeHint().width(), width() / 2);
    row->setVisible(open);
    actions->setVisible(showActions);
    // Children repaint themselves when they change. The section paints only
    // its dividers, which move when the row or the change bar shows or hides.
    if (!rearranged)
        return;
    arrange();
    update();
    if (resized)
        updateGeometry();
}

QSize TextPresetSection::sizeHint() const
{
    int height = metrics::list::headerHeight;
    if (open)
        height += cardHeight() + metrics::control::border;
    if (!actions->isHidden())
        height += actionsHeight() + metrics::control::border;
    return { 0, height };
}

void TextPresetSection::resizeEvent(QResizeEvent *)
{
    arrange();
}

void TextPresetSection::paintEvent(QPaintEvent *)
{
    // The cards and the change bar each end with a full-width divider.
    QPainter painter(this);
    const int rule = metrics::control::border;
    if (open)
        painter.fillRect(0, metrics::list::headerHeight + cardHeight(), width(), rule,
                         ui::dividerColor(this));
    if (!actions->isHidden())
        painter.fillRect(0, actions->geometry().bottom() + 1, width(), rule,
                         ui::dividerColor(this));
}

void TextPresetSection::arrange()
{
    const int band = metrics::list::headerHeight;
    header->setGeometry(0, 0, width(), band);
    const int toggleWidth = qMin(toggle->sizeHint().width(), width() / 2);
    toggle->setGeometry(width() - toggleWidth, 0, toggleWidth, band);
    header->setTrailingInset(toggleWidth);
    int y = band;
    if (open) {
        row->setGeometry(0, y, width(), cardHeight());
        y += cardHeight();
        // The cards end with a divider, as a list row does.
        y += metrics::control::border;
    }
    if (actions->isHidden())
        return;
    // Update and Save as new share the bar equally when both labels fit their
    // half. On a narrow screen each takes room in proportion to its label.
    auto *buttons = static_cast<QHBoxLayout *>(actions->layout());
    const int update = ChangeButton::fullWidth(updateButton->text()),
              saveAs = ChangeButton::fullWidth(saveAsButton->text());
    const bool equal = updateButton->isHidden() || qMax(update, saveAs) <= width() / 2;
    buttons->setStretchFactor(updateButton, equal ? 1 : update);
    buttons->setStretchFactor(saveAsButton, equal ? 1 : saveAs);
    actions->setGeometry(0, y, width(), actionsHeight());
}

// A page of tiles, painted as one surface so a page turn redraws once.
class TextPresetGrid final : public QWidget
{
public:
    explicit TextPresetGrid(TextPresetGallery *gallery) : QWidget(gallery), gallery(gallery)
    {
        setObjectName("rrTextPresetGrid");
        setAttribute(Qt::WA_OpaquePaintEvent);
        hold.setSingleShot(true);
        hold.setInterval(holdMilliseconds);
        connect(&hold, &QTimer::timeout, this, [this] {
            const auto id = presetAt(pressed);
            if (id.isEmpty() || !this->gallery->held)
                return;
            held = true;
            this->gallery->held(id);
        });
    }

    QRect cell(int slot) const
    {
        const int columns = gallery->columns();
        const int column = slot % columns, row = slot / columns;
        const int left = width() * column / columns, right = width() * (column + 1) / columns;
        const int top = height() * row / galleryRows, bottom = height() * (row + 1) / galleryRows;
        return { left, top, right - left, bottom - top };
    }

    int labelHeight() const
    {
        return ui::interfaceLineHeight(ui::interfaceFont(metrics::text::secondary)) + ui::px(30);
    }

    QSize previewSize() const
    {
        const auto area = cell(0);
        return { qMax(1, area.width() - metrics::control::border),
                 qMax(1, area.height() - labelHeight() - metrics::control::border) };
    }

    // The slot's index in the gallery's tiles, or -1.
    int tileAt(QPoint point) const
    {
        for (int slot = 0; slot < gallery->perPage(); ++slot)
            if (cell(slot).contains(point)) {
                const int index = gallery->page * gallery->perPage() + slot;
                return index < gallery->tileCount() ? index : -1;
            }
        return -1;
    }

    QString presetAt(int tile) const
    {
        return tile >= 0 && tile < gallery->presets.size() ? gallery->presets[tile].id : QString();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const auto colors = ui::themeColors(this);
        painter.fillRect(rect(), colors.paper);
        const int perPage = gallery->perPage();
        for (int slot = 0; slot < perPage; ++slot) {
            const int index = gallery->page * perPage + slot;
            if (index >= gallery->tileCount())
                break;
            paintTile(painter, slot, index, colors);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        event->accept();
        pressed = tileAt(event->pos());
        held = false;
        hold.start();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        hold.stop();
        const int tile = tileAt(event->pos());
        const bool wasHeld = held;
        const int was = pressed;
        held = false;
        pressed = -1;
        update();
        if (tile < 0 || wasHeld || tile != was)
            return;
        if (tile >= gallery->presets.size()) {
            if (gallery->saveRequested)
                gallery->saveRequested();
            return;
        }
        gallery->selected = gallery->presets[tile].id;
        update();
        if (gallery->chosen)
            gallery->chosen(gallery->selected);
    }

private:
    void paintTile(QPainter &painter, int slot, int index, const ui::ThemeColors &colors)
    {
        const auto area = cell(slot);
        const int rule = metrics::control::border;
        const auto divider = ui::dividerColor(this);
        if (area.right() + 1 < width())
            painter.fillRect(area.right() + 1 - rule, area.top(), rule, area.height(), divider);
        painter.fillRect(area.left(), area.bottom() + 1 - rule, area.width(), rule, divider);
        const QRect body(area.left(), area.top(), area.width() - rule, area.height() - rule);
        const int label = labelHeight();
        const QRect preview(body.left(), body.top(), body.width(), body.height() - label);
        const QRect caption(body.left(), preview.bottom() + 1, body.width(), label);
        const QFont nameFont = ui::interfaceFont(metrics::text::secondary);
        const QFont detailFont = ui::captionFont(chrome::captionText);
        const int inset = chrome::inset;
        if (index >= gallery->presets.size()) {
            // Save current text.
            const bool down = pressed == index;
            painter.fillRect(body, down ? colors.pressedPaper : colors.paper);
            const int icon = ui::px(56);
            const int lineHeight = ui::interfaceLineHeight(nameFont);
            const int top = body.center().y() - (icon + ui::px(16) + 2 * lineHeight) / 2;
            ui::controlIcon(ui::ControlIcon::Plus, colors.ink)
                    .paint(&painter, QRect(body.center().x() - icon / 2, top, icon, icon));
            painter.setPen(colors.ink);
            painter.setFont(nameFont);
            ui::drawInterfaceText(
                    painter, QRect(body.left(), top + icon + ui::px(16), body.width(), lineHeight),
                    strings::get("text_presets.save_current"), Qt::AlignCenter);
            painter.setFont(detailFont);
            painter.setPen(colors.secondaryInk);
            ui::drawInterfaceText(painter,
                                  QRect(body.left(), top + icon + ui::px(16) + lineHeight,
                                        body.width(), lineHeight),
                                  gallery->currentDetail.toUpper(), Qt::AlignCenter);
            return;
        }
        const auto &view = gallery->presets[index];
        const bool selected = view.id == gallery->selected;
        const bool down = pressed == index;
        painter.fillRect(preview, colors.paper);
        const auto image = gallery->previews.value(view.id);
        if (!image.isNull())
            painter.drawImage(preview.topLeft(), image);
        // Selected tiles get a heavy frame and an inverted caption.
        const bool marked = selected || down;
        painter.fillRect(caption, marked ? colors.ink : colors.paper);
        painter.fillRect(caption.left(), caption.top(), caption.width(), rule, colors.ink);
        if (marked)
            ui::paintEdges(painter, body,
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           colors.ink, ui::px(6));
        const auto ink = marked ? colors.paper : colors.ink;
        painter.setPen(ink);
        // The name always shows. On a narrow tile the size and spacing give
        // way to it.
        const auto detail = view.detail.toUpper();
        const int detailWidth = ui::interfaceTextWidth(detail, detailFont);
        const int nameFull = ui::interfaceTextWidth(view.name, nameFont);
        const auto room = [&](bool withDetail) {
            return caption.width() - 2 * inset - (withDetail ? detailWidth + inset : 0);
        };
        const bool showDetail = nameFull <= room(true);
        // The name and size share one baseline, so text in different fonts
        // sits on the same line.
        const int nameHeight = ui::interfaceLineHeight(nameFont);
        // Centre the capitals, not the line box, as optical labels do.
        const int capitals = ui::interfaceInkBounds(QStringLiteral("H"), nameFont).height();
        const int baseline = caption.top() + (caption.height() + capitals) / 2;
        const int captionAscent = ui::interfaceLineAscent(detailFont);
        const int captionHeight = ui::interfaceLineHeight(detailFont);
        const auto onBaseline = [&](int left, int width, int ascent, int height) {
            return QRect(left, baseline - ascent, width, height);
        };
        if (showDetail) {
            painter.setFont(detailFont);
            ui::drawInterfaceText(painter,
                                  onBaseline(caption.right() + 1 - inset - detailWidth,
                                             detailWidth + 2, captionAscent, captionHeight),
                                  detail, Qt::AlignLeft | Qt::AlignTop);
        }
        painter.setFont(nameFont);
        const auto name = ui::elideInterfaceText(view.name, nameFont, qMax(0, room(showDetail)));
        const int nameWidth = ui::interfaceTextWidth(name, nameFont);
        ui::drawInterfaceText(painter,
                              onBaseline(caption.left() + inset, nameWidth + 2,
                                         ui::interfaceLineAscent(nameFont), nameHeight),
                              name, Qt::AlignLeft | Qt::AlignTop);
    }

    TextPresetGallery *gallery;
    QTimer hold;
    int pressed = -1;
    bool held = false;
};

TextPresetGallery::TextPresetGallery(QWidget *parent) : ui::Panel(parent)
{
    setObjectName("rrTextPresetGallery");
    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);
    // The header matches the sleep screen gallery: a title in the brand face,
    // a heavy rule under it and a close cell on the right.
    auto *heading = new ui::Panel(this, Qt::BottomEdge);
    heading->setObjectName("rrTextPresetGalleryHeader");
    heading->setEdgeWidth(chrome::heavyRule);
    heading->setFixedHeight(chrome::actionHeight);
    auto *headingRows = new QHBoxLayout(heading);
    headingRows->setContentsMargins(0, 0, 0, chrome::heavyRule);
    headingRows->setSpacing(0);
    title = new SettingsTitle(strings::get("text_presets.gallery_title"), heading);
    title->setObjectName("rrTextPresetGalleryTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    ui::readerStyle(title, ui::styles::brand(chrome::titleText) + "background:transparent;");
    headingRows->addWidget(title, 1);
    auto *close = new ui::CellButton(ui::ControlIcon::Close, heading);
    close->setObjectName("rrTextPresetGalleryClose");
    close->setAccessibleName(strings::get("settings.close"));
    close->setRule(Qt::LeftEdge);
    close->setFixedWidth(chrome::cellWidth);
    headingRows->addWidget(close);
    connect(close, &QPushButton::clicked, this, [this] {
        if (closed)
            closed();
    });
    rows->addWidget(heading);
    hint = new ui::ListHeader(this);
    hint->setObjectName("rrTextPresetGalleryHint");
    hint->setShaded(true);
    hint->setCaptions(strings::get("text_presets.gallery_hint").toUpper());
    hint->setFixedHeight(metrics::list::headerHeight);
    rows->addWidget(hint);
    grid = new TextPresetGrid(this);
    rows->addWidget(grid, 1);
    pager = new ui::PagerBar(this, "rrTextPresetGallery");
    rows->addWidget(pager);
    connect(pager->previous, &QPushButton::clicked, this,
            [this] { showPage(pager->turnTarget(-1)); });
    connect(pager->next, &QPushButton::clicked, this, [this] { showPage(pager->turnTarget(1)); });
}

void TextPresetGallery::setPresets(const QVector<TextPresetView> &views, const QString &current,
                                   bool save, const QString &detail)
{
    // The reader syncs the gallery on each refresh. Unchanged presets keep
    // the page and its previews: on e-ink a repaint is a screen update.
    if (views == presets && current == selected && save == canSave && detail == currentDetail)
        return;
    presets = views;
    selected = current;
    canSave = save;
    currentDetail = detail;
    showPage(selectedPage());
}

int TextPresetGallery::columns() const
{
    // A phone in portrait is too narrow for two samples side by side.
    return isNarrowWidth(width(), ui::density()) ? 1 : 2;
}

int TextPresetGallery::perPage() const
{
    return columns() * galleryRows;
}

int TextPresetGallery::selectedPage() const
{
    for (int index = 0; index < presets.size(); ++index)
        if (presets[index].id == selected)
            return index / perPage();
    return page;
}

void TextPresetGallery::setHeading(const QString &text, const QString &instructions)
{
    title->setText(text);
    hint->setCaptions(instructions.toUpper());
}

void TextPresetGallery::setPreviews(const QHash<QString, QImage> &images)
{
    for (auto it = images.cbegin(); it != images.cend(); ++it)
        previews.insert(it.key(), it.value());
    // Tests read how many previews the last handover brought.
    setProperty("previews_shown", int(images.size()));
    grid->update();
}

void TextPresetGallery::clearPreviews()
{
    previews.clear();
    grid->update();
}

QSize TextPresetGallery::previewSize() const
{
    return grid->previewSize();
}

QStringList TextPresetGallery::visibleIds() const
{
    QStringList ids;
    const int count = perPage();
    for (int index = page * count; index < qMin(int(presets.size()), (page + 1) * count); ++index)
        ids.append(presets[index].id);
    return ids;
}

int TextPresetGallery::tileCount() const
{
    return presets.size() + (canSave ? 1 : 0);
}

int TextPresetGallery::pageCount() const
{
    return qMax(1, (tileCount() + perPage() - 1) / perPage());
}

void TextPresetGallery::showPage(int target)
{
    page = qBound(0, target, pageCount() - 1);
    pager->setPage(page, pageCount());
    grid->update();
    if (pageChanged)
        pageChanged();
}

void TextPresetGallery::resizeEvent(QResizeEvent *event)
{
    ui::Panel::resizeEvent(event);
    // The width decides the columns. When they change, the pages do too:
    // show the one with the preset in force again.
    if (perPage() != arrangedPerPage) {
        arrangedPerPage = perPage();
        page = qBound(0, selectedPage(), pageCount() - 1);
        pager->setPage(page, pageCount());
        grid->update();
    }
    // Showing the grid can resize it more than once. Render again only when
    // the previews would change size; each round repaints the whole grid.
    const auto size = previewSize();
    if (size == renderedSize && !previews.isEmpty())
        return;
    renderedSize = size;
    previews.clear();
    if (pageChanged)
        pageChanged();
}
} // namespace kobalt::reader
