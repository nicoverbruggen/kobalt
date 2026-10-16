#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/style/theme.h"
#include "support/strings.h"
#include "dictionary_panel.h"
#include "dictionaries/manager.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include <QFontMetrics>
#include <QPainter>
#include <QRawFont>
#include <QRegularExpression>

namespace kobalt {
namespace m = ui::metrics;
namespace c = ui::metrics::chrome;
DictionaryPanel::DictionaryPanel(QWidget *parent) : ui::Panel(parent, Qt::TopEdge) {
    setEdgeWidth(c::heavyRule);
    setObjectName("rrDictionary");
    dictionary = new ui::CellButton(ui::ControlIcon::Down, this, c::selectorIcon);
    dictionary->setObjectName("rrDictionarySelect");
    dictionary->setRule(Qt::LeftEdge);
    headword = new ui::PrivateTextLabel(this);
    headword->setObjectName("rrDictionaryWord");
    headword->setTextFormat(Qt::PlainText);
    headword->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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
        if (dismissed)
            dismissed();
    });
    text = new ui::PagedText(this);
    text->setObjectName("rrDefinition");
    text->setTextFont(ui::interfaceFont(c::labelText));
    text->setMargins(QMargins(c::inset, c::blockTop, c::inset, c::blockTop));
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
int DictionaryPanel::rowTop() const {
    return lower ? c::heavyRule : 0;
}
int DictionaryPanel::contentBottom() const {
    return height() - (lower ? 0 : c::heavyRule);
}
void DictionaryPanel::place(const QRect &selection) {
    const auto size = parentWidget()->size();
    const int h = size.height() * m::lookup::heightPercent / 100;
    // The sheet takes the half without the word and meets the page with its rule.
    lower = selection.center().y() < size.height() / 2;
    setEdges(lower ? Qt::TopEdge : Qt::BottomEdge);
    setGeometry(0, lower ? size.height() - h : 0, size.width(), h);
    arrange();
}
QString DictionaryPanel::formatDefinition(const QString &html) {
    const QRegularExpression boundary("</w\\s*>", QRegularExpression::CaseInsensitiveOption);
    if (!html.contains(boundary))
        return html;
    // Nickel returns separate entries without their opening paragraph or word
    // tags. Qt otherwise carries a preceding definition list into the next entry.
    QStringList entries;
    for (auto entry : html.split(boundary)) {
        if (entry.trimmed().isEmpty())
            continue;
        entry.remove(
            QRegularExpression("<var\\b[^>]*>.*?</var\\s*>", QRegularExpression::CaseInsensitiveOption |
                                                                 QRegularExpression::DotMatchesEverythingOption));
        entry.replace(QRegularExpression("<a([^>]*)/>", QRegularExpression::CaseInsensitiveOption), "<a\\1></a>");
        entry.replace(QRegularExpression("<p>\\s*(<(?:ol|ul)\\b)", QRegularExpression::CaseInsensitiveOption), "\\1");
        entry.replace(QRegularExpression("(</(?:ol|ul)>)\\s*</p>", QRegularExpression::CaseInsensitiveOption), "\\1");
        entries.append("<p>" + entry);
    }
    return entries.join(QString());
}
void DictionaryPanel::open(const QString &word, const DictionaryResult &result, const QRect &selection) {
    setAccessibleName(strings::get("dictionary_panel.definition_for").arg(word));
    this->word = word.simplified();
    // The word appears as it does in the book. A word the book face cannot draw
    // in full uses the interface sans, so fallback glyphs never mix in one word.
    const QFont book = bookFont(c::titleText);
    // QRawFont checks this face alone; QFontMetrics also counts fallback fonts.
    const auto bookRaw = QRawFont::fromFont(book);
    bool covered = !bookFamily.isEmpty();
    for (const auto character : this->word.toUcs4())
        if (covered && !bookRaw.supportsCharacter(character))
            covered = false;
    headwordFont = covered ? book : ui::interfaceFont(c::titleText);
    ui::readerStyle(headword, (covered ? QString("font-family:'%1';font-weight:400;font-style:normal;").arg(bookFamily)
                                       : QString()) +
                                  ui::styles::font(c::titleText) + "background:transparent;");
    dictionary->setText(result.dictionary.isEmpty() ? strings::get("dictionary_panel.dictionary_unavailable")
                                                    : result.dictionary);
    currentId = result.dictionaryId;
    dropdown->hide();
    const auto note = QString("font-size:%1px;color:%2;").arg(c::descriptionText).arg(ui::noteColor(this).name());
    QString html = formatDefinition(result.html);
    // The adapter reports a word without an entry as an error. Name the word instead.
    const bool missing = result.received && result.restored && html.trimmed().isEmpty();
    if (!result.error.isEmpty() && !missing)
        html = "<p>" + result.error.toHtmlEscaped() + "</p>";
    else if (!result.received || html.trimmed().isEmpty())
        html = "<p>" + strings::get("dictionary_panel.no_definition_for").arg(word).toHtmlEscaped() +
               "</p><p style=\"" + note + "\">" +
               (options.size() > 1 ? strings::get("dictionary_panel.try_another_dictionary")
                                   : strings::get("dictionary_panel.try_another_word"))
                   .toHtmlEscaped() +
               "</p>";
    if (!result.source.isEmpty())
        html += QString("<hr><p style=\"font-family:'%1';font-size:%2px;color:%3;\">%4</p><p style=\"%5\">%6</p>")
                    .arg(ui::chromeFont().family())
                    .arg(c::captionText)
                    .arg(ui::noteColor(this).name())
                    .arg(strings::get("dictionary_panel.source").toUpper().toHtmlEscaped(), note, result.source);
    place(selection);
    text->setHtml(html);
    show();
    raise();
    arrange();
}
QFont DictionaryPanel::bookFont(int pixels) const {
    QFont font = bookFamily.isEmpty() ? ui::interfaceFont(pixels) : QFont(bookFamily);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPixelSize(pixels);
    return font;
}
void DictionaryPanel::setBookFont(const QString &family) {
    bookFamily = family;
}
void DictionaryPanel::setLanguage(const QString &value) {
    language = DictionaryManager::normalizeLanguage(value);
}
void DictionaryPanel::setChoices(QVector<DictionaryChoice> values) {
    options = std::move(values);
    dictionary->setEnabled(options.size() > 1);
}
void DictionaryPanel::resizeEvent(QResizeEvent *) {
    arrange();
}
void DictionaryPanel::arrange() {
    const int top = rowTop(), bottom = contentBottom();
    // The word leads the row, like a printed entry. The dictionary sits beside close.
    int wantedSelector = dictionary->sizeHint().width();
    for (const auto &option : options)
        wantedSelector = qMax(wantedSelector,
                              dictionary->fontMetrics().boundingRect(option.name).width() + c::cellIcon + 2 * c::inset);
    const int selector = qMin(wantedSelector, (width() - c::cellWidth) * 2 / 3);
    const int selectorLeft = width() - c::cellWidth - selector;
    dictionary->setGeometry(selectorLeft, top, selector, c::actionHeight);
    close->setGeometry(width() - c::cellWidth, top, c::cellWidth, c::actionHeight);
    const int wordWidth = qMax(1, selectorLeft - 2 * c::inset);
    headword->setGeometry(c::inset, top, wordWidth, c::actionHeight);
    headword->setText(ui::elideInterfaceText(word, headwordFont, wordWidth));
    const int textTop = top + c::actionHeight + m::control::border;
    // A single page needs no pager, so it can use the whole sheet. Measuring
    // without the pager can clamp the page; restore it afterwards.
    const int wanted = text->page();
    text->setGeometry(0, textTop, width(), qMax(1, bottom - textTop));
    if (text->pages() > 1)
        text->setGeometry(0, textTop, width(), qMax(1, bottom - c::actionHeight - textTop));
    text->setPage(wanted);
    pages->setGeometry(0, bottom - c::actionHeight, width(), c::actionHeight);
    refresh();
    if (dropdown->isVisible())
        redrawChoices();
}
void DictionaryPanel::paintEvent(QPaintEvent *event) {
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, rowTop() + c::actionHeight, width(), m::control::border, palette().color(QPalette::Mid));
}
void DictionaryPanel::redrawChoices(bool atCurrent) {
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
    const int available = contentBottom() - rowTop() - c::actionHeight - 2 * m::control::border;
    const int usable = qMax(int(m::list::headerHeight + m::list::oneLine), available - c::actionHeight);
    struct Page {
        QVector<int> indices;
    };
    QVector<Page> pages(1);
    int used = 0;
    QString previous;
    auto heading = [this](const DictionaryChoice &choice) {
        if (language.isEmpty() || QLocale(language).language() == QLocale::C)
            return strings::get("dictionaries.section_installed");
        if (!choice.fallback && !choice.language.isEmpty() && choice.language != language)
            return strings::get("dictionaries.installed_other");
        return language.isEmpty()
                   ? strings::get("dictionaries.section_installed")
                   : strings::get("dictionary_panel.for_language").arg(DictionaryManager::languageName(language));
    };
    for (int i = 0; i < options.size(); ++i) {
        const QString caption = heading(options[i]);
        const int header = previous != caption ? int(m::list::headerHeight) : 0;
        if (used && used + header + m::list::oneLine > usable) {
            pages.append(Page{});
            used = 0;
            previous.clear();
        }
        used += (previous != caption ? int(m::list::headerHeight) : 0) + m::list::oneLine;
        pages.last().indices.append(i);
        previous = caption;
    }
    if (atCurrent)
        for (int p = 0; p < pages.size(); ++p)
            for (int index : pages[p].indices)
                if (options[index].id == currentId)
                    optionsPage = p;
    optionsPage = qBound(0, optionsPage, int(pages.size()) - 1);
    int y = 0;
    previous.clear();
    for (int index : pages[optionsPage].indices) {
        const auto &option = options[index];
        const auto caption = heading(option);
        if (previous != caption) {
            auto *header = new ui::ListHeader(dropdown);
            header->setCaptions(caption);
            header->setGeometry(0, y, width(), m::list::headerHeight);
            header->show();
            y += m::list::headerHeight;
            previous = caption;
        }
        auto *button = new ui::ListRow(dropdown);
        button->setObjectName(QString("rrDictionaryChoice%1").arg(optionButtons.size()));
        button->setTitle(option.name);
        button->setCheckable(true);
        button->setChecked(option.id == currentId);
        button->setGeometry(0, y, width(), m::list::oneLine);
        button->show();
        optionButtons.append(button);
        y += m::list::oneLine;
        connect(button, &QPushButton::clicked, this, [this, id = option.id] {
            dropdown->hide();
            refresh();
            if (chosen)
                chosen(id);
        });
    }
    choicesPaging->setPage(optionsPage, pages.size(), {}, true);
    choicesPaging->setGeometry(0, y, width(), c::actionHeight);
    dropdown->setGeometry(0, rowTop() + c::actionHeight + m::control::border, width(),
                          y + (pages.size() > 1 ? c::actionHeight : 0) + m::control::border);
    // The selector covers the definition without changing its pagination.
    dropdown->raise();
}
void DictionaryPanel::refresh() {
    pages->setPage(text->page(), text->pages());
    pages->setVisible(!dropdown->isVisible() && text->pages() > 1);
}
}
