#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/controls/query_edit.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include "dictionary_panel.h"
#include "reading/reading_density.h"
#include "dictionaries/manager.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include <QPainter>
#include <QRawFont>
#include <QRegularExpression>
#include <QGuiApplication>
#include <QInputMethod>
#include <QHideEvent>
#include <QTextDocumentFragment>

namespace kobalt {
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;

namespace {
class DictionaryChoiceRow final : public ui::ListRow
{
public:
    DictionaryChoiceRow(QWidget *parent, bool columnDivider)
        : ui::ListRow(parent), columnDivider(columnDivider)
    {
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ui::ListRow::paintEvent(event);
        if (columnDivider) {
            QPainter painter(this);
            ui::paintEdges(painter, rect(), Qt::RightEdge, ui::dividerColor(this));
        }
    }

private:
    bool columnDivider;
};
} // namespace

DictionaryPanel::DictionaryPanel(QWidget *parent, KeyboardFactory makeKeyboard)
    : ui::Panel(parent, Qt::TopEdge)
{
    setEdgeWidth(chrome::heavyRule);
    setObjectName("rrDictionary");
    dictionary = new ui::CellButton(ui::ControlIcon::Dictionaries, this);
    dictionary->setObjectName("rrDictionarySelect");
    dictionary->setDropdownIndicator(true);
    dictionary->setRule(Qt::LeftEdge);
    headword = new ui::PrivateTextLabel(this);
    headword->setObjectName("rrDictionaryWord");
    headword->setTextFormat(Qt::PlainText);
    headword->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    edit = new ui::CellButton(ui::ControlIcon::EditHighlight, this);
    edit->setObjectName("rrDictionaryEdit");
    edit->setRule(Qt::LeftEdge);
    edit->setAccessibleName(strings::get("dictionary_panel.edit_lookup"));
    edit->setEnabled(bool(makeKeyboard));
    connect(edit, &QPushButton::clicked, this, [this] { editQuery(); });
    query = new ui::QueryEdit(this);
    query->setObjectName("rrDictionaryQuery");
    query->setMaxLength(256);
    query->setAccessibleName(strings::get("dictionary_panel.lookup_word"));
    query->setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    query->setTextMargins(chrome::inset, 0, chrome::inset, 0);
    ui::readerStyle(query,
                    "QLineEdit{border:none;padding:0;background:palette(window);"
                            + ui::styles::font(chrome::titleText) + "}");
    query->hide();
    editHint = new ui::InterfaceLabel(strings::get("dictionary_panel.edit_hint"), this);
    editHint->setObjectName("rrDictionaryEditHint");
    editHint->setWordWrap(true);
    ui::readerStyle(editHint,
                    ui::styles::font(chrome::descriptionText)
                            + "background:transparent;color:palette(placeholder-text);");
    editHint->hide();
    submit = new ui::CellButton(ui::ControlIcon::Check, this);
    submit->setObjectName("rrDictionarySubmit");
    submit->setRule(Qt::LeftEdge);
    submit->setAccessibleName(strings::get("dictionary_panel.update_lookup"));
    submit->hide();
    connect(submit, &QPushButton::clicked, this, [this] { submitQuery(); });
    connect(query, &QLineEdit::returnPressed, this, [this] { submitQuery(); });
    connect(query, &QLineEdit::textChanged, this,
            [this](const QString &value) { submit->setEnabled(!value.simplified().isEmpty()); });
    if (makeKeyboard) {
        // The keyboard stays at the bottom of the reader while the dictionary
        // remains a compact sheet above it. Separate parents also keep key and
        // dictionary damage in separate refresh rectangles.
        keyboard = makeKeyboard(parent, query);
        keyboard->setSubmitText(QString::fromUtf8("✓"));
        keyboard->submitted = [this] { submitQuery(); };
    }
    connect(dictionary, &QPushButton::clicked, this, [this] {
        if (dropdown->isVisible()) {
            dropdown->hide();
            arrange();
        } else {
            redrawChoices(true);
            dropdown->show();
            dropdown->raise();
            refresh();
        }
    });
    dictionary->setAccessibleName(strings::get("dictionary_panel.choose_dictionary"));
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrDictionaryClose");
    close->setRule(Qt::LeftEdge);
    close->setAccessibleName(strings::get("dictionary_panel.close_definition"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (editing)
            finishEditing();
        else if (dismissed)
            dismissed();
    });
    search = new ui::CellButton(ui::ControlIcon::Search, this);
    search->setObjectName("rrDictionarySearch");
    search->setAccessibleName(strings::get("dictionary_panel.search_in_book"));
    search->setRule(Qt::LeftEdge);
    connect(search, &QPushButton::clicked, this, [this] {
        if (searchRequested && !word.isEmpty())
            searchRequested(word);
    });
    text = new ui::PagedText(this);
    text->setObjectName("rrDefinition");
    text->setTextFont(ui::interfaceFont(chrome::labelText));
    text->setMargins(QMargins(chrome::inset, chrome::blockTop, chrome::inset, chrome::blockTop));
    pages = new ui::PagerBar(this, "rrDictionary");
    pages->counter->setObjectName("rrDictionaryPages");
    connect(pages->previous, &QPushButton::clicked, this, [this] {
        text->setPage(text->page() - 1);
        refresh();
    });
    connect(pages->next, &QPushButton::clicked, this, [this] {
        text->setPage(text->page() + 1);
        refresh();
    });
    // An ink rule closes the list over the definition.
    dropdown = new ui::Panel(this, Qt::BottomEdge);
    dropdown->setObjectName("rrDictionaryChoices");
    choicesPaging = new ui::PagerBar(dropdown, "rrDictionaryChoices");
    choicesPaging->counter->setObjectName("rrDictionaryChoicesPages");
    connect(choicesPaging->previous, &QPushButton::clicked, this, [this] {
        optionsPage = choicesPaging->turnTarget(-1);
        redrawChoices();
    });
    connect(choicesPaging->next, &QPushButton::clicked, this, [this] {
        optionsPage = choicesPaging->turnTarget(1);
        redrawChoices();
    });
    dropdown->hide();
    hide();
}

int DictionaryPanel::rowTop() const
{
    return lower ? chrome::heavyRule : 0;
}

int DictionaryPanel::contentBottom() const
{
    return height() - (lower ? 0 : chrome::heavyRule);
}

void DictionaryPanel::place(const QRect &selection)
{
    this->selection = selection;
    const auto size = parentWidget()->size();
    int h = size.height() * metrics::lookup::heightPercent / 100;
    // A short screen leaves the usual share too little room for a definition.
    // The sheet grows into the half without the word, but never over it.
    if (!editing && isShortHeight(size.height(), ui::density())) {
        const bool below = selection.center().y() < size.height() / 2;
        const int room = below ? size.height() - selection.bottom() - 1 : selection.top();
        h = qBound(h, room - int(metrics::lookup::moveTolerance),
                   size.height() * metrics::lookup::shortHeightPercent / 100);
    }
    if (editing) {
        const auto keyboardBounds =
                parentWidget()->window()->property("reader_keyboard_bounds").toRect();
        if (!keyboardBounds.isEmpty()) {
            const int keyboardTop =
                    parentWidget()->mapFrom(parentWidget()->window(), keyboardBounds.topLeft()).y();
            h = qMin(h, keyboardTop);
        }
    }
    // The sheet takes the half without the word and meets the page with its rule.
    lower = !editing && selection.center().y() < size.height() / 2;
    setEdges(lower ? Qt::TopEdge : Qt::BottomEdge);
    // At the top, the sheet starts below a camera band.
    const int safeTop = lower ? 0 : parentWidget()->property("reader_safe_top").toInt();
    setGeometry(0, lower ? size.height() - h : safeTop, size.width(), h);
    arrange();
}

QString DictionaryPanel::formatDefinition(const QString &html)
{
    const QRegularExpression boundary("</w\\s*>", QRegularExpression::CaseInsensitiveOption);
    if (!html.contains(boundary))
        return html;
    // Nickel returns separate entries without their opening paragraph or word
    // tags. Qt otherwise carries a preceding definition list into the next entry.
    QStringList entries;
    for (auto entry : html.split(boundary)) {
        if (entry.trimmed().isEmpty())
            continue;
        entry.remove(QRegularExpression("<var\\b[^>]*>.*?</var\\s*>",
                                        QRegularExpression::CaseInsensitiveOption
                                                | QRegularExpression::DotMatchesEverythingOption));
        entry.replace(QRegularExpression("<a([^>]*)/>", QRegularExpression::CaseInsensitiveOption),
                      "<a\\1></a>");
        entry.replace(QRegularExpression("<p>\\s*(<(?:ol|ul)\\b)",
                                         QRegularExpression::CaseInsensitiveOption),
                      "\\1");
        entry.replace(QRegularExpression("(</(?:ol|ul)>)\\s*</p>",
                                         QRegularExpression::CaseInsensitiveOption),
                      "\\1");
        entries.append("<p>" + entry);
    }
    return entries.join(QString());
}

void DictionaryPanel::open(const QString &word, const DictionaryResult &result,
                           const QRect &selection)
{
    finishEditing(false);
    setAccessibleName(strings::get("dictionary_panel.definition_for").arg(word));
    this->word = word.simplified();
    // The word appears as it does in the book. A word the book face cannot draw
    // in full uses the interface sans, so fallback glyphs never mix in one word.
    const QFont book = bookFont(chrome::titleText);
    // QRawFont checks this face alone; QFontMetrics also counts fallback fonts.
    const auto bookRaw = QRawFont::fromFont(book);
    bool covered = !bookFamily.isEmpty();
    for (const auto character : this->word.toUcs4())
        if (covered && !bookRaw.supportsCharacter(character))
            covered = false;
    headwordFont = covered ? book : ui::interfaceFont(chrome::titleText);
    ui::readerStyle(headword,
                    (covered ? QString("font-family:'%1';font-weight:400;font-style:normal;")
                                       .arg(bookFamily)
                             : QString())
                            + ui::styles::font(chrome::titleText) + "background:transparent;");
    query->setFont(headwordFont);
    ui::readerStyle(query,
                    headword->property("reader_style").toString()
                            + "border:none;padding:0;background:palette(window);");
    const auto dictionaryName = result.dictionary.isEmpty()
            ? strings::get("dictionary_panel.dictionary_unavailable")
            : result.dictionary;
    dictionary->setAccessibleDescription(dictionaryName);
    dictionary->setToolTip(dictionaryName);
    currentId = result.dictionaryId;
    dropdown->hide();
    const auto note = QString("font-size:%1px;color:%2;")
                              .arg(int(chrome::descriptionText))
                              .arg(ui::noteColor(this).name());
    QString html = formatDefinition(result.html);
    // The adapter reports a word without an entry as an error. Name the word instead.
    const bool missing = result.received && result.restored && html.trimmed().isEmpty();
    if (!result.error.isEmpty() && !missing)
        html = "<p>" + result.error.toHtmlEscaped() + "</p>";
    else if (!result.received || html.trimmed().isEmpty())
        html = "<p>" + strings::get("dictionary_panel.no_definition_for").arg(word).toHtmlEscaped()
                + "</p><p style=\"" + note + "\">"
                + (options.size() > 1 ? strings::get("dictionary_panel.try_another_dictionary")
                                      : strings::get("dictionary_panel.try_another_word"))
                          .toHtmlEscaped()
                + "</p>";
    const bool downloaded = result.dictionaryId.startsWith("downloaded:");
    sourceText = downloaded && !result.source.isEmpty()
            ? strings::get("dictionary_panel.dictionary_sources")
                      .arg(QTextDocumentFragment::fromHtml(result.source).toPlainText())
            : QString();
    if (!downloaded && !result.source.isEmpty())
        html += QString("<hr><p style=\"font-family:'%1';font-size:%2px;color:%3;\">%4</p><p style=\"%5\">%6</p>")
                        .arg(ui::chromeFont().family())
                        .arg(int(chrome::captionText))
                        .arg(ui::noteColor(this).name())
                        .arg(strings::get("dictionary_panel.source").toUpper().toHtmlEscaped(),
                             note, result.source);
    definitionHtml = html;
    renderedHtml.clear();
    text->setPage(0);
    place(selection);
    show();
    raise();
    arrange();
}

QFont DictionaryPanel::bookFont(int pixels) const
{
    QFont font = bookFamily.isEmpty() ? ui::interfaceFont(pixels) : QFont(bookFamily);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPixelSize(pixels);
    return font;
}

void DictionaryPanel::setBookFont(const QString &family)
{
    if (bookFamily == family)
        return;
    bookFamily = family;
    if (definitionFamily.isEmpty())
        setDefinitionFont({}, definitionPercent);
}

QFont DictionaryPanel::definitionFont(const QString &family, int percent)
{
    QFont font = ui::interfaceFont(qMax(1, qRound(int(chrome::labelText) * percent / 100.0)));
    if (!family.isEmpty())
        font.setFamily(family);
    return font;
}

void DictionaryPanel::setDefinitionFont(const QString &family, int percent)
{
    definitionFamily = family;
    definitionPercent = percent;
    text->setTextFont(definitionFont(family.isEmpty() ? bookFamily : family, percent));
    text->setMinimumLineHeight(definitionLineHeight(percent, !family.isEmpty()));
    arrange();
}

int DictionaryPanel::definitionLineHeight(int percent, bool customFont)
{
    const int height = 100 + qMax(0, percent - 100) / 2;
    return customFont ? (height * 120 + 50) / 100 : (height > 100 ? height : 0);
}

void DictionaryPanel::setLanguage(const QString &value)
{
    language = DictionaryManager::normalizeLanguage(value);
}

void DictionaryPanel::setChoices(QVector<DictionaryChoice> values)
{
    options = std::move(values);
    dictionary->setEnabled(!editing && options.size() > 1);
}

void DictionaryPanel::resizeEvent(QResizeEvent *)
{
    arrange();
}

void DictionaryPanel::hideEvent(QHideEvent *event)
{
    finishEditing(false);
    ui::Panel::hideEvent(event);
}

void DictionaryPanel::keyPressEvent(QKeyEvent *event)
{
    if (editing && event->key() == Qt::Key_Escape) {
        finishEditing();
        event->accept();
        return;
    }
    ui::Panel::keyPressEvent(event);
}

void DictionaryPanel::editQuery()
{
    if (!keyboard || editing)
        return;
    dropdown->hide();
    editing = true;
    if (prepareEditorFont)
        prepareEditorFont(headwordFont);
    query->setFont(ui::renewedFont(headwordFont));
    query->setText(word);
    place(selection);
    query->setFocus();
    keyboard->show();
    place(selection);
}

void DictionaryPanel::finishEditing(bool restore)
{
    if (!editing)
        return;
    editing = false;
    ui::clearQueryFocus(query);
    keyboard->hide();
    QGuiApplication::inputMethod()->hide();
    if (restore)
        place(selection);
    else {
        query->hide();
        editHint->hide();
        submit->hide();
    }
}

void DictionaryPanel::submitQuery()
{
    if (!editing)
        return;
    const auto value = query->text().simplified();
    if (value.isEmpty())
        return;
    finishEditing();
    if (requested)
        requested(value);
}

void DictionaryPanel::updateDefinition()
{
    QString html = definitionHtml;
    if (!sourceText.isEmpty()) {
        const auto font = ui::interfaceFont(chrome::descriptionText);
        const auto source =
                ui::elideInterfaceText(sourceText, font, qMax(1, width() - 2 * chrome::inset));
        html += QString("<hr><p style=\"font-family:'%1';font-size:%2px;color:%3;\">%4</p>")
                        .arg(font.family().toHtmlEscaped())
                        .arg(int(chrome::descriptionText))
                        .arg(ui::noteColor(this).name(), source.toHtmlEscaped());
    }
    if (html == renderedHtml)
        return;
    renderedHtml = html;
    const int page = text->page();
    text->setHtml(html);
    text->setPage(page);
}

void DictionaryPanel::arrange()
{
    headword->setVisible(!editing);
    edit->setVisible(!editing);
    query->setVisible(editing);
    editHint->setVisible(editing);
    submit->setVisible(editing);
    search->setVisible(!editing);
    close->setAccessibleName(strings::get(editing ? "dictionary_panel.cancel_edit"
                                                  : "dictionary_panel.close_definition"));
    dictionary->setEnabled(!editing && options.size() > 1);
    const int top = rowTop(), bottom = contentBottom();
    // The word leads the row, like a printed entry. The dictionary sits beside close.
    // On a narrow panel the actions use compact cells, and the dictionary's name
    // wraps or shortens before the word does, so a word of ordinary length stays whole.
    const int cell = isNarrowWidth(width(), ui::density()) ? int(chrome::compactCellWidth)
                                                           : int(chrome::cellWidth);
    int selectorWidth = dictionary->sizeHint().width();
    const int actionCells = editing ? 2 : 3;
    const int wordRoom = width() - actionCells * cell - 2 * int(chrome::inset);
    const int wordWanted = qMin(ui::interfaceTextWidth(word, headwordFont), width() / 2);
    if (wordRoom - selectorWidth < wordWanted)
        selectorWidth = dictionary->widthForLabel(qMax(cell, wordRoom - wordWanted));
    const int selectorLeft = width() - cell - selectorWidth;
    dictionary->setGeometry(selectorLeft, top, selectorWidth, chrome::actionHeight);
    close->setGeometry(width() - cell, top, cell, chrome::actionHeight);
    const int editorLeft = selectorLeft - cell;
    const int editLeft = editorLeft - (editing ? 0 : cell);
    edit->setGeometry(editLeft, top, cell, chrome::actionHeight);
    search->setGeometry(editorLeft, top, cell, chrome::actionHeight);
    submit->setGeometry(search->geometry());
    query->setGeometry(0, top, qMax(1, editorLeft), chrome::actionHeight);
    const int wordWidth = qMax(1, editLeft - 2 * chrome::inset);
    headword->setGeometry(chrome::inset, top, wordWidth, chrome::actionHeight);
    headword->setText(ui::elideInterfaceText(word, headwordFont, wordWidth));
    int textTop = top + chrome::actionHeight + metrics::control::border;
    if (editing) {
        const int hintWidth = qMax(1, width() - 2 * chrome::inset);
        const int hintHeight = editHint->heightForWidth(hintWidth);
        editHint->setGeometry(chrome::inset, textTop + chrome::textGap, hintWidth, hintHeight);
        textTop = editHint->geometry().bottom() + 1 + chrome::textGap;
    }
    // A single page needs no pager, so it can use the whole sheet. Measuring
    // without the pager can clamp the page; restore it afterwards.
    updateDefinition();
    const int wanted = text->page();
    text->setGeometry(0, textTop, width(), qMax(1, bottom - textTop));
    if (text->pages() > 1)
        text->setGeometry(0, textTop, width(), qMax(1, bottom - chrome::actionHeight - textTop));
    text->setPage(wanted);
    pages->setGeometry(0, bottom - chrome::actionHeight, width(), chrome::actionHeight);
    refresh();
    if (dropdown->isVisible())
        redrawChoices();
}

void DictionaryPanel::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, rowTop() + chrome::actionHeight, width(), metrics::control::border,
                     palette().color(QPalette::Mid));
}

