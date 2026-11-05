#include "ui/controls/text.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/controls/panels.h"
#include "ui/style/theme.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "bookmark_list.h"
#include "ui/style/styles.h"
#include "ui/style/fonts.h"
#include <QLocale>
#include "render/highlight_paint.h"
#include "ui/controls/options.h"
#include <QPainter>
#include <algorithm>
#include <climits>
#include <QLineEdit>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QStyle>
#include "ui/dialogs/action_menu.h"
#include <QDynamicPropertyChangeEvent>
#include "storage/bookmark_store.h"
#include "reading/reading_density.h"

namespace kobalt {
namespace chrome = ui::metrics::chrome;
namespace strip = ui::metrics::contents;

BookmarkList::BookmarkList(QWidget *parent, KeyboardFactory factory)
    : Panel(parent), makeKeyboard(std::move(factory))
{
    window()->installEventFilter(this);
    setObjectName("rrBookmarkList");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrBookmarkListClose");
    close->setRule(Qt::RightEdge);
    close->setAccessibleName(strings::get("bookmarks.close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (back)
            back();
    });
    listHeader = new ui::ListHeader(this);
    listHeader->setTrailingInset(chrome::cellWidth);
    summary = listHeader->leftLabel();
    summary->setObjectName("rrBookmarkListSummary");
    pageHeading = listHeader->rightLabel();
    pageHeading->setObjectName("rrBookmarkListPageHeading");
    for (auto *label : { summary, pageHeading }) {
        label->setFont(ui::interfaceFont(chrome::captionText));
        ui::readerStyle(
                label,
                ui::styles::font(chrome::captionText)
                        + "background:transparent;border:none;color:palette(placeholder-text);");
    }
    empty = new ui::InterfaceLabel(strings::get("bookmarks.empty"), this);
    empty->setObjectName("rrBookmarkListEmpty");
    empty->setTextFormat(Qt::PlainText);
    empty->setAlignment(Qt::AlignCenter);
    ui::readerStyle(empty, ui::styles::font(chrome::labelText) + "background:transparent;");
    emptyHint = new ui::InterfaceLabel(strings::get("bookmarks.empty_hint"), this);
    emptyHint->setObjectName("rrBookmarkListEmptyHint");
    emptyHint->setTextFormat(Qt::PlainText);
    emptyHint->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    emptyHint->setWordWrap(true);
    ui::readerStyle(emptyHint,
                    ui::styles::font(chrome::descriptionText)
                            + "color:palette(placeholder-text);background:transparent;");
    pager = new ui::PagerBar(this, "rrBookmarkList");
    pager->previous->setAccessibleName(strings::get("bookmarks.previous"));
    pager->next->setAccessibleName(strings::get("bookmarks.next"));
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        if (pageCount() > 1) {
            page = pager->turnTarget(-1);
            redraw();
        }
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        if (pageCount() > 1) {
            page = pager->turnTarget(1);
            redraw();
        }
    });
    bookmarksTab = new ui::Button(strings::get("bookmarks.title"), this);
    annotationsTab = new ui::Button(strings::get("annotations.tab"), this);
    bookmarksTab->setObjectName("rrBookmarksTab");
    annotationsTab->setObjectName("rrAnnotationsTab");
    for (auto *tab : { bookmarksTab, annotationsTab }) {
        tab->setCheckable(true);
        static_cast<ui::Button *>(tab)->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
        tab->setFont(ui::brandFont(chrome::titleText));
        ui::readerStyle(tab,
                        "QPushButton{border:none;padding:0;min-height:0;"
                                + ui::styles::brand(chrome::titleText) + "}");
    }
    connect(bookmarksTab, &QPushButton::clicked, this, [this] {
        if (tabRequested)
            tabRequested(false);
    });
    connect(annotationsTab, &QPushButton::clicked, this, [this] {
        if (tabRequested)
            tabRequested(true);
    });
    sortButton = new ui::CellButton(ui::ControlIcon::Sort, this);
    moreButton = new ui::CellButton(ui::ControlIcon::More, this);
    moreButton->setObjectName("rrAnnotationMore");
    moreButton->setAccessibleName(strings::get("annotations.more"));
    moreButton->setRule(Qt::LeftEdge);
    connect(moreButton, &QPushButton::clicked, this, [this] {
        closeSort();
        if (actionsRequested)
            actionsRequested();
    });
    sortButton->setObjectName("rrMarksSort");
    sortButton->setAccessibleName(strings::get("marks.sort_title"));
    sortButton->setRule(Qt::LeftEdge);
    connect(sortButton, &QPushButton::clicked, this, [this] { openSort(); });
    hide();
}

