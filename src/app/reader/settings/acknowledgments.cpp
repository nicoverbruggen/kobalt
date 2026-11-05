#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/style/theme.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "ui/style/fonts.h"
#include "ui/style/styles.h"
#include "acknowledgments.h"
#include "ui/style/metrics.h"
#include "ui/panel_timing.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QResizeEvent>

namespace kobalt {
namespace styles = kobalt::ui::styles;
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;

namespace {
QString groupTitle(const QString &id)
{
    if (id == "kobalt")
        return strings::get("acknowledgments.group_kobalt");
    if (id == "engine")
        return strings::get("acknowledgments.group_engine");
    if (id == "nickel")
        return strings::get("acknowledgments.group_nickel");
    if (id == "fonts")
        return strings::get("acknowledgments.group_fonts");
    if (id == "icons")
        return strings::get("acknowledgments.group_icons");
    if (id == "author")
        return strings::get("acknowledgments.group_author");
    return id;
}
} // namespace

LicenseText::LicenseText(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrLicenseText");
    // License text is laid out in columns. Firmware 4 ships no fixed-width
    // font, so every firmware uses the bundled one. At this size most notices
    // keep their own line breaks.
    const QFont font = kobalt::ui::chromeFont(metrics::acknowledgments::noticeText);
    setFont(font);
    document.setFont(font);
}

void LicenseText::setText(const QString &text)
{
    // Notices remain unchanged in the resources. Remove print-layout indentation
    // only for display so headings and wrapped lines share the left margin.
    auto lines = text.split('\n');
    for (auto &line : lines) {
        int first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t'))
            ++first;
        line.remove(0, first);
    }
    page = 0;
    document.setPlainText(lines.join('\n'));
    stale = true;
    update();
}

void LicenseText::changeEvent(QEvent *event)
{
    // The document keeps the faces of its font. See refreshInterfaceText().
    if (event->type() == QEvent::FontChange) {
        document.setFont(kobalt::ui::renewedFont(document.font()));
        stale = true;
        update();
    }
    QWidget::changeEvent(event);
}

int LicenseText::pages()
{
    ensureLayout();
    return document.pages();
}

void LicenseText::resizeEvent(QResizeEvent *)
{
    stale = true;
}

void LicenseText::ensureLayout()
{
    if (!stale)
        return;
    stale = false;
    const int margin = metrics::acknowledgments::textMargin;
    document.layout(qMax(1, width() - 2 * margin), qMax(1, height() - 2 * margin));
    page = qBound(0, page, document.pages() - 1);
}

void LicenseText::paintEvent(QPaintEvent *)
{
    ensureLayout();
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    const int margin = metrics::acknowledgments::textMargin;
    const int pageHeight = qMax(1, height() - 2 * margin), start = page * pageHeight;
    painter.translate(margin, margin - start);
    document.draw(painter, QRectF(0, start, qMax(1, width() - 2 * margin), pageHeight), palette());
}

Acknowledgments::Acknowledgments(QWidget *parent) : QFrame(parent)
{
    setObjectName("rrAcknowledgments");
    setAutoFillBackground(true);
    QFile file(":/kobalt/acknowledgments.json");
    if (file.open(QIODevice::ReadOnly))
        entries = QJsonDocument::fromJson(file.readAll()).array();
    back = new ui::CellButton(ui::ControlIcon::Back, this, chrome::backIcon);
    back->setObjectName("rrAcknowledgmentsBack");
    back->setRule(Qt::RightEdge);
    back->setAccessibleName(strings::get("common.back"));
    title = new ui::InterfaceLabel(this);
    title->setObjectName("rrAcknowledgmentsTitle");
    title->setTextFormat(Qt::PlainText);
    ui::readerStyle(title, styles::brand(chrome::titleText) + "background:transparent;");
    section = new ui::InterfaceLabel(this);
    section->setObjectName("rrAcknowledgmentsSection");
    ui::captionLabel(section);
    detail = new ui::InterfaceLabel(this);
    detail->setObjectName("rrAcknowledgmentsDetail");
    ui::captionLabel(detail);
    detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // The GNU GPL and AGPL ask an interactive program to state its license and
    // that it has no warranty. The Kobalt entry below holds the license texts.
    intro = new ui::InterfaceLabel(
            strings::get(
                    "acknowledgments.kobalt_is_distributed_under_the_gnu_agpl_v3_without_any_warranty")
                    + ' '
                    + strings::get(
                            "acknowledgments.thanks_to_the_open_source_projects_that_make_this_reader_possible"),
            this);
    description = new ui::InterfaceLabel(this);
    description->setObjectName("rrLicenseDescription");
    for (auto *label : { intro, description }) {
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
    }
    ui::readerStyle(intro,
                    styles::font(chrome::descriptionText)
                            + "background:transparent;color:palette(placeholder-text);");
    ui::readerStyle(description, styles::font(chrome::labelText) + "background:transparent;");
    version = new ui::InterfaceLabel(this);
    version->setObjectName("rrLicenseVersion");
    ui::captionLabel(version);
    license = new ui::InterfaceLabel(this);
    license->setObjectName("rrLicenseName");
    ui::captionLabel(license);
    url = new ui::InterfaceLabel(this);
    url->setObjectName("rrLicenseUrl");
    url->setTextFormat(Qt::PlainText);
    ui::readerStyle(url, styles::caption(chrome::numberText));
    text = new LicenseText(this);
    pager = new ui::PagerBar(this, "rrAcknowledgments");
    connect(back, &QPushButton::clicked, this, [this] {
        if (entry >= 0) {
            entry = -1;
            redraw();
        } else if (finished)
            finished();
    });
    connect(pager->previous, &QPushButton::clicked, this, [this] { turnPage(-1); });
    connect(pager->next, &QPushButton::clicked, this, [this] { turnPage(1); });
}

void Acknowledgments::open()
{
    entry = -1;
    listPage = 0;
    listEntry = 0;
    redraw();
    show();
    raise();
}

void Acknowledgments::select(int index)
{
    ui::PanelTiming timing("licence");
    entry = index;
    const auto item = entries.at(index).toObject();
    const auto notices = item.value("notices").toArray();
    QString body;
    for (const auto &notice : notices) {
        QFile file(":/kobalt/notices/" + notice.toString());
        // Name each notice only when a project has several.
        if (!body.isEmpty())
            body += "\n\n";
        if (notices.size() > 1)
            body += notice.toString() + "\n\n";
        body += file.open(QIODevice::ReadOnly)
                ? QString::fromUtf8(file.readAll()).trimmed()
                : strings::get("acknowledgments.license_text_is_unavailable");
    }
    description->setText(item.value("description").toString());
    version->setText(item.value("version").toString());
    license->setText(item.value("license").toString());
    url->setText(item.value("url").toString());
    text->page = 0;
    text->setText(body);
    redraw();
    timing.field("chars", body.size());
    timing.finish(this);
}

void Acknowledgments::resizeEvent(QResizeEvent *)
{
    if (entry < 0)
        listPage = -1;
    redraw();
}

void Acknowledgments::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);
    QPainter painter(this);
    ui::paintHeaderRules(painter, this);
    painter.fillRect(0, divider, width(), metrics::control::border, ui::dividerColor(this));
}

