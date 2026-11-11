#include "word_page.h"
#include "dictionary_panel.h"
#include "word_list.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "reading/reading_density.h"
#include "support/strings.h"
#include <QDateTime>
#include <QLocale>
#include <QPainter>
#include <QRegularExpression>

namespace kobalt {
namespace chrome = ui::metrics::chrome;

WordPage::WordPage(QWidget *parent) : ui::Panel(parent)
{
    setObjectName("rrWordPage");
    backButton = new ui::CellButton(ui::ControlIcon::Back, this, chrome::backIcon);
    backButton->setObjectName("rrWordPageBack");
    backButton->setRule(Qt::RightEdge);
    backButton->setAccessibleName(strings::get("library.back"));
    connect(backButton, &QPushButton::clicked, this, [this] {
        if (back)
            back();
    });
    remove = new ui::CellButton(ui::ControlIcon::Trash, this);
    remove->setObjectName("rrWordPageRemove");
    remove->setRule(Qt::LeftEdge);
    remove->setAccessibleName(strings::get("words.remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!removeRequested)
            return;
        const auto word = current;
        confirmWordRemoval(this, word, [this, word] {
            if (removeRequested)
                removeRequested(word);
        });
    });
    passageCell = new ui::CellButton(ui::ControlIcon::Dictionary, this);
    passageCell->setObjectName("rrWordPagePassageCell");
    passageCell->setRule(Qt::LeftEdge);
    connect(passageCell, &QPushButton::clicked, this, [this] {
        if (passageRequested)
            passageRequested(current);
    });
    headword = new ui::PrivateTextLabel(this);
    headword->setObjectName("rrWordPageWord");
    headword->setTextFormat(Qt::PlainText);
    headword->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    definitionHeading = new ui::ListHeader(this);
    definitionHeading->setShaded(true);
    contextHeading = new ui::ListHeader(this);
    contextHeading->setShaded(true);
    definition = new ui::PagedText(this);
    definition->setObjectName("rrWordPageDefinition");
    definition->setMargins(
            QMargins(chrome::inset, chrome::blockTop, chrome::inset, chrome::blockTop));
    context = new ui::InterfaceLabel({}, this);
    context->setObjectName("rrWordPageContext");
    context->setTextFormat(Qt::RichText);
    context->setWordWrap(true);
    context->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    passage = new ui::Button(QString(), this);
    passage->setObjectName("rrWordPagePassage");
    ui::readerStyle(passage,
                    ui::styles::outlined() + "QPushButton{" + ui::styles::font(chrome::labelText)
                            + "}");
    passage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(passage, &QPushButton::clicked, this, [this] {
        if (passageRequested)
            passageRequested(current);
    });
    missing = new ui::InterfaceLabel(strings::get("words.book_missing"), this);
    missing->setObjectName("rrWordPageMissing");
    missing->setTextFormat(Qt::PlainText);
    missing->setAlignment(Qt::AlignCenter);
    missing->setWordWrap(true);
    ui::readerStyle(
            missing,
            ui::styles::font(chrome::descriptionText)
                    + QString("color:palette(placeholder-text);background:transparent;border:%1px dashed palette(mid);")
                              .arg(int(ui::metrics::control::border)));
    facts = new ui::InterfaceLabel({}, this);
    facts->setObjectName("rrWordPageFacts");
    facts->setTextFormat(Qt::PlainText);
    facts->setWordWrap(true);
    ui::readerStyle(facts,
                    ui::styles::font(chrome::descriptionText)
                            + "color:palette(placeholder-text);background:transparent;");
    pager = new ui::PagerBar(this, "rrWordPage");
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        definition->setPage(definition->page() - 1);
        arrange();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        definition->setPage(definition->page() + 1);
        arrange();
    });
    setFonts({}, {}, 100);
    hide();
}

QFont WordPage::bookFont(int pixels) const
{
    QFont font = bookFamily.isEmpty() ? ui::interfaceFont(pixels) : QFont(bookFamily);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPixelSize(pixels);
    return font;
}

// The headword and the sentence use the book's face, as in the dictionary
// sheet. The definition uses the dictionary font settings.
void WordPage::setFonts(const QString &family, const QString &definitionFamily,
                        int definitionPercent)
{
    bookFamily = family;
    headword->setFont(bookFont(chrome::titleText));
    ui::readerStyle(headword,
                    (family.isEmpty() ? QString()
                                      : QString("font-family:'%1';font-weight:400;").arg(family))
                            + ui::styles::font(chrome::titleText) + "background:transparent;");
    context->setFont(bookFont(ui::metrics::search::excerptText));
    ui::readerStyle(context,
                    (family.isEmpty() ? QString() : QString("font-family:'%1';").arg(family))
                            + QString("font-size:%1px;background:transparent;")
                                      .arg(int(ui::metrics::search::excerptText)));
    definition->setTextFont(DictionaryPanel::definitionFont(
            definitionFamily.isEmpty() ? family : definitionFamily, definitionPercent));
    definition->setMinimumLineHeight(
            DictionaryPanel::definitionLineHeight(definitionPercent, !definitionFamily.isEmpty()));
    // The first line's leading sits above its capitals. Give the last line the
    // same space below, so the definition has even padding in its section.
    const QFontMetrics metrics(definition->textFont());
    definition->setMargins(QMargins(chrome::inset, chrome::blockTop, chrome::inset,
                                    chrome::blockTop + metrics.lineSpacing() / 2));
    if (isVisible())
        arrange();
}

