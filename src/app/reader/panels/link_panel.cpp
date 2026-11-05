#include "ui/controls/text.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include "link_panel.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include <QPainter>

namespace kobalt {
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;

LinkPanel::LinkPanel(QWidget *parent) : Panel(parent, Qt::TopEdge)
{
    setEdgeWidth(chrome::heavyRule);
    setObjectName("rrLinkPanel");
    heading = new ui::PrivateTextLabel(this);
    heading->setObjectName("rrLinkHeading");
    QFont headingFont = ui::interfaceFont(chrome::countText);
    headingFont.setCapitalization(QFont::AllUppercase);
    heading->setFont(headingFont);
    heading->setTextFormat(Qt::PlainText);
    ui::readerStyle(heading, ui::styles::font(chrome::countText) + "background:transparent;");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrLinkClose");
    close->setRule(Qt::LeftEdge);
    close->setAccessibleName(strings::get("link_panel.close_footnote_or_link"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (dismissed)
            dismissed();
    });
    text = new ui::PagedText(this);
    text->setObjectName("rrLinkText");
    text->setTextFont(ui::interfaceFont(chrome::labelText));
    text->setMargins(QMargins(chrome::inset, chrome::blockTop, chrome::inset, chrome::blockTop));
    pages = new ui::PagerBar(this, "rrLink");
    pages->counter->setObjectName("rrLinkPages");
    connect(pages->previous, &QPushButton::clicked, this, [this] {
        text->setPage(text->page() - 1);
        refresh();
    });
    connect(pages->next, &QPushButton::clicked, this, [this] {
        text->setPage(text->page() + 1);
        refresh();
    });
    // Like Return to page, the row names where it goes: the chapter that
    // holds the note, usually the book's notes section.
    passage = new ui::CaptionButton(ui::ControlIcon::Link, this);
    passage->setObjectName("rrLinkOpen");
    passage->setText(strings::get("link_panel.show_in_book"));
    passage->setChevron(true);
    passage->setRule(Qt::TopEdge);
    connect(passage, &QPushButton::clicked, this, [this] {
        if (!destination.isEmpty() && navigate)
            navigate(destination);
    });
    hide();
}

int LinkPanel::rowTop() const
{
    return lower ? chrome::heavyRule : 0;
}

void LinkPanel::open(const LinkDetails &details, const QRect &reference, const QRect &readingArea)
{
    destination = details.target;
    heading->setText(details.kind == LinkKind::Footnote ? strings::get("link_panel.footnote")
                                                        : strings::get("link_panel.link"));
    QString html = details.html;
    if (!details.error.isEmpty())
        html = "<p>" + details.error.toHtmlEscaped() + "</p>";
    passage->setEnabled(true);
    passage->setVisible(!destination.isEmpty());
    // Headings such as "Footnotes:" or "Chapter I." read as a fragment after "In".
    QString chapter = details.chapter.trimmed();
    while (chapter.endsWith(':') || chapter.endsWith('.'))
        chapter.chop(1);
    passage->setCaption(chapter.isEmpty() ? QString()
                                          : strings::get("link_panel.in_chapter").arg(chapter));
    passage->setAccessibleName(passage->text()
                               + (chapter.isEmpty() ? QString() : " " + passage->caption()));
    const auto area = readingArea.isEmpty() ? parentWidget()->rect() : readingArea;
    const int h =
            qMin(parentWidget()->height() * metrics::lookup::heightPercent / 100, area.height());
    // Like the dictionary, the sheet takes the half without the reference.
    lower = reference.center().y() < area.center().y();
    setEdges(lower ? Qt::TopEdge : Qt::BottomEdge);
    setGeometry(area.left(), lower ? area.bottom() + 1 - h : area.top(), area.width(), h);
    text->setHtml(html);
    show();
    raise();
    arrange();
}

void LinkPanel::resizeEvent(QResizeEvent *)
{
    arrange();
}

void LinkPanel::arrange()
{
    const int top = rowTop(), bottom = height() - (lower ? 0 : chrome::heavyRule);
    heading->setGeometry(chrome::inset, top,
                         qMax(1, width() - chrome::cellWidth - 2 * chrome::inset),
                         chrome::actionHeight);
    close->setGeometry(width() - chrome::cellWidth, top, chrome::cellWidth, chrome::actionHeight);
    const int textTop = top + chrome::actionHeight + metrics::control::border;
    int textBottom = bottom;
    if (!passage->isHidden()) {
        textBottom -= metrics::links::returnHeight;
        passage->setGeometry(0, textBottom, width(), metrics::links::returnHeight);
    }
    // A single page needs no pager, so it can use the whole sheet. Measuring
    // without the pager can clamp the page; restore it afterwards.
    const int wanted = text->page();
    text->setGeometry(0, textTop, width(), qMax(1, textBottom - textTop));
    if (text->pages() > 1) {
        textBottom -= chrome::actionHeight;
        text->setGeometry(0, textTop, width(), qMax(1, textBottom - textTop));
        passage->move(0, textBottom);
    }
    text->setPage(wanted);
    // A lower sheet has no edge where it meets the footer strip, so the row
    // closes it. The pager and an upper sheet's heavy rule have their own.
    passage->setRule(lower && text->pages() <= 1 ? ui::Edges(Qt::TopEdge) | Qt::BottomEdge
                                                 : ui::Edges(Qt::TopEdge));
    pages->setGeometry(0, bottom - chrome::actionHeight, width(), chrome::actionHeight);
    refresh();
}

void LinkPanel::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, rowTop() + chrome::actionHeight, width(), metrics::control::border,
                     palette().color(QPalette::Mid));
}

