#include "note_editor.h"
#include "render/highlight_paint.h"
#include "support/strings.h"
#include "ui/controls/text.h"
#include "ui/dialogs/modals.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include <QDynamicPropertyChangeEvent>
#include <QFontMetrics>
#include <QTextDocument>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace kobalt {
namespace chrome = ui::metrics::chrome;
namespace notice = ui::metrics::notice;

namespace {
// The text box shows four lines, or two when the keyboard leaves less room.
constexpr int roomyLines = 4, tightLines = 2;
// A longer passage ends with an ellipsis on its last shown line.
constexpr int passageLines = 3;

// The padding around the text inside the box border.
int boxPadding()
{
    return ui::px(14);
}

// Whole lines, so that a box scrolled to the cursor never cuts a line.
int boxHeight(const QTextEdit *editor, int lines)
{
    const int line = QFontMetrics(editor->font()).lineSpacing();
    return lines * line + 2 * boxPadding() + 2 * int(ui::metrics::control::border);
}
} // namespace

NoteEditor::NoteEditor(QWidget *parent, const KeyboardFactory &makeKeyboard, const QString &title,
                       const NotePassage &passage, const QString &note)
    : QObject(parent)
{
    dialog = new ui::ChoiceModal(parent, { {}, title, {} },
                                 { strings::get("common.cancel"), "rrNoteCancel" },
                                 { strings::get("common.save"), "rrNoteSave" });
    dialog->setObjectName("rrNoteEditor");
    dialog->setDimmed(true);
    dialog->installEventFilter(this);
    parent->window()->installEventFilter(this);
    heading = dialog->titleLabel()->parentWidget();
    auto *card = dialog->card();

    auto *passageRow = new QWidget(card);
    passageRow->setObjectName("rrNotePassage");
    auto *passageLayout = new QVBoxLayout(passageRow);
    // The heading above keeps its own gap.
    passageLayout->setContentsMargins(notice::cardMargins.left(), 0, notice::cardMargins.right(),
                                      0);
    passageLayout->setSpacing(ui::px(8));
    if (!passage.chapter.isEmpty()) {
        auto *chapter = new ui::InterfaceLabel(passage.chapter, passageRow);
        chapter->setObjectName("rrNoteChapter");
        chapter->setFont(ui::interfaceFont(chrome::captionText));
        ui::readerStyle(chapter, ui::styles::font(chrome::captionText));
        passageLayout->addWidget(chapter);
    }
    auto *quoteLabel = new ui::PrivateTextLabel(passageRow);
    quote = quoteLabel;
    quoteLabel->setObjectName("rrNoteQuote");
    quoteLabel->setTextFormat(Qt::PlainText);
    quoteLabel->setFont(passage.font);
    // The card's style sheet sets the interface font; name the reading font
    // here, as list rows do for their excerpts.
    auto family = passage.font.family();
    family.replace('\\', "\\\\");
    family.replace('"', "\\\"");
    ui::readerStyle(quoteLabel,
                    QString("font-family:\"%1\";font-size:%2px;font-weight:400;"
                            "font-style:normal;background:transparent;border:none;")
                            .arg(family)
                            .arg(passage.font.pixelSize()));
    quoteLabel->setWordWrap(true);
    quoteLabel->setMaximumLines(passageLines);
    quoteLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    quoteLabel->setHighlight(passage.style.isEmpty() ? QStringLiteral("shaded") : passage.style,
                             render::highlightTint(passage.color), ui::px(44));
    quoteLabel->setText(passage.text.simplified());
    passageLayout->addWidget(quoteLabel);
    card->rows()->insertWidget(1, passageRow);

    auto *textRow = new QWidget(card);
    auto *textLayout = new QVBoxLayout(textRow);
    textLayout->setContentsMargins(notice::cardMargins.left(), notice::cardGap,
                                   notice::cardMargins.right(), notice::cardGap);
    textLayout->setSpacing(ui::px(8));
    editor = new QTextEdit(textRow);
    editor->setObjectName("rrNoteText");
    editor->setAcceptRichText(false);
    editor->setTabChangesFocus(true);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setAccessibleName(strings::get("annotations.note_placeholder"));
    editor->setPlaceholderText(strings::get("annotations.note_placeholder"));
    editor->setFont(ui::interfaceFont(chrome::labelText));
    ui::readerStyle(editor,
                    QString("QTextEdit{border:%1px solid palette(mid);border-radius:0;padding:%2px;"
                            "background:palette(window);color:palette(window-text);")
                                    .arg(int(ui::metrics::control::border))
                                    .arg(boxPadding())
                            + ui::styles::font(chrome::labelText) + "}");
    editor->document()->setDocumentMargin(0);
    editor->setPlainText(note);
    editor->moveCursor(QTextCursor::End);
    editor->setFixedHeight(boxHeight(editor, roomyLines));
    textLayout->addWidget(editor);
    auto *errorLabel = new ui::InterfaceLabel(textRow);
    error = errorLabel;
    errorLabel->setObjectName("rrNoteError");
    errorLabel->setWordWrap(true);
    errorLabel->setFont(ui::interfaceFont(chrome::captionText));
    ui::readerStyle(errorLabel, ui::styles::font(chrome::captionText));
    errorLabel->hide();
    textLayout->addWidget(errorLabel);
    card->rows()->insertWidget(2, textRow);

    connect(dialog->primaryButton(), &QPushButton::clicked, this, [this] {
        if (!dialog || !dialog->isVisible() || saving_)
            return;
        if (saveRequested) {
            saving_ = true;
            editor->setReadOnly(true);
            dialog->primaryButton()->setEnabled(false);
            dialog->secondaryButton()->setEnabled(false);
            saveRequested(text());
        }
    });
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, [this] { close(); });
    dialog->dismissed = [this] { close(); };
    dialog->setFocusPolicy(Qt::StrongFocus);
    place();
    dialog->show();
    dialog->raise();
    if (makeKeyboard) {
        keyboard = makeKeyboard(dialog, editor);
        keyboard->setSubmitText(strings::get("keyboard.done"));
        keyboard->submitted = [this] {
            // Submission may be inside the keyboard controller's own callback.
            QTimer::singleShot(0, this, [this] {
                if (keyboard)
                    keyboard->hide();
                if (dialog)
                    dialog->setFocus();
                place();
            });
        };
        keyboard->show();
    } else {
        editor->setFocus();
    }
    place();
}

