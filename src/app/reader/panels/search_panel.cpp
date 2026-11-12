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
#include "ui/controls/options.h"
#include <QGridLayout>
#include <QIntValidator>
#include <QLocale>
#include <algorithm>

namespace kobalt {
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;

namespace {
ui::ControlIcon scopeIcon(SearchScope scope)
{
    switch (scope) {
    case SearchScope::Book:
        return ui::ControlIcon::Dictionary;
    case SearchScope::Chapter:
        return ui::ControlIcon::SearchChapter;
    case SearchScope::Before:
        return ui::ControlIcon::SearchBefore;
    case SearchScope::After:
        return ui::ControlIcon::SearchAfter;
    }
    return ui::ControlIcon::Dictionary;
}

class ScopeIcon : public QWidget
{
public:
    ScopeIcon(SearchScope scope, QWidget *parent) : QWidget(parent), glyph(scopeIcon(scope)) { }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        ui::controlIcon(glyph, palette().color(QPalette::WindowText)).paint(&painter, rect());
    }

private:
    ui::ControlIcon glyph;
};
} // namespace

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
    status->hide();
    scopeStatus = new ui::InterfaceLabel(this);
    scopeStatus->setObjectName("rrSearchScopeStatus");
    ui::captionLabel(scopeStatus);
    static_cast<ui::InterfaceLabel *>(scopeStatus)->setOpticalCentering(true);
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
    original->setRule(Qt::BottomEdge);
    ui::readerStyle(original, ui::styles::font(chrome::labelText));
    scopeButton = new ui::CellButton(scopeIcon(searchScope), this, chrome::backIcon);
    scopeButton->setObjectName("rrSearchScope");
    scopeButton->setAccessibleName(strings::get("search_panel.scope"));
    scopeButton->setRule(Qt::LeftEdge);
    connect(scopeButton, &QPushButton::clicked, this, [this] { chooseScope(); });
    pageJump = ui::makeButton(pager, nullptr, "rrSearchPageJump", {}, [this] { choosePage(); });
    ui::readerStyle(pageJump,
                    "QPushButton{border:none;padding:0;background:palette(window);"
                            + ui::styles::font(chrome::countText)
                            + "}QPushButton:pressed{background:palette(midlight);}");
    pager->counter->setAttribute(Qt::WA_TransparentForMouseEvents);
    pager->counter->raise();
    updates.setSingleShot(true);
    updates.setInterval(2000);
    updates.setTimerType(Qt::PreciseTimer);
    connect(&updates, &QTimer::timeout, this, [this] { flush(); });
    hide();
}

QString SearchPanel::scopeLabel() const
{
    switch (searchScope) {
    case SearchScope::Chapter:
        return strings::get("search_panel.current_chapter");
    case SearchScope::Before:
        return strings::get("search_panel.up_to_here");
    case SearchScope::After:
        return strings::get("search_panel.after_here");
    case SearchScope::Book:
        return strings::get("search_panel.whole_book");
    }
    return {};
}

void SearchPanel::closePopup()
{
    if (!popup)
        return;
    popup->hide();
    popup->deleteLater();
    popup.clear();
}