void BookmarkList::setSortOrder(const QString &value)
{
    const auto valid = value == "newest" || value == "oldest" ? value : QStringLiteral("position");
    if (order == valid)
        return;
    order = valid;
    page = 0;
    sortItems();
    arrange();
    redraw();
}

BookmarkList::~BookmarkList()
{
    closePin();
}

void BookmarkList::closePin()
{
    if (keyboard) {
        keyboard->hide();
        keyboard->submitted = {};
        keyboard.reset();
    }
    if (pinPopup) {
        pinPopup->hide();
        pinPopup->deleteLater();
        pinPopup.clear();
    }
}

void BookmarkList::placePin()
{
    if (!pinPopup)
        return;
    pinPopup->setGeometry(parentWidget()->rect());
    const auto bounds = window()->property("reader_keyboard_bounds").toRect();
    const int bottom = bounds.isEmpty() ? pinPopup->height()
                                        : pinPopup->mapFrom(window(), bounds.topLeft()).y();
    if (auto *icons = pinPopup->findChild<QWidget *>("rrPinIconSection"))
        icons->setVisible(bounds.isEmpty());
    pinPopup->setCardArea(QRect(0, 0, pinPopup->width(), qMax(1, bottom)));
    const int cardWidth = qMin<int>(ui::metrics::notice::cardWidth,
                                    width() - 2 * ui::metrics::notice::sideClearance);
    auto *title = pinPopup->card()->findChild<QLabel *>("rrModalTitle");
    if (title)
        title->show();
    pinPopup->card()->fitToWidth(cardWidth);
    // On a short screen the keyboard leaves room for the name and the buttons
    // only. The title returns when the keyboard closes.
    if (title && !bounds.isEmpty()
        && pinPopup->card()->height() > bottom - 2 * int(ui::metrics::notice::sideClearance)) {
        title->hide();
        pinPopup->card()->fitToWidth(cardWidth);
    }
    pinPopup->centerCard();
}

bool BookmarkList::eventFilter(QObject *object, QEvent *event)
{
    if (pinPopup && event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()
                == "reader_keyboard_bounds")
        placePin();
    if (object == pinPopup && event->type() == QEvent::Resize) {
        if (keyboard)
            keyboard->place();
        placePin();
    }
    return Panel::eventFilter(object, event);
}

void BookmarkList::openRowActions(const BookmarkRow &row)
{
    closeSort();
    closePin();
    QVector<ui::ActionMenuItem> actions{ ui::actionItem(
            strings::get(row.pinIcon.isEmpty() ? "bookmarks.pin" : "bookmarks.modify"),
            "rrBookmarkPin", ui::ControlIcon::Pin, [this, row] { openPin(row); }) };
    if (!row.pinIcon.isEmpty())
        actions.append(ui::actionItem(strings::get("bookmarks.unpin"), "rrBookmarkUnpin",
                                      ui::ControlIcon::PinOff, [this, row] {
                                          if (pinRequested)
                                              pinRequested(row.position, {}, {});
                                      }));
    actions.append(ui::actionItem(strings::get("bookmarks.remove_this"), "rrBookmarkDelete",
                                  ui::ControlIcon::Trash, [this, row] {
                                      if (removeRequested)
                                          removeRequested(row.position);
                                  }));
    auto *menu = new ui::ActionMenu(parentWidget(), {}, actions);
    pinPopup = menu;
    menu->setObjectName("rrBookmarkRowActions");
    menu->dismissed = [this] { closePin(); };
    menu->setDimmed(true);
    menu->setProperty("reader_standard_modal", true);
    menu->setGeometry(parentWidget()->rect());
    menu->fitCard();
    menu->show();
    menu->raise();
    menu->setFocus();
}

