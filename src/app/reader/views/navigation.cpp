#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/panels.h"
#include "ui/controls/page_scrubber.h"
#include "ui/style/theme.h"
#include "navigation.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "support/strings.h"
#include <QPaintEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QLocale>
#include <QPainterPath>
#include <QStackedWidget>
#include <QtMath>

namespace kobalt::reader {
namespace chrome = ui::metrics::chrome;

namespace {
// The tab labels are capitals, so the cap height is their ink.
int tabCapital(int pixels)
{
    return ui::interfaceInkBounds(QStringLiteral("H"), ui::interfaceFont(pixels)).height();
}

constexpr ui::Px toolbarBorder = chrome::heavyRule;

// How much wider Zen mode's bottom rule is than the regular heavy rule. The
// group's layout adds it outside the tabs.
int zenOutset()
{
    return qMax(0, int(chrome::zenRule) - int(toolbarBorder));
}

// The bottom group's whole shape, out to its rule: `rect` with rounded top
// corners. Nothing of the group paints outside it.
QPainterPath zenOutline(const QRectF &rect)
{
    const qreal radius = chrome::zenCorner;
    QPainterPath path;
    path.addRoundedRect(rect.adjusted(0, 0, 0, radius), radius, radius);
    QPainterPath bounds;
    bounds.addRect(rect);
    return path.intersected(bounds);
}

// A rule along three sides of `rect`, open at `open`, with rounded corners
// on the closed side.
QPainterPath openFrame(const QRectF &rect, qreal radius, Qt::Edge open)
{
    QPainterPath path;
    const qreal d = 2 * radius;
    if (open == Qt::BottomEdge) {
        path.moveTo(rect.bottomLeft());
        path.lineTo(rect.left(), rect.top() + radius);
        path.arcTo(rect.left(), rect.top(), d, d, 180, -90);
        path.lineTo(rect.right() - radius, rect.top());
        path.arcTo(rect.right() - d, rect.top(), d, d, 90, -90);
        path.lineTo(rect.bottomRight());
    } else {
        path.moveTo(rect.topLeft());
        path.lineTo(rect.left(), rect.bottom() - radius);
        path.arcTo(rect.left(), rect.bottom() - d, d, d, 180, 90);
        path.lineTo(rect.right() - radius, rect.bottom());
        path.arcTo(rect.right() - d, rect.bottom() - d, d, d, 270, 90);
        path.lineTo(rect.topRight());
    }
    return path;
}

// A widget mask with rounded corners. Zen mode's groups lie over the page,
// and the mask keeps their paper and cells inside the rounded rule. A mask
// has no partial pixels, so it follows a curve a little outside the rule's:
// the rule's smoothed outer edge then blends into paper instead of being cut
// off in steps.
QRegion roundedMask(const QRect &rect, bool top)
{
    const qreal grow = 2, radius = chrome::zenCorner + grow;
    QPainterPath path;
    path.addRoundedRect(
            QRectF(rect).adjusted(-grow, top ? -grow : -radius, grow, top ? radius : grow), radius,
            radius);
    QPainterPath bounds;
    bounds.addRect(rect);
    return QRegion(path.intersected(bounds).toFillPolygon().toPolygon());
}

// Paper over the square corners outside a rounded frame, so a pressed or
// selected cell keeps the rounded shape.
void paintCornerMasks(QPainter &painter, const QRectF &rect, qreal radius, bool top,
                      const QColor &paper)
{
    QPainterPath rounded;
    rounded.addRoundedRect(rect.adjusted(0, top ? 0 : -radius, 0, top ? radius : 0), radius,
                           radius);
    const qreal y = top ? rect.top() : rect.bottom() - radius;
    for (const qreal x : { rect.left(), rect.right() - radius }) {
        QPainterPath corner;
        corner.addRect(QRectF(x, y, radius, radius));
        painter.fillPath(corner.subtracted(rounded), paper);
    }
}

class ToolbarRules final : public QWidget
{
public:
    explicit ToolbarRules(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setStyleSheet("background:transparent;");
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const int border = ui::metrics::control::border;
        const auto ink = palette().color(QPalette::Mid);
        const int status = property("status_height").toInt();
        const int zenWidth = property("zen_width").toInt();
        if (zenWidth > 0) {
            // Zen mode: a centered group hanging from the top edge, with the
            // status line above its cells and rounded bottom corners.
            const int left = property("zen_left").toInt(), cell = property("zen_cell").toInt();
            const qreal rule = chrome::zenRule;
            const QRectF group(left - rule, 0, zenWidth + 2 * rule, height());
            painter.setRenderHint(QPainter::Antialiasing);
            paintCornerMasks(painter, group, chrome::zenCorner, false,
                             palette().color(QPalette::Window));
            painter.fillRect(left, status - border, zenWidth, border, ink);
            // The cell dividers continue through the stats panel, between its
            // figures. A divider separates the panel from the cells.
            for (int column = 1; column < 3; ++column)
                painter.fillRect(left + column * cell - border, status, border, height() - status,
                                 ink);
            const int stats = property("zen_stats_top").toInt();
            if (stats > 0)
                painter.fillRect(left, stats - border, zenWidth, border, ink);
            painter.setPen(
                    QPen(palette().color(QPalette::WindowText), rule, Qt::SolidLine, Qt::FlatCap));
            painter.drawPath(openFrame(group.adjusted(rule / 2, -rule, -rule / 2, -rule / 2),
                                       chrome::zenCorner - rule / 2, Qt::TopEdge));
            return;
        }
        if (status)
            painter.fillRect(0, status - border, width(), border, ink);
        for (int cell = 1; cell <= property("right_actions").toInt(); ++cell) {
            painter.fillRect(width() - cell * chrome::cellWidth - border, status, border,
                             height() - status, ink);
        }
        ui::paintEdges(painter, rect(), Qt::BottomEdge, palette().color(QPalette::WindowText),
                       toolbarBorder);
    }
};

class ReadingRule final : public QWidget
{
public:
    explicit ReadingRule(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setStyleSheet("background:transparent;");
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        ui::paintEdges(painter, rect(), Qt::TopEdge, palette().color(QPalette::WindowText),
                       height());
    }
};

// Opaque paper below a Zen mode view, as the page uses for its blank header
// and footer. Along its top edge, the regular bar's heavy rule closes the
// view above the attached tabs.
class ZenViewBase final : public QWidget
{
public:
    explicit ZenViewBase(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_OpaquePaintEvent);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Window));
        ui::paintEdges(painter, rect(), Qt::TopEdge, palette().color(QPalette::WindowText),
                       toolbarBorder);
    }
};

