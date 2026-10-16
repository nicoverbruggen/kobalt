#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/style/theme.h"
#include "navigation.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/styles.h"
#include "support/strings.h"
#include <QPainter>
#include <QStackedWidget>
#include <QtMath>

namespace kobalt::reader {
namespace c = ui::metrics::chrome;
namespace {
constexpr ui::Px toolbarBorder = c::heavyRule;
class ToolbarRules final : public QWidget {
public:
    explicit ToolbarRules(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setStyleSheet("background:transparent;");
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const int border = ui::metrics::control::border;
        const auto ink = palette().color(QPalette::Mid);
        const int status = property("status_height").toInt();
        if (status)
            painter.fillRect(0, status - border, width(), border, ink);
        painter.fillRect(width() - c::cellWidth - border, status, border, height() - status, ink);
        painter.fillRect(width() - 2 * c::cellWidth - border, status, border, height() - status, ink);
        if (property("formatting_warning").toBool()) {
            painter.fillRect(width() - 3 * c::cellWidth - border, status, border, height() - status, ink);
        }
        painter.fillRect(0, height() - toolbarBorder, width(), toolbarBorder, palette().color(QPalette::WindowText));
    }
};
class ReadingRule final : public QWidget {
public:
    explicit ReadingRule(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setStyleSheet("background:transparent;");
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::WindowText));
    }
};
class Tab final : public ui::Button {
public:
    Tab(const QString &label, ui::ControlIcon icon, QWidget *parent) : ui::Button(label, parent), glyph(icon) {
        setCheckable(true);
        setFont(ui::interfaceFont(c::tabText));
        setAccessibleName(label);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }
    void setSampleFace(const QString &face) {
        auto next = ui::fontSample(face);
        // Samples are sized for the 40 pixel cell icons; grow with the tab icon.
        next.setPixelSize(qRound(next.pixelSize() * qreal(c::tabIcon) / c::cellIcon));
        if (sample == next)
            return;
        sample = next;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const bool active = isChecked() || isDown();
        const auto paper = palette().color(QPalette::Base);
        const auto ink = palette().color(QPalette::Text);
        painter.fillRect(rect(), active ? ink : paper);
        if (!active) {
            ui::paintEdges(painter, rect(), Qt::RightEdge, palette().color(QPalette::Mid));
        }
        if (!isEnabled())
            painter.setOpacity(.45);
        const auto color = active ? paper : ink;
        // The labels are capitals, so the cap height is their ink. Centering
        // the icon box and that ink as one group lines every tab up.
        const auto label = ui::interfaceFont(c::tabText);
        const int capital = qCeil(QFontMetricsF(label).tightBoundingRect(QStringLiteral("H")).height());
        const int top = (height() - c::tabIcon - c::tabGap - capital) / 2;
        const QRect icon((width() - c::tabIcon) / 2, top, c::tabIcon, c::tabIcon);
        painter.setPen(color);
        if (glyph == ui::ControlIcon::Typography) {
            painter.setFont(sample);
            ui::drawOpticalText(painter, QRect(0, icon.top(), width(), icon.height()), QStringLiteral("Aa"), true);
        } else
            ui::controlIcon(glyph, color).paint(&painter, icon);
        painter.setFont(label);
        ui::drawOpticalText(painter, QRect(ui::px(2), icon.bottom() + 1 + c::tabGap, width() - ui::px(4), capital),
                            text(), true);
    }

private:
    ui::ControlIcon glyph;
    QFont sample;
};
}
BottomNavigation::BottomNavigation(QWidget *parent) : ui::Panel(parent, Qt::TopEdge) {
    setObjectName("rrBottomNavigation");
    auto *rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, toolbarBorder, 0, 0);
    rows->setSpacing(0);
    returnRow = new QWidget(this);
    returnRow->setFixedHeight(ui::metrics::links::returnHeight);
    rows->addWidget(returnRow);
    auto *returnLayout = new QHBoxLayout(returnRow);
    returnLayout->setContentsMargins(0, 0, 0, 0);
    returnLayout->setSpacing(0);
    returnButton = new ui::CaptionButton(ui::ControlIcon::Undo, returnRow);
    returnButton->setObjectName("rrNavLinkReturn");
    returnButton->setRule(Qt::BottomEdge);
    returnButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    returnLayout->addWidget(returnButton, 1);
    auto *stay = new ui::CellButton(ui::ControlIcon::Close, returnRow);
    stay->setObjectName("rrNavLinkStay");
    stay->setAccessibleName(strings::get("reader.stay_here"));
    stay->setToolTip(stay->accessibleName());
    stay->setFixedWidth(c::cellWidth);
    stay->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    stay->setRule(ui::Edges(Qt::LeftEdge) | Qt::BottomEdge);
    returnLayout->addWidget(stay);
    connect(stay, &QPushButton::clicked, this, [this] {
        if (stayRequested)
            stayRequested();
    });
    connect(returnButton, &QPushButton::clicked, this, [this] {
        if (returnRequested)
            returnRequested();
    });
    auto *tabRow = new QWidget(this);
    tabRow->setFixedHeight(barHeight - toolbarBorder);
    rows->addWidget(tabRow);
    auto *row = new QHBoxLayout(tabRow);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    const ui::ControlIcon icons[] = {ui::ControlIcon::Chapters, ui::ControlIcon::Typography, ui::ControlIcon::Layout,
                                     ui::ControlIcon::Light,    ui::ControlIcon::Graph,      ui::ControlIcon::Search};
    const char *names[] = {"Chapters", "Text", "Layout", "Light", "Stats", "Search"};
    const QString labels[] = {strings::get("navigation.chapters"), strings::get("navigation.text"),
                              strings::get("navigation.layout"),   strings::get("navigation.light"),
                              strings::get("navigation.stats"),    strings::get("navigation.search")};
    for (int i = 0; i < 6; ++i) {
        auto *tab = new Tab(labels[i], icons[i], tabRow);
        tab->setObjectName(QString("rrNav%1").arg(names[i]));
        row->addWidget(tab, 1);
        tabs.append(tab);
        connect(tab, &QPushButton::pressed, this, [this] {
            update();
        });
        connect(tab, &QPushButton::released, this, [this] {
            update();
        });
        connect(tab, &QPushButton::clicked, this, [this, i] {
            if (activated)
                activated(NavigationDestination(i));
        });
    }
    hide();
}
void BottomNavigation::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    if (!panelOpen) {
        painter.fillRect(0, 0, width(), toolbarBorder, palette().color(QPalette::WindowText));
        return;
    }
    // An open panel draws the rule above the bar and leaves this band empty.
    // Extend the active tab's fill through it, so the fill meets that rule.
    for (auto *tab : tabs)
        if (tab->isChecked() || tab->isDown())
            painter.fillRect(tab->mapTo(this, QPoint()).x(), 0, tab->width(), toolbarBorder,
                             tab->palette().color(QPalette::Text));
}
void BottomNavigation::setPanelOpen(bool open) {
    if (panelOpen == open)
        return;
    panelOpen = open;
    update(QRect(0, 0, width(), toolbarBorder));
}
void BottomNavigation::select(int destination, bool enabled, bool lightAvailable) {
    for (int i = 0; i < tabs.size(); ++i) {
        if (panelOpen && tabs[i]->isChecked() != (i == destination))
            update(QRect(tabs[i]->mapTo(this, QPoint()).x(), 0, tabs[i]->width(), toolbarBorder));
        tabs[i]->setChecked(i == destination);
        tabs[i]->setEnabled(enabled && (i != int(NavigationDestination::Light) || lightAvailable));
    }
}

