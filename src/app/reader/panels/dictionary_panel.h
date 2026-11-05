#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/controls/paged_text.h"
#include "ui/style/metrics.h"
#include "platform/api/dictionary_service.h"
#include "platform/api/keyboard_controller.h"
#include <QFont>
#include <QLabel>

namespace kobalt {
// A sheet across the full width, fixed to the screen edge away from the word.
class DictionaryPanel : public ui::Panel
{
public:
    explicit DictionaryPanel(QWidget *parent, KeyboardFactory makeKeyboard = {});
    std::function<void()> dismissed;
    std::function<void(QString)> chosen;
    std::function<void(QString)> requested, searchRequested;
    std::function<void(const QFont &)> prepareEditorFont;
    void setChoices(QVector<DictionaryChoice> values);
    void setLanguage(const QString &language);
    // The resolved book face supplies the header and default definition text.
    void setBookFont(const QString &family);
    static QFont definitionFont(const QString &family, int percent);
    static int definitionLineHeight(int percent, bool customFont = false);
    void setDefinitionFont(const QString &family, int percent);
    void open(const QString &word, const DictionaryResult &result, const QRect &selection);
    void place(const QRect &selection);
    static QString formatDefinition(const QString &html);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    ui::PagerBar *pages, *choicesPaging;
    std::unique_ptr<KeyboardController> keyboard;
    ui::CellButton *dictionary, *close, *edit, *submit, *search;
    QLineEdit *query;
    QRect selection;
    bool editing = false;
    QLabel *headword, *editHint;
    QString word, bookFamily, language, definitionFamily;
    QString definitionHtml, sourceText, renderedHtml;
    int definitionPercent = 100;
    QFont headwordFont;
    QFont bookFont(int pixels) const;
    ui::Panel *dropdown;
    QVector<DictionaryChoice> options;
    QVector<QPushButton *> optionButtons;
    QString currentId;
    int optionsPage = 0;
    bool lower = true;
    // Open at the page that holds the selected dictionary.
    void redrawChoices(bool atCurrent = false);
    ui::PagedText *text;
    int rowTop() const;
    int contentBottom() const;
    void arrange();
    void updateDefinition();
    void refresh();
    void editQuery();
    void finishEditing(bool restore = true);
    void submitQuery();
};
} // namespace kobalt