void DictionaryPanel::redrawChoices(bool atCurrent)
{
    for (auto *button : optionButtons) {
        button->hide();
        button->setObjectName({});
        button->deleteLater();
    }
    optionButtons.clear();
    for (auto *child : dropdown->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (auto *header = dynamic_cast<ui::ListHeader *>(child)) {
            header->hide();
            header->deleteLater();
        }

    struct Page
    {
        QVector<int> indices;
    };

    QVector<Page> pages(1);
    const int columns = width() >= ui::px(900) ? 2 : 1;
    const int rowHeight = metrics::lookup::choiceHeight;
    int used = 0, column = 0;
    QString previous;
    const int available =
            contentBottom() - rowTop() - chrome::actionHeight - 2 * metrics::control::border;
    const int usable =
            qMax(int(metrics::list::headerHeight) + rowHeight, available - chrome::actionHeight);
    const auto heading = [this](const DictionaryChoice &choice) {
        if (language.isEmpty() || QLocale(language).language() == QLocale::C)
            return strings::get("dictionaries.section_installed");
        if (!choice.fallback && !choice.language.isEmpty() && choice.language != language)
            return strings::get("dictionaries.installed_other");
        return strings::get("dictionary_panel.for_language")
                .arg(DictionaryManager::languageName(language));
    };
    for (int i = 0; i < options.size(); ++i) {
        const QString caption = heading(options[i]);
        if (previous != caption)
            column = 0;
        int header = previous != caption ? int(metrics::list::headerHeight) : 0;
        if (used && column == 0 && used + header + rowHeight > usable) {
            pages.append(Page{});
            used = 0;
            previous.clear();
            header = metrics::list::headerHeight;
        }
        if (column == 0)
            used += header + rowHeight;
        previous = caption;
        pages.last().indices.append(i);
        column = (column + 1) % columns;
    }
    if (atCurrent)
        for (int p = 0; p < pages.size(); ++p)
            for (int index : pages[p].indices)
                if (options[index].id == currentId)
                    optionsPage = p;
    optionsPage = qBound(0, optionsPage, int(pages.size()) - 1);
    int y = 0;
    column = 0;
    previous.clear();
    for (int index : pages[optionsPage].indices) {
        const auto &option = options[index];
        const auto caption = heading(option);
        if (previous != caption) {
            if (column)
                y += rowHeight;
            column = 0;
            auto *header = new ui::ListHeader(dropdown);
            header->setCaptions(caption);
            header->setGeometry(0, y, width(), metrics::list::headerHeight);
            header->show();
            y += metrics::list::headerHeight;
            previous = caption;
        }
        auto *button = new DictionaryChoiceRow(dropdown, columns > 1 && column == 0);
        button->setObjectName(QString("rrDictionaryChoice%1").arg(optionButtons.size()));
        button->setTitle(option.name);
        button->setCheckable(true);
        button->setChecked(option.id == currentId);
        button->setToolTip(option.name);
        const int left = column * width() / columns, right = (column + 1) * width() / columns;
        button->setGeometry(left, y, right - left, rowHeight);
        button->show();
        optionButtons.append(button);
        column = (column + 1) % columns;
        if (!column)
            y += rowHeight;
        connect(button, &QPushButton::clicked, this, [this, id = option.id] {
            dropdown->hide();
            refresh();
            if (chosen)
                chosen(id);
        });
    }
    if (column)
        y += rowHeight;
    choicesPaging->setPage(optionsPage, pages.size(), {}, true);
    choicesPaging->setGeometry(0, y, width(), chrome::actionHeight);
    dropdown->setGeometry(0, rowTop() + chrome::actionHeight + metrics::control::border, width(),
                          y + (pages.size() > 1 ? chrome::actionHeight : 0)
                                  + metrics::control::border);
    // The selector covers the definition without changing its pagination.
    dropdown->raise();
}

void DictionaryPanel::refresh()
{
    // Counting the pages lays the text out and clamps its page first.
    const int count = text->pages();
    pages->setPage(text->page(), count);
    pages->setVisible(!dropdown->isVisible() && count > 1);
}
} // namespace kobalt
