#include "word_list.h"
#include "ui/controls/icons.h"
#include "ui/controls/options.h"
#include "ui/controls/text.h"
#include "ui/dialogs/modals.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include <QPainter>
#include <QSet>
#include <algorithm>
#include <climits>

namespace kobalt {
namespace chrome = ui::metrics::chrome;

void confirmWordRemoval(QWidget *host, const SavedWord &word, std::function<void()> confirmed)
{
    auto *choice =
            new ui::ChoiceModal(host,
                                { strings::get("words.remove_detail").arg(word.word),
                                  strings::get("words.remove_prompt"),
                                  {} },
                                { strings::get("common.cancel"), "rrKeepWord" },
                                { strings::get("words.remove_confirm"), "rrConfirmRemoveWord" });
    choice->setObjectName("rrRemoveWord");
    choice->setDimmed(true);
    choice->setGeometry(host->rect());
    ui::readerStyle(choice->descriptionLabel(),
                    ui::styles::font(chrome::descriptionText) + "color:palette(placeholder-text);");
    QPointer<ui::ChoiceModal> guard(choice);
    const auto finish = [guard, confirmed](bool remove) {
        if (!guard)
            return;
        guard->hide();
        guard->deleteLater();
        if (remove)
            confirmed();
    };
    QObject::connect(choice->secondaryButton(), &QPushButton::clicked, choice,
                     [finish] { finish(false); });
    QObject::connect(choice->primaryButton(), &QPushButton::clicked, choice,
                     [finish] { finish(true); });
    choice->dismissed = [finish] { finish(false); };
    choice->card()->fitToWidth(qMin<int>(ui::metrics::notice::cardWidth,
                                         host->width() - 2 * ui::metrics::notice::sideClearance));
    choice->centerCard();
    choice->show();
    choice->raise();
}

WordList::WordList(QWidget *parent, Scope scope) : ui::Panel(parent), scope(scope)
{
    setObjectName(scope == Scope::Book ? "rrWordList" : "rrMyWords");
    if (scope == Scope::Library) {
        backButton = new ui::CellButton(ui::ControlIcon::Back, this, chrome::backIcon);
        backButton->setObjectName("rrMyWordsBack");
        backButton->setRule(Qt::RightEdge);
        backButton->setAccessibleName(strings::get("library.back"));
        connect(backButton, &QPushButton::clicked, this, [this] {
            if (back)
                back();
        });
        title = new ui::InterfaceLabel(strings::get("words.title"), this);
        title->setTextFormat(Qt::PlainText);
        ui::readerStyle(title, ui::styles::brand(chrome::titleText) + "background:transparent;");
        count = new ui::InterfaceLabel({}, this);
        count->setObjectName("rrMyWordsCount");
        count->setTextFormat(Qt::PlainText);
        count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ui::readerStyle(count,
                        ui::styles::font(chrome::countText)
                                + "background:transparent;color:palette(placeholder-text);");
        // The strip below the bar, as in the Library: the book filter on the
        // left and the sort on the right.
        sortButton = new ui::CellButton(ui::ControlIcon::Sort, this);
        sortButton->setObjectName("rrMyWordsSort");
        sortButton->setAccessibleName(strings::get("marks.sort_title"));
        sortButton->setRule(ui::Edges(Qt::LeftEdge) | Qt::BottomEdge);
        ui::readerStyle(sortButton, "QPushButton{min-height:0;}");
        sortButton->setMinimumHeight(0);
        connect(sortButton, &QPushButton::clicked, this, [this] { openSort(); });
        filter = new ui::CaptionDropdown(this);
        filter->setObjectName("rrWordsFilter");
        filter->setAccessibleName(strings::get("words.filter"));
        filter->label = strings::get("words.filter");
        connect(filter, &QPushButton::clicked, this, [this] { openFilter(); });
    }
    empty = new ui::InterfaceLabel(strings::get("words.empty"), this);
    empty->setObjectName("rrWordListEmpty");
    empty->setTextFormat(Qt::PlainText);
    empty->setAlignment(Qt::AlignCenter);
    ui::readerStyle(empty, ui::styles::font(chrome::labelText) + "background:transparent;");
    emptyHint = new ui::InterfaceLabel(strings::get("words.empty_hint"), this);
    emptyHint->setTextFormat(Qt::PlainText);
    emptyHint->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    emptyHint->setWordWrap(true);
    ui::readerStyle(emptyHint,
                    ui::styles::font(chrome::descriptionText)
                            + "color:palette(placeholder-text);background:transparent;");
    pager = new ui::PagerBar(this, scope == Scope::Book ? "rrWords" : "rrMyWords");
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        page = pager->turnTarget(-1);
        redraw();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        page = pager->turnTarget(1);
        redraw();
    });
    hide();
}

