#pragma once
#include "platform/api/keyboard_controller.h"
#include <QFont>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QLabel;
class QTextEdit;
class QWidget;

namespace kobalt::ui {
class ChoiceModal;
}

namespace kobalt {
// The text that the note editor shows above its text box.
struct NotePassage
{
    QString text, chapter, style, color;
    QFont font;
};

// A modal card that writes the note of one saved highlight. It quotes the
// passage, keeps its text box above the keyboard and leaves saving to the
// owner. Destroying the editor closes its card and keyboard.
class NoteEditor final : public QObject
{
public:
    NoteEditor(QWidget *parent, const KeyboardFactory &keyboard, const QString &title,
               const NotePassage &passage, const QString &note);
    ~NoteEditor() override;

    QString text() const;
    // Shows a save error below the text box and keeps the text.
    void setError(const QString &message);

    // Returns false to keep the editor open, for example after an error.
    std::function<bool(const QString &)> saveRequested;
    // The owner destroys the editor, which may not happen inside this call.
    std::function<void()> closed;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void place();
    void close();
    QPointer<ui::ChoiceModal> dialog;
    QPointer<QTextEdit> editor;
    QPointer<QLabel> quote, error;
    QPointer<QWidget> heading;
    std::unique_ptr<KeyboardController> keyboard;
};
} // namespace kobalt
