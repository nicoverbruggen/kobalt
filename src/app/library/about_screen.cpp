#include "about_screen.h"
#include "app/about/about_page.h"
#include "app/reader/settings/acknowledgments.h"
#include "ui/controls/buttons.h"
#include "ui/controls/paging.h"
#include "ui/controls/text.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "support/strings.h"
#include <QLayout>
#include <QPainter>

namespace kobalt::library {
namespace chrome = ui::metrics::chrome;
namespace metrics = ui::metrics;
namespace styles = ui::styles;
using ui::readerStyle;

namespace {
// Center the title's visible ink on the Back cell, as the navigation labels do.
class AboutTitle final : public ui::InterfaceLabel
{
public:
    using ui::InterfaceLabel::InterfaceLabel;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(QPalette::WindowText));
        ui::drawOpticalText(painter, contentsRect(), text());
    }
};
} // namespace

AboutScreen::AboutScreen(QWidget *parent) : ui::Panel(parent)
{
    setObjectName("libraryAbout");
    back = new ui::CellButton(ui::ControlIcon::Back, this, chrome::backIcon);
    back->setObjectName("libraryAboutBack");
    back->setRule(Qt::RightEdge);
    back->setAccessibleName(strings::get("common.back"));
    title = new AboutTitle(strings::get("library.about"), this);
    title->setObjectName("libraryAboutTitle");
    title->setTextFormat(Qt::PlainText);
    readerStyle(title, styles::brand(chrome::titleText) + "background:transparent;");
    viewport = new QWidget(this);
    content = new AboutPage(viewport);
    pager = new ui::PagerBar(this, "libraryAbout");
    connect(back, &QPushButton::clicked, this, &QWidget::hide);
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        page = pager->turnTarget(-1);
        arrange();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        page = pager->turnTarget(1);
        arrange();
    });
    credits = new Acknowledgments(this);
    credits->finished = [this] { credits->hide(); };
    credits->hide();
    connect(content->acknowledgmentsButton, &QPushButton::clicked, this,
            [this] { credits->open(); });
    hide();
}

void AboutScreen::open()
{
    page = 0;
    credits->hide();
    arrange();
    show();
    raise();
}

void AboutScreen::dismiss()
{
    if (credits->isVisible())
        credits->goBack();
    else
        hide();
}

void AboutScreen::pageButton(int direction)
{
    ui::turnVisiblePageControl(credits->isVisible() ? static_cast<QWidget *>(credits) : this,
                               direction);
}

void AboutScreen::resizeEvent(QResizeEvent *)
{
    arrange();
}

void AboutScreen::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, chrome::actionHeight - chrome::heavyRule, width(), chrome::heavyRule,
                     palette().color(QPalette::WindowText));
}

void AboutScreen::arrange()
{
    const int bar = chrome::actionHeight - chrome::heavyRule;
    back->setGeometry(0, 0, chrome::cellWidth, bar);
    title->setGeometry(chrome::cellWidth + chrome::inset, 0,
                       qMax(1, width() - chrome::cellWidth - 2 * chrome::inset), bar);
    const int top = chrome::actionHeight;
    const int natural = content->fitWidth(width());
    const bool overflow = natural > height() - top;
    const int room = qMax(1, height() - top - (overflow ? int(chrome::actionHeight) : 0));
    const int body = qMax(natural, room);
    content->resize(width(), body);
    content->layout()->activate();
    const auto starts = content->pageStarts(room);
    page = qBound(0, page, int(starts.size()) - 1);
    const int end = page + 1 < starts.size() ? starts[page + 1] : body;
    const int used = qMin(room, end - starts[page]);
    viewport->setGeometry(0, top, width(), used);
    content->move(0, -starts[page]);
    pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
    pager->setPage(page, starts.size());
    pager->setBlankArea(
            { chrome::inset, top + used, qMax(1, width() - 2 * int(chrome::inset)), room - used });
    credits->setGeometry(rect());
    if (credits->isVisible())
        credits->raise();
}
} // namespace kobalt::library