void WordList::setWords(QVector<SavedWord> words, bool pending)
{
    const bool changedList = all.size() != words.size();
    all = std::move(words);
    loading = pending;
    // A filter for a book whose last word was removed falls back to every book.
    if (!book.isEmpty() && std::none_of(all.cbegin(), all.cend(), [this](const SavedWord &w) {
            return w.book == book;
        }))
        book.clear();
    if (changedList && isHidden())
        page = 0;
    filterItems();
    arrange();
    redraw();
}

void WordList::setPages(QHash<QString, int> value)
{
    pages = std::move(value);
    filterItems();
    arrange();
    redraw();
}

void WordList::setSortOrder(const QString &value)
{
    const QStringList valid = scope == Scope::Book
            ? QStringList{ "position", "newest", "oldest" }
            : QStringList{ "newest", "oldest", "alphabetical" };
    const auto next = valid.contains(value) ? value : valid.first();
    if (order == next)
        return;
    order = next;
    page = 0;
    filterItems();
    arrange();
    redraw();
}

void WordList::setGlossFont(const QString &family)
{
    if (glossFamily == family)
        return;
    glossFamily = family;
    redraw();
}

void WordList::setExcerptFont(const QString &family)
{
    if (excerptFamily == family)
        return;
    excerptFamily = family;
    arrange();
    redraw();
}

// The bookmark list's excerpt face and size: the word reads as it does in the book.
QFont WordList::excerptFont() const
{
    QFont font = excerptFamily.isEmpty() ? ui::interfaceFont() : QFont(excerptFamily);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPixelSize(ui::metrics::search::excerptText);
    return font;
}

void WordList::filterItems()
{
    items.clear();
    for (const auto &word : all)
        if (book.isEmpty() || word.book == book)
            items.append(word);
    std::stable_sort(items.begin(), items.end(), [&](const SavedWord &a, const SavedWord &b) {
        if (order == "position") {
            const int left = pages.value(a.position, -1), right = pages.value(b.position, -1);
            if (left != right)
                return (left < 0 ? INT_MAX : left) < (right < 0 ? INT_MAX : right);
            return a.lookedUp > b.lookedUp;
        }
        if (order == "alphabetical")
            return QString::localeAwareCompare(SavedWord::key(a.word), SavedWord::key(b.word)) < 0;
        return order == "oldest" ? a.created < b.created : a.lookedUp > b.lookedUp;
    });
}

int WordList::top() const
{
    return scope == Scope::Book ? 0 : int(chrome::actionHeight + chrome::toolStripHeight);
}

// The bookmark row: a caption, one line in the book's face and a caption below.
int WordList::rowHeight() const
{
    const auto font = excerptFont();
    const int textLineHeight = ui::needsPrivateText(QStringLiteral("Mg"), font)
            ? ui::measurePrivateTextLayout(QStringLiteral("Mg"), font).lineHeight
            : QFontMetrics(font).lineSpacing();
    const int lineHeight = qMax(
            ui::px(44),
            qMax(textLineHeight, ui::interfaceLineHeight(ui::interfaceFont(chrome::captionText))));
    return qMax(int(ui::metrics::list::threeLines),
                2 * int(ui::metrics::list::lineInset) + 3 * lineHeight + int(ui::px(4))
                        + int(ui::metrics::control::border));
}

