#pragma once
#include "buttons.h"
#include <QLabel>
#include <QPointer>

namespace kobalt::ui {
// Uses the deepest visible pager, including a list inside a paged settings sheet.
// A disabled page arrow still consumes the request at a text/list boundary.
bool turnVisiblePageControl(QWidget *scope, int direction);
// A full-width bar with a cell at each end and the count between them.
// It hides itself when there is only one page.
// Horizontal swipes in the parent panel use its arrows. Controls with their
// own gestures can opt out with the reader_owns_swipes property.
// reader_swipe_hold preserves a row's 550 ms hold action instead of paging.
class PagerBar : public QWidget {
public:
    PagerBar(QWidget *parent, const QString &prefix);
    ~PagerBar() override;
    void setPage(int page, int count, const QString &label = {}, bool wrap = false);
    // Coordinates in the pager's parent. Never reserve space or cover list rows.
    void setBlankArea(const QRect &bounds);
    // Resolve a user turn using the last presented page and list bounds.
    int turnTarget(int direction) const;
    QLabel *counter;
    CellButton *previous, *next;

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    int page = 0, count = 1;
    bool wrap = false;
    QPointer<QLabel> continuationHint;
    QRect blankArea;
    void updateHint();
};
class PageControls : public QWidget {
public:
    PageControls(QWidget *parent, const QString &prefix, bool compact = false);
    void setPage(int page, int count);
    void setCompact(bool value);
    QLabel *counter;
    QPushButton *previous, *next;

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    bool compact;
    void arrange();
};
class PageNavigation {
public:
    PageNavigation(QWidget *parent, const QString &prefix);
    void layout(const QSize &size, bool compact = false);
    void setPagingVisible(bool visible);
    void setPage(int page, int count);
    QLabel *title, *counter;
    QPushButton *back, *previous, *next;

private:
    PageControls *paging;
};
}