void BookmarkList::openPin(const BookmarkRow &row)
{
    closeSort();
    closePin();
    int count = 0;
    for (const auto &item : items)
        count += !item.pinIcon.isEmpty();
    if (row.pinIcon.isEmpty() && count >= BookmarkStore::maxPins) {
        auto *notice = new ui::NoticeModal(
                parentWidget(),
                { strings::get("bookmarks.pin_limit"), strings::get("bookmarks.pin"), {} },
                { strings::get("common.ok"), "rrPinLimitClose" });
        pinPopup = notice;
        connect(notice->primaryButton(), &QPushButton::clicked, this, [this] { closePin(); });
        notice->dismissed = [this] { closePin(); };
        notice->setObjectName("rrPinLimit");
        notice->setDimmed(true);
        placePin();
        notice->show();
        notice->raise();
        return;
    }
    auto *dialog = new ui::ChoiceModal(
            parentWidget(),
            { {}, strings::get(row.pinIcon.isEmpty() ? "bookmarks.pin" : "bookmarks.modify"), {} },
            { strings::get("common.cancel"), "rrPinCancel" },
            { strings::get("common.save"), "rrPinSave" });
    pinPopup = dialog;
    dialog->setObjectName("rrPinEditor");
    dialog->setDimmed(true);
    dialog->installEventFilter(this);
    auto *card = dialog->card();
    auto *title = card->findChild<QLabel *>("rrModalTitle");
    title->setProperty("reader_modal_heading", true);
    title->style()->unpolish(title);
    title->style()->polish(title);
    auto *input = new QLineEdit(card);
    kobalt::ui::prepareNativeEditor(input);
    input->setObjectName("rrPinName");
    input->setMaxLength(128);
    input->setText(row.pinName.isEmpty() ? row.chapter : row.pinName);
    input->setAccessibleName(strings::get("bookmarks.pin_name"));
    input->setPlaceholderText(strings::get("bookmarks.pin_name"));
    input->setMinimumHeight(chrome::actionHeight);
    input->setTextMargins(ui::metrics::notice::cardMargins.left(), 0,
                          ui::metrics::notice::cardMargins.right(), 0);
    ui::readerStyle(
            input,
            QString("QLineEdit{border:%1px solid palette(mid);border-radius:0;padding:0;background:palette(window);color:palette(window-text);")
                            .arg(int(ui::metrics::control::border))
                    + ui::styles::font(chrome::labelText) + "}");
    auto *inputRow = new QWidget(card);
    auto *inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(
            ui::metrics::notice::cardMargins.left(), ui::metrics::notice::cardMargins.top(),
            ui::metrics::notice::cardMargins.right(), ui::metrics::notice::cardGap);
    inputLayout->addWidget(input);
    card->rows()->insertWidget(1, inputRow);
    auto selected = std::make_shared<QString>(
            row.pinIcon.isEmpty() ? ui::bookmarkIconNames().first() : row.pinIcon);
    auto *iconSection = new QWidget(card);
    iconSection->setObjectName("rrPinIconSection");
    auto *sectionLayout = new QVBoxLayout(iconSection);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(0);
    auto *gridWidget = new QWidget(iconSection);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(ui::metrics::notice::cardMargins.left(), 0,
                             ui::metrics::notice::cardMargins.right(),
                             ui::metrics::notice::cardMargins.bottom());
    grid->setSpacing(ui::px(12));
    // A short screen, such as a phone in landscape, has room for one row of
    // icons between the name and the buttons. It shows six per page.
    const bool shortScreen = isShortHeight(parentWidget()->height(), ui::density());
    const int columns = shortScreen ? 6 : 4, iconRows = shortScreen ? 1 : 3;
    for (int row = 0; row < iconRows; ++row)
        grid->setRowMinimumHeight(row, chrome::actionHeight);
    auto *group = new QButtonGroup(gridWidget);
    group->setExclusive(true);
    const auto icons = ui::bookmarkIconNames();
    const int pageSize = columns * iconRows;
    const int pageCount = (icons.size() + pageSize - 1) / pageSize;
    auto page = std::make_shared<int>(qMax(0, int(icons.indexOf(*selected))) / pageSize);
    QVector<ui::CellButton *> iconButtons;
    for (int i = 0; i < pageSize; ++i) {
        auto *button = new ui::CellButton(ui::ControlIcon::Star, gridWidget,
                                          qRound(1.2 * int(chrome::cellIcon)));
        button->setRule(ui::Edges(Qt::LeftEdge) | Qt::TopEdge | Qt::RightEdge | Qt::BottomEdge);
        button->setCheckable(true);
        button->setFixedHeight(chrome::actionHeight);
        group->addButton(button);
        grid->addWidget(button, i / columns, i % columns);
        iconButtons.append(button);
        connect(button, &QPushButton::clicked, this, [selected, page, icons, i, pageSize] {
            const int index = *page * pageSize + i;
            if (index < icons.size())
                *selected = icons[index];
        });
    }
    sectionLayout->addWidget(gridWidget);
    auto *pager = new ui::PagerBar(iconSection, "rrPinIcons");
    sectionLayout->addWidget(pager);
    pager->setVisible(pageCount > 1);
    const auto showPage = [this, page, icons, selected, iconButtons, group, pager, pageCount,
                           pageSize] {
        group->setExclusive(false);
        for (int i = 0; i < iconButtons.size(); ++i) {
            auto *button = iconButtons[i];
            const int index = *page * pageSize + i;
            button->setVisible(index < icons.size());
            if (index >= icons.size())
                continue;
            const auto token = icons[index];
            button->setObjectName("rrPinIcon_" + token);
            button->setAccessibleName(
                    strings::get(("bookmarks.icon_" + token).toLatin1().constData()));
            button->setGlyph(ui::bookmarkIcon(token));
            button->setChecked(*selected == token);
        }
        group->setExclusive(true);
        pager->setPage(*page, pageCount);
        placePin();
    };
    connect(pager->previous, &QPushButton::clicked, this, [page, showPage] {
        --*page;
        showPage();
    });
    connect(pager->next, &QPushButton::clicked, this, [page, showPage] {
        ++*page;
        showPage();
    });
    card->rows()->insertWidget(2, iconSection);
    showPage();
    const auto finish = [this, dialog, row, input, selected] {
        if (!pinPopup || pinPopup != dialog || !dialog->isVisible())
            return;
        if (!pinRequested || !pinRequested(row.position, input->text(), *selected))
            return;
        dialog->hide();
        // Submission may be inside the keyboard controller's own callback.
        QTimer::singleShot(0, this, [this, guard = QPointer<ui::ModalOverlay>(dialog)] {
            if (pinPopup == guard)
                closePin();
        });
    };
    connect(dialog->primaryButton(), &QPushButton::clicked, this, finish);
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, [this] { closePin(); });
    const auto finishNaming = [this, dialog] {
        if (pinPopup != dialog)
            return;
        if (keyboard)
            keyboard->hide();
        dialog->setFocus();
        placePin();
    };
    connect(input, &QLineEdit::returnPressed, this, finishNaming);
    dialog->dismissed = [this] { closePin(); };
    dialog->setFocusPolicy(Qt::StrongFocus);
    placePin();
    dialog->show();
    dialog->raise();
    dialog->setFocus();
    if (makeKeyboard) {
        keyboard = makeKeyboard(dialog, input);
        keyboard->setSubmitText(strings::get("keyboard.done"));
        keyboard->submitted = finishNaming;
        placePin();
    }
}