void WordPage::open(const SavedWord &word, const QString &action, const QString &note)
{
    current = word;
    setAccessibleName(word.word);
    definitionHeading->setCaptions(strings::get("words.definition"), word.dictionary);
    contextHeading->setCaptions(strings::get("words.in_context"), word.chapter);
    if (word.definition.isEmpty()) {
        // A word saved without a dictionary entry, such as a name, says so.
        definition->setHtml(QString("<p style=\"color:%1;\">%2</p>")
                                    .arg(ui::noteColor(this).name(),
                                         strings::get("words.no_entry_detail").toHtmlEscaped()));
    } else {
        definition->setHtml(DictionaryPanel::formatDefinition(word.definition));
    }
    definition->setPage(0);
    // Underline the word where the sentence has it, as search results do. An
    // edited lookup may name a word the sentence does not contain.
    auto html = word.context.toHtmlEscaped();
    const QRegularExpression match("\\b" + QRegularExpression::escape(word.word.toHtmlEscaped())
                                           + "\\b",
                                   QRegularExpression::CaseInsensitiveOption
                                           | QRegularExpression::UseUnicodePropertiesOption);
    const auto found = match.match(html);
    if (found.hasMatch())
        html.replace(found.capturedStart(), found.capturedLength(),
                     "<b><u>" + found.captured() + "</u></b>");
    context->setText(html);
    context->setVisible(!word.context.isEmpty());
    contextHeading->setVisible(!word.context.isEmpty() || !word.chapter.isEmpty());
    this->action = action;
    passage->setText(action);
    passageCell->setAccessibleName(action);
    missing->setText(note.isEmpty() ? strings::get("words.book_missing") : note);
    QStringList details;
    if (!word.title.isEmpty())
        details << word.title;
    if (word.lookups > 1)
        details << strings::get("words.looked_up").arg(word.lookups);
    if (word.created > 0)
        details << strings::get("words.first_looked_up")
                           .arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(word.created)
                                                           .toLocalTime()
                                                           .date(),
                                                   "d MMM yyyy"));
    facts->setText(details.join(QStringLiteral(" · ")));
    show();
    raise();
    arrange();
}

void WordPage::resizeEvent(QResizeEvent *)
{
    arrange();
}

void WordPage::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, chrome::actionHeight - chrome::heavyRule, width(), chrome::heavyRule,
                     palette().color(QPalette::WindowText));
}

void WordPage::arrange()
{
    const int bar = chrome::actionHeight - chrome::heavyRule,
              inner = qMax(1, width() - 2 * int(chrome::inset));
    // A short screen keeps the definition and the sentence; the action moves
    // into the bar and the lookup details leave.
    const bool compact = isShortHeight(height(), ui::density());
    passage->setVisible(!compact && !action.isEmpty());
    passageCell->setVisible(compact && !action.isEmpty());
    missing->setVisible(action.isEmpty());
    backButton->setGeometry(0, 0, chrome::cellWidth, bar);
    remove->setGeometry(width() - chrome::cellWidth, 0, chrome::cellWidth, bar);
    passageCell->setGeometry(width() - 2 * chrome::cellWidth, 0, chrome::cellWidth, bar);
    const int cells = passageCell->isVisible() ? 3 : 2;
    const int wordLeft = chrome::cellWidth + chrome::inset,
              wordWidth = qMax(1, width() - cells * chrome::cellWidth - 2 * chrome::inset);
    headword->setGeometry(wordLeft, 0, wordWidth, bar);
    headword->setText(ui::elideInterfaceText(current.word, headword->font(), wordWidth));
    const int header = ui::metrics::list::headerHeight;
    definitionHeading->setGeometry(0, chrome::actionHeight, width(), header);
    // Lay out the sentence and the button from the bottom up. The definition
    // takes the space that is left and pages when it needs more.
    const int pagerTop = height() - chrome::actionHeight;
    int bottom = pagerTop;
    const int factsHeight = compact || facts->text().isEmpty() ? 0 : facts->heightForWidth(inner);
    const int actionHeight = passage->isVisible() || missing->isVisible()
            ? qMax(passage->sizeHint().height(), int(chrome::actionHeight))
            : 0;
    const int contextHeight = context->isVisible() ? context->heightForWidth(inner) : 0;
    int y = definitionHeading->geometry().bottom() + 1;
    const int lower = (contextHeading->isVisible() ? header : 0)
            + (contextHeight ? chrome::blockTop + contextHeight : 0)
            + (actionHeight ? chrome::blockTop + actionHeight : 0)
            + (factsHeight ? chrome::textGap + factsHeight : 0) + chrome::blockTop;
    int definitionHeight = qMax(int(chrome::actionHeight), bottom - y - lower);
    definition->setGeometry(0, y, width(), definitionHeight);
    // A definition that fits keeps the sentence right below it.
    if (definition->pages() == 1) {
        definitionHeight = qMin(definitionHeight, definition->contentHeight());
        definition->setGeometry(0, y, width(), definitionHeight);
    }
    y += definitionHeight;
    contextHeading->setGeometry(0, y, width(), header);
    if (contextHeading->isVisible())
        y += header;
    if (contextHeight) {
        y += chrome::blockTop;
        context->setGeometry(chrome::inset, y, inner, contextHeight);
        y += contextHeight;
    }
    if (actionHeight) {
        y += chrome::blockTop;
        passage->setGeometry(chrome::inset, y, inner, actionHeight);
        missing->setGeometry(chrome::inset, y, inner, actionHeight);
        y += actionHeight;
    }
    if (factsHeight) {
        y += chrome::textGap;
        facts->setGeometry(chrome::inset, y, inner, factsHeight);
    }
    facts->setVisible(factsHeight > 0);
    const int pages = definition->pages();
    pager->setGeometry(0, pagerTop, width(), chrome::actionHeight);
    pager->setPage(definition->page(), pages);
    pager->setVisible(pages > 1);
    update();
}
} // namespace kobalt
