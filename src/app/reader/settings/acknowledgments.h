#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include <QFrame>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include "ui/style/rich_text.h"
#include <functional>

namespace kobalt {
class LicenseText : public QWidget
{
public:
    explicit LicenseText(QWidget *parent);
    void setText(const QString &text);
    // Lays the notice out first if its text or size changed.
    int pages();
    int page = 0;

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *event) override;

private:
    ui::PrivateRichText document;
    bool stale = true;
    void ensureLayout();
};

// The project list and each license page use the header geometry: a caption
// strip, a back cell and the title in the brand serif.
class Acknowledgments : public QFrame
{
public:
    explicit Acknowledgments(QWidget *parent);
    std::function<void()> finished;
    void open();
    // Return from a license to the projects, then to the owning About view.
    void goBack();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    QJsonArray entries;
    ui::CellButton *back;
    ui::PagerBar *pager;
    QLabel *title, *section, *detail, *intro;
    QLabel *description, *version, *license, *url;
    QVector<QPushButton *> rows;
    QVector<ui::ListHeader *> headings;
    // Each list page holds entry indexes; -1 marks a group heading before the next entry.
    QVector<QVector<int>> listPages;
    LicenseText *text;
    // listPage -1 asks redraw() for the page that holds listEntry, so rotation keeps the place.
    int entry = -1, listPage = 0, listEntry = 0, divider = 0;
    void select(int index);
    void turnPage(int direction);
    void redraw(bool paging = false);
};
} // namespace kobalt
