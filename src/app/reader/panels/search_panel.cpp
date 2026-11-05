#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/controls/query_edit.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include "search_panel.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include <QGuiApplication>
#include <QInputMethod>
#include <QPainter>
#include <QtMath>

namespace kobalt {
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;

SearchPanel::SearchPanel(QWidget *parent, KeyboardFactory makeKeyboard) : ui::Panel(parent)
{
    setObjectName("rrSearch");
    back = new ui::CellButton(ui::ControlIcon::Back, this, chrome::backIcon);
    back->setObjectName("rrSearchBack");
    back->setRule(Qt::RightEdge);
    back->setAccessibleName(strings::get("common.back"));
    query = new ui::QueryEdit(this);
    query->setObjectName("rrSearchQuery");
    query->setMaxLength(256);
    ui::readerStyle(query,
                    "QLineEdit{border:none;padding:0;background:palette(window);"
                            + ui::styles::font(chrome::queryText) + "}");
    query->setTextMargins(chrome::inset - metrics::control::border, 0,
                          metrics::search::clearWidth + metrics::search::clearRight, 0);
    query->setPlaceholderText(strings::get("search_panel.find_a_word_or_phrase"));
    query->setClearButtonEnabled(false);
    clear = ui::makeButton(query, nullptr, "rrSearchClear", {}, [this] {
        query->clear();
        query->setFocus();
        keyboard->show();
    });
    clear->setAccessibleName(strings::get("search_panel.clear_search"));
    ui::buttonIcon(clear, ui::ControlIcon::Close);
    clear->setIconSize(QSize(metrics::search::clearIcon, metrics::search::clearIcon));
    ui::readerStyle(
            clear,
            "QPushButton{border:none;padding:0;min-height:0;background:palette(window);} QPushButton:pressed{background:palette(midlight);}");
    clear->hide();
    connect(query, &QLineEdit::textChanged, this,
            [this](const QString &text) { clear->setVisible(!text.isEmpty()); });
    query->setAccessibleName(strings::get("reader.search_book"));
    query->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    keyboard = makeKeyboard(this, query);
    Q_ASSERT(keyboard);
    keyboard->setSubmitText(strings::get("search_panel.search"));
    keyboard->submitted = [this] { run(); };
    submit = new ui::CellButton(ui::ControlIcon::Search, this, chrome::backIcon);
    submit->setObjectName("rrSearchSubmit");
    submit->setRule(Qt::LeftEdge);
    submit->setAccessibleName(strings::get("search_panel.search"));
    connect(submit, &QPushButton::clicked, this, [this] { run(); });
    original =
            ui::makeCaptionButton(this, nullptr, "rrSearchOriginal",
                                  strings::get("search_panel.return_to_original_position"), [this] {
                                      if (returnToStart)
                                          returnToStart();
                                  });
    status = new ui::InterfaceLabel(strings::get("search_panel.search_this_book"), this);
    status->setObjectName("rrSearchStatus");
    ui::captionLabel(status);
    connect(query, &QLineEdit::returnPressed, this, [this] { run(); });
    connect(back, &QPushButton::clicked, this, [this] {
        if (dismissed)
            dismissed();
    });
    pager = new ui::PagerBar(this, "rrSearch");
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        resultPage = pager->turnTarget(-1);
        redraw();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        resultPage = pager->turnTarget(1);
        redraw();
    });
    empty = new ui::InterfaceLabel(this);
    empty->setObjectName("rrSearchEmpty");
    empty->setTextFormat(Qt::PlainText);
    empty->setWordWrap(true);
    ui::readerStyle(empty, ui::styles::font(chrome::labelText) + "background:transparent;");
    hint = new ui::InterfaceLabel(this);
    hint->setObjectName("rrSearchHint");
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    ui::readerStyle(hint,
                    ui::styles::font(chrome::descriptionText)
                            + "background:transparent;color:palette(placeholder-text);");
    edit = ui::makeCaptionButton(this, nullptr, "rrSearchEdit", strings::get("search_panel.edit"),
                                 [this] {
                                     query->setFocus();
                                     keyboard->show();
                                 });
    for (auto *action : { original, edit }) {
        action->setRule(Qt::BottomEdge);
        ui::readerStyle(action, ui::styles::font(chrome::labelText));
    }
    edit->setRule(ui::Edges(Qt::TopEdge) | Qt::BottomEdge);
    updates.setSingleShot(true);
    updates.setInterval(500);
    connect(&updates, &QTimer::timeout, this, [this] { flush(); });
    hide();
}