// Zen mode's bottom group: one heavy rule with rounded top corners, or under a
// full-screen view, the view's straight heavy rule with dividers at both ends.
// It is
// drawn over the tabs, so a selected tab keeps the rounded shape.
class ZenFrame final : public QWidget
{
public:
    explicit ZenFrame(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setStyleSheet("background:transparent;");
    }

    void setAttached(bool value)
    {
        if (attached == value)
            return;
        attached = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (attached) {
            // The rule continues the paper's rule below the view. The tabs
            // start below it; dividers close them at both ends, as they
            // separate them.
            const int band = zenOutset() + int(toolbarBorder);
            const int border = ui::metrics::control::border;
            painter.fillRect(0, zenOutset(), width(), toolbarBorder,
                             palette().color(QPalette::WindowText));
            const auto divider = palette().color(QPalette::Mid);
            painter.fillRect(band - border, band, border, height() - band, divider);
            painter.fillRect(width() - band, band, border, height() - band, divider);
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal rule = chrome::zenRule, radius = chrome::zenCorner;
        const QRectF outer(rect());
        // Paper over each corner square outside the rule's inner curve, so a
        // selected tab keeps the rounded shape. The group's mask clips anything
        // outside the rule.
        const QRectF inner = outer.adjusted(rule, rule, -rule, 0);
        QPainterPath tabs;
        tabs.addRoundedRect(inner.adjusted(0, 0, 0, radius), radius - rule, radius - rule);
        for (const qreal x : { outer.left(), outer.right() - radius }) {
            QPainterPath corner;
            corner.addRect(QRectF(x, outer.top(), radius, radius));
            painter.fillPath(corner.subtracted(tabs), palette().color(QPalette::Window));
        }
        painter.setPen(
                QPen(palette().color(QPalette::WindowText), rule, Qt::SolidLine, Qt::FlatCap));
        painter.drawPath(openFrame(outer.adjusted(rule / 2, rule / 2, -rule / 2, rule),
                                   radius - rule / 2, Qt::BottomEdge));
    }

private:
    bool attached = false;
};

// Zen mode's reading stats button shows the chart icon with a chevron. The
// chevron points down while the panel is closed and up while it is open.
class ZenStatsButton final : public QPushButton
{
public:
    explicit ZenStatsButton(QWidget *parent) : QPushButton(parent)
    {
        setObjectName("rrZenStats");
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(strings::get("zen.reading_stats"));
        setToolTip(accessibleName());
        setCheckable(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool pressed = isDown();
        const auto paper = palette().color(QPalette::Window);
        const auto ink = palette().color(QPalette::WindowText);
        painter.fillRect(rect(), pressed ? ink : paper);
        const int icon = chrome::cellIcon, chevron = chrome::selectorIcon, gap = ui::px(10);
        const int left = (width() - icon - gap - chevron) / 2;
        const auto color = pressed ? paper : ink;
        ui::controlIcon(ui::ControlIcon::Graph, color)
                .paint(&painter, QRect(left, (height() - icon) / 2, icon, icon));
        ui::controlIcon(isChecked() ? ui::ControlIcon::Up : ui::ControlIcon::Down, color)
                .paint(&painter,
                       QRect(left + icon + gap, (height() - chevron) / 2, chevron, chevron));
    }
};

// Zen mode's reading stats panel shows one figure under each top cell, a
// number with its units over a caption.
class ZenStatsPanel final : public QWidget
{
public:
    struct Figure
    {
        QVector<QPair<QString, QString>> parts; // Each number with its unit.
        QString caption;
        bool approximate = false;

        bool operator==(const Figure &other) const
        {
            return parts == other.parts && caption == other.caption
                    && approximate == other.approximate;
        }
    };

    explicit ZenStatsPanel(QWidget *parent) : QWidget(parent) { setObjectName("rrZenStatsPanel"); }

    void setFigures(const QVector<Figure> &next)
    {
        if (figures == next)
            return;
        figures = next;
        setAccessibleName(text());
        update();
    }

    // The figures' text, for checks and accessibility.
    QString text() const
    {
        QStringList out;
        for (const auto &figure : figures) {
            QStringList number;
            for (const auto &part : figure.parts)
                number << part.first + (part.second.isEmpty() ? QString() : " " + part.second);
            out << (figure.approximate ? QStringLiteral("≈ ") : QString()) + number.join(' ') + " "
                            + figure.caption;
        }
        return out.join(", ");
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Window));
        painter.setPen(palette().color(QPalette::WindowText));
        const int column = figures.isEmpty() ? 0 : width() / figures.size();
        // Large text or a long value can be wider than a column. All figures
        // then shrink together, so they keep one size.
        auto numberFont = ui::interfaceFont(chrome::zenStatText);
        auto unitFont = ui::interfaceFont(chrome::zenStatUnit);
        auto approximateFont = ui::interfaceFont(chrome::zenStatApproximate);
        auto captionFont = ui::captionFont();
        // Each figure as tokens: the ≈ of an estimate, then each number with
        // its unit in the smaller size.
        const auto tokens = [&](const Figure &figure) {
            QVector<QPair<QString, const QFont *>> out;
            if (figure.approximate)
                out.append({ QStringLiteral("≈"), &approximateFont });
            for (const auto &part : figure.parts) {
                out.append({ part.first, &numberFont });
                if (!part.second.isEmpty())
                    out.append({ part.second, &unitFont });
            }
            return out;
        };
        const int gap = ui::px(6), captionGap = ui::textPx(20), margin = ui::px(12);
        const auto figureWidth = [&](const Figure &figure) {
            const auto parts = tokens(figure);
            int total = 0;
            for (int t = 0; t < parts.size(); ++t)
                total += ui::interfaceTextWidth(parts[t].first, *parts[t].second)
                        + (t + 1 < parts.size() ? gap : 0);
            return total;
        };
        qreal scale = 1;
        for (const auto &figure : figures) {
            const int widest = qMax(figureWidth(figure),
                                    ui::interfaceTextWidth(figure.caption.toUpper(), captionFont));
            if (widest > column - 2 * margin)
                scale = qMin(scale, qreal(column - 2 * margin) / widest);
        }
        if (scale < 1) {
            for (auto *font : { &numberFont, &unitFont, &approximateFont, &captionFont })
                font->setPixelSize(qMax(1, int(font->pixelSize() * scale)));
        }
        const QFontMetrics numbers(numberFont), captions(captionFont);
        const int block = numbers.ascent() + captionGap + captions.height();
        const int baseline = (height() - block) / 2 + numbers.ascent();
        for (int i = 0; i < figures.size(); ++i) {
            const auto &figure = figures[i];
            const auto parts = tokens(figure);
            const int total = figureWidth(figure);
            int x = i * column + (column - total) / 2;
            for (const auto &token : parts) {
                painter.setFont(*token.second);
                painter.drawText(x, baseline, token.first);
                x += ui::interfaceTextWidth(token.first, *token.second) + gap;
            }
            painter.setFont(captionFont);
            painter.drawText(QRect(i * column, baseline + captionGap, column, captions.height()),
                             Qt::AlignHCenter | Qt::AlignTop, figure.caption.toUpper());
        }
    }

private:
    QVector<Figure> figures;
};

