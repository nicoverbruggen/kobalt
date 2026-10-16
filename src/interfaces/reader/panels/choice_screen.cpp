#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/style/theme.h"
#include "choice_screen.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "platform/api/kobo_refresh.h"
#include <QFontInfo>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include "ui/input.h"
#include "support/strings.h"

namespace kobalt {
namespace m = ui::metrics;
namespace styles = ui::styles;
using ui::readerStyle;
using ui::fontSample;
using platform::textRefresh;
namespace {
class FontCell final : public ui::Button {
public:
    explicit FontCell(QWidget *parent) : ui::Button(parent), detail(new QLabel(this)) {
        detail->setAttribute(Qt::WA_TransparentForMouseEvents);
        detail->setAlignment(Qt::AlignCenter);
        detail->setWordWrap(true);
        hold.setSingleShot(true);
        connect(&hold, &QTimer::timeout, this, [this] {
            if (!isDown() || !isVisible() || !isEnabled() || !defaultRequested)
                return;
            held = true;
            setDown(false);
            defaultRequested();
        });
    }
    QLabel *detail;
    bool placeholder = false, fontMode = false;
    QFont helpFont;
    std::function<void()> defaultRequested;

protected:
    void mousePressEvent(QMouseEvent *event) override {
        held = false;
        ui::Button::mousePressEvent(event);
        if (event->button() == Qt::LeftButton && isDown() && defaultRequested) {
            press = ui::mousePosition(event).toPoint();
            hold.start(550);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if ((ui::mousePosition(event).toPoint() - press).manhattanLength() > ui::px(12))
            hold.stop();
        ui::Button::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        hold.stop();
        if (held) {
            held = false;
            setDown(false);
            event->accept();
            return;
        }
        ui::Button::mouseReleaseEvent(event);
    }
    void leaveEvent(QEvent *event) override {
        hold.stop();
        ui::Button::leaveEvent(event);
    }
    void hideEvent(QHideEvent *event) override {
        hold.stop();
        ui::Button::hideEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent *event) override {
        if (!defaultRequested) {
            ui::Button::contextMenuEvent(event);
            return;
        }
        hold.stop();
        setDown(false);
        defaultRequested();
        event->accept();
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (defaultRequested && (event->key() == Qt::Key_Menu ||
                                 (event->key() == Qt::Key_F10 && event->modifiers().testFlag(Qt::ShiftModifier)))) {
            defaultRequested();
            event->accept();
            return;
        }
        ui::Button::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent *event) override {
        ui::Button::paintEvent(event);
        if (!fontMode)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto ink = parentWidget()->palette().color(isChecked() ? QPalette::Window : QPalette::WindowText);
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
            ui::drawOpticalText(painter, circle, QStringLiteral("Aa"), true);
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
    QTimer hold;
    QPoint press;
    bool held = false;
};
}
void ChoiceScreen::setFontAppearance(const ui::SampleAppearance &appearance) {
    if (fontAppearance == appearance)
        return;
    fontAppearance = appearance;
    if (mode == Mode::Fonts)
        for (int row = 0; row < labels.size(); ++row) {
            const int index = itemIndex(row);
            labels[row]->setAppearance(
                index >= 0 && index < items.size() && !items[index].placeholder ? appearance : ui::SampleAppearance());
        }
}
QJsonObject ChoiceScreen::fontSamples() const {
    QJsonObject result;
    if (mode == Mode::Fonts)
        for (int row = 0; row < labels.size(); ++row)
            if (labels[row]->isVisible())
                result[labels[row]->text()] = QFontInfo(labels[row]->font()).family();
    return result;
}

ChoiceScreen::ChoiceScreen(QWidget *parent) : kobalt::ui::Panel(parent), navigation(this, "rrChoices") {
    setObjectName("rrChoices");
    fontHeader = new ui::Panel(this, ui::Edges(Qt::TopEdge));
    fontHeader->setEdgeWidth(m::chrome::heavyRule);
    fontTitle = new QLabel(strings::get("reader.font"), fontHeader);
    fontTitle->setObjectName("rrFontTitle");
    readerStyle(fontTitle, styles::font(m::text::body, true) + "background:transparent;");
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
            requestFontPage();
            redraw();
        }
    });
    connect(fontPager->next, &QPushButton::clicked, this, [this] {
        if (pageCount() > 1) {
            page = fontPager->turnTarget(1);
            requestFontPage();
            redraw();
        }
    });
    fontSectionBar = new ui::Panel(this);
    fontSectionBar->setObjectName("rrFontSectionBar");
    fontSectionBar->setFill(QPalette::Midlight);
    fontSection = new QLabel(fontSectionBar);
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

void ChoiceScreen::showChoices(const QString &heading, QVector<Choice> choices, const QString &current,
                               Mode presentation) {
    mode = presentation;
    if (mode == Mode::Chapters) {
        setEdges({});
        contents->setGeometry(rect());
        contents->showChapters(heading, std::move(choices), current);
        redraw();
        show();
        raise();
        if (changed)
            changed();
        return;
    }
    // The selector supplies the shared top edge of its dropdown.
    setEdges(mode == Mode::Fonts ? ui::Edges()
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
    if (mode == Mode::Fonts)
        requestFontPage();
    redraw();
    for (auto *widget : findChildren<QWidget *>())
        widget->ensurePolished();
    show();
    raise();
    if (changed)
        changed();
}

int ChoiceScreen::chapterPage() const {
    return contents->currentPage();
}
void ChoiceScreen::setChapterPosition(const QString &text) {
    contents->setPosition(text);
}

void ChoiceScreen::selectValue(const QString &value) {
    selectedValue = value;
    redraw();
}

void ChoiceScreen::setDropdownGeometry(const QRect &bounds) {
    fittingHeight = true;
    setGeometry(bounds);
    fittingHeight = false;
}
void ChoiceScreen::setFontGeometry(const QRect &bounds) {
    if (geometry() == bounds)
        return;
    setDropdownGeometry(bounds);
    redraw();
}
void ChoiceScreen::resizeEvent(QResizeEvent *) {
    if (fittingHeight)
        return;
    layoutChoices();
    redraw();
}

int ChoiceScreen::pageCount() const {
    return mode == Mode::Fonts ? qMax(1, int(fontPages.size()))
                               : qMax(1, (int(items.size()) + pageSize - 1) / pageSize);
}

int ChoiceScreen::itemIndex(int row) const {
    if (mode == Mode::Fonts) {
        if (page < 0 || page >= fontPages.size() || row < 0 || row >= fontPages[page].size())
            return -1;
        return fontPages[page][row];
    }
    return page * pageSize + row;
}

int ChoiceScreen::pageForIndex(int index) const {
    if (mode == Mode::Fonts) {
        for (int i = 0; i < fontPages.size(); ++i)
            if (fontPages[i].contains(index))
                return i;
        return 0;
    }
    return index / pageSize;
}

void ChoiceScreen::requestFontPage() {
    if (!fontPageRequested || mode != Mode::Fonts || page < 0 || page >= fontPages.size())
        return;
    QStringList families;
    for (const int index : fontPages[page]) {
        const QString family = items[index].value.isEmpty() ? QStringLiteral("Libron") : items[index].value;
        if (!families.contains(family))
            families.append(family);
    }
    fontPageRequested(families);
}

void ChoiceScreen::layoutChoices() {
    if (mode == Mode::Chapters) {
        contents->setGeometry(rect());
        return;
    }
    if (mode == Mode::Fonts) {
        pageSize = 12;
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

void ChoiceScreen::redraw() {
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
        row->setTitlePixels(m::text::body);
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
        row->setAccessory(current ? ui::ListRow::Accessory{ui::ListRow::Check{}}
                                  : ui::ListRow::Accessory{ui::ListRow::None{}});
        row->setGeometry(2, 2 + index * m::list::oneLine, width() - 4, m::list::oneLine);
    }
    navigation.setPage(0, 1);
    navigation.setPagingVisible(false);
    navigation.title->hide();
    navigation.back->hide();

    if (changed && isVisible())
        changed();
}
int ChoiceScreen::fontPanelHeight() const {
    return m::choices::pickerHeader + ui::px(42) + m::chrome::actionHeight + 4 * m::choices::pickerRow;
}
void ChoiceScreen::redrawFonts() {
    contents->hide();
    navigation.title->hide();
    navigation.back->hide();
    navigation.setPagingVisible(false);
    page = qBound(0, page, pageCount() - 1);
    pageSize = 12;
    const int header = m::choices::pickerHeader, sectionHeight = ui::px(42), pager = m::chrome::actionHeight;
    fontHeader->setGeometry(0, 0, width(), header);
    const int cell = qMin(int(m::choices::pickerHeader), width() / 4);
    fontTitle->setGeometry(m::chrome::inset, 1, qMax(0, width() - cell - m::chrome::inset), header - 2);
    fontClose->setGeometry(width() - cell, m::chrome::heavyRule, cell, header - m::chrome::heavyRule);
    fontSectionBar->setGeometry(0, header, width(), sectionHeight);
    fontSection->setGeometry(m::chrome::inset, 0, width() - 2 * m::chrome::inset, sectionHeight);
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
                    defaultRequested({});
            } else if (selected)
                selected(items[index].value);
        });
    }
    const int body = qMax(4, height() - header - sectionHeight - pager);
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
        fontCell->defaultRequested = !item.placeholder && !item.value.isEmpty()
                                         ? std::function<void()>([this, value = item.value] {
                                               if (defaultRequested)
                                                   defaultRequested(value);
                                           })
                                         : std::function<void()>{};
        button->setObjectName(item.name);
        button->setAccessibleName(item.placeholder ? item.detail : item.label);
        button->setAccessibleDescription(item.detail);
        button->setCheckable(true);
        button->setChecked(selectedItem);
        const int columns = 3;
        const int col = row % 3, line = row / 3;
        button->setGeometry(col * width() / columns, header + sectionHeight + line * body / 4,
                            (col + 1) * width() / columns - col * width() / columns,
                            (line + 1) * body / 4 - line * body / 4);
        readerStyle(
            button,
            QString(
                "QPushButton{min-height:0;padding:0;border:0;border-right:%2px solid palette(light);border-bottom:%2px solid palette(light);background:%1;}")
                .arg(selectedItem ? "palette(window-text)" : "palette(window)")
                .arg(m::control::border));
        const QFont labelFont = fontSample(item.value.isEmpty() ? QStringLiteral("Libron") : item.value);
        label->setFont(labelFont);
        fontCell->helpFont = labelFont;
        label->setAppearance(item.placeholder ? ui::SampleAppearance() : fontAppearance);
        label->optical = true;
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setText(item.label);
        label->setAlignment(Qt::AlignCenter);
        label->setObjectName(item.name + "Title");
        auto family = labelFont.family();
        family.replace('"', "\\\"");
        readerStyle(
            label,
            QString("background:transparent;color:%1;border:none;font-family:\"%2\";font-size:%3px;font-weight:400;")
                .arg(selectedItem ? "palette(window)" : "palette(window-text)")
                .arg(family)
                .arg(labelFont.pixelSize()));
        auto *detail = fontCell->detail;
        detail->setObjectName(item.name + "Detail");
        detail->setText(item.detail.toUpper());
        const auto caption = ui::captionFont(ui::px(18));
        detail->setFont(caption);
        readerStyle(detail, QString("background:transparent;color:%1;border:none;font-family:\"%2\";font-size:%3px;")
                                .arg(selectedItem ? "palette(window)" : ui::noteColor(this).name())
                                .arg(caption.family())
                                .arg(caption.pixelSize()));
        const int inset = ui::px(12), detailWidth = qMax(1, button->width() - 2 * inset);
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
    fontHeader->raise();
    fontSectionBar->raise();
    fontPager->raise();
    if (changed && isVisible())
        changed();
}

}
