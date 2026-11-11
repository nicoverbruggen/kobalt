#pragma once
#include "ui/controls/buttons.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include "ui/dialogs/modals.h"
#include <functional>
#include "platform/api/keyboard_controller.h"
#include "storage/custom_pins.h"

namespace kobalt {
struct BookmarkRow
{
    QString position, chapter, excerpt;
    QDateTime created;
    int page = -1; // Zero-based, or -1 when the position no longer resolves.
    bool current = false;
    int endPage = -1;
    QString style = {};
    QString color = QStringLiteral("gray");
    int sourceOrder = -1;
    QString pinName = {}, pinIcon = {};
};

// The Bookmarks list covers the header's action row with its own bar, like
// the Chapters list. Bookmark rows open a page or an actions menu.
class BookmarkList : public ui::Panel
{
public:
    explicit BookmarkList(QWidget *parent, KeyboardFactory keyboard = {});
    ~BookmarkList() override;

    QWidget *pinDialog() const { return pinHelp ? pinHelp.data() : pinPopup.data(); }

    std::function<bool(const QString &, const QString &, const QString &)> pinRequested;
    void showBookmarks(QVector<BookmarkRow> rows);
    void showAnnotations(QVector<BookmarkRow> rows, bool loading = false);
    void setSortOrder(const QString &order);
    void setFeatures(bool bookmarks, bool annotations, bool words = false);

    void setCustomPins(const storage::CustomPins &pins) { customPins = pins; }

    void setCustomPinDirectory(const QString &directory) { customPinDirectory = directory; }

    QString sortOrder() const { return order; }

    std::function<void(const QString &)> sortRequested;
    void setExcerptFont(const QString &family);

    bool annotationsShown() const { return annotations; }

    // The Words tab shows `view` below this list's strip. The strip's action
    // cell then opens every book's words.
    void setWordsView(QWidget *view);
    void showWords();
    // A child panel that covers the whole list, such as every book's words,
    // and follows its size.
    void addCoveringPanel(QWidget *panel);

    bool wordsShown() const { return words; }

    std::function<void()> wordsRequested, allWordsRequested;

    std::function<void(bool)> tabRequested;
    std::function<void(const QString &)> selected, removeRequested;
    std::function<void()> back;
    std::function<void()> actionsRequested;
    std::function<void()> loadingCancelled;
    // Return chapter/excerpt rows in the requested ID order for one visible page.
    std::function<bool(const QStringList &, QVector<BookmarkRow> *)> annotationDetails;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void showItems(QVector<BookmarkRow> list, bool annotationTab, bool pending = false);
    void sortItems();
    void arrange();
    void redraw();
    int pageCount() const;
    QVector<BookmarkRow> items;
    QStringList visibleDetailIds;
    QVector<BookmarkRow> visibleDetails;
    bool visibleDetailsReady = false;
    QVector<QPushButton *> buttons, removes;
    KeyboardFactory makeKeyboard;
    std::unique_ptr<KeyboardController> keyboard;
    QPointer<ui::ModalOverlay> pinPopup;
    QPointer<ui::ModalOverlay> pinHelp;
    void showPinHelp();
    void showPinErrors();
    void closePinHelp();
    void openPin(const BookmarkRow &row);
    void openRowActions(const BookmarkRow &row);
    void closePin();
    void placePin();
    QPushButton *bookmarksTab, *annotationsTab, *wordsTab = nullptr;
    QWidget *wordsView = nullptr;
    QVector<QPointer<QWidget>> coveringPanels;
    bool words = false;
    ui::CaptionDropdown *sortButton;
    ui::CellButton *moreButton;
    QPointer<ui::ModalOverlay> sortPopup;
    void openSort();
    void closeSort();
    QVector<int> pageStarts, itemHeights, excerptLines;
    QHash<QString, int> excerptLineCache;
    QFont excerptCacheFont;
    int excerptCacheWidth = -1, excerptCacheLineHeight = -1;
    QString order = QStringLiteral("position");
    bool annotations = false;
    bool loading = false;
    QString excerptFamily;
    storage::CustomPins customPins;
    QString customPinDirectory = QStringLiteral("custom-pins/");
    int listTop() const;
    int rowHeight() const;
    QFont excerptFont() const;
    QLabel *empty, *emptyHint;
    ui::CellButton *close;
    ui::PagerBar *pager;
    int page = 0, rows = 1;
};
} // namespace kobalt