class Tab final : public ui::Button
{
public:
    Tab(const QString &label, ui::ControlIcon icon, QWidget *parent)
        : ui::Button(label, parent), glyph(icon)
    {
        setCheckable(true);
        setFont(ui::interfaceFont(chrome::tabText));
        setAccessibleName(label);
        setToolTip(label);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }

    // Changes the label and icon, as the toolbar's marks button does.
    void setDestination(const QString &label, ui::ControlIcon icon)
    {
        if (text() == label && glyph == icon)
            return;
        setText(label);
        setAccessibleName(label);
        setToolTip(label);
        glyph = icon;
        update();
    }

    void setLabelsHidden(bool hidden)
    {
        if (labelsHidden == hidden)
            return;
        labelsHidden = hidden;
        update();
    }

    void setSampleFace(const QString &face)
    {
        auto next = ui::fontSample(face);
        // Samples are sized for the 40 pixel cell icons; grow with the tab icon.
        next.setPixelSize(qRound(next.pixelSize() * qreal(chrome::tabIcon) / chrome::cellIcon
                                 * ui::detail::iconScale));
        if (sample == next)
            return;
        sample = next;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool active = isChecked() || isDown();
        const auto paper = palette().color(QPalette::Base);
        const auto ink = ui::labelInk(this, QPalette::Text);
        painter.fillRect(rect(), active ? ink : paper);
        // In Zen mode the group's heavy rule follows the last tab.
        if (!active && !property("reader_group_end").toBool()) {
            ui::paintEdges(painter, rect(), Qt::RightEdge, palette().color(QPalette::Mid));
        }
        if (!isEnabled())
            painter.setOpacity(.45);
        const auto color = active ? paper : ink;
        // Centering the icon box and the label's ink as one group lines every
        // tab up.
        const auto label = ui::interfaceFont(chrome::tabText);
        const int capital = tabCapital(chrome::tabText);
        const int top = labelsHidden ? (height() - chrome::tabIcon) / 2
                                     : (height() - chrome::tabIcon - chrome::tabGap - capital) / 2;
        const QRect icon((width() - chrome::tabIcon) / 2, top, chrome::tabIcon, chrome::tabIcon);
        painter.setPen(color);
        if (glyph == ui::ControlIcon::Typography) {
            painter.setFont(sample);
            ui::drawOpticalText(painter, QRect(0, icon.top(), width(), icon.height()),
                                QStringLiteral("Aa"), true, {});
        } else {
            const auto vector = ui::controlIcon(glyph, color);
            // Annotate has no QIcon form. The notice painter draws the same glyph.
            if (vector.isNull())
                ui::paintOpeningIcon(painter, glyph, icon, color);
            else
                vector.paint(&painter, icon);
        }
        if (labelsHidden)
            return;
        painter.setFont(label);
        ui::drawOpticalText(
                painter,
                QRect(ui::px(2), icon.bottom() + 1 + chrome::tabGap, width() - ui::px(4), capital),
                text(), true);
    }

private:
    ui::ControlIcon glyph;
    QFont sample;
    bool labelsHidden = false;
};
} // namespace

BottomNavigation::BottomNavigation(QWidget *parent) : ui::Panel(parent, Qt::TopEdge)
{
    setObjectName("rrBottomNavigation");
    setProperty("reader_navigation_bar", true);
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
    stay->setFixedWidth(chrome::cellWidth);
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
    tabLayout = row;
    // Search, Annotate and Bookmarks follow the destinations' order. Only
    // Zen mode shows them.
    const ui::ControlIcon icons[] = { ui::ControlIcon::Chapters,   ui::ControlIcon::Compass,
                                      ui::ControlIcon::Typography, ui::ControlIcon::Layout,
                                      ui::ControlIcon::Light,      ui::ControlIcon::Graph,
                                      ui::ControlIcon::Search,     ui::ControlIcon::Annotate,
                                      ui::ControlIcon::Bookmark };
    const char *names[] = { "Chapters", "Seek",   "Text",     "Layout",   "Light",
                            "Stats",    "Search", "Annotate", "Bookmarks" };
    const QString labels[] = {
        strings::get("navigation.chapters"), strings::get("navigation.seek"),
        strings::get("navigation.text"),     strings::get("navigation.layout"),
        strings::get("navigation.light"),    strings::get("navigation.stats"),
        strings::get("navigation.search"),   strings::get("navigation.annotate"),
        strings::get("navigation.bookmarks")
    };
    for (int i = 0; i < 9; ++i) {
        auto *tab = new Tab(labels[i], icons[i], tabRow);
        tab->setObjectName(QString("rrNav%1").arg(names[i]));
        tab->setVisible(i <= int(NavigationDestination::Stats));
        row->addWidget(tab, 1);
        tabs.append(tab);
        // The tab paints its own feedback. Only its extension into the panel
        // rule belongs to this widget; repainting the bar flashes every tab.
        const auto updateTabBand = [this, tab] {
            if (panelOpen)
                update(QRect(tab->mapTo(this, QPoint()).x(), 0, tab->width(), toolbarBorder));
        };
        connect(tab, &QPushButton::pressed, this, updateTabBand);
        connect(tab, &QPushButton::released, this, updateTabBand);
        connect(tab, &QPushButton::clicked, this, [this, i] {
            if (activated)
                activated(NavigationDestination(i));
        });
    }
    zenFrame = new ZenFrame(this);
    zenFrame->hide();
    hide();
}