void BottomNavigation::setTextFace(const QString &face) {
    static_cast<Tab *>(tabs[int(NavigationDestination::Text)])->setSampleFace(face);
}
void BottomNavigation::setLightTab(bool shown) {
    tabs[int(NavigationDestination::Light)]->setVisible(shown);
}
void BottomNavigation::setStatsTab(bool shown) {
    tabs[int(NavigationDestination::Stats)]->setVisible(shown);
}
void BottomNavigation::setReturnLabel(const QString &label, const QString &note) {
    returnRow->setVisible(!label.isEmpty());
    returnButton->setText(label);
    returnButton->setCaption(note);
    returnButton->setAccessibleName(label + " " + note);
}
int BottomNavigation::navigationHeight() const {
    return barHeight + (returnRow->isHidden() ? 0 : ui::metrics::links::returnHeight);
}

Navigation::Navigation(QWidget *parent, const ToolbarComposition &controls)
    : bar(new BottomNavigation(parent)), pageCount(new QLabel(controls.panel)), toolbar(controls.panel),
      toolbarRows(controls.rows), exitAction(controls.exitAction), settings(controls.settings),
      bookmarks(controls.bookmarks), clock(controls.clock), session(controls.session), battery(controls.battery),
      bluetooth(controls.bluetooth), formattingWarning(controls.formattingWarning) {
    upperEdge = new ReadingRule(parent);
    upperEdge->setObjectName("rrNavigationUpperEdge");
    lowerEdge = new ReadingRule(parent);
    lowerEdge->setObjectName("rrNavigationLowerEdge");
    toolbar->setEdges({});
    toolbarRows->setEnabled(false);
    rules = new ToolbarRules(toolbar);
    rules->lower();
    sessionStyle = session->styleSheet();
    settingsStyle = settings->styleSheet();
    bookmarksStyle = bookmarks->styleSheet();
    ui::readerStyle(session, ui::styles::font(c::countText) + "font-style:italic;");
    ui::readerStyle(settings, "QPushButton{border:none;padding:0;}");
    settings->setFixedSize(c::cellWidth, c::actionHeight);
    if (auto *button = dynamic_cast<ui::Button *>(settings)) {
        settingsPaint = button->paintMode();
        button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
    }
    ui::readerStyle(bookmarks, "QPushButton{border:none;padding:0;}");
    bookmarks->setFixedSize(c::cellWidth, c::actionHeight);
    if (auto *button = dynamic_cast<ui::Button *>(bookmarks)) {
        bookmarksPaint = button->paintMode();
        button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
    }
    if (formattingWarning) {
        formattingWarningStyle = formattingWarning->styleSheet();
        formattingWarning->setFixedSize(c::cellWidth, c::actionHeight);
        ui::readerStyle(formattingWarning, "QPushButton{border:none;padding:0;}");
        if (auto *button = dynamic_cast<ui::Button *>(formattingWarning)) {
            formattingWarningPaint = button->paintMode();
            button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        }
    }
    pageCount->setObjectName("rrMenuPageCount");
    pageCount->setAlignment(Qt::AlignCenter);
    pageCount->setAttribute(Qt::WA_TransparentForMouseEvents);
    ui::readerStyle(pageCount, ui::styles::font(c::countText));
    pageCount->setFont(ui::interfaceFont(c::countText));
    bar->activated = [this](NavigationDestination destination) {
        if (activated)
            activated(destination);
    };
    bar->returnRequested = [this] {
        if (returnRequested)
            returnRequested();
    };
    bar->stayRequested = [this] {
        if (stayRequested)
            stayRequested();
    };
}
Navigation::~Navigation() {
    delete bar;
    delete pageCount;
    delete rules;
    delete upperEdge;
    delete lowerEdge;
    if (auto *button = dynamic_cast<ui::Button *>(settings))
        button->setPaintMode(settingsPaint);
    if (auto *button = dynamic_cast<ui::Button *>(bookmarks))
        button->setPaintMode(bookmarksPaint);
    if (formattingWarning) {
        if (auto *button = dynamic_cast<ui::Button *>(formattingWarning)) {
            button->setPaintMode(formattingWarningPaint);
        }
        formattingWarning->setStyleSheet(formattingWarningStyle);
        formattingWarning->setFixedSize(ui::metrics::control::toolbarButtonSize,
                                        ui::metrics::control::toolbarButtonSize);
    }
    session->setStyleSheet(sessionStyle);
    settings->setStyleSheet(settingsStyle);
    bookmarks->setStyleSheet(bookmarksStyle);
    settings->setFixedSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize);
    bookmarks->setFixedSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize);
    toolbar->setEdges(Qt::BottomEdge);
    toolbarRows->setEnabled(true);
    toolbarRows->invalidate();
}
QRect Navigation::panelBounds(QSize size, int height) const {
    return QRect(0, size.height() - BottomNavigation::barHeight - height, size.width(), height);
}
void Navigation::stackPanel(QWidget *panel) const {
    // Keep the header rule in place when a bottom panel opens. Raising the
    // rule again would repaint unchanged toolbar pixels beneath it.
    panel->stackUnder(upperEdge);
}
void Navigation::update(QSize size, const NavigationState &state) {
    state.footer->hide();
    toolbar->setGeometry(0, 0, size.width(), toolbarHeight());
    rules->setProperty("status_height", statusHeight());
    clock->setVisible(statusVisible);
    battery->setVisible(statusVisible);
    session->setVisible(statusVisible && !session->text().isEmpty());
    rules->setGeometry(toolbar->rect());
    rules->show();
    rules->raise();
    clock->setGeometry(c::inset, ui::px(10), ui::px(160), ui::px(40));
    int sessionInset = ui::px(196);
    battery->move(size.width() - c::inset - battery->width(), ui::px(10));
    if (bluetooth) {
        bluetooth->setVisible(statusVisible && bluetooth->active());
        bluetooth->move(battery->x() - int(ui::metrics::panel::bluetoothGap) - bluetooth->width(), ui::px(10));
        if (bluetooth->active()) {
            const int rightInset = size.width() - bluetooth->x() + int(c::inset);
            sessionInset = qMax(sessionInset, rightInset);
        }
    }
    session->setGeometry(sessionInset, ui::px(10), qMax(0, size.width() - 2 * sessionInset), ui::px(40));
    exitAction->setGeometry(ui::px(24), statusHeight(), exitAction->sizeHint().width(), c::actionHeight);
    settings->setGeometry(size.width() - c::cellWidth, statusHeight(), c::cellWidth, c::actionHeight);
    bookmarks->setGeometry(size.width() - 2 * c::cellWidth, statusHeight(), c::cellWidth, c::actionHeight);
    if (formattingWarning) {
        formattingWarning->setGeometry(size.width() - 3 * c::cellWidth, statusHeight(), c::cellWidth, c::actionHeight);
    }
    pageCount->setText(state.pageCount);
    const int rightActions = formattingWarning && !formattingWarning->isHidden() ? 3 : 2;
    if (rules->property("formatting_warning").toBool() != (rightActions == 3)) {
        rules->setProperty("formatting_warning", rightActions == 3);
        rules->update();
    }
    const int labelLeft = qMax(exitAction->geometry().right() + 1, size.width() / 3);
    const int labelRight = qMin(size.width() - rightActions * int(c::cellWidth), 2 * size.width() / 3);
    const int labelWidth = qMax(0, labelRight - labelLeft);
    pageCount->setGeometry(labelLeft, exitAction->y(), labelWidth, exitAction->height());
    pageCount->show();
    for (auto *action : state.topActions)
        action->hide();
    bar->setReturnLabel(state.returnLabel, state.returnNote);
    const int height = bar->navigationHeight();
    bar->setGeometry(0, size.height() - height, size.width(), height);
    bar->setVisible(state.visible);
    if (!state.visible) {
        upperEdge->hide();
        lowerEdge->hide();
        return;
    }
    bar->setStatsTab(!state.statsPinned);
    bar->setLightTab(state.lightAvailable);
    for (auto *panel : {state.appearance, state.stats})
        if (panel->isVisible())
            panel->setGeometry(panelBounds(size, panel->height()));
    // Chapters covers the header's action row with its own bar; the status strip stays.
    if (state.chaptersOpen) {
        state.chapters->setGeometry(0, statusHeight(), size.width(), size.height() - height - statusHeight());
    }
    bar->select(state.active, state.enabled, state.lightAvailable);
    int top = toolbarHeight(), bottom = bar->y();
    if (state.settings->isVisible())
        top = state.settings->geometry().bottom() + 1;
    for (auto *panel : {state.appearance, state.stats, state.light})
        if (panel->isVisible())
            bottom = qMin(bottom, panel->y());
    bar->setPanelOpen(bottom < bar->y());
    if (!state.chaptersOpen) {
        upperEdge->setGeometry(0, top - ui::px(2), size.width(), ui::px(2));
        upperEdge->show();
        lowerEdge->setGeometry(0, bottom, size.width(), toolbarBorder);
        lowerEdge->show();
    } else {
        upperEdge->hide();
        lowerEdge->hide();
    }
    // Keep the bar below the font picker even while the picker is hidden.
    // Opening or closing it then leaves the header rule's order unchanged.
    if (!state.chaptersOpen) {
        bar->stackUnder(state.chapters);
    } else {
        bar->raise();
    }
    // Keep the same stacking order on replay. Raising each rule in turn
    // invalidates its pixels even when no panel or rule has changed.
    lowerEdge->stackUnder(bar);
    upperEdge->stackUnder(lowerEdge);
    if (state.chaptersOpen) {
        state.chapters->stackUnder(upperEdge);
    }
    // Settings opens raised above the reading bars. Its heavy bottom rule is
    // upperEdge, so keep the panel beneath it. Move it only when it is above
    // the rule; restacking an unchanged order would repaint the panel.
    auto *settings = state.settings;
    if (settings->isVisible() && settings->parentWidget() == upperEdge->parentWidget()) {
        const auto siblings = settings->parentWidget()->children();
        if (siblings.indexOf(settings) > siblings.indexOf(upperEdge))
            settings->stackUnder(upperEdge);
    }
}
bool Navigation::contains(QPoint global) const {
    return bar->isVisible() && QRect(bar->mapToGlobal(QPoint()), bar->size()).contains(global);
}
}
