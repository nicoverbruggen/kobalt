#pragma once
#include "ui/controls/panels.h"
#include "library/library_index.h"
#include "platform/api/keyboard_controller.h"
#include <functional>

class QLineEdit;
class QPushButton;

namespace kobalt::ui {
class CellButton;
class PagerBar;
class ModalOverlay;
class ActionMenu;
class LoadingIndicator;
} // namespace kobalt::ui

namespace kobalt::library {
class AboutScreen;
class BookPages;
class FolderPage;
class SortSheet;

// The Android and desktop library. It draws in device pixels on the reader's
// canvas, with the reader's header, rows, pager and dialogs. The host owns the
// index and every file operation; the view only reports what was chosen.
class LibraryView final : public ui::Panel
{
public:
    LibraryView(const Index &index, std::function<QString(const QString &)> coverPath,
                QWidget *parent = nullptr);
    ~LibraryView() override;
    // Call after the index changes. While scanning, the view redraws at most
    // once a second, which keeps e-paper refreshes down.
    void refresh();
    void setScanning(bool scanning, int found);
    void setDark(bool dark);
    void setKeyboardFactory(KeyboardFactory factory);
    void showNotice(const QString &title, const QString &message);
    // The system back action: closes the topmost layer. False when only the
    // library itself is left.
    bool dismiss();
    void pageButton(int direction);

    std::function<void(const QString &key)> openBook, removeFolder, removeBook;
    std::function<void(const QString &key, Book::Mark mark)> markBook;
    std::function<void()> addFolder, openFile, rescan;
    std::function<void(const Settings &settings)> settingsChanged;
    // On Android a book opened on its own is Kobalt's copy, which removal
    // deletes. The confirmation says so.
    bool singleBooksAreCopies = false;

protected:
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *) override;
    void paintEvent(QPaintEvent *) override;

private:
    const Index &index;
    std::function<QString(const QString &)> coverPath;
    ui::CellButton *search, *more, *closeSearch;
    QVector<ui::CellButton *> modes;
    QLineEdit *query;
    std::unique_ptr<KeyboardController> keyboard;
    QPushButton *sort;
    ui::LoadingIndicator *progress;
    BookPages *pages;
    ui::PagerBar *pager;
    QWidget *empty;
    SortSheet *sortSheet = nullptr;
    ui::ActionMenu *moreSheet = nullptr;
    FolderPage *folderPage = nullptr;
    AboutScreen *about = nullptr;
    ui::ModalOverlay *overlay = nullptr;
    QTimer *throttle;
    bool scanning = false, searching = false, dark = false;
    int found = 0;
    void arrange();
    void rebuild(bool keepPage = true);
    void applySettings(const Settings &settings);
    void showBookMenu(const QString &key);
    void showMoreSheet();
    void showMenu(ui::ActionMenu *menu, const char *name);
    void confirmRemove(const QString &key);
    void showFolders();
    void showAbout();
    void closeOverlay();
    QWidget *buildEmpty();
    void setSearching(bool value);
};
} // namespace kobalt::library