void BottomNavigation::paintEvent(QPaintEvent *event)
{
    if (zen && zenAttached) {
        // Square paper under the view's heavy rule, which the frame draws.
        QPainter(this).fillRect(rect(), palette().color(QPalette::Window));
        return;
    }
    if (zen) {
        // Paper fills the group. Its mask leaves the page visible outside the
        // rounded corners.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(zenOutline(rect()), palette().color(QPalette::Window));
        return;
    }
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    if (!panelOpen) {
        ui::paintEdges(painter, rect(), Qt::TopEdge, palette().color(QPalette::WindowText),
                       toolbarBorder);
        return;
    }
    // An open panel draws the rule above the bar and leaves this band empty.
    // Extend the active tab's fill through it, so the fill meets that rule.
    for (auto *tab : tabs)
        if (!tab->isHidden() && (tab->isChecked() || tab->isDown()))
            painter.fillRect(tab->mapTo(this, QPoint()).x(), 0, tab->width(), toolbarBorder,
                             tab->palette().color(QPalette::Text));
}

void BottomNavigation::setPanelOpen(bool open)
{
    if (panelOpen == open)
        return;
    panelOpen = open;
    update(QRect(0, 0, width(), toolbarBorder));
}

void BottomNavigation::select(int destination, bool enabled, bool lightAvailable)
{
    using Nav = NavigationDestination;
    for (int i = 0; i < tabs.size(); ++i) {
        // Each tab gets its final state once. Setting it twice repaints it.
        bool checked = i == destination, available = true;
        if (i == int(Nav::Light))
            available = lightAvailable;
        if (i == int(Nav::Annotate)) {
            checked = zen && zenAnnotating;
            available = zenAnnotateAvailable;
        }
        if (i == int(Nav::Bookmarks)) {
            checked = zen && zenBookmarksOpen;
            available = zenBookmarksAvailable;
        }
        if (panelOpen && tabs[i]->isChecked() != checked)
            update(QRect(tabs[i]->mapTo(this, QPoint()).x(), 0, tabs[i]->width(), toolbarBorder));
        tabs[i]->setChecked(checked);
        tabs[i]->setEnabled(enabled && available);
    }
}

void BottomNavigation::setZen(bool value, bool annotations, bool bookmarks, bool annotating,
                              bool bookmarksOpen, bool annotateAvailable, bool bookmarksAvailable,
                              bool annotationsOnly)
{
    zenAnnotating = annotating;
    zenBookmarksOpen = bookmarksOpen;
    zenAnnotateAvailable = annotateAvailable;
    zenBookmarksAvailable = bookmarksAvailable;
    // Without the Bookmarks feature the list holds annotations only.
    static_cast<Tab *>(tabs[int(NavigationDestination::Bookmarks)])
            ->setDestination(strings::get(annotationsOnly ? "navigation.annotations"
                                                          : "navigation.bookmarks"),
                             annotationsOnly ? ui::ControlIcon::Note : ui::ControlIcon::Bookmark);
    using Nav = NavigationDestination;
    tabs[int(Nav::Text)]->setVisible(!value);
    tabs[int(Nav::Layout)]->setVisible(!value);
    tabs[int(Nav::Search)]->setVisible(value);
    tabs[int(Nav::Annotate)]->setVisible(value && annotations);
    tabs[int(Nav::Bookmarks)]->setVisible(value && bookmarks);
    if (value) {
        tabs[int(Nav::Seek)]->hide();
        tabs[int(Nav::Light)]->hide();
        tabs[int(Nav::Stats)]->hide();
    }
    if (zen != value) {
        zen = value;
        // The page shows outside the group, so the bar is no longer opaque.
        setAttribute(Qt::WA_OpaquePaintEvent, !zen);
        const int outset = zen ? zenOutset() : 0;
        layout()->setContentsMargins(outset, outset + toolbarBorder, outset, 0);
        tabLayout->setContentsMargins(zen ? int(toolbarBorder) : 0, 0, zen ? int(toolbarBorder) : 0,
                                      0);
        zenFrame->setVisible(zen);
        update();
    }
    QPushButton *last = nullptr;
    for (auto *tab : tabs)
        if (!tab->isHidden())
            last = tab;
    for (auto *tab : tabs) {
        const bool end = zen && tab == last;
        if (tab->property("reader_group_end").toBool() != end) {
            tab->setProperty("reader_group_end", end);
            tab->update();
        }
    }
    zenFrame->setGeometry(rect());
    zenFrame->raise();
    updateLabels();
}

void BottomNavigation::setZenAttached(bool attached)
{
    attached = attached && zen;
    if (zenAttached == attached)
        return;
    zenAttached = attached;
    static_cast<ZenFrame *>(zenFrame)->setAttached(attached);
    update();
}

int BottomNavigation::zenTabCount() const
{
    int count = 0;
    for (auto *tab : tabs)
        count += !tab->isHidden();
    return qMax(1, count);
}

int BottomNavigation::zenCellWidth(int available) const
{
    // The group keeps the usual inset from the screen edges.
    const int room = available - 2 * (zenOutset() + int(toolbarBorder) + int(chrome::inset));
    return qMin(int(chrome::zenCellWidth), room / zenTabCount());
}

void BottomNavigation::setTextFace(const QString &face)
{
    static_cast<Tab *>(tabs[int(NavigationDestination::Text)])->setSampleFace(face);
}

void BottomNavigation::setLightTab(bool shown)
{
    tabs[int(NavigationDestination::Light)]->setVisible(shown && !zen);
    updateLabels();
}