void SearchPanel::chooseScope()
{
    closePopup();
    keyboard->hide();
    ui::clearQueryFocus(query);
    popup = new ui::ModalOverlay(this);
    popup->setObjectName("rrSearchScopes");
    popup->setGeometry(rect());
    popup->setDimmed(true);
    popup->dismissed = [this] { closePopup(); };
    auto *card = popup->card();
    card->heading(strings::get("search_panel.scope"));
    auto *options = new ui::RadioList(card);
    options->setUniformRows(false);
    options->setMinimumRowHeight(chrome::listActionHeight);
    options->setTopDivider(true);
    const QVector<SearchScope> scopes = { SearchScope::Book, SearchScope::Chapter,
                                          SearchScope::Before, SearchScope::After };
    const char *keys[] = { "search_panel.whole_book", "search_panel.current_chapter",
                           "search_panel.up_to_here", "search_panel.after_here" };
    const int ids[] = { 3, 0, 1, 2 };
    int current = 0;
    for (int i = 0; i < scopes.size(); ++i) {
        auto *option = options->add(
                QString("rrSearchScope%1").arg(ids[i]), strings::get(keys[i]),
                strings::get(qPrintable(QString::fromLatin1(keys[i]) + "_description")));
        auto *icon = new ScopeIcon(scopes[i], option);
        icon->setObjectName(QString("rrSearchScopeIcon%1").arg(ids[i]));
        option->setPreview(icon, QSize(chrome::backIcon, chrome::backIcon));
        if (searchScope == scopes[i]) {
            options->setCurrent(i);
            current = i;
        }
    }
    options->chosen = [this, scopes](int index) {
        searchScope = scopes[index];
        closePopup();
        if (!query->text().trimmed().isEmpty())
            run();
        else {
            updates.stop();
            clearDetails();
            hits.clear();
            shownCount = 0;
            submittedQuery.clear();
            searched = complete = false;
            scanning = false;
            status->setText(strings::get("search_panel.search_this_book"));
            if (requested)
                requested({});
            redraw();
            query->setFocus();
            keyboard->show();
        }
    };
    card->rows()->addWidget(options);
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    options->setFixedHeight(options->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    const int available = height() - 2 * metrics::notice::sideClearance;
    if (card->height() > available) {
        auto *pages = new ui::PagerBar(card, "rrSearchScopes");
        card->rows()->addWidget(pages);
        const int room = qMax(
                1, available - (card->height() - options->height()) - int(chrome::actionHeight));
        QVector<int> starts{ 0 };
        int used = 0;
        for (int row = 0; row < scopes.size(); ++row) {
            const int rowHeight = options->rowHeight(row, cardWidth - 4);
            if (used && used + rowHeight > room) {
                starts.append(row);
                used = 0;
            }
            used += rowHeight;
        }
        auto page = std::make_shared<int>(0);
        for (int i = 0; i < starts.size(); ++i)
            if (starts[i] <= current)
                *page = i;
        const auto showPage = [this, options, pages, card, cardWidth, starts, room, page] {
            const int first = starts[*page];
            const int last =
                    *page + 1 < starts.size() ? starts[*page + 1] : options->options().size();
            options->showRows(first, last - first);
            options->setFixedHeight(room);
            pages->setPage(*page, starts.size());
            card->fitToWidth(cardWidth);
            popup->centerCard();
        };
        connect(pages->previous, &QPushButton::clicked, this, [pages, page, showPage] {
            *page = pages->turnTarget(-1);
            showPage();
        });
        connect(pages->next, &QPushButton::clicked, this, [pages, page, showPage] {
            *page = pages->turnTarget(1);
            showPage();
        });
        showPage();
    }
    popup->centerCard();
    popup->show();
    popup->raise();
}

void SearchPanel::choosePage()
{
    if (resultPageCount <= 1)
        return;
    closePopup();
    keyboard->hide();
    ui::clearQueryFocus(query);
    auto *dialog =
            new ui::ChoiceModal(this,
                                { strings::get("search_panel.page_range").arg(resultPageCount),
                                  strings::get("search_panel.go_to_page"),
                                  {} },
                                { strings::get("common.cancel"), "rrSearchPageCancel" },
                                { strings::get("search_panel.go"), "rrSearchPageGo" });
    popup = dialog;
    dialog->setObjectName("rrSearchPageDialog");
    dialog->setGeometry(rect());
    dialog->setDimmed(true);
    dialog->dismissed = [this] { closePopup(); };
    auto *card = dialog->card();
    auto *entry = new QWidget(card);
    auto *layout = new QVBoxLayout(entry);
    layout->setContentsMargins(chrome::inset, 0, chrome::inset,
                               metrics::notice::cardMargins.bottom());
    ui::readerStyle(dialog->descriptionLabel(), ui::styles::font(chrome::labelText));
    layout->setSpacing(metrics::notice::cardGap);
    auto *number = new QLineEdit(entry);
    number->setObjectName("rrSearchPageNumber");
    number->setAccessibleName(strings::get("search_panel.go_to_page"));
    number->setAttribute(Qt::WA_InputMethodEnabled, false);
    number->setValidator(new QIntValidator(1, resultPageCount, number));
    number->setAlignment(Qt::AlignCenter);
    number->setMinimumHeight(chrome::actionHeight);
    ui::readerStyle(number,
                    "QLineEdit{border:1px solid palette(mid);background:palette(window);"
                            + ui::styles::font(chrome::labelText) + "}");
    number->setPlaceholderText(QString::number(resultPage + 1));
    dialog->primaryButton()->setEnabled(false);
    layout->addWidget(number);
    auto *keys = new QGridLayout;
    keys->setSpacing(ui::px(12));
    layout->addLayout(keys);
    const int columns = width() > height() ? 6 : 3;
    for (int position = 0; position < 12; ++position) {
        // Landscape keeps digits together and puts Clear after Backspace.
        const int i =
                columns == 6 && position >= 9 ? (position == 11 ? 9 : position + 1) : position;
        const QString value = i < 9 ? QString::number(i + 1) : i == 10 ? "0" : QString();
        auto *button =
                ui::makeButton(entry, nullptr, qPrintable(QString("rrSearchPageKey%1").arg(i)),
                               value, [number, value, i] {
                                   if (i == 9)
                                       number->clear();
                                   else if (i == 11)
                                       number->backspace();
                                   else
                                       number->insert(value);
                               });
        button->setFocusPolicy(Qt::NoFocus);
        ui::readerStyle(
                button,
                QString("QPushButton{border:%1px solid palette(mid);border-radius:%2px;min-height:%3px;max-height:%3px;"
                        "background:palette(window);color:palette(window-text);padding:0;")
                                .arg(int(ui::px(2)))
                                .arg(int(ui::px(12)))
                                .arg(columns == 6 ? ui::px(80) : int(chrome::actionHeight))
                        + ui::styles::font(i == 9 ? int(chrome::descriptionText)
                                                  : int(chrome::queryText))
                        + "}QPushButton:pressed{background:palette(midlight);}");
        if (i == 9) {
            button->setText(strings::get("search_panel.clear_number"));
            button->setAccessibleName(strings::get("search_panel.clear_page"));
        } else if (i == 11) {
            ui::buttonIcon(button, ui::ControlIcon::KeyboardBackspace);
            button->setIconSize(QSize(ui::px(48), ui::px(48)));
            button->setAccessibleName(strings::get("search_panel.erase_digit"));
        }
        keys->addWidget(button, position / columns, position % columns);
    }
    card->rows()->insertWidget(1, entry);
    const auto go = [this, number] {
        if (!number->hasAcceptableInput())
            return;
        resultPage = qBound(0, number->text().toInt() - 1, resultPageCount - 1);
        closePopup();
        redraw();
    };
    connect(dialog->primaryButton(), &QPushButton::clicked, this, go);
    connect(number, &QLineEdit::returnPressed, this, go);
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, [this] { closePopup(); });
    connect(number, &QLineEdit::textChanged, dialog, [dialog, number] {
        dialog->primaryButton()->setEnabled(number->hasAcceptableInput());
    });
    dialog->fitCard();
    dialog->show();
    dialog->raise();
    number->setFocus();
}