void BookmarkList::closeSort()
{
    if (!sortPopup)
        return;
    sortPopup->hide();
    sortPopup->deleteLater();
    sortPopup.clear();
}

void BookmarkList::openSort()
{
    if (sortPopup)
        return;
    sortPopup = new ui::ModalOverlay(this);
    sortPopup->setObjectName("rrMarksSortPopup");
    sortPopup->setGeometry(rect());
    sortPopup->setDimmed(true);
    sortPopup->dismissed = [this] { closeSort(); };
    auto *card = sortPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(ui::metrics::notice::cardMargins.top());
    auto *title = card->title(strings::get("marks.sort_title"));
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(ui::metrics::notice::cardMargins.bottom());
    auto *options = new ui::RadioList(card);
    options->setMinimumRowHeight(chrome::listActionHeight);
    options->setTopDivider(true);
    const QStringList values{ "position", "newest", "oldest" };
    const QStringList keys{ "marks.sort_position", "marks.sort_newest", "marks.sort_oldest" };
    for (int i = 0; i < values.size(); ++i) {
        options->add("rrMarksSort_" + values[i], strings::get(keys[i].toLatin1().constData()),
                     strings::get((keys[i] + "_description").toLatin1().constData()));
        if (order == values[i])
            options->setCurrent(i);
    }
    options->chosen = [this, values](int index) {
        setSortOrder(values[index]);
        closeSort();
        if (sortRequested)
            sortRequested(order);
    };
    card->rows()->addWidget(options);
    const int cardWidth = qMin<int>(ui::metrics::notice::cardWidth,
                                    width() - 2 * ui::metrics::notice::sideClearance);
    options->setFixedHeight(options->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    sortPopup->centerCard();
    sortPopup->show();
    sortPopup->raise();
}

void BookmarkList::sortItems()
{
    std::stable_sort(
            items.begin(), items.end(), [this](const BookmarkRow &a, const BookmarkRow &b) {
                if (order != "position" && a.created != b.created) {
                    if (!a.created.isValid() || !b.created.isValid())
                        return a.created.isValid();
                    return order == "newest" ? a.created > b.created : a.created < b.created;
                }
                if (a.page != b.page)
                    return (a.page < 0 ? INT_MAX : a.page) < (b.page < 0 ? INT_MAX : b.page);
                return (a.sourceOrder < 0 ? INT_MAX : a.sourceOrder)
                        < (b.sourceOrder < 0 ? INT_MAX : b.sourceOrder);
            });
}

void BookmarkList::setExcerptFont(const QString &family)
{
    if (excerptFamily == family) {
        return;
    }
    excerptFamily = family;
    arrange();
    redraw();
}

QFont BookmarkList::excerptFont() const
{
    QFont font = excerptFamily.isEmpty() ? ui::interfaceFont() : QFont(excerptFamily);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setPixelSize(ui::metrics::search::excerptText);
    return font;
}

int BookmarkList::rowHeight() const
{
    const auto font = excerptFont();
    const auto sample = QStringLiteral("Mg");
    const bool privateText = ui::needsPrivateText(sample, font);
    const QFontMetrics metrics(font);
    const int excerptHeight = privateText
            ? qMax(ui::measurePrivateTextSize(sample, font).height(),
                   ui::renderPrivateText(sample, font).image.height())
            : qMax(metrics.height(), metrics.tightBoundingRect(sample).height());
    const int lineHeight =
            qMax(excerptHeight, ui::interfaceLineHeight(ui::interfaceFont(chrome::labelText)));
    return qMax(int(ui::metrics::list::threeLines),
                2 * int(ui::metrics::list::lineInset) + 4 * lineHeight
                        + int(ui::metrics::control::border));
}

int BookmarkList::pageCount() const
{
    return qMax(1, int(pageStarts.size()));
}

int BookmarkList::listTop() const
{
    return chrome::actionHeight + strip::stripHeight;
}

void BookmarkList::showAnnotations(QVector<BookmarkRow> list, bool pending)
{
    showItems(std::move(list), true, pending);
}

void BookmarkList::showBookmarks(QVector<BookmarkRow> list)
{
    showItems(std::move(list), false);
}

void BookmarkList::showItems(QVector<BookmarkRow> list, bool annotationTab, bool pending)
{
    if (loading && annotations && !annotationTab && loadingCancelled)
        loadingCancelled();
    const bool reopening = !isVisible() || annotations != annotationTab;
    annotations = annotationTab;
    loading = pending;
    visibleDetailIds.clear();
    visibleDetails.clear();
    visibleDetailsReady = false;
    items = loading ? QVector<BookmarkRow>() : std::move(list);
    if (annotations && annotationDetails) {
        for (auto &item : items) {
            item.chapter.clear();
            item.excerpt.clear();
        }
    }
    sortItems();
    if (reopening)
        page = 0;
    arrange();
    redraw();
    show();
    raise();
}

void BookmarkList::resizeEvent(QResizeEvent *)
{
    arrange();
    redraw();
}

void BookmarkList::hideEvent(QHideEvent *event)
{
    closePin();
    closeSort();
    visibleDetailIds.clear();
    visibleDetails.clear();
    visibleDetailsReady = false;
    if (loading && annotations && loadingCancelled)
        loadingCancelled();
    if (annotations && annotationDetails) {
        for (int i = 0; i < buttons.size(); ++i) {
            auto *row = static_cast<ui::ListRow *>(buttons[i]);
            row->setTitle({});
            row->setDetail({});
            row->setAccessibleName({});
            row->setEnabled(false);
            removes[i]->setEnabled(false);
        }
    }
    ui::Panel::hideEvent(event);
}

void BookmarkList::showEvent(QShowEvent *event)
{
    ui::Panel::showEvent(event);
    if (annotations && annotationDetails && !loading)
        redraw();
}

void BookmarkList::paintEvent(QPaintEvent *event)
{
    ui::Panel::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(0, chrome::actionHeight - chrome::heavyRule, width(), chrome::heavyRule,
                     palette().color(QPalette::WindowText));
    painter.fillRect(0, listTop() - ui::metrics::control::border, width(),
                     ui::metrics::control::border, ui::dividerColor(this));
}

void BookmarkList::arrange()
{
    const int first = pageStarts.value(page, 0), top = listTop(),
              available = qMax(1, height() - top);
    const auto font = excerptFont();
    const int textLineHeight = ui::needsPrivateText(QStringLiteral("Mg"), font)
            ? ui::measurePrivateTextLayout(QStringLiteral("Mg"), font).lineHeight
            : QFontMetrics(font).lineSpacing();
    const int lineHeight = qMax(
            ui::px(44),
            qMax(textLineHeight, ui::interfaceLineHeight(ui::interfaceFont(chrome::captionText))));
    const int textWidth = qMax(1, width() - chrome::cellWidth - 2 * ui::metrics::list::inset);
    if (excerptCacheFont != font || excerptCacheWidth != textWidth
        || excerptCacheLineHeight != textLineHeight) {
        excerptLineCache.clear();
        excerptCacheFont = font;
        excerptCacheWidth = textWidth;
        excerptCacheLineHeight = textLineHeight;
    }
    // Keep only this list's excerpts. Sorting and reopening reuse wrapping,
    // while removed or replaced text leaves the cache with the next layout.
    QHash<QString, int> nextLineCache;
    nextLineCache.reserve(items.size());
    itemHeights.clear();
    excerptLines.clear();
    int total = 0;
    for (const auto &item : items) {
        const bool lazy = annotations && bool(annotationDetails);
        int count =
                lazy ? 2 : nextLineCache.value(item.excerpt, excerptLineCache.value(item.excerpt));
        if (count == 0)
            count = qBound(1,
                           (ui::wrappedInterfaceTextHeight(item.excerpt, font, textWidth)
                            + textLineHeight - 1)
                                   / qMax(1, textLineHeight),
                           2);
        if (!lazy)
            nextLineCache.insert(item.excerpt, count);
        const int itemHeight = qMax(int(ui::metrics::list::threeLines),
                                    2 * int(ui::metrics::list::lineInset) + (count + 2) * lineHeight
                                            + int(ui::px(annotations ? 12 : 4))
                                            + int(ui::metrics::control::border));
        excerptLines.append(count);
        itemHeights.append(itemHeight);
        total += itemHeight;
    }
    excerptLineCache.swap(nextLineCache);
    const int capacity = total > available ? qMax(1, available - chrome::actionHeight) : available;
    pageStarts = { 0 };
    int used = 0;
    for (int i = 0; i < items.size(); ++i) {
        if (used > 0 && used + itemHeights[i] > capacity) {
            pageStarts.append(i);
            used = 0;
        }
        used += itemHeights[i];
    }
    page = 0;
    while (page + 1 < pageStarts.size() && pageStarts[page + 1] <= first)
        ++page;
    rows = (page + 1 < pageStarts.size() ? pageStarts[page + 1] : items.size()) - pageStarts[page];
    const int barHeight = chrome::actionHeight - chrome::heavyRule;
    moreButton->setEnabled(bool(actionsRequested) && !loading);
    const int reserved = 3 * chrome::cellWidth;
    const int tabWidth = qMax(1, (width() - reserved) / 2);
    close->setGeometry(0, 0, chrome::cellWidth, barHeight);
    annotationsTab->setGeometry(chrome::cellWidth, 0, tabWidth, barHeight);
    bookmarksTab->setGeometry(chrome::cellWidth + tabWidth, 0,
                              qMax(1, width() - reserved - tabWidth), barHeight);
    moreButton->setGeometry(width() - chrome::cellWidth, 0, chrome::cellWidth, barHeight);
    listHeader->setGeometry(0, chrome::actionHeight, width(), ui::metrics::list::headerHeight);
    sortButton->setGeometry(width() - 2 * chrome::cellWidth, 0, chrome::cellWidth, barHeight);
    if (sortPopup) {
        sortPopup->setGeometry(rect());
        sortPopup->centerCard();
    }
    pager->setGeometry(0, height() - chrome::actionHeight, width(), chrome::actionHeight);
    const int itemHeight = rowHeight();
    empty->setGeometry(chrome::inset, top + itemHeight / 2, qMax(1, width() - 2 * chrome::inset),
                       itemHeight / 2);
    emptyHint->setGeometry(2 * chrome::inset, top + itemHeight,
                           qMax(1, width() - 4 * chrome::inset), itemHeight);
}

void BookmarkList::redraw()
{
    const int first = pageStarts.value(page, 0);
    rows = (page + 1 < pageStarts.size() ? pageStarts[page + 1] : items.size()) - first;
    const bool lazy = annotations && bool(annotationDetails);
    QVector<BookmarkRow> details;
    bool detailsReady = !lazy;
    if (lazy) {
        // Widgets only show this page. The bounded detail cache can survive a
        // layout change when the ordered visible IDs remain the same.
        for (auto *button : buttons) {
            auto *row = static_cast<ui::ListRow *>(button);
            row->setTitle({});
            row->setDetail({});
            row->setAccessibleName({});
        }
        if (isVisible() && !loading && rows > 0) {
            QStringList ids;
            for (int i = first; i < first + rows; ++i)
                ids.append(items[i].position);
            if (visibleDetailIds != ids) {
                visibleDetailIds = ids;
                visibleDetails.clear();
                visibleDetailsReady = annotationDetails(ids, &visibleDetails)
                        && visibleDetails.size() == ids.size();
                if (visibleDetailsReady) {
                    for (int i = 0; i < ids.size(); ++i)
                        if (visibleDetails[i].position != ids[i]) {
                            visibleDetailsReady = false;
                            break;
                        }
                }
                if (!visibleDetailsReady)
                    visibleDetails.clear();
            }
            detailsReady = visibleDetailsReady;
            details = visibleDetails;
        }
    }
    bookmarksTab->setChecked(!annotations);
    annotationsTab->setChecked(annotations);
    const bool currentBookmarked = std::any_of(items.cbegin(), items.cend(),
                                               [](const BookmarkRow &row) { return row.current; });
    moreButton->setGlyph(annotations                 ? ui::ControlIcon::ExportAction
                                 : currentBookmarked ? ui::ControlIcon::BookmarkRemove
                                                     : ui::ControlIcon::BookmarkAdd);
    moreButton->setAccessibleName(
            strings::get(annotations ? "annotations.export" : "bookmarks.actions"));
    empty->setText(strings::get(loading               ? "annotations.loading"
                                        : annotations ? "annotations.empty"
                                                      : "bookmarks.empty"));
    emptyHint->setText(
            strings::get(annotations ? "annotations.empty_hint" : "bookmarks.empty_hint"));
    page = qBound(0, page, pageCount() - 1);
    empty->setVisible(items.isEmpty());
    emptyHint->setVisible(items.isEmpty() && !loading);
    sortButton->setEnabled(!loading);
    pageHeading->setVisible(!items.isEmpty());
    const auto caption = annotations && items.size() == 1
            ? strings::get("annotations.summary_one")
            : strings::get(annotations ? "annotations.summary" : "bookmarks.summary")
                      .arg(items.size());
    listHeader->setCaptions(loading ? QString() : caption,
                            items.isEmpty() ? QString() : strings::get("chapter_contents.page"));
    pager->setVisible(pageCount() > 1);
    pager->setPage(page, pageCount(), {}, true);
    int used = listTop();
    for (int i = first; i < first + rows; ++i)
        used += itemHeights.value(i);
    pager->setBlankArea({ chrome::inset, used, width() - 2 * chrome::inset, pager->y() - used });
    while (buttons.size() < qMin(rows, int(items.size()))) {
        const int row = buttons.size();
        auto *button = new ui::ListRow(this);
        auto *remove = button->addAction({}, strings::get("bookmarks.remove"));
        // An unresolved bookmark still needs its independent removal action.
        remove->setParent(this);
        buttons.append(button);
        removes.append(remove);
        connect(button, &QPushButton::clicked, this, [this, row] {
            const int index = pageStarts.value(page, 0) + row;
            if (buttons[row]->isEnabled() && index < items.size() && items[index].page >= 0
                && selected)
                selected(items[index].position);
        });
        connect(remove, &QPushButton::clicked, this, [this, row] {
            const int index = pageStarts.value(page, 0) + row;
            if (removes[row]->isEnabled() && index < items.size()) {
                if (!annotations) {
                    openRowActions(items[index]);
                } else if (removeRequested) {
                    removeRequested(items[index].position);
                }
            }
        });
    }
    int lastPage = 0;
    for (const auto &item : items)
        lastPage = qMax(lastPage, item.page);
    const int numberWidth = ui::interfaceTextWidth(QString::number(lastPage + 1),
                                                   ui::interfaceFont(chrome::captionText));
    int y = listTop();
    for (int row = 0; row < buttons.size(); ++row) {
        const int index = pageStarts.value(page, 0) + row;
        const bool shown = row < rows && index < items.size();
        buttons[row]->setVisible(shown);
        removes[row]->setVisible(shown);

        if (!shown) {
            if (lazy) {
                buttons[row]->setEnabled(false);
                removes[row]->setEnabled(false);
            }
            continue;
        }
        const auto &item = items[index];
        const QString chapter =
                lazy ? (detailsReady ? details[row].chapter : QString()) : item.chapter;
        const QString excerpt = lazy
                ? (detailsReady ? details[row].excerpt : strings::get("annotations.load_failed"))
                : item.excerpt;
        const int itemHeight = itemHeights[index];
        auto *button = buttons[row];
        button->setObjectName(QString(annotations ? "rrAnnotation%1" : "rrBookmark%1").arg(index));
        removes[row]->setObjectName(
                QString(annotations ? "rrAnnotationRemove%1" : "rrBookmarkMore%1").arg(index));
        removes[row]->setAccessibleName(
                strings::get(annotations ? "annotations.remove" : "bookmarks.actions"));

        button->setAccessibleName(chapter);
        button->setEnabled(item.page >= 0 && detailsReady);
        removes[row]->setEnabled(detailsReady);
        button->setGeometry(0, y, width(), itemHeight);
        auto *itemRow = static_cast<ui::ListRow *>(button);
        itemRow->setActionIcon(annotations ? ui::ControlIcon::Close : ui::ControlIcon::More);
        itemRow->setDetailFont(excerptFont());
        itemRow->setMetadataFont(ui::interfaceFont(chrome::captionText));
        itemRow->setTitle(item.pinName.isEmpty() ? chapter : item.pinName);
        itemRow->setTitlePixels(ui::px(24));
        itemRow->setTitleIcon(ui::ControlIcon::Pin, !annotations && !item.pinIcon.isEmpty(),
                              !annotations);
        itemRow->setTitleGap(ui::px(annotations ? 12 : 4));
        itemRow->titleLabel()->setAlignment(Qt::AlignLeft
                                            | (annotations ? Qt::AlignVCenter : Qt::AlignBottom));
        itemRow->setDetailLines(excerptLines[index]);
        itemRow->setDetail(excerpt);
        itemRow->setDetailHighlight(
                annotations && detailsReady
                        ? (item.style.isEmpty() ? QStringLiteral("shaded") : item.style)
                        : QString(),
                render::highlightTint(item.color), ui::px(44));
        QStringList metadata;
        if (item.created.isValid())
            metadata << QLocale().toString(item.created.toLocalTime(), "d MMM yyyy, HH:mm");
        itemRow->setCaption(metadata.join(QStringLiteral(" · ")));
        itemRow->setAccessory(ui::ListRow::Caption{
                item.page >= 0
                        ? (annotations && item.endPage > item.page
                                   ? QString("%1–%2").arg(item.page + 1).arg(item.endPage + 1)
                                   : QString::number(item.page + 1))
                        : QString(),
                numberWidth });
        y += itemHeight;
    }
    update();
}
} // namespace kobalt