void SearchPanel::open(bool returnAvailable, int originalPage, const QString &initialQuery)
{
    if (!initialQuery.isEmpty())
        query->setText(initialQuery);
    canReturn = returnAvailable;
    invalidateRows();
    original->setCaption(strings::get("search_panel.page_number").arg(originalPage + 1));
    show();
    raise();
    redraw();
    // Returning from a result keeps the list and its return-position control
    // available. Tapping the query opens the keyboard again for editing.
    if (query->text().isEmpty()) {
        query->setFocus();
        keyboard->show();
    } else {
        ui::clearQueryFocus(query);
        keyboard->hide();
    }
    if (!initialQuery.isEmpty())
        run();
}

void SearchPanel::setExcerptFont(const QString &family)
{
    if (family == excerptFamily)
        return;
    excerptFamily = family;
    rowHeight = 0;
    invalidateRows();
}

void SearchPanel::setChapters(const QVector<ChapterLink> &links)
{
    bool same = links.size() == chapters.size();
    for (int i = 0; same && i < links.size(); ++i)
        same = links[i].page == chapters[i].page && links[i].title == chapters[i].title;
    if (same)
        return;
    chapters = links;
    invalidateRows();
}

void SearchPanel::invalidateRows()
{
    renderedStarts.fill(QString());
}

QFont SearchPanel::excerptFont() const
{
    QFont font = excerptFamily.isEmpty() ? ui::interfaceFont() : QFont(excerptFamily);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPixelSize(metrics::search::excerptText);
    return font;
}

QString SearchPanel::caption(const SearchHit &hit) const
{
    // The chapter that starts last at or before the hit's page contains it.
    QString title;
    int start = -1;
    for (const auto &chapter : chapters)
        if (chapter.page >= 0 && chapter.page <= hit.page && chapter.page >= start
            && !chapter.title.trimmed().isEmpty()) {
            start = chapter.page;
            title = chapter.title.simplified();
        }
    return title.isEmpty() ? strings::get("search_panel.page_number").arg(hit.page + 1)
                           : strings::get("search_panel.page_chapter").arg(hit.page + 1).arg(title);
}

void SearchPanel::run()
{
    const auto text = query->text().trimmed();
    if (text.isEmpty())
        return;
    ui::clearQueryFocus(query);
    keyboard->hide();
    QGuiApplication::inputMethod()->hide();
    updates.stop();
    pendingStatus.clear();
    invalidateRows();
    rowHeight = 0;
    submittedQuery = text;
    hits.clear();
    resultPage = 0;
    scanning = true;
    status->setText(strings::get("search_panel.searching"));
    redraw();
    if (requested)
        requested(text);
}

void SearchPanel::append(const SearchBatch &batch, int totalPages)
{
    if (batch.reset) {
        updates.stop();
        pendingStatus.clear();
        invalidateRows();
        hits.clear();
        resultPage = 0;
    }
    const bool firstResults = hits.isEmpty() && !batch.hits.isEmpty();
    scanning = !batch.done;
    hits += batch.hits;
    pendingStatus = batch.limited ? strings::get("search_panel.limited").arg(hits.size())
            : batch.done          ? (hits.isEmpty() ? strings::get("search_panel.no_matches_found")
                                                    : strings::get("search_panel.matches").arg(hits.size()))
                                  : strings::get("search_panel.matches_searching")
                                   .arg(hits.size())
                                   .arg(100 * batch.nextPage / qMax(1, totalPages));
    if (batch.reset || firstResults || batch.done) {
        flush();
    } else if (!updates.isActive()) {
        updates.start();
    }
}

void SearchPanel::flush()
{
    updates.stop();
    status->setText(pendingStatus);
    redraw();
}

void SearchPanel::stop()
{
    updates.stop();
    ui::clearQueryFocus(query);
    keyboard->hide();
    QGuiApplication::inputMethod()->hide();
    if (scanning)
        status->setText(strings::get("search_panel.search_stopped_matches").arg(hits.size()));
    scanning = false;
}

void SearchPanel::resizeEvent(QResizeEvent *)
{
    redraw();
    keyboard->place();
}

