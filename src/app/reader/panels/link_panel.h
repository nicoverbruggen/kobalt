#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/dialogs/modals.h"
#include "engine/crengine/reader_links.h"
#include <QUrl>
#include <QScrollArea>
#include "ui/controls/paged_text.h"

namespace kobalt {
// The same sheet as the dictionary, fixed to an edge of the reading area.
class LinkPanel : public ui::Panel
{
public:
    explicit LinkPanel(QWidget *parent);
    void open(const LinkDetails &details, const QRect &reference, const QRect &readingArea = {});
    std::function<void()> dismissed;
    std::function<void(const QString &)> navigate;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;

private:
    QString destination;
    ui::PagedText *text;
    ui::PagerBar *pages;
    QLabel *heading;
    ui::CellButton *close;
    ui::CaptionButton *passage;
    bool lower = true;
    int rowTop() const;
    void arrange();
    void refresh();
};

class WebsiteDialog : public ui::ChoiceModal
{
public:
    explicit WebsiteDialog(QWidget *parent);
    void open(const QString &target);
    std::function<bool()> browserAvailable;
    std::function<bool(const QUrl &)> openBrowser;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    QString destination;
    QLabel *address, *error;
    QScrollArea *scroll = nullptr;
    QPushButton *confirm;
    void arrange();
};
} // namespace kobalt
