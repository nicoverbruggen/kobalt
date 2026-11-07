#pragma once
#include "ui/controls/panels.h"

class QPushButton;

namespace kobalt {
class AboutPage;
class Acknowledgments;

namespace ui {
class CellButton;
class PagerBar;
} // namespace ui

namespace library {
// Library presentation keeps system bars, safe areas and navigation in its host.
class AboutScreen final : public ui::Panel
{
public:
    explicit AboutScreen(QWidget *parent);
    void open();
    void dismiss();
    void pageButton(int direction);

protected:
    void resizeEvent(QResizeEvent *) override;
    void paintEvent(QPaintEvent *) override;

private:
    ui::CellButton *back;
    QLabel *title;
    QWidget *viewport;
    AboutPage *content;
    ui::PagerBar *pager;
    Acknowledgments *credits;
    int page = 0;
    void arrange();
};
} // namespace library
} // namespace kobalt