void BottomNavigation::setDefaultView(int destination, bool navigateEnabled, bool statsEnabled)
{
    for (auto tab : { NavigationDestination::Seek, NavigationDestination::Stats })
        tabs[int(tab)]->setVisible(
                !zen && int(tab) != destination
                && (tab == NavigationDestination::Seek ? navigateEnabled : statsEnabled));
    updateLabels();
}

void BottomNavigation::setLabelsHidden(bool hidden)
{
    labelsHidden = hidden;
    updateLabels();
}

void BottomNavigation::resizeEvent(QResizeEvent *event)
{
    ui::Panel::resizeEvent(event);
    zenFrame->setGeometry(rect());
    updateLabels();
}

void BottomNavigation::updateLabels()
{
    // A large interface scale leaves the tabs too narrow for some labels.
    // Hide them all, as the option does, rather than elide a few.
    int shown = 0;
    for (auto *tab : tabs)
        shown += !tab->isHidden();
    bool fit = shown > 0;
    const auto font = ui::interfaceFont(chrome::tabText);
    for (auto *tab : tabs)
        if (!tab->isHidden()
            && ui::interfaceTextWidth(tab->text(), font)
                    > (zen ? tab->width() : width() / qMax(1, shown)) - ui::px(4))
            fit = false;
    for (auto *tab : tabs) {
        static_cast<Tab *>(tab)->setLabelsHidden(labelsHidden || !fit);
        tab->setProperty("labels_hidden", labelsHidden || !fit);
    }
}

void BottomNavigation::setReturnLabel(const QString &label, const QString &note)
{
    returnRow->setVisible(!label.isEmpty());
    returnButton->setText(label);
    returnButton->setCaption(note);
    returnButton->setAccessibleName(label + " " + note);
}

// Large text makes the tab icon and label taller. The bar grows by the same
// amount, so they keep the space they have at the standard size. As at the
// standard size, the bar keeps its height when the labels are hidden.
int BottomNavigation::barHeight()
{
    if (ui::textScale() == 1)
        return chrome::actionHeight;
    const int icon = chrome::tabIcon - ui::px(chrome::tabIcon.design());
    const int label = tabCapital(chrome::tabText) - tabCapital(ui::px(chrome::tabText.design()));
    return chrome::actionHeight + icon + label;
}

int BottomNavigation::navigationHeight() const
{
    return barHeight() + (zen ? zenOutset() : 0)
            + (returnRow->isHidden() ? 0 : ui::metrics::links::returnHeight);
}

