#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/controls/page_scrubber.h"
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
// The tab labels are capitals, so the cap height is their ink.
int tabCapital(int pixels) {
    return ui::interfaceInkBounds(QStringLiteral("H"), ui::interfaceFont(pixels)).height();
}
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
        for (int cell = 1; cell <= property("right_actions").toInt(); ++cell) {
            painter.fillRect(width() - cell * c::cellWidth - border, status, border, height() - status, ink);
        }
        ui::paintEdges(painter, rect(), Qt::BottomEdge, palette().color(QPalette::WindowText), toolbarBorder);
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
        ui::paintEdges(painter, rect(), Qt::TopEdge, palette().color(QPalette::WindowText), height());
    }
};
class Tab final : public ui::Button {
public:
    Tab(const QString &label, ui::ControlIcon icon, QWidget *parent) : ui::Button(label, parent), glyph(icon) {
        setCheckable(true);
        setFont(ui::interfaceFont(c::tabText));
        setAccessibleName(label);
        setToolTip(label);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }
    void setLabelsHidden(bool hidden) {
        if (labelsHidden == hidden)
            return;
        labelsHidden = hidden;
        update();
    }
    void setSampleFace(const QString &face) {
        auto next = ui::fontSample(face);
        // Samples are sized for the 40 pixel cell icons; grow with the tab icon.
        next.setPixelSize(qRound(next.pixelSize() * qreal(c::tabIcon) / c::cellIcon * ui::detail::iconScale));
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
        // Centering the icon box and the label's ink as one group lines every
        // tab up.
        const auto label = ui::interfaceFont(c::tabText);
        const int capital = tabCapital(c::tabText);
        const int top = labelsHidden ? (height() - c::tabIcon) / 2 : (height() - c::tabIcon - c::tabGap - capital) / 2;
        const QRect icon((width() - c::tabIcon) / 2, top, c::tabIcon, c::tabIcon);
        painter.setPen(color);
        if (glyph == ui::ControlIcon::Typography) {
            painter.setFont(sample);
            ui::drawOpticalText(painter, QRect(0, icon.top(), width(), icon.height()), QStringLiteral("Aa"), true, {});
        } else
            ui::controlIcon(glyph, color).paint(&painter, icon);
        if (labelsHidden)
            return;
        painter.setFont(label);
        ui::drawOpticalText(painter, QRect(ui::px(2), icon.bottom() + 1 + c::tabGap, width() - ui::px(4), capital),
                            text(), true);
    }

private:
    ui::ControlIcon glyph;
    QFont sample;
    bool labelsHidden = false;
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
    tabRow->setFixedHeight(barHeight() - toolbarBorder);
    rows->addWidget(tabRow);
    auto *row = new QHBoxLayout(tabRow);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    const ui::ControlIcon icons[] = {ui::ControlIcon::Chapters, ui::ControlIcon::Compass, ui::ControlIcon::Typography,
                                     ui::ControlIcon::Layout,   ui::ControlIcon::Light,   ui::ControlIcon::Graph};
    const char *names[] = {"Chapters", "Seek", "Text", "Layout", "Light", "Stats"};
    const QString labels[] = {strings::get("navigation.chapters"), strings::get("navigation.seek"),
                              strings::get("navigation.text"),     strings::get("navigation.layout"),
                              strings::get("navigation.light"),    strings::get("navigation.stats")};
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
        ui::paintEdges(painter, rect(), Qt::TopEdge, palette().color(QPalette::WindowText), toolbarBorder);
        return;
    }
    // An open panel draws the rule above the bar and leaves this band empty.
    // Extend the active tab's fill through it, so the fill meets that rule.
    for (auto *tab : tabs)
        if (!tab->isHidden() && (tab->isChecked() || tab->isDown()))
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
    updateLabels();
}

void BottomNavigation::setDefaultView(int destination) {
    for (auto tab : {NavigationDestination::Seek, NavigationDestination::Stats})
        tabs[int(tab)]->setVisible(int(tab) != destination);
    updateLabels();
}