void WordList::resizeEvent(QResizeEvent *)
{
    arrange();
    redraw();
}

void WordList::hideEvent(QHideEvent *event)
{
    closePopup();
    ui::Panel::hideEvent(event);
}

void WordList::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    if (scope == Scope::Library)
        painter.fillRect(0, chrome::actionHeight - chrome::heavyRule, width(), chrome::heavyRule,
                         palette().color(QPalette::WindowText));
    if (scope == Scope::Library)
        painter.fillRect(0, top() - ui::metrics::control::border, width(),
                         ui::metrics::control::border, ui::dividerColor(this));
}

void WordList::arrange()
{
    if (scope == Scope::Library) {
        const int bar = chrome::actionHeight - chrome::heavyRule;
        backButton->setGeometry(0, 0, chrome::cellWidth, bar);
        const int countWidth =
                ui::interfaceTextWidth(count->text(), ui::interfaceFont(chrome::countText));
        const int titleLeft = chrome::cellWidth + chrome::inset,
                  countRight = width() - chrome::inset;
        count->setGeometry(countRight - countWidth, 0, countWidth, bar);
        title->setGeometry(titleLeft, 0,
                           qMax(1, countRight - countWidth - chrome::inset - titleLeft), bar);
        const int stripHeight = chrome::toolStripHeight;
        filter->setGeometry(0, chrome::actionHeight, width() - chrome::cellWidth,
                            stripHeight - ui::metrics::control::border);
        sortButton->setGeometry(width() - chrome::cellWidth, chrome::actionHeight,
                                chrome::cellWidth, stripHeight);
    }
    const int first = pageStarts.value(page, 0), height = rowHeight();
    const int available = qMax(1, this->height() - top());
    const int fits = qMax(1, available / height);
    const int perPage =
            items.size() > fits ? qMax(1, (available - chrome::actionHeight) / height) : fits;
    pageStarts.clear();
    for (int i = 0; i < qMax(1, int(items.size())); i += perPage)
        pageStarts.append(i);
    page = 0;
    while (page + 1 < pageStarts.size() && pageStarts[page + 1] <= first)
        ++page;
    pager->setGeometry(0, this->height() - chrome::actionHeight, width(), chrome::actionHeight);
    empty->setGeometry(chrome::inset, top() + height / 2, qMax(1, width() - 2 * chrome::inset),
                       height / 2);
    emptyHint->setGeometry(2 * chrome::inset, top() + height, qMax(1, width() - 4 * chrome::inset),
                           height);
}