Navigation::Navigation(QWidget *parent, const ToolbarComposition &controls)
    : bar(new BottomNavigation(parent)),
      toolbar(controls.panel),
      toolbarRows(controls.rows),
      exitAction(controls.exitAction),
      settings(controls.settings),
      bookmarks(controls.bookmarks),
      annotate(controls.annotate),
      clock(controls.clock),
      session(controls.session),
      battery(controls.battery),
      bluetooth(controls.bluetooth),
      wifi(controls.wifi),
      formattingWarning(controls.formattingWarning),
      search(controls.search),
      sync(controls.sync)
{
    zenViewBase = new ZenViewBase(parent);
    zenViewBase->setObjectName("rrZenViewBase");
    upperEdge = new ReadingRule(parent);
    upperEdge->setObjectName("rrNavigationUpperEdge");
    lowerEdge = new ReadingRule(parent);
    lowerEdge->setObjectName("rrNavigationLowerEdge");
    toolbar->setEdges({});
    toolbarRows->setEnabled(false);
    rules = new ToolbarRules(toolbar);
    rules->lower();
    zenMark = new QLabel(strings::get("navigation.zen"), toolbar);
    zenMark->setObjectName("rrZenMark");
    zenMark->setAlignment(Qt::AlignCenter);
    // The section heading caption, inverted to paper capitals on an ink block.
    // The reader's label style would otherwise replace the font.
    ui::readerStyle(zenMark,
                    QString("QLabel{%1font-weight:bold;background:palette(window-text);"
                            "color:palette(window);padding:%2px %3px;}")
                            .arg(ui::styles::caption(), QString::number(ui::px(2)),
                                 QString::number(ui::px(12))));
    auto markFont = ui::captionFont();
    markFont.setBold(true);
    zenMark->setFont(markFont);
    zenMark->hide();
    zenStatsButton = new ZenStatsButton(toolbar);
    zenStatsButton->hide();
    QObject::connect(zenStatsButton, &QPushButton::clicked, zenStatsButton, [this] {
        if (zenStatsToggled)
            zenStatsToggled();
    });
    zenStatsPanel = new ZenStatsPanel(toolbar);
    zenStatsPanel->hide();
    sessionStyle = session->styleSheet();
    settingsStyle = settings->styleSheet();
    bookmarksStyle = bookmarks->styleSheet();
    ui::readerStyle(session, ui::styles::font(chrome::countText) + "font-style:italic;");
    ui::readerStyle(settings, "QPushButton{border:none;padding:0;}");
    settings->setFixedSize(chrome::cellWidth, chrome::actionHeight);
    if (auto *button = dynamic_cast<ui::Button *>(settings)) {
        settingsPaint = button->paintMode();
        button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
    }
    ui::readerStyle(bookmarks, "QPushButton{border:none;padding:0;}");
    bookmarks->setFixedSize(chrome::cellWidth, chrome::actionHeight);
    if (auto *button = dynamic_cast<ui::Button *>(bookmarks)) {
        bookmarksPaint = button->paintMode();
        button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
    }
    if (search) {
        searchStyle = search->styleSheet();
        ui::readerStyle(search, "QPushButton{border:none;padding:0;}");
        search->setFixedSize(chrome::cellWidth, chrome::actionHeight);
        if (auto *button = dynamic_cast<ui::Button *>(search)) {
            searchPaint = button->paintMode();
            button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        }
    }
    if (sync) {
        syncStyle = sync->styleSheet();
        ui::readerStyle(sync, "QPushButton{border:none;padding:0;}");
        sync->setFixedSize(chrome::cellWidth, chrome::actionHeight);
        if (auto *button = dynamic_cast<ui::Button *>(sync)) {
            syncPaint = button->paintMode();
            button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        }
    }
    if (annotate) {
        annotateStyle = annotate->styleSheet();
        ui::readerStyle(annotate, "QPushButton{border:none;padding:0;}");
        annotate->setFixedSize(chrome::cellWidth, chrome::actionHeight);
        if (auto *button = dynamic_cast<ui::Button *>(annotate)) {
            annotatePaint = button->paintMode();
            button->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        }
    }
    if (formattingWarning) {
        formattingWarningStyle = formattingWarning->styleSheet();
        formattingWarning->setFixedSize(chrome::cellWidth, chrome::actionHeight);
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

Navigation::~Navigation()
{
    delete bar;
    delete zenMark;
    delete zenStatsButton;
    delete zenStatsPanel;
    delete rules;
    delete upperEdge;
    delete lowerEdge;
    delete zenViewBase;
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
        annotate->setFixedSize(ui::metrics::control::toolbarButtonSize,
                               ui::metrics::control::toolbarButtonSize);
        if (auto *button = dynamic_cast<ui::Button *>(annotate)) {
            button->setPaintMode(annotatePaint);
        }
    }
    if (search) {
        search->setStyleSheet(searchStyle);
        search->setFixedSize(ui::metrics::control::toolbarButtonSize,
                             ui::metrics::control::toolbarButtonSize);
        if (auto *button = dynamic_cast<ui::Button *>(search)) {
            button->setPaintMode(searchPaint);
        }
    }
    session->setStyleSheet(sessionStyle);
    if (sync) {
        sync->setStyleSheet(syncStyle);
        sync->setFixedSize(ui::metrics::control::toolbarButtonSize,
                           ui::metrics::control::toolbarButtonSize);
        if (auto *button = dynamic_cast<ui::Button *>(sync))
            button->setPaintMode(syncPaint);
    }
    settings->setStyleSheet(settingsStyle);
    bookmarks->setStyleSheet(bookmarksStyle);
    settings->setFixedSize(ui::metrics::control::toolbarButtonSize,
                           ui::metrics::control::toolbarButtonSize);
    bookmarks->setFixedSize(ui::metrics::control::toolbarButtonSize,
                            ui::metrics::control::toolbarButtonSize);
    toolbar->setEdges(Qt::BottomEdge);
    toolbarRows->setEnabled(true);
    toolbarRows->invalidate();
}

QRect Navigation::panelBounds(QSize size, int height) const
{
    return QRect(0, contentBottom(size) - height, size.width(), height);
}

void Navigation::stackPanel(QWidget *panel) const
{
    // Keep the header rule in place when a bottom panel opens. Raising the
    // rule again would repaint unchanged toolbar pixels beneath it.
    panel->stackUnder(upperEdge);
}

void Navigation::placeZenToolbar(QSize size, const QVector<QWidget *> &actions,
                                 const NavigationState &state)
{
    // The back arrow, reading stats and Settings in a centered group, below a
    // status line with the Zen mark, the clock and a low battery. The other
    // actions are tabs in the bottom group, or hidden. Like the bottom group,
    // it keeps the usual inset from the screen edges.
    const int cell = qMin(int(chrome::zenTopCellWidth),
                          (size.width() - 2 * (int(chrome::zenRule) + int(chrome::inset))) / 3);
    const int groupWidth = 3 * cell, left = (size.width() - groupWidth) / 2;
    for (auto *action : actions)
        if (action && action != settings)
            action->hide();
    settings->setFixedWidth(cell);
    settings->setGeometry(left + 2 * cell, statusHeight(), cell, chrome::actionHeight);
    // The arrow alone. Its accessible name stays Exit Kobalt or Library.
    if (auto *title = exitAction->findChild<QWidget *>("rrReturnTitle"))
        title->hide();
    exitAction->setProperty("reader_arrow_only", true);
    exitAction->setGeometry(left, statusHeight(), cell, chrome::actionHeight);
    if (exitAction->property("reader_return_divider").toBool()) {
        exitAction->setProperty("reader_return_divider", false);
        exitAction->update();
    }
    zenStatsButton->setChecked(zenStatsShown);
    zenStatsButton->setGeometry(left + cell, statusHeight(), cell, chrome::actionHeight);
    zenStatsButton->show();
    const int statsTop = statusHeight() + chrome::actionHeight;
    if (zenStatsShown) {
        const auto unit = [](int minutes) {
            QVector<QPair<QString, QString>> parts;
            if (minutes < 60)
                return parts << qMakePair(QString::number(minutes), strings::get("zen.minutes"));
            parts << qMakePair(QString::number(minutes / 60), strings::get("zen.hours"));
            if (minutes % 60)
                parts << qMakePair(QString::number(minutes % 60), strings::get("zen.minutes"));
            return parts;
        };
        using Figure = ZenStatsPanel::Figure;
        QVector<Figure> figures{
            { unit(state.sessionMinutes), strings::get("zen.this_session"), false },
            { { qMakePair(QLocale().toString(state.pageTurns), QString()) },
              strings::get("zen.pages_turned"),
              false },
            { state.chapterMinutes < 0 ? QVector<QPair<QString, QString>>{ qMakePair(
                                                 QStringLiteral("–"), QString()) }
                                       : unit(state.chapterMinutes),
              strings::get("zen.chapter_left"),
              state.chapterMinutes >= 0 && state.chapterApproximate },
        };
        static_cast<ZenStatsPanel *>(zenStatsPanel)->setFigures(figures);
        zenStatsPanel->setGeometry(left, statsTop, groupWidth,
                                   chrome::zenStatsHeight - int(chrome::zenRule));
    }
    zenStatsPanel->setVisible(zenStatsShown);
    if (rules->property("zen_stats_top").toInt() != (zenStatsShown ? statsTop : 0)) {
        rules->setProperty("zen_stats_top", zenStatsShown ? statsTop : 0);
        rules->update();
    }
    // The status row lies between the top of the status line and its
    // divider. The clock's digits, the Zen mark and the battery are centered
    // on it by their ink, not by their text boxes.
    const int line = ui::metrics::panel::statusHeight;
    const int rowTop = statusHeight() - (statusVisible ? int(chrome::statusHeight) : 0);
    const int rowCenter =
            rowTop + (int(chrome::statusHeight) - int(ui::metrics::control::border)) / 2;
    // The clock centers its digits' ink in its label, so the label centers
    // on the row.
    if (auto *label = dynamic_cast<ui::InterfaceLabel *>(clock))
        label->setOpticalCentering(true);
    const int clockTop = rowCenter - line / 2;
    const int clockWidth = qMin(ui::textPx(160), clock->sizeHint().width());
    clock->setGeometry(left + (groupWidth - clockWidth) / 2, clockTop, clockWidth, line);
    // The mark keeps the same space to its left as above and below it, and
    // the battery's icon the same space to its right.
    zenMark->adjustSize();
    const int markTop = rowCenter - zenMark->height() / 2, margin = markTop - rowTop;
    zenMark->move(left + margin, markTop);
    clock->setVisible(statusVisible);
    zenMark->setVisible(statusVisible);
    const int level = battery->level();
    battery->setVisible(statusVisible && level >= 0 && level <= 20);
    // The icon's ink ends before the indicator's right edge.
    const int batteryInk = battery->width() - int(ui::metrics::panel::batteryIconSize) + 1
            + qCeil(ui::batteryWidth(ui::metrics::panel::batteryBodyHeight));
    battery->move(left + groupWidth - margin - batteryInk, clockTop);
    session->hide();
    if (bluetooth)
        bluetooth->hide();
    if (wifi)
        wifi->hide();
    // Only the group paints. The page shows beside it.
    const auto shape = roundedMask(QRect(left - int(chrome::zenRule), 0,
                                         groupWidth + 2 * int(chrome::zenRule), toolbarHeight()),
                                   false);
    // Setting a mask repaints the group, so an unchanged mask is not set again.
    if (toolbar->mask() != shape)
        toolbar->setMask(shape);
    if (rules->property("zen_left").toInt() != left
        || rules->property("zen_width").toInt() != groupWidth
        || rules->property("zen_cell").toInt() != cell) {
        rules->setProperty("zen_left", left);
        rules->setProperty("zen_width", groupWidth);
        rules->setProperty("zen_cell", cell);
        rules->update();
    }
}

void Navigation::placeToolbar(QSize size, const QVector<QWidget *> &actions, bool zen)
{
    // Undo Zen mode's top group first, so the exit caption counts below.
    if (!toolbar->mask().isEmpty())
        toolbar->clearMask();
    zenMark->hide();
    zenStatsButton->hide();
    if (auto *label = dynamic_cast<ui::InterfaceLabel *>(clock))
        label->setOpticalCentering(false);
    zenStatsPanel->hide();
    if (exitAction->property("reader_arrow_only").toBool()) {
        exitAction->setProperty("reader_arrow_only", false);
        if (auto *title = exitAction->findChild<QWidget *>("rrReturnTitle"))
            title->show();
        exitAction->update();
    }
    if (rules->property("zen_width").toInt()) {
        rules->setProperty("zen_width", 0);
        rules->update();
    }
    clock->setVisible(statusVisible);
    battery->setVisible(statusVisible);
    // Zen mode hides the minutes-open count, also under Chapters and Bookmarks.
    session->setVisible(statusVisible && !zen && !session->text().isEmpty());
    settings->setFixedWidth(chrome::cellWidth);
    int rightActions = 0;
    for (auto *action : actions)
        if (action && !action->isHidden())
            ++rightActions;
    // A large interface scale can leave too little room for the exit caption.
    // The arrow then keeps a cell of its own, as each action does.
    // When the caption nearly reaches the actions, the exit action meets them,
    // so its divider and theirs draw as one line.
    const int exitWidth = exitAction->sizeHint().width();
    const int exitRoom = size.width() - rightActions * chrome::cellWidth;
    const int exitCell = exitWidth > exitRoom              ? int(chrome::cellWidth)
            : exitRoom - exitWidth < chrome::cellWidth / 2 ? exitRoom
                                                           : exitWidth;
    // A narrow gap should not look like an empty toolbar cell.
    const bool returnDivider = exitRoom - exitCell >= int(chrome::cellWidth);
    if (exitAction->property("reader_return_divider").toBool() != returnDivider) {
        exitAction->setProperty("reader_return_divider", returnDivider);
        exitAction->update();
    }
    // The status line keeps its proportions as large text enlarges it. It
    // sits at the bottom of a taller camera band, or inside the action row.
    const int statusTop = inlineStatus ? statusHeight()
                    + (int(chrome::actionHeight) - int(ui::metrics::panel::statusHeight)) / 2
                                       : statusHeight()
                    - (statusVisible ? int(ui::metrics::chrome::statusHeight) : 0) + ui::textPx(10);
    const int statusLeft = inlineStatus ? exitCell + int(chrome::inset) : int(chrome::inset);
    const int statusRight = size.width()
            - (inlineStatus ? rightActions * int(chrome::cellWidth) : 0) - chrome::inset;
    // The final width at once: a full width first, then the text width, would
    // repaint the toolbar on every update.
    clock->setGeometry(statusLeft, statusTop, qMin(ui::textPx(160), clock->sizeHint().width()),
                       ui::metrics::panel::statusHeight);
    battery->move(statusRight - battery->width(), statusTop);
    const int statusGap = ui::metrics::panel::bluetoothGap;
    QWidget *previousStatus = nullptr;
    const auto placeStatus = [&](QWidget *indicator) {
        const int x = previousStatus ? previousStatus->x() - statusGap - indicator->width()
                                     : battery->x() - statusGap
                        - (int(ui::metrics::panel::bluetoothIconSize) + indicator->width()) / 2;
        indicator->move(x, statusTop);
        previousStatus = indicator;
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
    updateSessionGeometry();
    int actionRight = size.width();
    for (auto *action : actions) {
        if (!action || action->isHidden())
            continue;
        actionRight -= chrome::cellWidth;
        action->setGeometry(actionRight, statusHeight(), chrome::cellWidth, chrome::actionHeight);
    }
    exitAction->setGeometry(0, statusHeight(), exitCell, chrome::actionHeight);
    if (rules->property("right_actions").toInt() != rightActions) {
        rules->setProperty("right_actions", rightActions);
        rules->update();
    }
}

void Navigation::update(QSize size, const NavigationState &state)
{
    // Zen mode's top group belongs to the reading menu alone. Chapters,
    // Bookmarks, Search and Settings keep the regular top bar.
    zenTop = state.zen && !state.chaptersOpen && !state.settings->isVisible() && !state.searchOpen;
    zenStatsShown = zenTop && state.zenStats;
    state.footer->hide();
    toolbar->setGeometry(0, 0, size.width(), toolbarHeight());
    rules->setProperty("status_height", statusHeight());
    rules->setGeometry(toolbar->rect());
    rules->show();
    rules->raise();
    const QVector<QWidget *> actions{
        settings, bookmarks, annotate, search, sync, formattingWarning
    };
    if (zenTop)
        placeZenToolbar(size, actions, state);
    else
        placeToolbar(size, actions, state.zen);
    for (auto *action : state.topActions)
        action->hide();
    const bool scrubberShown = scrubber
            && (state.scrubberVisible || state.active == int(NavigationDestination::Seek)
                || scrubber->hasActions())
            && state.visible
            && (state.active < 0 || state.active == int(NavigationDestination::Seek))
            && !state.chaptersOpen && !state.settings->isVisible() && !state.appearance->isVisible()
            && !state.stats->isVisible() && !state.light->isVisible();
    bar->setReturnLabel(scrubber ? QString() : state.returnLabel, state.returnNote);
    bar->setZen(state.zen, state.annotationsEnabled, state.bookmarksEnabled, state.annotating,
                state.bookmarksOpen, annotate && annotate->isEnabled(),
                bookmarks && bookmarks->isEnabled(), state.annotationsOnly);
    // Under Chapters and Bookmarks the Zen tabs attach to the view.
    const bool zenView = state.zen && state.chaptersOpen && !state.settings->isVisible();
    bar->setZenAttached(zenView);
    const int height = bar->navigationHeight();
    if (state.zen) {
        // A centered group, as wide as its visible tabs.
        const int width = bar->zenTabCount() * bar->zenCellWidth(size.width())
                + 2 * (zenOutset() + int(toolbarBorder));
        bar->setGeometry((size.width() - width) / 2, size.height() - safeBottom - height, width,
                         height);
        // Attached, the group is square from the view's rule down. Above
        // that rule, the view shows.
        const auto shape = zenView
                ? QRegion(0, zenOutset(), bar->width(), bar->height() - zenOutset())
                : roundedMask(QRect(QPoint(), bar->size()), true);
        if (bar->mask() != shape)
            bar->setMask(shape);
    } else {
        bar->setGeometry(0, size.height() - safeBottom - height, size.width(), height);
        if (!bar->mask().isEmpty())
            bar->clearMask();
    }
    bar->setVisible(state.visible && !state.settings->isVisible());
    if (scrubber) {
        scrubber->setBarVisible(state.scrubberVisible);
        scrubber->setGeometry(0, toolbarHeight(), size.width(),
                              qMax(1, bar->y() - toolbarHeight()));
        scrubber->setVisible(scrubberShown);
        if (scrubberShown)
            scrubber->stackUnder(upperEdge);
    }
    if (!state.visible) {
        upperEdge->hide();
        lowerEdge->hide();
        zenViewBase->hide();
        return;
    }
    bar->setLightTab(state.lightAvailable);
    bar->setDefaultView(state.defaultView, state.navigateEnabled, state.statsEnabled);
    bar->setLabelsHidden(state.hideTabLabels);
    for (auto *panel : { state.appearance, state.stats })
        if (panel->isVisible())
            panel->setGeometry(panelBounds(size, panel->height()));
    // Chapters covers the header's action row with its own bar; the status strip stays.
    if (state.chaptersOpen) {
        // In Zen mode the view ends on the attached tabs' heavy rule, as it
        // ends on the regular bar's rule.
        state.chapters->setGeometry(0, statusHeight(), size.width(),
                                    bar->y() + (zenView ? zenOutset() : 0) - statusHeight());
    }
    bar->select(state.active, state.enabled, state.lightAvailable);
    int top = toolbarHeight(), bottom = bar->y();
    if (state.settings->isVisible())
        top = state.settings->geometry().bottom() + 1;
    for (auto *panel : { state.appearance, state.stats, state.light })
        if (panel->isVisible())
            bottom = qMin(bottom, panel->y());
    const bool attachedScrubber = scrubberShown && state.scrubberVisible;
    bar->setPanelOpen(bottom < bar->y() || attachedScrubber);
    // Zen mode's groups draw their own rules. Under Chapters and Bookmarks the
    // paper below the view draws the heavy rule above the attached tabs. Each
    // rule gets its final state once: showing and hiding it again repaints it.
    if (!state.chaptersOpen && !state.zen) {
        upperEdge->setGeometry(0, top - ui::px(2), size.width(), ui::px(2));
        upperEdge->show();
        lowerEdge->setGeometry(0, bottom, size.width(),
                               attachedScrubber ? int(ui::metrics::control::border)
                                                : int(toolbarBorder));
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
    if (zenView) {
        const int end = bar->y() + zenOutset();
        // The view fills the screen, so no page text shows beside the
        // bottom group. Stack the paper only when it appears: restacking
        // repaints it.
        zenViewBase->setGeometry(0, end, size.width(), size.height() - end);
        if (zenViewBase->isHidden()) {
            zenViewBase->stackUnder(lowerEdge);
            zenViewBase->show();
        }
    } else {
        zenViewBase->hide();
    }
}

void Navigation::updateSessionGeometry()
{
    clock->resize(qMin(ui::textPx(160), clock->sizeHint().width()), clock->height());
    int right = battery->x();
    if (bluetooth && bluetooth->active())
        right = qMin(right, bluetooth->x());
    if (wifi && wifi->active())
        right = qMin(right, wifi->x());
    right = qMax(0, right - int(chrome::inset));
    const int left = qMin(right, clock->geometry().right() + 1 + int(chrome::inset));
    // Center the session in the gap after the time text, without reserving
    // the clock's unused width or moving the status icons.
    session->setGeometry(left, clock->y(), right - left, ui::metrics::panel::statusHeight);
}

bool Navigation::contains(QPoint global) const
{
    return bar->isVisible() && QRect(bar->mapToGlobal(QPoint()), bar->size()).contains(global);
}
} // namespace kobalt::reader