NoteEditor::~NoteEditor()
{
    if (keyboard) {
        keyboard->hide();
        keyboard->submitted = {};
        keyboard.reset();
    }
    if (dialog) {
        dialog->hide();
        dialog->deleteLater();
    }
}

QString NoteEditor::text() const
{
    return editor ? editor->toPlainText() : QString();
}

void NoteEditor::saved()
{
    saving_ = false;
    close();
}

void NoteEditor::setError(const QString &message)
{
    saving_ = false;
    editor->setReadOnly(false);
    dialog->primaryButton()->setEnabled(true);
    dialog->secondaryButton()->setEnabled(true);
    if (!error)
        return;
    error->setText(message);
    error->setVisible(!message.isEmpty());
    place();
}

void NoteEditor::close()
{
    if (saving_ || !dialog || !dialog->isVisible())
        return;
    if (keyboard)
        keyboard->hide();
    dialog->hide();
    const auto callback = closed;
    if (callback)
        callback();
}

void NoteEditor::place()
{
    if (!dialog || !dialog->parentWidget())
        return;
    auto *host = dialog->parentWidget();
    dialog->setGeometry(host->rect());
    const auto bounds = host->window()->property("reader_keyboard_bounds").toRect();
    const int bottom = bounds.isEmpty() ? dialog->height()
                                        : dialog->mapFrom(host->window(), bounds.topLeft()).y();
    dialog->setCardArea(QRect(0, 0, dialog->width(), qMax(1, bottom)));
    const int cardWidth =
            qMin<int>(notice::cardWidth, host->width() - 2 * int(notice::sideClearance));
    const int room = bottom - 2 * int(notice::sideClearance);
    auto *card = dialog->card();
    auto *passage = card->findChild<QWidget *>("rrNotePassage");
    // A short screen with the keyboard open drops the title, then the
    // passage, then text box lines. They return when the keyboard closes.
    if (heading)
        heading->show();
    if (passage)
        passage->show();
    editor->setFixedHeight(boxHeight(editor, roomyLines));
    card->fitToWidth(cardWidth);
    if (quote) {
        // The label draws at most passageLines lines but asks for the height
        // of the whole passage. Give it the height of the lines it draws.
        const int pitch = qMax(int(ui::px(44)), QFontMetrics(quote->font()).lineSpacing());
        const int whole = ui::wrappedInterfaceTextHeight(quote->text(), quote->font(),
                                                         qMax(1, quote->width()));
        const int lines = qBound(1, (whole + pitch - 1) / pitch, passageLines);
        quote->setFixedHeight(lines * pitch);
        card->rows()->activate();
        card->fitToWidth(cardWidth);
    }
    if (!bounds.isEmpty()) {
        for (auto *widget : { heading.data(), passage }) {
            if (card->height() <= room || !widget)
                continue;
            widget->hide();
            card->fitToWidth(cardWidth);
        }
        if (card->height() > room) {
            editor->setFixedHeight(boxHeight(editor, tightLines));
            card->fitToWidth(cardWidth);
        }
    }
    dialog->centerCard();
}

bool NoteEditor::eventFilter(QObject *object, QEvent *event)
{
    if (dialog && event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()
                == "reader_keyboard_bounds")
        place();
    if (object == dialog && event->type() == QEvent::Resize) {
        if (keyboard)
            keyboard->place();
        place();
    }
    return QObject::eventFilter(object, event);
}
} // namespace kobalt
