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
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QStyle>
#include "ui/dialogs/action_menu.h"
#include <QDynamicPropertyChangeEvent>
#include "storage/bookmark_store.h"
#include "reading/reading_density.h"

namespace kobalt {
namespace chrome = ui::metrics::chrome;

BookmarkList::BookmarkList(QWidget *parent, KeyboardFactory factory)
    : Panel(parent), makeKeyboard(std::move(factory))
{
    window()->installEventFilter(this);
    setObjectName("rrBookmarkList");
    close = new ui::CellButton(ui::ControlIcon::Close, this);
    close->setObjectName("rrBookmarkListClose");
    close->setRule(Qt::LeftEdge);
    close->setAccessibleName(strings::get("bookmarks.close"));
    connect(close, &QPushButton::clicked, this, [this] {
        if (back)
            back();
    });
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
    // The strip below the tabs, as in the Library: the sort on the left and
    // the tab's action on the right. paintEvent draws its divider.
    sortButton = new ui::CaptionDropdown(this);
    sortButton->label = strings::get("marks.sort_title");
    moreButton = new ui::CellButton(ui::ControlIcon::More, this);
    moreButton->setObjectName("rrAnnotationMore");
    moreButton->setAccessibleName(strings::get("annotations.more"));
    // The cell carries the strip's divider; a button's usual minimum height
    // would reach under the rows.
    moreButton->setRule(ui::Edges(Qt::LeftEdge) | Qt::BottomEdge);
    ui::readerStyle(moreButton, "QPushButton{min-height:0;}");
    moreButton->setMinimumHeight(0);
    connect(moreButton, &QPushButton::clicked, this, [this] {
        closeSort();
        if (words) {
            if (allWordsRequested)
                allWordsRequested();
        } else if (actionsRequested) {
            actionsRequested();
        }
    });
    sortButton->setObjectName("rrMarksSort");
    sortButton->setAccessibleName(strings::get("marks.sort_title"));
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

void BookmarkList::setWordsView(QWidget *view)
{
    wordsView = view;
    wordsTab = new ui::Button(strings::get("words.tab"), this);
    wordsTab->setObjectName("rrWordsTab");
    wordsTab->setCheckable(true);
    static_cast<ui::Button *>(wordsTab)->setPaintMode(ui::ButtonPaintMode::ToolbarCell);
    wordsTab->setFont(ui::brandFont(chrome::titleText));
    ui::readerStyle(wordsTab,
                    "QPushButton{border:none;padding:0;min-height:0;"
                            + ui::styles::brand(chrome::titleText) + "}");
    connect(wordsTab, &QPushButton::clicked, this, [this] {
        if (wordsRequested)
            wordsRequested();
    });
    arrange();
}

void BookmarkList::addCoveringPanel(QWidget *panel)
{
    coveringPanels.append(panel);
    panel->setGeometry(rect());
}

void BookmarkList::showWords()
{
    if (!wordsView)
        return;
    if (loading && annotations && loadingCancelled)
        loadingCancelled();
    closePin();
    closeSort();
    words = true;
    annotations = loading = false;
    arrange();
    redraw();
    show();
    raise();
    wordsView->show();
    wordsView->raise();
}

void BookmarkList::showPinHelp()
{
    if (pinHelp)
        return;
    if (keyboard)
        keyboard->hide();
    placePin();
#if defined(KOBALT_CONFIG_DIR)
    const auto location = strings::get("bookmarks.custom_help_kobo");
#elif defined(Q_OS_ANDROID)
    const auto location = strings::get("bookmarks.custom_help_android");
#else
    const auto location = strings::get("bookmarks.custom_help_desktop").arg(customPinDirectory);
#endif
    const auto description = location + "\n\n" + strings::get("bookmarks.custom_help_reload")
            + "\n\n" + strings::get("bookmarks.custom_help_limits");
    auto *notice = new ui::NoticeModal(
            parentWidget(), { description, strings::get("bookmarks.custom_help_title"), {} },
            { strings::get("common.ok"), "rrPinHelpClose" });
    pinHelp = notice;
    notice->setObjectName("rrPinHelpPopup");
    notice->setGeometry(parentWidget()->rect());
    connect(notice->primaryButton(), &QPushButton::clicked, this, [this] { closePinHelp(); });
    notice->dismissed = [this] { closePinHelp(); };
    notice->show();
    notice->raise();
    notice->fitCard();
}

void BookmarkList::showPinErrors()
{
    if (pinHelp)
        return;
    if (keyboard)
        keyboard->hide();
    placePin();
    auto *notice = new ui::ChoiceModal(
            parentWidget(), { {}, strings::get("bookmarks.custom_errors_title"), {} },
            { strings::get("bookmarks.custom_help_action"), "rrPinErrorsHelp" },
            { strings::get("common.ok"), "rrPinErrorsClose" });
    pinHelp = notice;
    notice->setObjectName("rrPinErrorsPopup");
    notice->setGeometry(parentWidget()->rect());
    auto *card = notice->card();
    const auto message = [card](const QString &text, const QString &name = QString()) {
        auto *label = card->message(text, name);
        label->setTextFormat(Qt::PlainText);
        label->setContentsMargins(ui::metrics::chrome::inset, ui::metrics::notice::cardGap,
                                  ui::metrics::chrome::inset, ui::metrics::notice::cardGap);
        ui::readerStyle(label, ui::styles::font(chrome::descriptionText));
        card->rows()->removeWidget(label);
        card->rows()->insertWidget(card->rows()->count() - 1, label);
    };
    if (customPins.skipped) {
        message(strings::get("bookmarks.custom_errors_count").arg(customPins.skipped));
        for (int i = 0; i < customPins.failedFiles.size(); ++i) {
            auto *row = new ui::ListRow(card);
            row->setTitle(customPins.failedFiles[i]);
            row->titleLabel()->setObjectName(QString("rrPinFailedFile%1").arg(i));
            row->setTitlePixels(chrome::descriptionText);
            row->setFocusPolicy(Qt::NoFocus);
            row->setAttribute(Qt::WA_TransparentForMouseEvents);
            row->setFixedHeight(row->sizeHint().height());
            card->rows()->insertWidget(card->rows()->count() - 1, row);
        }
        const int remaining = customPins.skipped - customPins.failedFiles.size();
        if (remaining > 0)
            message(strings::get("bookmarks.custom_errors_more").arg(remaining));
    }
    if (customPins.truncated)
        message(strings::get("bookmarks.custom_errors_limit"));
    connect(notice->secondaryButton(), &QPushButton::clicked, this, [this] {
        closePinHelp();
        showPinHelp();
    });
    connect(notice->primaryButton(), &QPushButton::clicked, this, [this] { closePinHelp(); });
    notice->dismissed = [this] { closePinHelp(); };
    notice->show();
    notice->raise();
    notice->fitCard();
}

void BookmarkList::closePinHelp()
{
    if (!pinHelp)
        return;
    pinHelp->hide();
    pinHelp->deleteLater();
    pinHelp.clear();
    if (pinPopup)
        pinPopup->setFocus();
}

void BookmarkList::closePin()
{
    closePinHelp();
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
    auto *heading = pinPopup->card()->findChild<QWidget *>("rrModalHeading");
    if (heading)
        heading->show();
    pinPopup->card()->fitToWidth(cardWidth);
    // On a short screen the keyboard leaves room for the name and the buttons
    // only. The title returns when the keyboard closes.
    if (heading && !bounds.isEmpty()
        && pinPopup->card()->height() > bottom - 2 * int(ui::metrics::notice::sideClearance)) {
        heading->hide();
        pinPopup->card()->fitToWidth(cardWidth);
    }
    pinPopup->centerCard();
    if (heading) {
        for (const auto *name : { "rrPinHelp", "rrPinErrors" }) {
            if (auto *button = heading->findChild<QWidget *>(name)) {
                button->setGeometry(heading->width() - chrome::cellWidth, 0, chrome::cellWidth,
                                    heading->height());
                button->raise();
            }
        }
    }
    if (pinHelp) {
        pinHelp->setGeometry(parentWidget()->rect());
        pinHelp->fitCard();
    }
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
    auto *heading = dialog->titleLabel()->parentWidget();
    dialog->titleLabel()->setContentsMargins(chrome::cellWidth, 0, chrome::cellWidth, 0);
    const bool failed = customPins.skipped || customPins.truncated;
    auto *info = new ui::CellButton(failed ? ui::ControlIcon::Warning : ui::ControlIcon::Question,
                                    heading);
    info->setObjectName(failed ? "rrPinErrors" : "rrPinHelp");
    info->setRule(ui::Edges(Qt::LeftEdge) | Qt::BottomEdge);
    info->setAccessibleName(
            strings::get(failed ? "bookmarks.custom_errors_title" : "bookmarks.custom_help_title"));
    connect(info, &QPushButton::clicked, this, [this, failed] {
        if (failed)
            showPinErrors();
        else
            showPinHelp();
    });
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
            row.pinIcon.isEmpty() ? (customPins.icons.isEmpty() ? ui::bookmarkIconNames().first()
                                                                : customPins.icons.first().id)
                                  : row.pinIcon);
    auto *iconSection = new QWidget(card);
    iconSection->setObjectName("rrPinIconSection");
    auto *sectionLayout = new QVBoxLayout(iconSection);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(0);
    auto *gridRow = new QWidget(iconSection);
    auto *padding = new QVBoxLayout(gridRow);
    padding->setContentsMargins(ui::metrics::notice::cardMargins.left(), 0,
                                ui::metrics::notice::cardMargins.right(),
                                ui::metrics::notice::cardMargins.bottom());
    auto *gridWidget = new ui::Panel(gridRow);
    gridWidget->setObjectName("rrPinIconGrid");
    gridWidget->setFill(QPalette::Mid);
    padding->addWidget(gridWidget);
    auto *grid = new QGridLayout(gridWidget);
    const int border = ui::metrics::control::border;
    grid->setContentsMargins(border, border, border, border);
    grid->setSpacing(border);
    // A short screen, such as a phone in landscape, has room for one row of
    // icons between the name and the buttons. It shows six per page.
    const bool shortScreen = isShortHeight(parentWidget()->height(), ui::density());
    const int columns = shortScreen ? 6 : 4, iconRows = shortScreen ? 1 : 3;
    // Reserve the dividers as well as the fixed-height cells.
    gridWidget->setFixedHeight(iconRows * int(chrome::actionHeight) + (iconRows + 1) * border);
    for (int row = 0; row < iconRows; ++row)
        grid->setRowMinimumHeight(row, chrome::actionHeight);
    for (int column = 0; column < columns; ++column)
        grid->setColumnStretch(column, 1);
    auto *group = new QButtonGroup(gridWidget);
    group->setExclusive(true);
    QStringList icons;
    for (const auto &icon : customPins.icons)
        icons.append(icon.id);
    // Retain a removed icon as one selected placeholder in its own pin editor.
    if (row.pinIcon.startsWith("custom:") && !customPins.find(row.pinIcon))
        icons.append(row.pinIcon);
    icons.append(ui::bookmarkIconNames());
    const int pageSize = columns * iconRows;
    const int pageCount = (icons.size() + pageSize - 1) / pageSize;
    auto page = std::make_shared<int>(qMax(0, int(icons.indexOf(*selected))) / pageSize);
    QVector<ui::CellButton *> iconButtons;
    QVector<QStackedLayout *> cells;
    for (int i = 0; i < pageSize; ++i) {
        auto *cell = new QWidget(gridWidget);
        cell->setFixedHeight(chrome::actionHeight);
        auto *stack = new QStackedLayout(cell);
        stack->setContentsMargins(0, 0, 0, 0);
        cells.append(stack);
        grid->addWidget(cell, i / columns, i % columns);
        auto *button = new ui::CellButton(ui::ControlIcon::Star, cell,
                                          qRound(1.2 * int(chrome::cellIcon)));
        button->setCheckable(true);
        button->setFixedHeight(chrome::actionHeight);
        group->addButton(button);
        stack->addWidget(button);
        iconButtons.append(button);
        auto *empty = new ui::Panel(cell);
        empty->setFixedHeight(chrome::actionHeight);
        stack->addWidget(empty);
        connect(button, &QPushButton::clicked, this, [selected, page, icons, i, pageSize] {
            const int index = *page * pageSize + i;
            if (index < icons.size())
                *selected = icons[index];
        });
    }
    sectionLayout->addWidget(gridRow);
    auto *pager = new ui::PagerBar(iconSection, "rrPinIcons");
    sectionLayout->addWidget(pager);
    pager->setVisible(pageCount > 1);
    const auto showPage = [this, page, icons, selected, iconButtons, cells, group, pager, pageCount,
                           pageSize] {
        group->setExclusive(false);
        for (int i = 0; i < iconButtons.size(); ++i) {
            auto *button = iconButtons[i];
            const int index = *page * pageSize + i;
            cells[i]->setCurrentIndex(index < icons.size() ? 0 : 1);
            if (index >= icons.size())
                continue;
            const auto token = icons[index];
            button->setObjectName("rrPinIcon_" + token);
            const auto *custom = customPins.find(token);
            const bool missing = !custom && token.startsWith("custom:");
            if (custom)
                button->setAccessibleName(custom->name);
            else if (missing)
                button->setAccessibleName(
                        strings::get("bookmarks.custom_missing").arg(token.mid(7)));
            else
                button->setAccessibleName(
                        strings::get(("bookmarks.icon_" + token).toLatin1().constData()));
            button->setToolTip("<p>" + button->accessibleName().toHtmlEscaped() + "</p>");
            button->setGlyph(missing ? ui::ControlIcon::Question : ui::bookmarkIcon(token));
            button->setGlyphMask(custom ? custom->mask : QImage());
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
    card->heading(strings::get("marks.sort_title"));
    auto *options = new ui::RadioList(card);
    options->setMinimumRowHeight(chrome::listActionHeight);
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
    return chrome::actionHeight + chrome::toolStripHeight;
}

void BookmarkList::setFeatures(bool bookmarks, bool annotations, bool words)
{
    bookmarksTab->setVisible(bookmarks);
    annotationsTab->setVisible(annotations);
    if (wordsTab)
        wordsTab->setVisible(words);
    arrange();
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
    const bool reopening = !isVisible() || annotations != annotationTab || words;
    annotations = annotationTab;
    words = false;
    if (wordsView)
        wordsView->hide();
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
    // The gaps between neighbouring tabs are their dividers.
    QVector<QPushButton *> shown;
    for (auto *tab : { bookmarksTab, annotationsTab, wordsTab })
        if (tab && !tab->isHidden())
            shown.append(tab);
    for (int i = 0; i + 1 < shown.size(); ++i)
        painter.fillRect(shown[i]->geometry().right() + 1, 0, ui::metrics::control::border,
                         chrome::actionHeight - chrome::heavyRule, ui::dividerColor(this));
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
    moreButton->setEnabled(words || (bool(actionsRequested) && !loading));
    // The bar holds the tabs, then close on the right, as in Text Presets.
    // A divider separates neighbouring tabs. Each tab takes its label and an
    // equal share of the rest, so a short label leaves room for a long one;
    // without room for padding, the labels touch before any is shortened.
    QVector<QPushButton *> tabs;
    for (auto *tab : { bookmarksTab, annotationsTab, wordsTab })
        if (tab && !tab->isHidden())
            tabs.append(tab);
    const int rule = ui::metrics::control::border;
    const int room = qMax(1, width() - chrome::cellWidth);
    QVector<int> widths;
    int text = 0;
    for (auto *tab : tabs) {
        widths.append(ui::interfaceTextWidth(tab->text(), tab->font()));
        text += widths.last();
    }
    const int count = qMax(1, int(tabs.size()));
    const int padding = text + 2 * count * int(chrome::inset) <= room ? 2 * int(chrome::inset) : 0;
    const int needed = text + count * padding;
    int x = 0;
    for (int i = 0; i < tabs.size(); ++i) {
        const bool last = i + 1 == tabs.size();
        const int share = needed <= room ? widths[i] + padding + (room - needed) / count
                                         : widths[i] * room / qMax(1, needed);
        const int tabWidth = last ? room - x : share;
        tabs[i]->setGeometry(x, 0, qMax(1, tabWidth - (last ? 0 : rule)), barHeight);
        x += tabWidth;
    }
    close->setGeometry(width() - chrome::cellWidth, 0, chrome::cellWidth, barHeight);
    const int strip = chrome::toolStripHeight;
    sortButton->setGeometry(0, chrome::actionHeight, width() - chrome::cellWidth, strip - rule);
    moreButton->setGeometry(width() - chrome::cellWidth, chrome::actionHeight, chrome::cellWidth,
                            strip);
    if (wordsView)
        wordsView->setGeometry(0, top, width(), qMax(1, height() - top));
    for (const auto &panel : coveringPanels)
        if (panel)
            panel->setGeometry(rect());
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
    bookmarksTab->setChecked(!annotations && !words);
    annotationsTab->setChecked(annotations);
    if (wordsTab)
        wordsTab->setChecked(words);
    sortButton->setText(strings::get(order == "newest"           ? "marks.sort_newest"
                                             : order == "oldest" ? "marks.sort_oldest"
                                                                 : "marks.sort_position"));
    const bool currentBookmarked = std::any_of(items.cbegin(), items.cend(),
                                               [](const BookmarkRow &row) { return row.current; });
    moreButton->setGlyph(words                       ? ui::ControlIcon::Books
                                 : annotations       ? ui::ControlIcon::ExportAction
                                 : currentBookmarked ? ui::ControlIcon::BookmarkRemove
                                                     : ui::ControlIcon::BookmarkAdd);
    moreButton->setAccessibleName(strings::get(words                 ? "words.all_books"
                                                       : annotations ? "annotations.export"
                                                                     : "bookmarks.actions"));
    if (words) {
        // The word list draws its own rows below the strip.
        for (auto *widget : std::initializer_list<QWidget *>{ empty, emptyHint, pager })
            widget->hide();
        for (int row = 0; row < buttons.size(); ++row) {
            buttons[row]->hide();
            removes[row]->hide();
        }
        sortButton->setEnabled(true);
        update();
        return;
    }
    empty->setText(strings::get(loading               ? "annotations.loading"
                                        : annotations ? "annotations.empty"
                                                      : "bookmarks.empty"));
    emptyHint->setText(
            strings::get(annotations ? "annotations.empty_hint" : "bookmarks.empty_hint"));
    page = qBound(0, page, pageCount() - 1);
    empty->setVisible(items.isEmpty());
    emptyHint->setVisible(items.isEmpty() && !loading);
    sortButton->setEnabled(!loading);
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
        itemRow->setActionIcon(annotations ? ui::ControlIcon::Trash : ui::ControlIcon::More);
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
