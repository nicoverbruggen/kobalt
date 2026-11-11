#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/controls/rows.h"
#include "storage/saved_words.h"
#include <QHash>
#include <QPointer>
#include <functional>

namespace kobalt {
namespace ui {
class ModalOverlay;
}

// Asks before removing a saved word, over `host`. `confirmed` runs on Remove.
void confirmWordRemoval(QWidget *host, const SavedWord &word, std::function<void()> confirmed);

// Saved words, in the rows of the Bookmarks list. The Book scope fills the
// Words tab under the Bookmarks list's own bar. The Library scope is a full
// screen with its own bar, a book filter and every book's words.
class WordList : public ui::Panel
{
public:
    enum class Scope { Book, Library };
    WordList(QWidget *parent, Scope scope);
    // `loading` shows the list's loading text until the words arrive.
    void setWords(QVector<SavedWord> words, bool loading = false);
    // Book scope: the zero-based page of each saved position, as the reader
    // resolved it. A position without a page has none in this layout.
    void setPages(QHash<QString, int> pages);

    int pageOf(const QString &position) const { return pages.value(position, -1); }

    // "position" (Book scope only), "newest", "oldest" or "alphabetical".
    void setSortOrder(const QString &order);

    QString sortOrder() const { return order; }

    void setExcerptFont(const QString &family);
    // The definition line under each word uses the dictionary font setting.
    void setGlossFont(const QString &family);

    const QVector<SavedWord> &words() const { return all; }

    std::function<void(const SavedWord &)> selected, removeRequested;
    std::function<void(const QString &)> sortRequested;
    std::function<void()> back;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *event) override;

private:
    Scope scope;
    QVector<SavedWord> all, items;
    QString order = QStringLiteral("newest"), book, excerptFamily, glossFamily;
    bool loading = false;
    int page = 0, rows = 0;
    QVector<int> pageStarts;
    QHash<QString, int> pages;
    ui::CellButton *backButton = nullptr, *sortButton = nullptr;
    QLabel *title = nullptr, *count = nullptr, *empty, *emptyHint;
    ui::CaptionDropdown *filter = nullptr;
    ui::PagerBar *pager;
    QVector<ui::ListRow *> buttons;
    QVector<QPushButton *> removes;
    QPointer<ui::ModalOverlay> popup;
    int top() const;
    int rowHeight() const;
    QFont excerptFont() const;
    void filterItems();
    void arrange();
    void redraw();
    void closePopup();
    void openSort();
    void openFilter();
};
} // namespace kobalt