void LinkPanel::refresh()
{
    // Counting the pages lays the text out and clamps its page first.
    const int count = text->pages();
    pages->setPage(text->page(), count);
}

namespace {
class WebsiteAddressScrollArea final : public QScrollArea
{
public:
    explicit WebsiteAddressScrollArea(QWidget *parent) : QScrollArea(parent) { }

    QSize minimumSizeHint() const override
    {
        // The dialog supplies the viewport size. Keep native scrollbar hints
        // out of the card's minimum-size calculation.
        return QSize(0, 0);
    }
};
} // namespace

WebsiteDialog::WebsiteDialog(QWidget *parent)
    : ui::ChoiceModal(parent, { " ", strings::get("link_panel.browser_confirmation"), {} },
                      { strings::get("common.cancel"), "rrLinkNo" },
                      { strings::get("link_panel.open_browser"), "rrLinkOpen" })
{
    setObjectName("rrBrowserConfirmation");
    auto *content = card();
    content->findChild<QLabel *>("rrModalTitle")->setObjectName("rrBrowserPrompt");
    address = descriptionLabel();
    address->setObjectName("rrBrowserUrl");
    content->rows()->removeWidget(address);
    address->setMinimumHeight(0);
    address->setMaximumHeight(QWIDGETSIZE_MAX);
    scroll = new WebsiteAddressScrollArea(content);
    scroll->setObjectName("rrBrowserUrlScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    address->setTextFormat(Qt::PlainText);
    address->setWordWrap(true);
    address->setAlignment(Qt::AlignCenter | Qt::AlignTop);
    address->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    address->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    ui::readerStyle(address, ui::styles::font(chrome::descriptionText));
    scroll->setWidget(address);
    content->rows()->insertWidget(1, scroll);
    error = new ui::InterfaceLabel(strings::get("link_panel.browser_unavailable"), content);
    error->setObjectName("rrBrowserError");
    error->setWordWrap(true);
    error->setAlignment(Qt::AlignCenter);
    content->rows()->insertWidget(2, error);
    error->setTextFormat(Qt::PlainText);
    error->hide();
    connect(secondaryButton(), &QPushButton::clicked, this, [this] {
        if (dismissed)
            dismissed();
    });
    confirm = primaryButton();
    connect(confirm, &QPushButton::clicked, this, [this] {
        const QUrl url(destination, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty()
            || (url.scheme() != "http" && url.scheme() != "https"))
            return;
        if (browserAvailable && browserAvailable() && openBrowser && openBrowser(url)) {
            if (dismissed)
                dismissed();
        } else {
            error->show();
            arrange();
        }
    });
}

void WebsiteDialog::open(const QString &target)
{
    destination = target;
    address->setText(target);
    const QUrl url(target, QUrl::StrictMode);
    const bool available = url.isValid() && !url.host().isEmpty()
            && (url.scheme() == "http" || url.scheme() == "https") && browserAvailable
            && browserAvailable();
    confirm->setEnabled(available);
    error->setVisible(!available);
    arrange();
    show();
    raise();
    confirm->setFocus();
}

void WebsiteDialog::arrange()
{
    const int width = qMin<int>(metrics::notice::cardWidth,
                                parentWidget()->width() - 2 * metrics::notice::sideClearance);
    const auto margins = card()->rows()->contentsMargins();
    const int textWidth = qMax(1, width - margins.left() - margins.right());
    address->ensurePolished();
    // Qt shows the scroll bar even for a short address that fits. Show it
    // only for an address taller than the box.
    const int needed = qMax<int>(ui::px(60), address->heightForWidth(textWidth));
    const int most = qMax<int>(chrome::actionHeight, height() / 3);
    scroll->setVerticalScrollBarPolicy(needed > most ? Qt::ScrollBarAsNeeded
                                                     : Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(qMin(most, needed));
    card()->fitToWidth(width);
    centerCard();
}

void WebsiteDialog::resizeEvent(QResizeEvent *event)
{
    ui::ModalOverlay::resizeEvent(event);
    if (scroll)
        arrange();
}

} // namespace kobalt