void WordList::redraw()
{
    page = qBound(0, page, qMax(1, int(pageStarts.size())) - 1);
    const int first = pageStarts.value(page, 0);
    rows = qMin(int(items.size()) - first,
                page + 1 < pageStarts.size() ? pageStarts[page + 1] - first : INT_MAX);
    rows = qMax(0, rows);
    if (scope == Scope::Library) {
        count->setText(all.size() == 1 ? strings::get("words.count_one")
                                       : strings::get("words.count").arg(all.size()));
        QString shown = strings::get("words.all_books");
        for (const auto &word : all)
            if (word.book == book) {
                shown = word.title.isEmpty() ? strings::get("words.untitled") : word.title;
                break;
            }
        filter->setText(shown);
        filter->setEnabled(!loading && !all.isEmpty());
        sortButton->setEnabled(!loading && !all.isEmpty());
        arrange();
    }
    empty->setText(strings::get(loading ? "words.loading" : "words.empty"));
    empty->setVisible(items.isEmpty());
    emptyHint->setVisible(items.isEmpty() && !loading);
    pager->setVisible(pageStarts.size() > 1);
    pager->setPage(page, qMax(1, int(pageStarts.size())), {}, true);
    const int height = rowHeight();
    pager->setBlankArea({ chrome::inset, top() + rows * height, width() - 2 * chrome::inset,
                          pager->y() - top() - rows * height });
    while (buttons.size() < rows) {
        const int row = buttons.size();
        auto *button = new ui::ListRow(this);
        auto *remove = button->addAction({}, strings::get("words.remove"));
        remove->setParent(this);
        buttons.append(button);
        removes.append(remove);
        connect(button, &QPushButton::clicked, this, [this, row] {
            const int index = pageStarts.value(page, 0) + row;
            if (index < items.size() && selected)
                selected(items[index]);
        });
        connect(remove, &QPushButton::clicked, this, [this, row] {
            const int index = pageStarts.value(page, 0) + row;
            if (index >= items.size() || !removeRequested)
                return;
            const auto word = items[index];
            confirmWordRemoval(scope == Scope::Book && parentWidget() ? parentWidget() : this, word,
                               [this, word] {
                                   if (removeRequested)
                                       removeRequested(word);
                               });
        });
    }
    int numberWidth = 0;
    if (scope == Scope::Book)
        for (int i = first; i < first + rows; ++i)
            numberWidth =
                    qMax(numberWidth,
                         ui::interfaceTextWidth(QString::number(pageOf(items[i].position) + 1),
                                                ui::interfaceFont(chrome::captionText)));
    int y = top();
    // The book's face unless Settings chose a dictionary font, as in the sheet.
    QFont glossFont =
            glossFamily.isEmpty() ? ui::interfaceFont(chrome::descriptionText) : QFont(glossFamily);
    glossFont.setHintingPreference(QFont::PreferNoHinting);
    glossFont.setPixelSize(chrome::descriptionText);
    for (int row = 0; row < buttons.size(); ++row) {
        const bool shown = row < rows;
        buttons[row]->setVisible(shown);
        removes[row]->setVisible(shown);
        if (!shown)
            continue;
        const int index = first + row;
        const auto &word = items[index];
        auto *item = buttons[row];
        item->setObjectName(QString(scope == Scope::Book ? "rrWord%1" : "rrMyWord%1").arg(index));
        removes[row]->setObjectName(
                QString(scope == Scope::Book ? "rrWordRemove%1" : "rrMyWordRemove%1").arg(index));
        removes[row]->setAccessibleName(strings::get("words.remove"));
        item->setGeometry(0, y, width(), height);
        item->setActionIcon(ui::ControlIcon::Trash);
        item->setDetailFont(excerptFont());
        item->setMetadataFont(ui::interfaceFont(chrome::captionText));
        item->setCaptionFont(glossFont);
        // In the Library, the caption names the book; in a book, its chapter.
        item->setTitle(scope == Scope::Book ? word.chapter
                                            : (word.title.isEmpty() ? strings::get("words.untitled")
                                                                    : word.title));
        item->setTitlePixels(ui::px(24));
        item->setTitleGap(ui::px(4));
        item->titleLabel()->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
        item->setDetailLines(1);
        item->setDetailHighlight(QString(), QColor(), ui::px(44));
        item->setDetail(word.word);
        // A word saved without a dictionary entry says so in place of its definition.
        const auto gloss = word.gloss();
        item->setCaption(gloss.isEmpty() ? strings::get("words.no_entry") : gloss);
        item->setAccessibleName(word.word);
        const int pageNumber = scope == Scope::Book ? pageOf(word.position) : -1;
        item->setAccessory(ui::ListRow::Caption{
                pageNumber >= 0 ? QString::number(pageNumber + 1) : QString(), numberWidth });
        y += height;
    }
    update();
}

void WordList::closePopup()
{
    if (!popup)
        return;
    popup->hide();
    popup->deleteLater();
    popup.clear();
}