void SearchPanel::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    ui::paintHeaderRules(painter, this);
}

void SearchPanel::redraw()
{
    const int first = resultPage * pageSize, textWidth = qMax(1, width() - 2 * chrome::inset);
    const auto font = excerptFont();
    // Book text keeps a reading line height rather than the tight UI default.
    const QString excerptBlock = QString("<div style=\"line-height:%1%\">%2</div>")
                                         .arg(metrics::search::excerptLineHeight);
    if (!rowHeight || measuredWidth != width()) {
        measuredWidth = width();
        // Room for two excerpt lines, or for a long bold match on its own.
        QStringList sample;
        for (int line = 0; line < metrics::search::excerptLines; ++line)
            sample.append("M");
        qreal lines = ui::excerptTextHeight(excerptBlock.arg(sample.join("<br>")), font, textWidth);
        lines = qMax(
                lines,
                ui::excerptTextHeight(
                        excerptBlock.arg("<b><u>" + submittedQuery.toHtmlEscaped() + "</u></b>"),
                        font, textWidth));
        rowHeight = qMax(int(metrics::list::threeLines),
                         metrics::search::excerptTop + qCeil(lines) + metrics::search::rowBottom);
    }
    const bool returning = canReturn && !hits.isEmpty();
    const int listTop = chrome::headerHeight() + (returning ? metrics::list::twoLines : 0);
    pageSize = qMax(1, (height() - listTop - chrome::actionHeight) / rowHeight);
    const int count = qMax(1, (int(hits.size()) + pageSize - 1) / pageSize);
    resultPage = qBound(0, first / pageSize, count - 1);
    status->setGeometry(chrome::inset, 0, textWidth,
                        chrome::statusHeight - metrics::control::border);
    const int cellHeight = chrome::actionHeight - chrome::heavyRule;
    back->setGeometry(0, chrome::statusHeight, chrome::cellWidth, cellHeight);
    submit->setGeometry(width() - chrome::cellWidth, chrome::statusHeight, chrome::cellWidth,
                        cellHeight);
    query->setGeometry(chrome::cellWidth, chrome::statusHeight,
                       qMax(1, width() - 2 * chrome::cellWidth), cellHeight);
    clear->setGeometry(query->width() - metrics::search::clearWidth - metrics::search::clearRight,
                       0, metrics::search::clearWidth, query->height());
    pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
    pager->setPage(resultPage, count,
                   strings::get("search_panel.result_range")
                           .arg(resultPage * pageSize + 1)
                           .arg(qMin((resultPage + 1) * pageSize, int(hits.size()))),
                   true);
    const int used =
            listTop + qMin(pageSize, qMax(0, int(hits.size()) - resultPage * pageSize)) * rowHeight;
    pager->setBlankArea({ chrome::inset, used, width() - 2 * chrome::inset, pager->y() - used });
    original->setVisible(returning);
    original->setGeometry(0, chrome::headerHeight(), width(), metrics::list::twoLines);
    const bool waiting = hits.isEmpty() && !scanning;
    empty->setVisible(waiting);
    hint->setVisible(waiting);
    edit->setVisible(waiting && !submittedQuery.isEmpty());
    empty->setText(submittedQuery.isEmpty()
                           ? strings::get("search_panel.find_a_word_or_phrase")
                           : strings::get("search_panel.empty_query").arg(submittedQuery));
    hint->setText(submittedQuery.isEmpty() ? strings::get("search_panel.search_hint")
                                           : strings::get("search_panel.no_matches_hint"));
    const int emptyHeight = empty->heightForWidth(textWidth),
              hintHeight = hint->heightForWidth(textWidth);
    empty->setGeometry(chrome::inset, chrome::headerHeight() + metrics::search::emptyTop, textWidth,
                       emptyHeight);
    hint->setGeometry(chrome::inset, empty->geometry().bottom() + 1 + chrome::textGap, textWidth,
                      hintHeight);
    edit->setGeometry(0, hint->geometry().bottom() + 1 + metrics::search::emptyBottom, width(),
                      metrics::list::twoLines);
    auto family = font.family();
    family.replace('\\', "\\\\");
    family.replace('"', "\\\"");
    const auto excerptStyle =
            QString("background:transparent;border:none;font-family:\"%1\";font-size:%2px;font-weight:400;")
                    .arg(family)
                    .arg(int(metrics::search::excerptText));
    while (buttons.size() < pageSize) {
        const int row = buttons.size();
        renderedStarts.append(QString());
        renderedSizes.append(QSize());
        auto *button = new ui::Button(this);
        buttons.append(button);
        auto *meta = new ui::PrivateTextLabel(button);
        metadata.append(meta);
        ui::captionLabel(meta);
        meta->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *label = new ui::WholeLineLabel(button);
        labels.append(label);
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setTextFormat(Qt::RichText);
        ui::readerStyle(
                button,
                QString("QPushButton{border:none;border-radius:0;border-bottom:%1px solid palette(light);background:palette(window);min-height:0;padding:0;}"
                        " QPushButton:pressed{background:palette(midlight);}")
                        .arg(int(metrics::control::border)));
        connect(button, &QPushButton::clicked, this, [this, row] {
            const int index = resultPage * pageSize + row;
            if (index < hits.size() && selected) {
                stop();
                selected(hits[index]);
            }
        });
    }
    for (int row = 0; row < buttons.size(); ++row) {
        const int index = resultPage * pageSize + row;
        const bool visible = row < pageSize && index < hits.size();
        buttons[row]->setVisible(visible);
        if (!visible)
            continue;
        const auto &hit = hits[index];
        buttons[row]->setGeometry(0, listTop + row * rowHeight, width(), rowHeight);
        metadata[row]->setGeometry(chrome::inset, metrics::search::captionTop, textWidth,
                                   metrics::search::excerptTop - metrics::search::captionTop
                                           - chrome::textGap);
        labels[row]->setGeometry(chrome::inset, metrics::search::excerptTop, textWidth,
                                 rowHeight - metrics::search::excerptTop
                                         - metrics::search::rowBottom);
        if (labels[row]->property("reader_style").toString() != excerptStyle)
            ui::readerStyle(labels[row], excerptStyle);
        if (renderedStarts[row] == hit.start && renderedSizes[row] == labels[row]->size())
            continue;
        renderedStarts[row] = hit.start;
        renderedSizes[row] = labels[row]->size();
        buttons[row]->setObjectName(QString("rrSearchResult%1").arg(index));
        metadata[row]->setText(
                ui::elideInterfaceText(caption(hit), metadata[row]->font(), textWidth));
        // The engine keeps a space at the edges of its context only where the
        // book has one, so punctuation stays attached to the match.
        QString before = hit.before, after = hit.after;
        if (before.trimmed().isEmpty())
            before.clear();
        if (after.trimmed().isEmpty())
            after.clear();
        auto excerpt = [&] {
            return excerptBlock.arg(
                    (before.isEmpty() ? QString() : QStringLiteral("…") + before.toHtmlEscaped())
                    + "<b><u>" + hit.text.toHtmlEscaped() + "</u></b>"
                    + (after.isEmpty() ? QString() : after.toHtmlEscaped() + QStringLiteral("…")));
        };
        // Trim context, never the match, using the same rich-text width as the label.
        while (ui::excerptTextHeight(excerpt(), font, labels[row]->width()) > labels[row]->height()
               && (!before.isEmpty() || !after.isEmpty())) {
            if (before.size() >= after.size())
                before.remove(0, before.at(0).isHighSurrogate() ? 2 : 1);
            else
                after.chop(after.at(after.size() - 1).isLowSurrogate() ? 2 : 1);
        }
        // Engine context can already end mid-word; keep whole words at the outer ends.
        if (!before.isEmpty()) {
            int boundary = 0;
            while (boundary < before.size() && !before.at(boundary).isSpace())
                ++boundary;
            before = before.mid(boundary);
            while (!before.isEmpty() && before.at(0).isSpace())
                before.remove(0, 1);
        }
        if (!after.isEmpty()) {
            int boundary = after.size() - 1;
            while (boundary >= 0 && !after.at(boundary).isSpace())
                --boundary;
            after = after.left(qMax(0, boundary));
            while (!after.isEmpty() && after.at(after.size() - 1).isSpace())
                after.chop(1);
        }
        labels[row]->setText(excerpt());
        buttons[row]->setAccessibleName(
                strings::get("search_panel.page_detail").arg(hit.page + 1).arg(hit.text));
    }
}
} // namespace kobalt