void BottomNavigation::setLabelsHidden(bool hidden) {
    labelsHidden = hidden;
    updateLabels();
}
void BottomNavigation::resizeEvent(QResizeEvent *event) {
    ui::Panel::resizeEvent(event);
    updateLabels();
}
void BottomNavigation::updateLabels() {
    // A large interface scale leaves the tabs too narrow for some labels.
    // Hide them all, as the option does, rather than elide a few.
    int shown = 0;
    for (auto *tab : tabs)
        shown += !tab->isHidden();
    bool fit = shown > 0;
    const auto font = ui::interfaceFont(c::tabText);
    for (auto *tab : tabs)
        if (!tab->isHidden() && ui::interfaceTextWidth(tab->text(), font) > width() / qMax(1, shown) - ui::px(4))
            fit = false;
    for (auto *tab : tabs) {
        static_cast<Tab *>(tab)->setLabelsHidden(labelsHidden || !fit);
        tab->setProperty("labels_hidden", labelsHidden || !fit);
    }
}

void BottomNavigation::setReturnLabel(const QString &label, const QString &note) {
    returnRow->setVisible(!label.isEmpty());
    returnButton->setText(label);
    returnButton->setCaption(note);
    returnButton->setAccessibleName(label + " " + note);
}
// Large text makes the tab icon and label taller. The bar grows by the same
// amount, so they keep the space they have at the standard size. As at the
// standard size, the bar keeps its height when the labels are hidden.
int BottomNavigation::barHeight() {
    if (ui::textScale() == 1)
        return c::actionHeight;
    const int icon = c::tabIcon - ui::px(c::tabIcon.design());
    const int label = tabCapital(c::tabText) - tabCapital(ui::px(c::tabText.design()));
    return c::actionHeight + icon + label;
}
int BottomNavigation::navigationHeight() const {
    return barHeight() + (returnRow->isHidden() ? 0 : ui::metrics::links::returnHeight);
}

