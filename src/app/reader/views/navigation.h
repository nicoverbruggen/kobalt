#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/options.h"
#include "ui/controls/panels.h"
#include "ui/style/metrics.h"
#include <QWidget>
#include <functional>
#include <QVector>

class QBoxLayout;
class QLayout;
class QSpacerItem;
class QStackedWidget;
class QPushButton;
class QLabel;
class QFrame;

namespace kobalt {
namespace ui {
class Panel;
class DescribedOption;
class CaptionButton;
class PageScrubber;
} // namespace ui

struct ToolbarComposition
{
    ui::Panel *panel = nullptr;
    QLayout *rows = nullptr;
    QWidget *exitAction = nullptr;
    QPushButton *settings = nullptr, *bookmarks = nullptr, *annotate = nullptr;
    QLabel *clock = nullptr, *session = nullptr, *battery = nullptr;
    ui::BluetoothIndicator *bluetooth = nullptr;
    ui::WifiIndicator *wifi = nullptr;
    QPushButton *formattingWarning = nullptr;
    QPushButton *search = nullptr, *sync = nullptr;
};
enum class NavigationDestination { Chapters, Seek, Text, Layout, Light, Stats, Search };

struct NavigationState
{
    QWidget *footer, *appearance, *stats, *chapters;
    bool chaptersOpen, visible, enabled, lightAvailable;
    int active;
    QString returnLabel;
    QVector<QWidget *> topActions;
    QWidget *settings, *light;
    QString returnNote;
    bool scrubberVisible = true;
    int defaultView = -1;
    bool hideTabLabels = false;
};
} // namespace kobalt

namespace kobalt::reader {
// Navigation owns its tabs; reader panels retain their existing controls and actions.
class BottomNavigation : public ui::Panel
{
public:
    explicit BottomNavigation(QWidget *parent);
    std::function<void(NavigationDestination)> activated;
    void select(int destination, bool enabled, bool lightAvailable);
    static int barHeight();
    void setPanelOpen(bool open);
    void setTextFace(const QString &face);
    void setReturnLabel(const QString &label, const QString &note);
    void setLightTab(bool shown);
    void setDefaultView(int destination);
    void setLabelsHidden(bool hidden);
    std::function<void()> returnRequested;
    std::function<void()> stayRequested;
    int navigationHeight() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // Shows the labels unless the option hides them or one does not fit.
    void updateLabels();
    bool panelOpen = false;
    bool labelsHidden = false;
    QVector<QPushButton *> tabs;
    ui::CaptionButton *returnButton;
    QWidget *returnRow;
};

// Navigation arranges existing panels and reports actions. It does not own book state.
class Navigation final
{
public:
    Navigation(QWidget *parent, const ToolbarComposition &toolbar);
    ~Navigation();
    Navigation(const Navigation &) = delete;
    Navigation &operator=(const Navigation &) = delete;
    std::function<void(NavigationDestination)> activated;
    std::function<void()> returnRequested;
    std::function<void()> stayRequested;

    int statsTopPadding() const { return ui::metrics::chrome::statsTopPadding; }

    void setStatusVisible(bool visible) { statusVisible = visible; }

    void setTextFace(const QString &face) { bar->setTextFace(face); }

    // A camera band at the top holds the status line when it is tall enough.
    // Otherwise the status line and the actions start below the band.
    void setSafeArea(int top, int bottom)
    {
        safeTop = top;
        safeBottom = bottom;
    }

    // A short screen keeps one bar: the status line moves into the action
    // row, between the exit action and the actions on the right.
    void setInlineStatus(bool value) { inlineStatus = value; }

    int statusHeight() const
    {
        const int status =
                statusVisible && !inlineStatus ? int(ui::metrics::chrome::statusHeight) : 0;
        return safeTop >= status ? safeTop : safeTop + status;
    }

    int toolbarHeight() const { return statusHeight() + ui::metrics::chrome::actionHeight; }

    Qt::Edge panelEdge() const { return Qt::TopEdge; }

    QRect panelBounds(QSize size, int height) const;
    void stackPanel(QWidget *panel) const;

    void setScrubber(ui::PageScrubber *widget) { scrubber = widget; }

    void update(QSize size, const NavigationState &state);
    void updateSessionGeometry();

    int navigationTop() const { return bar->y(); }

    bool isVisible() const { return bar->isVisible(); }

    int contentBottom(QSize size) const
    {
        return size.height() - safeBottom - BottomNavigation::barHeight();
    }

    bool contains(QPoint global) const;

private:
    ui::PageScrubber *scrubber = nullptr;
    bool statusVisible = true;
    bool inlineStatus = false;
    int safeTop = 0, safeBottom = 0;
    BottomNavigation *bar;
    ui::Panel *toolbar;
    QLayout *toolbarRows;
    QWidget *exitAction, *rules, *settings, *bookmarks, *annotate, *upperEdge, *lowerEdge;
    QLabel *clock, *session, *battery;
    ui::BluetoothIndicator *bluetooth;
    ui::WifiIndicator *wifi;
    QPushButton *formattingWarning;
    QPushButton *search, *sync;
    QString searchStyle, syncStyle;
    ui::ButtonPaintMode syncPaint = ui::ButtonPaintMode::Native;
    ui::ButtonPaintMode searchPaint = ui::ButtonPaintMode::Native;
    QString sessionStyle, settingsStyle, bookmarksStyle, annotateStyle, formattingWarningStyle;
    ui::ButtonPaintMode annotatePaint = ui::ButtonPaintMode::Native;
    ui::ButtonPaintMode settingsPaint = ui::ButtonPaintMode::Native,
                        bookmarksPaint = ui::ButtonPaintMode::Native,
                        formattingWarningPaint = ui::ButtonPaintMode::Native;
};
} // namespace kobalt::reader