void WordList::openSort()
{
    closePopup();
    popup = new ui::ModalOverlay(this);
    popup->setObjectName("rrMyWordsSortPopup");
    popup->setGeometry(rect());
    popup->setDimmed(true);
    popup->dismissed = [this] { closePopup(); };
    auto *card = popup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(ui::metrics::notice::cardMargins.top());
    auto *heading = card->title(strings::get("marks.sort_title"));
    heading->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(ui::metrics::notice::cardMargins.bottom());
    auto *options = new ui::RadioList(card);
    options->setMinimumRowHeight(chrome::listActionHeight);
    options->setTopDivider(true);
    const QStringList values{ "newest", "oldest", "alphabetical" };
    const QStringList keys{ "marks.sort_newest", "marks.sort_oldest", "words.sort_alphabetical" };
    for (int i = 0; i < values.size(); ++i) {
        options->add("rrMyWordsSort_" + values[i], strings::get(keys[i].toLatin1().constData()),
                     strings::get((keys[i] + "_description").toLatin1().constData()));
        if (order == values[i])
            options->setCurrent(i);
    }
    options->chosen = [this, values](int index) {
        setSortOrder(values[index]);
        closePopup();
        if (sortRequested)
            sortRequested(order);
    };
    card->rows()->addWidget(options);
    const int cardWidth = qMin<int>(ui::metrics::notice::cardWidth,
                                    width() - 2 * ui::metrics::notice::sideClearance);
    options->setFixedHeight(options->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    popup->centerCard();
    popup->show();
    popup->raise();
}

// Every book with saved words, most recent lookup first, after All books.
void WordList::openFilter()
{
    closePopup();
    QStringList keys{ QString() }, names{ strings::get("words.all_books") };
    QSet<QString> seen;
    for (const auto &word : all)
        if (!seen.contains(word.book)) {
            seen.insert(word.book);
            keys.append(word.book);
            names.append(word.title.isEmpty() ? strings::get("words.untitled") : word.title);
        }
    popup = new ui::ModalOverlay(this);
    popup->setObjectName("rrMyWordsFilterPopup");
    popup->setGeometry(rect());
    popup->setDimmed(true);
    popup->dismissed = [this] { closePopup(); };
    auto *card = popup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(ui::metrics::notice::cardMargins.top());
    auto *heading = card->title(strings::get("words.filter"));
    heading->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(ui::metrics::notice::cardMargins.bottom());
    auto *options = new ui::RadioList(card);
    options->setMinimumRowHeight(chrome::listActionHeight);
    options->setTopDivider(true);
    for (int i = 0; i < names.size(); ++i) {
        options->add(QString("rrMyWordsBook%1").arg(i), names[i]);
        if (keys[i] == book)
            options->setCurrent(i);
    }
    const int cardWidth = qMin<int>(ui::metrics::notice::cardWidth,
                                    width() - 2 * ui::metrics::notice::sideClearance);
    // A long list pages inside the card, below its title.
    const int rowsHeight = options->rowHeight(cardWidth - 4);
    const int room = height() - 4 * chrome::actionHeight;
    const int perPage = qMax(1, room / qMax(1, rowsHeight));
    auto *paging = new ui::PagerBar(card, "rrMyWordsBooks");
    auto shownPage = std::make_shared<int>(qMax(0, int(keys.indexOf(book))) / perPage);
    const int pageCount = (int(names.size()) + perPage - 1) / perPage;
    const auto showPage = [options, paging, shownPage, perPage, pageCount, names, cardWidth] {
        const int first = *shownPage * perPage;
        options->showRows(first, qMin(perPage, int(names.size()) - first));
        options->setFixedHeight(qMin(perPage, int(names.size()) - first)
                                * options->rowHeight(cardWidth - 4));
        paging->setPage(*shownPage, pageCount);
    };
    connect(paging->previous, &QPushButton::clicked, card, [showPage, shownPage] {
        --*shownPage;
        showPage();
    });
    connect(paging->next, &QPushButton::clicked, card, [showPage, shownPage] {
        ++*shownPage;
        showPage();
    });
    options->chosen = [this, keys](int index) {
        book = keys.value(index);
        page = 0;
        closePopup();
        filterItems();
        arrange();
        redraw();
    };
    card->rows()->addWidget(options);
    card->rows()->addWidget(paging);
    paging->setVisible(pageCount > 1);
    showPage();
    card->fitToWidth(cardWidth);
    popup->centerCard();
    popup->show();
    popup->raise();
}
} // namespace kobalt
