#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/style/theme.h"
#include "choice_screen.h"
#include "reading/reading_density.h"
#include "ui/style/fonts.h"
#include "ui/style/styles.h"
#include "ui/style/metrics.h"
#include "platform/api/kobo_refresh.h"
#include <QFontInfo>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include "ui/input/input.h"
#include "ui/input/hold_timer.h"
#include <QPointer>
#include "support/strings.h"

namespace kobalt {
namespace metrics = ui::metrics;
namespace styles = ui::styles;
using platform::textRefresh;
using ui::fontSample;
using ui::readerStyle;

namespace {
class FontCell final : public ui::Button
{
public:
    explicit FontCell(QWidget *parent) : ui::Button(parent), detail(new ui::InterfaceLabel(this))
    {
        setProperty("reader_swipe_hold", true);
        detail->setAttribute(Qt::WA_TransparentForMouseEvents);
        detail->setAlignment(Qt::AlignCenter);
        detail->setWordWrap(true);
        hold.setSingleShot(true);
        hold.setInterval(metrics::input::longPressMilliseconds);
        connect(&hold, &QTimer::timeout, this, [this] { triggerHold(); });
    }

    QLabel *detail;
    bool placeholder = false, fontMode = false;
    QFont helpFont;
    std::function<void()> defaultRequested;

protected:
    void nextCheckState() override
    {
        // Only the confirmed font selection changes the checkmark.
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::TouchCancel) {
            hold.stop();
            held = false;
            setDown(false);
            event->accept();
            return true;
        }
        return ui::Button::event(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        held = false;
        ui::Button::mousePressEvent(event);
        if (event->button() == Qt::LeftButton && isDown() && defaultRequested) {
            press = ui::mousePosition(event).toPoint();
            hold.start(event);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((ui::mousePosition(event).toPoint() - press).manhattanLength() > ui::px(12))
            hold.stop();
        ui::Button::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if ((ui::mousePosition(event).toPoint() - press).manhattanLength() > ui::px(12))
            hold.stop();
        QPointer<FontCell> guard(this);
        if (hold.finish(event))
            triggerHold();
        if (!guard)
            return;
        if (held) {
            held = false;
            setDown(false);
            event->accept();
            return;
        }
        ui::Button::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        hold.stop();
        ui::Button::leaveEvent(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        hold.stop();
        ui::Button::hideEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!defaultRequested) {
            ui::Button::contextMenuEvent(event);
            return;
        }
        hold.stop();
        setDown(false);
        defaultRequested();
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (defaultRequested
            && (event->key() == Qt::Key_Menu
                || (event->key() == Qt::Key_F10
                    && event->modifiers().testFlag(Qt::ShiftModifier)))) {
            defaultRequested();
            event->accept();
            return;
        }
        ui::Button::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        ui::Button::paintEvent(event);
        if (!fontMode)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto ink = parentWidget()->palette().color(isChecked() ? QPalette::Window
                                                                     : QPalette::WindowText);
        painter.setPen(QPen(ink, ui::px(2), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (placeholder) {
            const qreal side = qMin<int>(ui::px(64), qMin(width() / 3, height() / 2));
            const QPointF center(width() / 2.0, (height() - detail->height()) / 2.0 - ui::px(4));
            const QRectF circle(center.x() - side / 2, center.y() - side / 2, side, side);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(circle);
            auto font = helpFont;
            font.setPixelSize(qRound(side * 0.55));
            painter.setFont(font);
            // The sample face may be private to Kobalt, which Qt cannot draw.
            ui::drawOpticalText(painter, circle, QStringLiteral("Aa"), true, {});
        }
        if (isChecked() && !placeholder) {
            const int inset = ui::px(16), side = ui::px(36);
            const QPointF origin(width() - inset - side, height() - inset - side);
            painter.setPen(QPen(ink, ui::px(3), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPolygonF check;
            check << origin + QPointF(side * 11.0 / 48, side * 24.0 / 48)
                  << origin + QPointF(side * 21.0 / 48, side * 34.0 / 48)
                  << origin + QPointF(side * 37.0 / 48, side * 14.0 / 48);
            painter.drawPolyline(check);
        }
    }

private:
    void triggerHold()
    {
        if (held || !isDown() || !isVisible() || !isEnabled() || !defaultRequested)
            return;
        held = true;
        setDown(false);
        defaultRequested();
    }

    ui::HoldTimer hold;
    QPoint press;
    bool held = false;
};
} // namespace

void ChoiceScreen::setFontAppearance(const ui::SampleAppearance &appearance)
{
    if (fontAppearance == appearance)
        return;
    fontAppearance = appearance;
    if (mode == Mode::Fonts)
        for (int row = 0; row < labels.size(); ++row) {
            const int index = itemIndex(row);
            labels[row]->setAppearance(index >= 0 && index < items.size()
                                                       && !items[index].placeholder
                                               ? appearance
                                               : ui::SampleAppearance());
        }
}

QJsonObject ChoiceScreen::fontSamples() const
{
    QJsonObject result;
    if (mode == Mode::Fonts)
        for (int row = 0; row < labels.size(); ++row)
            if (labels[row]->isVisible())
                result[labels[row]->text()] = QFontInfo(labels[row]->font()).family();
    return result;
}

ChoiceScreen::ChoiceScreen(QWidget *parent)
    : kobalt::ui::Panel(parent), navigation(this, "rrChoices")
{
    setObjectName("rrChoices");
    fontHeader = new ui::Panel(this, ui::Edges(Qt::TopEdge));
    fontHeader->setEdgeWidth(metrics::chrome::heavyRule);
    fontTitle = new ui::InterfaceLabel(strings::get("reader.font"), fontHeader);
    fontTitle->setObjectName("rrFontTitle");
    readerStyle(fontTitle, styles::font(metrics::text::body, true) + "background:transparent;");
    auto *close = new ui::CellButton(ui::ControlIcon::Close, fontHeader);
    close->setObjectName("rrFontClose");
    close->setAccessibleName(strings::get("reader.close_fonts"));
    close->setRule(Qt::LeftEdge);
    connect(close, &QPushButton::clicked, this, [this] {
        if (back)
            back();
    });
    fontClose = close;
    fontPager = new ui::PagerBar(this, "rrFontPager");
    connect(fontPager->previous, &QPushButton::clicked, this, [this] {
        if (pageCount() > 1) {
            page = fontPager->turnTarget(-1);
            redraw();
        }
    });
    connect(fontPager->next, &QPushButton::clicked, this, [this] {
        if (pageCount() > 1) {
            page = fontPager->turnTarget(1);
            redraw();
        }
    });
    fontSectionBar = new ui::Panel(this);
    fontSectionBar->setObjectName("rrFontSectionBar");
    fontSectionBar->setFill(QPalette::Midlight);
    fontSection = new ui::InterfaceLabel(fontSectionBar);
    fontSection->setObjectName("rrFontSection");
    fontSection->setFont(ui::captionFont(ui::px(18)));
    readerStyle(fontSection, styles::caption() + "background:transparent;");
    fontHeader->hide();
    fontPager->hide();
    fontSectionBar->hide();
    contents = new ChapterContents(this);
    contents->selected = [this](const QString &value) {
        if (selected)
            selected(value);
    };
    contents->back = [this] {
        if (back)
            back();
    };
    contents->changed = [this] {
        if (changed)
            changed();
    };
    connect(navigation.back, &QPushButton::clicked, this, [this] {
        if (back)
            back();
    });
    connect(navigation.previous, &QPushButton::clicked, this, [this] {
        if (page > 0) {
            --page;
            redraw();
        }
    });
    connect(navigation.next, &QPushButton::clicked, this, [this] {
        if (page + 1 < pageCount()) {
            ++page;
            redraw();
        }
    });
}

void ChoiceScreen::showChoices(const QString &heading, QVector<Choice> choices,
                               const QString &current, Mode presentation, ChapterGroups groups)
{
    mode = presentation;
    fontPager->setBlankArea({});
    if (mode == Mode::Chapters) {
        setEdges({});
        contents->setGeometry(rect());
        contents->showChapters(heading, std::move(choices), current, groups);
        redraw();
        show();
        raise();
        if (changed)
            changed();
        return;
    }
    // The selector supplies the shared top edge of its dropdown.
    setEdges(mode == Mode::Fonts
                     ? ui::Edges()
                     : ui::Edges(Qt::LeftEdge) | Qt::RightEdge | Qt::BottomEdge | Qt::TopEdge);
    setEdgeWidth(mode == Mode::Sizes ? 2 : 1);
    navigation.title->setText(heading);
    items = std::move(choices);
    selectedValue = current;
    page = 0;
    layoutChoices();
    for (int i = 0; i < items.size(); ++i)
        if (!current.isEmpty() && items[i].value == current)
            page = pageForIndex(i);
    redraw();
    for (auto *widget : findChildren<QWidget *>())
        widget->ensurePolished();
    show();
    raise();
    if (changed)
        changed();
}

int ChoiceScreen::chapterPage() const
{
    return contents->currentPage();
}

void ChoiceScreen::setChapterPosition(const QString &text)
{
    contents->setPosition(text);
}

void ChoiceScreen::selectValue(const QString &value)
{
    selectedValue = value;
    redraw();
}

void ChoiceScreen::setDropdownGeometry(const QRect &bounds)
{
    fittingHeight = true;
    setGeometry(bounds);
    fittingHeight = false;
}

void ChoiceScreen::setFontGeometry(const QRect &bounds)
{
    if (geometry() == bounds)
        return;
    setDropdownGeometry(bounds);
    layoutChoices();
    redraw();
}

void ChoiceScreen::resizeEvent(QResizeEvent *)
{
    if (fittingHeight)
        return;
    layoutChoices();
    redraw();
}

int ChoiceScreen::pageCount() const
{
    return mode == Mode::Fonts ? qMax(1, int(fontPages.size()))
                               : qMax(1, (int(items.size()) + pageSize - 1) / pageSize);
}

int ChoiceScreen::itemIndex(int row) const
{
    if (mode == Mode::Fonts) {
        if (page < 0 || page >= fontPages.size() || row < 0 || row >= fontPages[page].size())
            return -1;
        return fontPages[page][row];
    }
    return page * pageSize + row;
}

int ChoiceScreen::pageForIndex(int index) const
{
    if (mode == Mode::Fonts) {
        for (int i = 0; i < fontPages.size(); ++i)
            if (fontPages[i].contains(index))
                return i;
        return 0;
    }
    return index / pageSize;
}

void ChoiceScreen::layoutChoices()
{
    if (mode == Mode::Chapters) {
        contents->setGeometry(rect());
        return;
    }
    if (mode == Mode::Fonts) {
        pageSize = parentWidget() && isPhoneScreen(parentWidget()->size(), ui::density()) ? 6 : 12;
        fontPages.clear();
        QStringList sections;
        for (const auto &item : items)
            if (!sections.contains(item.section))
                sections.append(item.section);
        for (const auto &section : sections) {
            QVector<int> indices;
            for (int i = 0; i < items.size(); ++i)
                if (items[i].section == section) {
                    if (indices.size() == pageSize) {
                        fontPages.append(indices);
                        indices.clear();
                    }
                    indices.append(i);
                }
            if (!indices.isEmpty())
                fontPages.append(indices);
        }
        return;
    }
    pageSize = qMax(1, int(items.size()));
    page = 0;
    navigation.layout(size(), true);
}

void ChoiceScreen::redraw()
{
    for (auto *row : sizeRows)
        row->setVisible(mode == Mode::Sizes);
    fontHeader->setVisible(mode == Mode::Fonts);
    fontPager->setVisible(mode == Mode::Fonts);
    fontSectionBar->setVisible(mode == Mode::Fonts);
    if (mode == Mode::Fonts) {
        redrawFonts();
        return;
    }
    if (mode == Mode::Chapters) {
        for (auto *button : buttons)
            button->hide();
        navigation.title->hide();
        navigation.back->hide();
        navigation.setPagingVisible(false);
        contents->show();
        return;
    }
    contents->hide();
    for (auto *button : buttons)
        button->hide();
    while (sizeRows.size() < items.size()) {
        const int index = sizeRows.size();
        auto *row = new ui::ListRow(this);
        row->setCheckable(true);
        sizeRows.append(row);
        row->setTitlePixels(metrics::text::body);
        connect(row, &QPushButton::clicked, this, [this, index] {
            if (index < items.size() && selected)
                selected(items[index].value);
        });
    }
    for (int index = 0; index < sizeRows.size(); ++index) {
        auto *row = sizeRows[index];
        row->setVisible(index < items.size());
        if (index >= items.size())
            continue;
        row->setObjectName(items[index].name);
        row->titleLabel()->setObjectName(items[index].name + "Title");
        const bool current = items[index].value == selectedValue;
        row->setTitle(items[index].label);
        row->setChecked(current);
        row->setAccessory(current ? ui::ListRow::Accessory{ ui::ListRow::Check{} }
                                  : ui::ListRow::Accessory{ ui::ListRow::None{} });
        row->setGeometry(2, 2 + index * metrics::list::oneLine, width() - 4,
                         metrics::list::oneLine);
    }
    navigation.setPage(0, 1);
    navigation.setPagingVisible(false);
    navigation.title->hide();
    navigation.back->hide();

    if (changed && isVisible())
        changed();
}

int ChoiceScreen::fontPanelHeight() const
{
    return metrics::choices::pickerHeader + ui::px(42) + metrics::chrome::actionHeight
            + 4 * metrics::choices::pickerRow;
}

void ChoiceScreen::redrawFonts()
{
    contents->hide();
    navigation.title->hide();
    navigation.back->hide();
    navigation.setPagingVisible(false);
    page = qBound(0, page, pageCount() - 1);
    const int header = metrics::choices::pickerHeader, sectionHeight = ui::px(42),
              pager = metrics::chrome::actionHeight;
    fontHeader->setGeometry(0, 0, width(), header);
    const int cell = qMin(int(metrics::choices::pickerHeader), width() / 4);
    fontTitle->setGeometry(metrics::chrome::inset, 1,
                           qMax(0, width() - cell - metrics::chrome::inset), header - 2);
    fontClose->setGeometry(width() - cell, metrics::chrome::heavyRule, cell,
                           header - metrics::chrome::heavyRule);
    fontSectionBar->setGeometry(0, header, width(), sectionHeight);
    fontSection->setGeometry(metrics::chrome::inset, 0, width() - 2 * metrics::chrome::inset,
                             sectionHeight);
    const int first = itemIndex(0);
    fontSection->setText(first < 0 ? QString() : items[first].section.toUpper());
    fontPager->setGeometry(0, height() - pager, width(), pager);
    fontPager->setPage(page, pageCount(), {}, true);
    fontPager->show();
    while (buttons.size() < pageSize) {
        const int row = buttons.size();
        auto *button = new FontCell(this);
        buttons.append(button);
        auto *label = new ui::SampleLabel(button);
        labels.append(label);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(button, &QPushButton::clicked, this, [this, row] {
            const int index = itemIndex(row);
            if (index < 0 || index >= items.size())
                return;
            if (items[index].placeholder) {
                if (defaultRequested)
                    defaultRequested({}, true);
            } else if (selected)
                selected(items[index].value);
        });
    }
    const int body = qMax(4, height() - header - sectionHeight - pager);
    // Phone pages show six choices with room for the font name and captions.
    // Wider e-reader sheets retain twelve choices.
    const int columns = pageSize == 6 ? (width() > height() ? 3 : 2)
            : body / 4 < int(metrics::choices::pickerRow) * 3 / 4 && width() >= 6 * ui::px(200) ? 6
                                                                                                : 3;
    const int rows = pageSize / columns;
    int used = header + sectionHeight;
    for (int row = 0; row < buttons.size(); ++row) {
        const int index = itemIndex(row);
        auto *button = buttons[row];
        auto *label = labels[row];
        auto *fontCell = static_cast<FontCell *>(button);
        const bool visible = row < pageSize && index >= 0 && index < items.size();
        button->setVisible(visible);
        if (!visible)
            continue;
        const auto &item = items[index];
        const bool selectedItem = !item.placeholder && item.value == selectedValue;
        fontCell->fontMode = true;
        fontCell->placeholder = item.placeholder;
        fontCell->defaultRequested = !item.placeholder
                ? std::function<void()>([this, value = item.value] {
                      if (defaultRequested)
                          defaultRequested(value, false);
                  })
                : std::function<void()>{};
        // Qt 5.2 does not repaint the area that a caption leaves when it is
        // hidden. A cell that shows another font repaints completely, so a
        // caption from the previous page cannot stay on screen.
        if (button->objectName() != item.name)
            button->update();
        button->setObjectName(item.name);
        button->setAccessibleName(item.placeholder ? item.detail : item.label);
        button->setAccessibleDescription(item.detail);
        button->setCheckable(true);
        button->setChecked(selectedItem);
        const int col = row % columns, line = row / columns;
        button->setGeometry(col * width() / columns, header + sectionHeight + line * body / rows,
                            (col + 1) * width() / columns - col * width() / columns,
                            (line + 1) * body / rows - line * body / rows);
        readerStyle(
                button,
                QString("QPushButton{min-height:0;padding:0;border:0;border-right:%2px solid palette(light);border-bottom:%2px solid palette(light);background:%1;}")
                        .arg(selectedItem ? "palette(window-text)" : "palette(window)")
                        .arg(int(metrics::control::border)));
        const QFont labelFont = ui::chromeFont(metrics::text::body);
        const auto family = item.value.isEmpty() ? QStringLiteral("Libron") : item.value;
        label->setFont(labelFont);
        // The default slot draws its Aa with the private renderer.
        fontCell->helpFont = QFont(family);
        label->setAppearance(item.placeholder ? ui::SampleAppearance() : fontAppearance);
        label->setPreviewSource(ui::fontPreviewSource(family, item.fontFiles));
        label->optical = true;
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setText(item.label);
        label->setAlignment(Qt::AlignCenter);
        label->setObjectName(item.name + "Title");
        auto styleFamily = labelFont.family();
        styleFamily.replace('"', "\\\"");
        readerStyle(
                label,
                QString("background:transparent;color:%1;border:none;font-family:\"%2\";font-size:%3px;font-weight:400;")
                        .arg(selectedItem ? "palette(window)" : "palette(window-text)")
                        .arg(styleFamily)
                        .arg(labelFont.pixelSize()));
        auto *detail = fontCell->detail;
        detail->setObjectName(item.name + "Detail");
        detail->setText(item.detail.toUpper());
        const auto caption = ui::captionFont(ui::px(18));
        detail->setFont(caption);
        readerStyle(
                detail,
                QString("background:transparent;color:%1;border:none;font-family:\"%2\";font-size:%3px;")
                        .arg(selectedItem ? "palette(window)" : ui::noteColor(this).name())
                        .arg(caption.family())
                        .arg(caption.pixelSize()));
        const int inset = ui::px(12), detailWidth = qMax(1, button->width() - 2 * inset);
        // A short cell, as in two rows of six, keeps a line for the name. The
        // caption drops its first lines, which give the least detail. Only a
        // caption with several lines measures its sample, which loads that face.
        auto lines = item.detail.toUpper().split('\n');
        const int sampleHeight =
                !item.placeholder && lines.size() > 1 ? fontSample(family).pixelSize() : 0;
        while (sampleHeight && lines.size() > 1
               && button->height() - detail->heightForWidth(detailWidth) - ui::px(32)
                       < sampleHeight) {
            lines.removeFirst();
            detail->setText(lines.join('\n'));
        }
        const int detailHeight = item.detail.isEmpty() ? 0 : detail->heightForWidth(detailWidth);
        const int titleLimit = item.detail.isEmpty() ? ui::px(96) : ui::px(64);
        const int titleHeight = item.placeholder
                ? ui::px(64)
                : qMax(0, qMin<int>(titleLimit, button->height() - detailHeight - ui::px(32)));
        const int top = qMax(0, (button->height() - titleHeight - detailHeight - ui::px(8)) / 2);
        label->setGeometry(inset, top, detailWidth, titleHeight);
        detail->setGeometry(inset, top + titleHeight + ui::px(8), detailWidth, detailHeight);
        detail->setVisible(!item.detail.isEmpty());
        label->setVisible(!item.placeholder);
    }
    for (auto *button : buttons)
        if (!button->isHidden())
            used = qMax(used, button->geometry().bottom() + 1);
    fontPager->setBlankArea({ metrics::chrome::inset, used, width() - 2 * metrics::chrome::inset,
                              fontPager->y() - used });
    fontHeader->raise();
    fontSectionBar->raise();
    fontPager->raise();
    if (changed && isVisible())
        changed();
}

} // namespace kobalt