void SearchPanel::open(bool returnAvailable, int originalPage, const QString &initialQuery)
{
    if (!initialQuery.isEmpty()) {
        query->setText(initialQuery);
        // The dictionary action explicitly asks to search the book.
        searchScope = SearchScope::Book;
    }
    canReturn = returnAvailable;
    clearDetails();
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
    // Closing cancels worker jobs. Reopen an incomplete query with a fresh cursor.
    if (!initialQuery.isEmpty() || (restartOnOpen && query->text().trimmed() == submittedQuery))
        run();
    restartOnOpen = false;
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

void SearchPanel::clearDetails()
{
    ++detailsEpoch;
    visibleFirst = -1;
    visibleCount = 0;
    visibleHits.clear();
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
    // In chapter mode a hit names its chapter, with an estimated page once
    // there is one.
    if (!hit.chapter.isEmpty())
        return hit.page < 0 ? hit.chapter.simplified()
                            : strings::get("search_panel.page_chapter")
                                      .arg(hit.page + 1)
                                      .arg(hit.chapter.simplified());
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
    closePopup();
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
    clearDetails();
    hits.clear();
    shownCount = 0;
    resultPage = 0;
    scanning = true;
    searched = true;
    complete = false;
    status->setText(strings::get("search_panel.searching"));
    redraw();
    if (requested)
        requested(text);
}

void SearchPanel::append(const SearchBatch &batch, int totalPages)
{
    if (batch.reset) {
        closePopup();
        updates.stop();
        pendingStatus.clear();
        invalidateRows();
        clearDetails();
        hits.clear();
        shownCount = 0;
        resultPage = 0;
    }
    const bool firstResults = hits.isEmpty() && !batch.hits.isEmpty();
    scanning = !batch.done;
    hits += batch.hits;
    searched = true;
    complete = batch.done;
    pendingStatus = strings::get(batch.done ? (hits.isEmpty() ? "search_panel.no_matches_found"
                                                              : "search_panel.search_complete")
                                            : "search_panel.searching");
    Q_UNUSED(totalPages);
    if (batch.reset || firstResults || batch.done) {
        flush();
    } else if (!updates.isActive()) {
        updates.start();
    }
}

void SearchPanel::flush()
{
    updates.stop();
    shownCount = hits.size();
    status->setText(pendingStatus);
    redraw();
}

void SearchPanel::stop()
{
    clearDetails();
    closePopup();
    updates.stop();
    ui::clearQueryFocus(query);
    keyboard->hide();
    QGuiApplication::inputMethod()->hide();
    restartOnOpen = restartOnOpen || scanning;
    if (scanning)
        status->setText(strings::get("search_panel.search_stopped"));
    scanning = false;
}

void SearchPanel::resizeEvent(QResizeEvent *)
{
    closePopup();
    redraw();
    keyboard->place();
}

void SearchPanel::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, chrome::actionHeight - chrome::heavyRule, width(), chrome::heavyRule,
                     palette().color(QPalette::WindowText));
    painter.fillRect(0, chrome::actionHeight + chrome::statusHeight - metrics::control::border,
                     width(), metrics::control::border, palette().color(QPalette::Mid));
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
    const bool returning = canReturn && shownCount > 0;
    const int scopeBottom = chrome::actionHeight + chrome::statusHeight;
    const auto matches =
            strings::get(shownCount == 1 ? "search_panel.match" : "search_panel.matches")
                    .arg(QLocale().toString(shownCount));
    scopeStatus->setText(searched ? strings::get(complete ? "search_panel.matches_in_scope"
                                                          : "search_panel.matches_so_far_in_scope")
                                            .arg(matches, scopeLabel())
                                  : strings::get("search_panel.now_searching").arg(scopeLabel()));
    scopeStatus->setAccessibleDescription(status->text());
    scopeStatus->setGeometry(chrome::inset, chrome::actionHeight, textWidth, chrome::statusHeight);
    const int listTop = scopeBottom + (returning ? metrics::list::twoLines : 0);
    pageSize = qMax(1, (height() - listTop - chrome::actionHeight) / rowHeight);
    const int count = qMax(1, (shownCount + pageSize - 1) / pageSize);
    resultPageCount = count;
    resultPage = qBound(0, first / pageSize, count - 1);
    const int cellHeight = chrome::actionHeight - chrome::heavyRule;
    back->setGeometry(0, 0, chrome::cellWidth, cellHeight);
    submit->setGeometry(width() - chrome::cellWidth, 0, chrome::cellWidth, cellHeight);
    query->setGeometry(chrome::cellWidth, 0, qMax(1, width() - 3 * chrome::cellWidth), cellHeight);
    clear->setGeometry(query->width() - metrics::search::clearWidth - metrics::search::clearRight,
                       0, metrics::search::clearWidth, query->height());
    pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
    const int used =
            listTop + qMin(pageSize, qMax(0, shownCount - resultPage * pageSize)) * rowHeight;
    const auto pageCaption =
            strings::get(complete ? "search_panel.result_pages"
                                  : "search_panel.result_pages_so_far")
                    .arg(QLocale().toString(resultPage + 1), QLocale().toString(count));
    pager->setPage(resultPage, count, pageCaption, true,
                   { chrome::inset, used, width() - 2 * chrome::inset, pager->y() - used },
                   shownCount > 0);
    pageJump->setEnabled(count > 1);
    scopeButton->setGeometry(width() - 2 * chrome::cellWidth, 0, chrome::cellWidth, cellHeight);
    scopeButton->setGlyph(scopeIcon(searchScope));
    scopeButton->setAccessibleName(strings::get("search_panel.scope") + ": " + scopeLabel());
    pageJump->setGeometry(chrome::cellWidth, metrics::control::border,
                          qMax(1, width() - 2 * chrome::cellWidth),
                          pager->height() - metrics::control::border);
    pageJump->setAccessibleName(strings::get("search_panel.go_to_page") + ". " + pageCaption);
    original->setVisible(returning);
    original->setGeometry(0, scopeBottom, width(), metrics::list::twoLines);
    const bool waiting = shownCount == 0 && !scanning;
    empty->setVisible(waiting);
    hint->setVisible(waiting);
    empty->setText(submittedQuery.isEmpty()
                           ? strings::get("search_panel.find_a_word_or_phrase")
                           : strings::get("search_panel.empty_query").arg(submittedQuery));
    hint->setText(submittedQuery.isEmpty() ? strings::get("search_panel.search_hint")
                                           : strings::get("search_panel.no_matches_hint"));
    const int emptyHeight = empty->heightForWidth(textWidth),
              hintHeight = hint->heightForWidth(textWidth);
    empty->setGeometry(chrome::inset, scopeBottom + metrics::search::emptyTop, textWidth,
                       emptyHeight);
    hint->setGeometry(chrome::inset, empty->geometry().bottom() + 1 + chrome::textGap, textWidth,
                      hintHeight);
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
            if (row < visibleHits.size() && selected) {
                const auto hit = visibleHits[row];
                stop();
                selected(hit);
            }
        });
    }
    const int firstVisible = resultPage * pageSize;
    const int countVisible = qMin(pageSize, shownCount - firstVisible);
    if (isVisible() && (visibleFirst != firstVisible || visibleCount != countVisible)) {
        clearDetails();
        visibleFirst = firstVisible;
        visibleCount = countVisible;
        const auto requestedHits = hits.mid(firstVisible, countVisible);
        const bool needsDetails =
                std::any_of(requestedHits.cbegin(), requestedHits.cend(),
                            [](const SearchHit &hit) { return hit.text.isEmpty(); });
        if (countVisible && detailsRequested && needsDetails) {
            const auto epoch = detailsEpoch;
            QPointer<SearchPanel> guard(this);
            detailsRequested(requestedHits, [guard, epoch](QVector<SearchHit> resolved) {
                if (!guard || guard->detailsEpoch != epoch)
                    return;
                guard->visibleHits = std::move(resolved);
                guard->invalidateRows();
                guard->redraw();
            });
        } else {
            visibleHits = requestedHits;
        }
    }
    for (int row = 0; row < buttons.size(); ++row) {
        const int index = resultPage * pageSize + row;
        const bool visible = row < pageSize && index < shownCount;
        buttons[row]->setVisible(visible);
        if (!visible)
            continue;
        buttons[row]->setObjectName(QString("rrSearchResult%1").arg(index));
        buttons[row]->setGeometry(0, listTop + row * rowHeight, width(), rowHeight);
        // An estimated page label can arrive before the excerpt, and a
        // resolved excerpt can be opened before its book page is known.
        const bool ready = row < visibleHits.size() && !visibleHits[row].text.isEmpty();
        buttons[row]->setEnabled(ready);
        if (!ready) {
            metadata[row]->clear();
            labels[row]->clear();
            buttons[row]->setAccessibleName({});
            continue;
        }
        const auto &hit = visibleHits[row];
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
                hit.page >= 0 || hit.chapter.isEmpty()
                        ? strings::get("search_panel.page_detail").arg(hit.page + 1).arg(hit.text)
                        : strings::get("search_panel.chapter_detail")
                                  .arg(hit.chapter.simplified(), hit.text));
    }
}
} // namespace kobalt