void Acknowledgments::turnPage(int direction)
{
    if (entry >= 0) {
        text->page = qBound(0, text->page + direction, text->pages() - 1);
        text->update();
        pager->setPage(text->page, text->pages());
    } else {
        listPage = pager->turnTarget(direction);
        redraw(true);
    }
}

void Acknowledgments::redraw(bool paging)
{
    const bool reading = entry >= 0;
    const int textWidth = qMax(1, width() - 2 * chrome::inset),
              captionHeight = chrome::statusHeight - metrics::control::border;
    back->setGeometry(0, chrome::statusHeight, chrome::cellWidth,
                      chrome::actionHeight - chrome::heavyRule);
    title->setGeometry(chrome::cellWidth + chrome::inset, chrome::statusHeight,
                       qMax(1, width() - chrome::cellWidth - 2 * chrome::inset),
                       chrome::actionHeight - chrome::heavyRule);
    section->setGeometry(chrome::inset, 0, textWidth / 2, captionHeight);
    detail->setGeometry(width() / 2, 0, qMax(1, width() / 2 - chrome::inset), captionHeight);
    pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
    const auto item = reading ? entries.at(entry).toObject() : QJsonObject();
    title->setText(reading ? item.value("name").toString()
                           : strings::get("acknowledgments.acknowledgments"));
    section->setText(reading ? strings::get("acknowledgments.acknowledgments")
                             : strings::get("settings.about"));
    detail->setText(reading ? item.value("license").toString()
                            : strings::get("acknowledgments.projects").arg(entries.size()));
    intro->setVisible(!reading);
    for (auto *label : { static_cast<QWidget *>(description), static_cast<QWidget *>(version),
                         static_cast<QWidget *>(license), static_cast<QWidget *>(url),
                         static_cast<QWidget *>(text) })
        label->setVisible(reading);
    int count = 1;
    if (reading) {
        int y = chrome::headerHeight() + chrome::blockTop;
        description->setGeometry(chrome::inset, y, textWidth,
                                 description->heightForWidth(textWidth));
        y = description->geometry().bottom() + 1 + chrome::textGap;
        const int captionLine = ui::interfaceLineHeight(ui::captionFont()) + chrome::textGap;
        const int versionWidth = version->text().isEmpty()
                ? 0
                : qMin(textWidth / 2, ui::interfaceTextWidth(version->text(), ui::captionFont()));
        version->setGeometry(chrome::inset, y, versionWidth, captionLine);
        const int licenseLeft = chrome::inset
                + (versionWidth ? versionWidth + metrics::acknowledgments::captionGap : 0);
        license->setGeometry(licenseLeft, y, qMax(1, width() - chrome::inset - licenseLeft),
                             captionLine);
        y += captionLine;
        url->setGeometry(chrome::inset, y, textWidth,
                         ui::interfaceLineHeight(ui::chromeFont(chrome::numberText))
                                 + chrome::textGap);
        divider = url->geometry().bottom() + 1 + chrome::blockTop;
        // The document supplies its own margin; line its text up with the inset.
        const int top = divider + metrics::control::border + chrome::blockTop
                - metrics::acknowledgments::textMargin;
        const int side = chrome::inset - metrics::acknowledgments::textMargin, wanted = text->page;
        // Measuring without the pager can clamp the page; restore it afterwards.
        text->setGeometry(side, top, qMax(1, width() - 2 * side), qMax(1, height() - top));
        if (text->pages() > 1)
            text->setGeometry(side, top, qMax(1, width() - 2 * side),
                              qMax(1, height() - chrome::actionHeight - top));
        text->page = wanted;
        count = text->pages();
    } else {
        intro->setGeometry(chrome::inset, chrome::headerHeight() + chrome::blockTop, textWidth,
                           intro->heightForWidth(textWidth));
        divider = intro->geometry().bottom() + 1 + chrome::blockTop;
    }
    const int rowsTop = divider + metrics::control::border;
    // Page the list by height. A group heading always has a row below it, and
    // a group that continues on the next page repeats its heading there.
    listPages.clear();
    const int available = qMax<int>(metrics::list::headerHeight + metrics::list::twoLines,
                                    height() - rowsTop - chrome::actionHeight);
    QVector<int> current;
    QString group;
    int used = 0;
    for (int index = 0; index < entries.size(); ++index) {
        const auto next = entries.at(index).toObject().value("group").toString();
        const int need =
                (next != group ? metrics::list::headerHeight : 0) + metrics::list::twoLines;
        if (!current.isEmpty() && used + need > available) {
            listPages.append(current);
            current.clear();
            used = 0;
        }
        if (next != group || current.isEmpty()) {
            current.append(-1);
            used += metrics::list::headerHeight;
        }
        current.append(index);
        used += metrics::list::twoLines;
        group = next;
    }
    if (!current.isEmpty())
        listPages.append(current);
    if (listPage < 0) {
        listPage = 0;
        for (int i = 0; i < listPages.size(); ++i)
            if (listPages.at(i).contains(listEntry))
                listPage = i;
    }
    if (!reading)
        count = qMax(1, int(listPages.size()));
    int &page = reading ? text->page : listPage;
    page = qBound(0, page, count - 1);
    const auto shown = reading || listPages.isEmpty() ? QVector<int>() : listPages.at(listPage);
    for (int index : shown)
        if (index >= 0) {
            listEntry = index;
            break;
        }
    int rowCount = 0, headingCount = 0;
    for (int index : shown)
        ++(index < 0 ? headingCount : rowCount);
    while (rows.size() < rowCount) {
        auto *button = new ui::ListRow(this);
        rows.append(button);
        connect(button, &QPushButton::clicked, this,
                [this, button] { select(button->property("reader_entry").toInt()); });
    }
    while (headings.size() < headingCount) {
        auto *heading = new ui::ListHeader(this);
        headings.append(heading);
    }
    int y = rowsTop, row = 0, heading = 0;
    for (int i = 0; i < shown.size(); ++i) {
        const int index = shown.at(i);
        // A heading takes the group of the entry after it.
        const auto project = entries.at(index >= 0 ? index : shown.at(i + 1)).toObject();
        if (index < 0) {
            auto *label = headings.at(heading++);
            label->leftLabel()->setObjectName("rrAcknowledgmentsGroup_"
                                              + project.value("group").toString());
            label->setCaptions(groupTitle(project.value("group").toString()));
            label->setGeometry(0, y, width(), metrics::list::headerHeight);
            label->show();
            y += metrics::list::headerHeight;
            continue;
        }
        auto *button = rows.at(row++);
        button->setObjectName(QString("rrAcknowledgment%1").arg(index));
        button->setProperty("reader_entry", index);
        auto *itemRow = static_cast<ui::ListRow *>(button);
        itemRow->setTitle(project.value("name").toString());
        itemRow->setCaption(project.value("license").toString());
        itemRow->setAccessory(ui::ListRow::Chevron{});
        button->setGeometry(0, y, width(), metrics::list::twoLines);
        button->show();
        button->update();
        y += metrics::list::twoLines;
    }
    for (; row < rows.size(); ++row)
        rows.at(row)->hide();
    for (; heading < headings.size(); ++heading)
        headings.at(heading)->hide();
    pager->setPage(page, count, {}, !reading);
    if (!reading)
        pager->setBlankArea({ chrome::inset, y, width() - 2 * chrome::inset, pager->y() - y });
    if (reading)
        text->update();
    if (!paging)
        update();
}
} // namespace kobalt