Navigation::Navigation(QWidget *parent, const ToolbarComposition &controls)
    : bar(new BottomNavigation(parent)), toolbar(controls.panel), toolbarRows(controls.rows),
      exitAction(controls.exitAction), settings(controls.settings), bookmarks(controls.bookmarks),
      annotate(controls.annotate), clock(controls.clock), session(controls.session), battery(controls.battery),
      bluetooth(controls.bluetooth), wifi(controls.wifi), formattingWarning(controls.formattingWarning), search(controls.search), sync(controls.sync) {
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
    if (search) {
        searchStyle = search->styleSheet();
        ui::readerStyle(search, "QPushButton{border:none;padding:0;}");
        search->setFixedSize(c::cellWidth, c::actionHeight);
        if (auto *button = dynamic_cast<ui::Button *>(search)) {
            searchPaint = button->paintMode();
            button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        }
    }
    if (sync) {
        syncStyle = sync->styleSheet();
        ui::readerStyle(sync, "QPushButton{border:none;padding:0;}");
        sync->setFixedSize(c::cellWidth, c::actionHeight);
        if (auto *button = dynamic_cast<ui::Button *>(sync)) {
            syncPaint = button->paintMode();
            button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        }
    }
    if (annotate) {
        annotateStyle = annotate->styleSheet();
        ui::readerStyle(annotate, "QPushButton{border:none;padding:0;}");
        annotate->setFixedSize(c::cellWidth, c::actionHeight);
        if (auto *button = dynamic_cast<ui::Button *>(annotate)) {
            annotatePaint = button->paintMode();
            button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        }
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
    if (annotate) {
        annotate->setStyleSheet(annotateStyle);
        annotate->setFixedSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize);
        if (auto *button = dynamic_cast<ui::Button *>(annotate)) {
            button->setPaintMode(annotatePaint);
        }
    }
    if (search) {
        search->setStyleSheet(searchStyle);
        search->setFixedSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize);
        if (auto *button = dynamic_cast<ui::Button *>(search)) {
            button->setPaintMode(searchPaint);
        }
    }
    session->setStyleSheet(sessionStyle);
    if (sync) {
        sync->setStyleSheet(syncStyle);
        sync->setFixedSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize);
        if (auto *button = dynamic_cast<ui::Button *>(sync)) button->setPaintMode(syncPaint);
    }
    settings->setStyleSheet(settingsStyle);
    bookmarks->setStyleSheet(bookmarksStyle);
    settings->setFixedSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize);
    bookmarks->setFixedSize(ui::metrics::control::toolbarButtonSize, ui::metrics::control::toolbarButtonSize);
    toolbar->setEdges(Qt::BottomEdge);
    toolbarRows->setEnabled(true);
    toolbarRows->invalidate();
}
QRect Navigation::panelBounds(QSize size, int height) const {
    return QRect(0, contentBottom(size) - height, size.width(), height);
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
    const int syncActions = sync && !sync->isHidden() ? 1 : 0;
    const int rightActions =
        (formattingWarning && !formattingWarning->isHidden() ? 3 : 2) + (annotate ? 1 : 0) + (search ? 1 : 0) + syncActions;
    // A large interface scale can leave too little room for the exit caption.
    // The arrow then keeps a cell of its own, as each action does.
    // When the caption nearly reaches the actions, the exit action meets them,
    // so its divider and theirs draw as one line.
    const int exitWidth = exitAction->sizeHint().width();
    const int exitRoom = size.width() - rightActions * c::cellWidth;
    const int exitCell = exitWidth > exitRoom ? int(c::cellWidth) : exitRoom - exitWidth < c::cellWidth / 2 ? exitRoom : exitWidth;
    // A narrow gap should not look like an empty toolbar cell.
    const bool returnDivider = exitRoom - exitCell >= int(c::cellWidth);
    if (exitAction->property("reader_return_divider").toBool() != returnDivider) {
        exitAction->setProperty("reader_return_divider", returnDivider);
        exitAction->update();
    }
    // The status line keeps its proportions as large text enlarges it. It
    // sits at the bottom of a taller camera band, or inside the action row.
    const int statusTop = inlineStatus
        ? statusHeight() + (int(c::actionHeight) - int(ui::metrics::panel::statusHeight)) / 2
        : statusHeight() - (statusVisible ? int(ui::metrics::chrome::statusHeight) : 0) + ui::textPx(10);
    const int statusLeft = inlineStatus ? exitCell + int(c::inset) : int(c::inset);
    const int statusRight = size.width() - (inlineStatus ? rightActions * int(c::cellWidth) : 0) - c::inset;
    clock->setGeometry(statusLeft, statusTop, ui::textPx(160), ui::metrics::panel::statusHeight);
    battery->move(statusRight - battery->width(), statusTop);
    int sessionInset = inlineStatus ? qMax(statusLeft + ui::textPx(196) - int(c::inset), size.width() - battery->x() + int(c::inset))
                                    : ui::textPx(196);
    const int statusGap = ui::metrics::panel::bluetoothGap;
    QWidget *previousStatus = nullptr;
    const auto placeStatus = [&](QWidget *indicator) {
        const int x = previousStatus ? previousStatus->x() - statusGap - indicator->width()
            : battery->x() - statusGap - (int(ui::metrics::panel::bluetoothIconSize) + indicator->width()) / 2;
        indicator->move(x, statusTop);
        previousStatus = indicator;
        sessionInset = qMax(sessionInset, size.width() - indicator->x() + int(c::inset));
    };
    if (bluetooth) {
        bluetooth->setVisible(statusVisible && bluetooth->active());
        if (bluetooth->active()) {
            placeStatus(bluetooth);
        }
    }
    if (wifi) {
        wifi->setVisible(statusVisible && wifi->active());
        if (wifi->active())
            placeStatus(wifi);
    }
    session->setGeometry(sessionInset, statusTop, qMax(0, size.width() - 2 * sessionInset),
                         ui::metrics::panel::statusHeight);
    settings->setGeometry(size.width() - c::cellWidth, statusHeight(), c::cellWidth, c::actionHeight);
    bookmarks->setGeometry(size.width() - 2 * c::cellWidth, statusHeight(), c::cellWidth, c::actionHeight);
    if (annotate) {
        annotate->setGeometry(size.width() - 3 * c::cellWidth, statusHeight(), c::cellWidth, c::actionHeight);
    }
    if (search) {
        search->setGeometry(size.width() - (annotate ? 4 : 3) * c::cellWidth, statusHeight(), c::cellWidth,
                            c::actionHeight);
    }
    if (sync) {
        sync->setGeometry(size.width() - ((annotate ? 4 : 3) + (search ? 1 : 0)) * c::cellWidth,
                          statusHeight(), c::cellWidth, c::actionHeight);
    }
    if (formattingWarning) {
        formattingWarning->setGeometry(size.width() - ((annotate ? 4 : 3) + (search ? 1 : 0) + syncActions) * c::cellWidth,
                                       statusHeight(), c::cellWidth, c::actionHeight);
    }
    exitAction->setGeometry(0, statusHeight(), exitCell, c::actionHeight);
    if (rules->property("right_actions").toInt() != rightActions) {
        rules->setProperty("right_actions", rightActions);
        rules->update();
    }
    for (auto *action : state.topActions)
        action->hide();
    const bool scrubberShown =
        scrubber &&
        (state.scrubberVisible || state.active == int(NavigationDestination::Seek) || scrubber->hasActions()) &&
        state.visible && (state.active < 0 || state.active == int(NavigationDestination::Seek)) &&
        !state.chaptersOpen && !state.settings->isVisible() && !state.appearance->isVisible() &&
        !state.stats->isVisible() && !state.light->isVisible();
    bar->setReturnLabel(scrubber ? QString() : state.returnLabel, state.returnNote);
    const int height = bar->navigationHeight();
    bar->setGeometry(0, size.height() - safeBottom - height, size.width(), height);
    bar->setVisible(state.visible && !state.settings->isVisible());
    if (scrubber) {
        scrubber->setBarVisible(state.scrubberVisible);
        scrubber->setGeometry(0, toolbarHeight(), size.width(), qMax(1, bar->y() - toolbarHeight()));
        scrubber->setVisible(scrubberShown);
        if (scrubberShown)
            scrubber->stackUnder(upperEdge);
    }
    if (!state.visible) {
        upperEdge->hide();
        lowerEdge->hide();
        return;
    }
    bar->setLightTab(state.lightAvailable);
    bar->setDefaultView(state.defaultView);
    bar->setLabelsHidden(state.hideTabLabels);
    for (auto *panel : {state.appearance, state.stats})
        if (panel->isVisible())
            panel->setGeometry(panelBounds(size, panel->height()));
    // Chapters covers the header's action row with its own bar; the status strip stays.
    if (state.chaptersOpen) {
        state.chapters->setGeometry(0, statusHeight(), size.width(), bar->y() - statusHeight());
    }
    bar->select(state.active, state.enabled, state.lightAvailable);
    int top = toolbarHeight(), bottom = bar->y();
    if (state.settings->isVisible())
        top = state.settings->geometry().bottom() + 1;
    for (auto *panel : {state.appearance, state.stats, state.light})
        if (panel->isVisible())
            bottom = qMin(bottom, panel->y());
    const bool attachedScrubber = scrubberShown && state.scrubberVisible;
    bar->setPanelOpen(bottom < bar->y() || attachedScrubber);
    if (!state.chaptersOpen) {
        upperEdge->setGeometry(0, top - ui::px(2), size.width(), ui::px(2));
        upperEdge->show();
        lowerEdge->setGeometry(0, bottom, size.width(),
                               attachedScrubber ? int(ui::metrics::control::border) : int(toolbarBorder));
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
    if (state.settings->isVisible()) {
        upperEdge->hide();
        lowerEdge->hide();
    }
}
bool Navigation::contains(QPoint global) const {
    return bar->isVisible() && QRect(bar->mapToGlobal(QPoint()), bar->size()).contains(global);
}
}
