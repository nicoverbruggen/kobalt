#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/options.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "assets/font_catalog.h"
#include "reader_p.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "interfaces/reader/ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "reading/appearance_settings.h"
#include "reading/page_navigation.h"
#include "reading/sleep_screen.h"
#include "storage/reader_storage.h"
#include <QDateTime>

namespace kobalt::reader {
namespace strings = kobalt::strings;
namespace styles = kobalt::ui::styles;
namespace m = kobalt::ui::metrics;
using kobalt::ui::readerStyle;
using kobalt::ui::makeButton;
using kobalt::platform::textRefresh;

void Reader::confirmCustomReplace(const std::function<void()> &proceed, const std::function<void()> &cancel) {
    using kobalt::headerfooter::Preset;
    // Custom itself, or a Custom that still matches Kobalt, has nothing to lose.
    if (chromePreset == Preset::Custom || !customModified()) {
        proceed();
        return;
    }
    if (customReplaceDialog)
        return;
    auto *choice = new ui::ChoiceModal(
        this,
        {strings::get("header_footer.replace_custom_detail").arg(kobalt::headerfooter::presetLabel(chromePreset)),
         strings::get("header_footer.replace_custom_prompt"),
         {}},
        {strings::get("common.cancel"), "rrKeepCustomLayout"},
        {strings::get("header_footer.replace_custom"), "rrReplaceCustomLayout"});
    customReplaceDialog = choice;
    customReplaceDialog->setObjectName("rrReplaceCustom");
    auto *text = choice->descriptionLabel();
    text->setObjectName("rrReplaceCustomDetail");
    readerStyle(text, styles::font(m::chrome::descriptionText) + "color:palette(placeholder-text);");
    auto finish = [this, proceed, cancel](bool replace) {
        if (!customReplaceDialog)
            return;
        customReplaceDialog->hide();
        customReplaceDialog->deleteLater();
        customReplaceDialog.clear();
        if (replace)
            proceed();
        else
            cancel();
        refresh();
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [finish] {
        finish(false);
    });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [finish] {
        finish(true);
    });
    // A tap outside the card keeps the layout, as Cancel does.
    choice->dismissed = [finish] {
        finish(false);
    };
    choice->card()->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    customReplaceDialog->centerCard();
    for (auto *widget : customReplaceDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(customReplaceDialog);
    customReplaceDialog->show();
    customReplaceDialog->raise();
    customReplaceDialog->setFocus();
}

void Reader::showFontDefaultDialog(const QString &face) {
    if (model->temporaryTypography())
        return;
    if (fontDefaultDialog || !appearanceEdit.panelOpen())
        return;
    const auto current = preferences.value("default_font_face").toString(defaultFontFace);
    const bool instructions = face.isEmpty(), alreadyDefault = face == current;
    const ui::ModalContent content{
        instructions     ? strings::get("reader.default_font_instructions")
        : alreadyDefault ? strings::get("reader.already_default_font").arg(fontDisplayName(face))
                         : (face == defaultFontFace ? strings::get("reader.restore_default_font_description")
                                                    : strings::get("reader.make_default_font_description")),
        instructions || alreadyDefault ? strings::get("reader.your_default_font")
                                       : strings::get("reader.make_default_font").arg(fontDisplayName(face)),
        {}};
    const ui::ModalAction primary{instructions || alreadyDefault ? strings::get("common.ok")
                                                                 : strings::get("reader.make_default"),
                                  "rrFontDefaultConfirm"};
    ui::ModalOverlay *dialog;
    QLabel *message;
    QPushButton *confirm;
    QPushButton *cancel = nullptr;
    if (instructions || alreadyDefault) {
        auto *notice = new ui::NoticeModal(this, content, primary);
        dialog = notice;
        message = notice->descriptionLabel();
        confirm = notice->primaryButton();
    } else {
        auto *choice =
            new ui::ChoiceModal(this, content, {strings::get("common.cancel"), "rrFontDefaultCancel"}, primary);
        dialog = choice;
        message = choice->descriptionLabel();
        confirm = choice->primaryButton();
        cancel = choice->secondaryButton();
    }
    fontDefaultDialog = dialog;
    dialog->setObjectName("rrFontDefaultDialog");
    auto *card = dialog->card();
    message->setObjectName("rrFontDefaultMessage");
    card->findChild<QLabel *>("rrModalTitle")->setObjectName("rrFontDefaultTitle");
    const auto dismiss = [this, dialog] {
        dialog->hide();
        dialog->deleteLater();
        fontDefaultDialog.clear();
        choices->setFocus();
        refresh();
    };
    dialog->dismissed = dismiss;
    if (cancel)
        connect(cancel, &QPushButton::clicked, this, dismiss);
    connect(confirm, &QPushButton::clicked, this,
            [this, face, instructions, alreadyDefault, message, card, dialog, dismiss] {
                if (instructions || alreadyDefault) {
                    dismiss();
                    return;
                }
                if (appearanceEdit.preview() && appearanceEdit.preview()->busy())
                    return;
                auto next = preferences;
                next["default_font_face"] = face;
                if (!writePreferences(next)) {
                    message->setText(strings::get("reader.could_not_save_reader_settings"));
                    card->fitToWidth(card->width());
                    dialog->centerCard();
                    return;
                }
                preferences = next;
                dismiss();
                mutateAppearance([face](ReaderSession &preview) {
                    auto settings = preview.state();
                    settings["default_font_face"] = face;
                    preview.apply(settings);
                });
                showFontPicker();
            });
    for (auto *button : card->findChildren<QPushButton *>()) {
        button->setFocusPolicy(Qt::StrongFocus);
    }
    card->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    dialog->centerCard();
    dialog->show();
    dialog->raise();
    confirm->setFocus();
    for (auto *widget : dialog->findChildren<QWidget *>())
        textRefresh(widget);
}

void Reader::showFormattingWarning() {
    if (formattingWarningDialog || closing || lifecycleHidden || services.suspended()) {
        return;
    }
    model->cancelPendingTurn();
    auto *notice = new kobalt::ui::NoticeModal(
        this, {strings::get("reader.formatting_warning_description"), strings::get("reader.formatting_warning"), {}},
        {strings::get("common.ok"), "rrFormattingWarningConfirm"});
    formattingWarningDialog = notice;
    notice->setObjectName("rrFormattingWarningDialog");
    const auto dismiss = [this, notice] {
        notice->hide();
        notice->deleteLater();
        formattingWarningDialog.clear();
        updateOrientationBlock();
        refresh();
    };
    notice->dismissed = dismiss;
    connect(notice->primaryButton(), &QPushButton::clicked, this, dismiss);
    notice->card()->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    notice->centerCard();
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    updateOrientationBlock();
    for (auto *widget : notice->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
}

bool Reader::progressBlocked() const {
    return bool(debugDialog) || bool(finishDialog) || bool(bookmarkErrorDialog) || bool(formattingWarningDialog) ||
           bool(settingsWriteErrorDialog) || book->deviceTransition() || services.suspended() || !isVisible() ||
           lightPopup->isVisible() || model->busy() || (chapterGraph->isVisible() && !statsPinned()) ||
           dictionaryPanel->isVisible() || searchPanel->isVisible() || linksActive() || lookupPending ||
           appearanceEdit.panelOpen() || appearanceEdit.applying() || general->isVisible() || choices->isVisible() ||
           acknowledgments->isVisible();
}

void Reader::dismissProgressChoice() {
    if (syncDialog) {
        syncDialog->hide();
        syncDialog->deleteLater();
        syncDialog.clear();
    }
}

void Reader::showProgressChoice() {
    if (!book->pendingProgress()) {
        dismissProgressChoice();
        return;
    }
    if (progressBlocked() || importingProgress)
        return;
    if (syncDialog) {
        syncDialog->show();
        syncDialog->raise();
        return;
    }
    if (comparingProgress)
        return;
    const auto shown = book->candidate();
    const auto anchor = model->state().value("position").toString();
    comparingProgress = true;
    model->compareNativePosition(shown.location, [this, shown, anchor](const kobalt::PositionComparison &result) {
        using Status = kobalt::PositionComparison::Status;
        comparingProgress = false;
        if (result.status == Status::Stale)
            return;
        if (!book->pendingProgress() || !(book->candidate() == shown) || progressBlocked() ||
            model->state().value("position").toString() != anchor)
            return;
        buildProgressChoice(shown, result.direction, result.status == Status::Compared);
    });
}

void Reader::buildProgressChoice(const kobalt::ReadingProgress &shown, int direction, bool located) {
    model->cancelPendingTurn();
    QString prompt;
    if (shown.status == 2)
        prompt = strings::get("sync.finished");
    else if (!located)
        prompt = strings::get("sync.unlocated");
    else if (direction < 0)
        prompt = strings::get("sync.before");
    else if (direction > 0)
        prompt = strings::get("sync.after");
    else
        prompt = strings::get("sync.same");
    auto *choice = new ui::ChoiceModal(this, {prompt, strings::get("sync.title"), {}},
                                       {strings::get("reader.keep_reading_here"), "rrKeepPosition"},
                                       {strings::get("reader.go_to_synced_position"), "rrGoToSyncedPosition"});
    syncDialog = choice;
    auto *card = choice->card();
    card->setObjectName("rrSyncedPosition");
    auto *message = choice->descriptionLabel();
    message->setObjectName("rrSyncedPositionMessage");
    auto *keep = choice->secondaryButton(), *go = choice->primaryButton();
    if (orientation)
        orientation->setBlocked(true);
    // A choice that cannot be made is left out rather than shown disabled.
    go->setVisible(!shown.location.isEmpty());
    if (shown.location.isEmpty()) {
        keep->setProperty("reader_primary_action", true);
        readerStyle(keep, styles::primary());
    }
    connect(keep, &QPushButton::clicked, this, [this, shown] {
        if (progressBlocked())
            return;
        book->acknowledge(shown, false);
        dismissProgressChoice();
        save();
        refresh();
        progressTick();
    });
    connect(go, &QPushButton::clicked, this, [this, shown, message, go, keep] {
        if (progressBlocked())
            return;
        importingProgress = true;
        go->setEnabled(false);
        keep->setEnabled(false);
        model->goToNative(shown.location, [this, shown, message, go, keep](const QString &error) {
            importingProgress = false;
            if (!error.isEmpty()) {
                // Staying is the only choice left, so it becomes an acknowledgement.
                message->setText(strings::get("sync.position_unavailable"));
                go->hide();
                keep->setText(strings::get("common.ok"));
                keep->setProperty("reader_primary_action", true);
                readerStyle(keep, styles::primary());
                if (syncDialog) {
                    syncDialog->card()->fitToWidth(syncDialog->card()->width());
                    syncDialog->centerCard();
                }
                keep->setEnabled(true);
                return;
            }
            book->acknowledge(shown, true);
            dismissProgressChoice();
            save();
        });
    });
    card->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    syncDialog->centerCard();
    for (auto *widget : syncDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(syncDialog);
    syncDialog->show();
    syncDialog->raise();
    syncDialog->setFocus();
}

#ifndef KOBALT_READER_HARDWARE
void Reader::showProgressDebug() {
    if (debugDialog) {
        debugDialog->show();
        debugDialog->raise();
        return;
    }
    auto *dialog = new kobalt::ui::ModalOverlay(this);
    debugDialog = dialog;
    auto *card = dialog->card();
    card->setObjectName("rrProgressDebug");
    auto *rows = card->rows();
    auto *message =
        card->message(strings::get("reader.simulate_nickel_progress_these_controls_change_only_the_synthetic"));
    auto dismiss = [this, dialog] {
        dialog->hide();
        dialog->deleteLater();
        debugDialog.clear();
    };
    const bool fixture = book->identity() == "file:///mnt/onboard/progress-integration.kepub.epub";
    auto action = [this, dialog, card, rows, message, fixture, dismiss](const char *name, const QString &label,
                                                                        const QString &kind) {
        auto *button = new kobalt::ui::Button(label, card);
        button->setObjectName(name);
        button->setEnabled(fixture && bool(services.debugProgressAction));
        rows->addWidget(button);
        connect(button, &QPushButton::clicked, dialog, [this, message, kind, dismiss] {
            const auto current = book->readProgress();
            QJsonObject command = kobalt::qt::object({{"book", book->identity()}, {"action", kind}});
            if (kind != "fail-next-save") {
                const bool duplicate = kind == "duplicate", stale = kind == "stale";
                const QString next = current.location.startsWith("OEBPS/c2.xhtml") ? "OEBPS/c3.xhtml#kobo.9.1:9"
                                                                                   : "OEBPS/c2.xhtml#kobo.8.1:7";
                const qint64 stamp =
                    duplicate ? current.modified
                    : stale   ? current.modified - 1000
                              : qMax(current.modified, QDateTime::currentMSecsSinceEpoch() / 1000 * 1000) + 1000;
                command["action"] = QStringLiteral("persist");
                command["location"] = duplicate ? current.location : next;
                command["modified"] = QString::number(stamp);
                command["status"] = kind == "finished" ? 2 : duplicate ? current.status : 1;
            }
            const auto result = services.debugProgressAction ? services.debugProgressAction(command) : QJsonObject();
            if (!result.value("ok").toBool()) {
                message->setText(strings::get("reader.the_simulation_failed_no_successful_update_was_reported"));
                if (debugDialog) {
                    debugDialog->card()->fitToWidth(debugDialog->card()->width());
                    debugDialog->centerCard();
                }
                return;
            }
            dismiss();
            applySettings();
            book->invalidateProgress();
        });
    };
    action("rrDebugNewPosition", strings::get("reader.new_synced_position"), "new");
    action("rrDebugDuplicate", strings::get("reader.duplicate_current_update"), "duplicate");
    action("rrDebugStale", strings::get("reader.older_synced_position"), "stale");
    action("rrDebugFinished", strings::get("reader.book_marked_finished"), "finished");
    action("rrDebugSaveFailure", strings::get("reader.fail_next_native_save"), "fail-next-save");
    auto *done = new kobalt::ui::Button(strings::get("common.back"), card);
    done->setObjectName("rrDebugBack");
    rows->addWidget(done);
    connect(done, &QPushButton::clicked, dialog, dismiss);
    card->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    dialog->centerCard();
    for (auto *widget : dialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(dialog);
    dialog->show();
    dialog->raise();
    dialog->setFocus();
}
#endif

void Reader::showFinishChoice() {
    if (finishDialog || !book->offerFinish())
        return;
    model->cancelPendingTurn();
    auto *choice = new ui::ChoiceModal(this, {{}, strings::get("finish.prompt"), {}},
                                       {strings::get("finish.still_reading"), "rrStillReading"},
                                       {strings::get("finish.mark_finished"), "rrMarkFinished"});
    finishDialog = choice;
    finishDialog->setObjectName("rrFinishBook");
    finishDialog->setGeometry(rect());
    auto *card = finishDialog->card();
    card->findChild<QLabel *>("rrModalTitle")->setObjectName("rrFinishBookPrompt");
    auto choose = [this](bool finished) {
        if (!finishDialog)
            return;
        if (services.suspended() || book->deviceTransition() || model->busy())
            return;
        if (finished)
            book->markFinished();
        finishDialog->hide();
        finishDialog->deleteLater();
        finishDialog.clear();
        finishChoiceResolved = exitPending || finished;
        exitPending = exitPending || finished;
        updateOrientationBlock();
        save();
        refresh();
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [choose] {
        choose(false);
    });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [choose] {
        choose(true);
    });
    card->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    finishDialog->centerCard();
    for (auto *widget : finishDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(finishDialog);
    finishDialog->show();
    finishDialog->raise();
    finishDialog->setFocus();
    updateOrientationBlock();
}

void Reader::guardExit() {
    exitGuarded = true;
    const int generation = ++exitGuardGeneration;
    kobalt::qt::later(0, this, [this, generation] {
        kobalt::qt::later(m::contents::exitGuardMilliseconds, this, [this, generation] {
            if (generation == exitGuardGeneration)
                exitGuarded = false;
        });
    });
}

void Reader::askRemoveBookmark(const QString &position) {
    if (removeDialog || !bookmarks->available())
        return;
    const auto &list = bookmarks->bookmarks();
    int index = -1;
    for (int i = 0; i < list.size(); ++i)
        if (list[i].position == position) {
            index = i;
            break;
        }
    if (index < 0)
        return;
    const int number = bookmarks->page(index);
    // One line of the page's first words, cut at a word. The row's excerpt
    // leaves out the chapter title, as in the list.
    QString excerpt = list[index].excerpt;
    for (const auto &row : bookmarkRows())
        if (row.position == position) {
            excerpt = row.excerpt;
            break;
        }
    if (excerpt.size() > 80) {
        excerpt = excerpt.left(80);
        excerpt = excerpt.left(qMax(40, excerpt.lastIndexOf(' '))) + QString::fromUtf8("…");
    }
    const auto detail = number >= 0 ? strings::get("bookmarks.remove_detail").arg(number + 1).arg(excerpt) : excerpt;
    auto *choice = new ui::ChoiceModal(this, {detail, strings::get("bookmarks.remove_prompt"), {}},
                                       {strings::get("common.cancel"), "rrKeepBookmark"},
                                       {strings::get("bookmarks.remove"), "rrConfirmRemoveBookmark"});
    removeDialog = choice;
    removeDialog->setObjectName("rrRemoveBookmark");
    auto *card = choice->card();
    card->findChild<QLabel *>("rrModalTitle")->setObjectName("rrRemoveBookmarkPrompt");
    auto *text = choice->descriptionLabel();
    text->setObjectName("rrRemoveBookmarkDetail");
    readerStyle(text, styles::font(m::chrome::descriptionText) + "color:palette(placeholder-text);");
    auto finish = [this, position](bool remove) {
        if (!removeDialog)
            return;
        removeDialog->hide();
        removeDialog->deleteLater();
        removeDialog.clear();
        if (remove) {
            bookmarks->remove(position);
            page->bookmarked = bookmarks->marked(model->page());
            bookmarkList->showBookmarks(bookmarkRows(),
                                        strings::get("stats.page_of").arg(model->page() + 1).arg(model->pages()));
        }
        refresh();
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [finish] {
        finish(false);
    });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [finish] {
        finish(true);
    });
    card->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    removeDialog->centerCard();
    for (auto *widget : removeDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(removeDialog);
    removeDialog->show();
    removeDialog->raise();
    removeDialog->setFocus();
}

void Reader::openBookmarks() {
    if (!bookmarks->available())
        return;
    if (!bookmarks->writable()) {
        showBookmarkError();
        return;
    }
    model->cancelPendingTurn();
    if (general->isVisible() || appearanceEdit.panelOpen()) {
        appearanceEdit.request(AppearanceDestination::Bookmarks);
        if (general->isVisible())
            applySettings();
        else
            finishPreview();
        return;
    }
    lightPopup->hide();
    chapterGraph->hide();
    choices->hide();
    menu->hide();
    bottomControls->hide();
    const int top = navigation ? navigation->statusHeight() : 0;
    bookmarkList->setGeometry(0, top, width(), (navigation ? navigation->contentBottom(size()) : height()) - top);
    bookmarkList->showBookmarks(bookmarkRows(),
                                strings::get("stats.page_of").arg(model->page() + 1).arg(model->pages()));
    refresh();
}

void Reader::showSettingsWriteError() {
    if (settingsWriteErrorDialog) {
        settingsWriteErrorDialog->raise();
        return;
    }
    model->cancelPendingTurn();
    auto *notice = new ui::NoticeModal(
        this,
        {strings::get("reader.settings_write_error_description"), strings::get("reader.settings_write_error"), {}},
        {strings::get("common.ok"), "rrSettingsWriteErrorConfirm"});
    settingsWriteErrorDialog = notice;
    notice->setObjectName("rrSettingsWriteError");
    const auto dismiss = [this, notice] {
        notice->hide();
        notice->deleteLater();
        settingsWriteErrorDialog.clear();
        updateOrientationBlock();
        refresh();
    };
    notice->dismissed = dismiss;
    connect(notice->primaryButton(), &QPushButton::clicked, this, dismiss);
    notice->card()->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    notice->centerCard();
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    updateOrientationBlock();
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
}

void Reader::showBookmarkError() {
    if (bookmarkErrorDialog || bookmarks->error().isEmpty() || closing || lifecycleHidden || services.suspended() ||
        !isVisible())
        return;
    model->cancelPendingTurn();
    auto *notice = new ui::NoticeModal(this, {bookmarks->error(), strings::get("bookmarks.title"), {}},
                                       {strings::get("common.ok"), "rrBookmarkErrorConfirm"});
    bookmarkErrorDialog = notice;
    notice->setObjectName("rrBookmarkError");
    notice->descriptionLabel()->setObjectName("rrBookmarkErrorMessage");
    const auto dismiss = [this, notice] {
        notice->hide();
        notice->deleteLater();
        bookmarkErrorDialog.clear();
        updateOrientationBlock();
        refresh();
    };
    notice->dismissed = dismiss;
    connect(notice->primaryButton(), &QPushButton::clicked, this, dismiss);
    notice->card()->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    notice->centerCard();
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    updateOrientationBlock();
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(notice);
}

QVector<kobalt::BookmarkRow> Reader::bookmarkRows() const {
    QVector<kobalt::BookmarkRow> rows;
    const auto &list = bookmarks->bookmarks();
    const auto contents = model->chapters();
    for (int i = 0; i < list.size(); ++i) {
        kobalt::BookmarkRow row{list[i].position, list[i].chapter,    list[i].excerpt,
                                list[i].created,  bookmarks->page(i), false};
        row.current = row.page >= 0 && row.page == model->page();
        // Name the chapter the page is in now; the stored title covers a missing contents list.
        for (const auto &link : contents)
            if (link.page >= 0 && row.page >= 0 && link.page <= row.page)
                row.chapter = link.title;
        row.excerpt = kobalt::excerptAfterChapter(row.excerpt, row.chapter);
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const kobalt::BookmarkRow &a, const kobalt::BookmarkRow &b) {
        return (a.page < 0 ? INT_MAX : a.page) < (b.page < 0 ? INT_MAX : b.page);
    });
    return rows;
}

// Only the choices whose name does not say what they do get a description,
// so the pop-up also fits a landscape screen.
QString Reader::refreshExplanation(kobalt::ScreenRefresh mode) {
    if (mode == kobalt::ScreenRefresh::Smart)
        return strings::get("settings.refresh_smart_description").arg(kobalt::smartRefreshTurns);
    if (mode == kobalt::ScreenRefresh::Manual)
        return strings::get("settings.refresh_manually_description");
    return {};
}

QString Reader::refreshLabel(kobalt::ScreenRefresh mode) {
    switch (mode) {
    case kobalt::ScreenRefresh::Smart:
        return strings::get("settings.refresh_smart");
    case kobalt::ScreenRefresh::Chapter:
        return strings::get("settings.refresh_every_chapter");
    case kobalt::ScreenRefresh::Pages6:
        return strings::get("settings.refresh_every_pages").arg(6);
    case kobalt::ScreenRefresh::Pages10:
        return strings::get("settings.refresh_every_pages").arg(10);
    case kobalt::ScreenRefresh::Pages20:
        return strings::get("settings.refresh_every_pages").arg(20);
    case kobalt::ScreenRefresh::Manual:
        return strings::get("settings.refresh_manually");
    }
    return {};
}

// The row's caption shows the mode that Settings saves when it closes.
void Reader::showScreenRefresh() {
    refreshButton->setCaption(refreshLabel(screenRefreshMode).toUpper());
}

void Reader::closeScreenRefresh() {
    if (!refreshPopup)
        return;
    refreshPopup->hide();
    refreshPopup->deleteLater();
    refreshPopup.clear();
}

// A titled pop-up with one row per mode. Picking a mode or tapping
// outside the card closes it.
void Reader::openScreenRefresh() {
    if (refreshPopup)
        return;
    namespace c = m::chrome;
    refreshPopup = new kobalt::ui::ModalOverlay(this);
    refreshPopup->setObjectName("rrScreenRefreshPopup");
    refreshPopup->setGeometry(rect());
    refreshPopup->installEventFilter(this);
    auto *card = refreshPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(m::notice::cardMargins.top());
    auto *title = card->title(strings::get("reader.refresh_the_screen"), "rrScreenRefreshTitle");
    title->setContentsMargins(c::inset, 0, c::inset, 0);
    card->rows()->addSpacing(m::notice::cardMargins.bottom());
    const int cardWidth = qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance);
    // The shared pick-one list: rows share the tallest height a choice needs.
    auto *choices = new kobalt::ui::RadioList(card);
    choices->setObjectName("rrScreenRefreshChoices");
    choices->setMinimumRowHeight(c::listActionHeight);
    choices->setTopDivider(true);
    for (const auto mode : kobalt::screenRefreshModes) {
        choices->add(QString("rrRefresh_" + kobalt::screenRefreshValue(mode)), refreshLabel(mode),
                     refreshExplanation(mode));
        if (mode == screenRefreshMode)
            choices->setCurrent(choices->options().size() - 1);
    }
    choices->chosen = [this](int index) {
        screenRefreshMode = kobalt::screenRefreshModes[index];
        showScreenRefresh();
        closeScreenRefresh();
    };
    card->rows()->addWidget(choices);
    // The card sizes from its layout; give the list its height at the card's width.
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    refreshPopup->centerCard();
    for (auto *widget : refreshPopup->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(refreshPopup);
    refreshPopup->show();
    refreshPopup->raise();
}

namespace {
QString sleepLabel(sleep::Mode mode) {
    switch (mode) {
    case sleep::Mode::Stock:
        return strings::get("settings.sleep_stock");
    case sleep::Mode::Cover:
        return strings::get("settings.sleep_cover");
    case sleep::Mode::Page:
        return strings::get("settings.sleep_page");
    }
    return {};
}
QString sleepExplanation(sleep::Mode mode) {
    switch (mode) {
    case sleep::Mode::Stock:
        return strings::get("settings.sleep_stock_description");
    case sleep::Mode::Cover:
        return strings::get("settings.sleep_cover_description");
    case sleep::Mode::Page:
        return strings::get("settings.sleep_page_description");
    }
    return {};
}
}

void Reader::showSleepScreen() {
    sleepScreenButton->setCaption(sleepLabel(sleep::mode(preferences)).toUpper());
}

void Reader::closeSleepScreen() {
    if (!sleepPopup) {
        return;
    }
    sleepPopup->hide();
    sleepPopup->deleteLater();
    sleepPopup.clear();
}

void Reader::openSleepScreen() {
    if (sleepPopup) {
        return;
    }
    namespace c = m::chrome;
    sleepPopup = new ui::ModalOverlay(this);
    sleepPopup->setObjectName("rrSleepScreenPopup");
    sleepPopup->setGeometry(rect());
    sleepPopup->installEventFilter(this);
    sleepPopup->dismissed = [this] {
        closeSleepScreen();
    };
    auto *card = sleepPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(m::notice::cardMargins.top());
    auto *title = card->title(strings::get("settings.sleep_screen"), "rrSleepScreenTitle");
    title->setContentsMargins(c::inset, 0, c::inset, 0);
    card->rows()->addSpacing(m::notice::cardMargins.bottom());
    const int cardWidth = qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrSleepScreenChoices");
    choices->setMinimumRowHeight(c::listActionHeight);
    choices->setTopDivider(true);
    for (const auto mode : sleep::modes) {
        choices->add("rrSleep_" + sleep::value(mode), sleepLabel(mode), sleepExplanation(mode));
        if (mode == sleep::mode(preferences)) {
            choices->setCurrent(choices->options().size() - 1);
        }
    }
    choices->chosen = [this](int index) {
        auto next = preferences;
        next[sleep::preferenceKey] = sleep::value(sleep::modes[index]);
        next.remove("kobalt_sleep_screen");
        if (writePreferences(next)) {
            preferences = next;
            prepareSleepCover();
            showSleepScreen();
            settingsError->clear();
            settingsError->hide();
        } else {
            settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
            settingsError->show();
        }
        closeSleepScreen();
    };
    card->rows()->addWidget(choices);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    sleepPopup->centerCard();
    for (auto *widget : sleepPopup->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
    textRefresh(sleepPopup);
    sleepPopup->show();
    sleepPopup->raise();
    sleepPopup->setFocus();
}

namespace {
QString navigationLabel(pageNavigation::Mode mode) {
    switch (mode) {
    case pageNavigation::Mode::TapsAndSwipes:
        return strings::get("settings.navigation_taps_and_swipes");
    case pageNavigation::Mode::TapsOnly:
        return strings::get("settings.navigation_taps_only");
    case pageNavigation::Mode::SwipesOnly:
        return strings::get("settings.navigation_swipes_only");
    case pageNavigation::Mode::ButtonsOnly:
        return strings::get("settings.navigation_buttons_only");
    }
    return {};
}
QString tapZoneLabel(tapZones::Layout layout) {
    switch (layout) {
    case tapZones::Layout::LeftRight:
        return strings::get("settings.tap_left_right");
    case tapZones::Layout::TopBottom:
        return strings::get("settings.tap_top_bottom");
    case tapZones::Layout::OneHanded:
        return strings::get("settings.tap_one_handed");
    }
    return {};
}
QString navigationExplanation(pageNavigation::Mode mode) {
    switch (mode) {
    case pageNavigation::Mode::TapsAndSwipes:
        return strings::get("settings.navigation_taps_and_swipes_description");
    case pageNavigation::Mode::TapsOnly:
        return strings::get("settings.navigation_taps_only_description");
    case pageNavigation::Mode::SwipesOnly:
        return strings::get("settings.navigation_swipes_only_description");
    case pageNavigation::Mode::ButtonsOnly:
        return strings::get("settings.navigation_buttons_only_description");
    }
    return {};
}
}

void Reader::showTapZones() {
    tapZonesButton->setCaption(tapZoneLabel(pageNavigation::tapLayout(preferences)).toUpper());
}

void Reader::closeTapZones() {
    if (!tapZonesPopup)
        return;
    tapZonesPopup->hide();
    tapZonesPopup->deleteLater();
    tapZonesPopup.clear();
}

void Reader::openTapZones() {
    if (tapZonesPopup)
        return;
    namespace c = m::chrome;
    tapZonesPopup = new ui::ModalOverlay(this);
    tapZonesPopup->setObjectName("rrTapZonesPopup");
    tapZonesPopup->setGeometry(rect());
    tapZonesPopup->installEventFilter(this);
    tapZonesPopup->dismissed = [this] {
        closeTapZones();
    };
    auto *card = tapZonesPopup->card();
    card->rows()->setContentsMargins(c::inset, m::notice::cardMargins.top(), c::inset, m::notice::cardMargins.bottom());
    card->title(strings::get("settings.tap_zones"), "rrTapZonesTitle");
    card->message(strings::get("settings.tap_zones_hint"), "rrTapZonesHint");
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(m::control::itemGap);
    card->rows()->addLayout(row);
    auto *group = new QButtonGroup(card);
    const bool rtl = model->state().value("page_progression_rtl").toBool();
    for (const auto layout : tapZones::layouts) {
        auto *button = new ui::TapZoneButton(layout, rtl, card);
        button->setObjectName("rrTapZones_" + pageNavigation::tapValue(layout));
        button->setText(tapZoneLabel(layout));
        button->setAccessibleName(button->text());
        button->setChecked(layout == pageNavigation::tapLayout(preferences));
        group->addButton(button);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, layout] {
            auto next = preferences;
            next[pageNavigation::tapPreferenceKey] = pageNavigation::tapValue(layout);
            if (layout == tapZones::Layout::OneHanded)
                next[pageNavigation::preferenceKey] = pageNavigation::value(pageNavigation::Mode::TapsAndSwipes);
            if (writePreferences(next)) {
                preferences = next;
                showTapZones();
                showPageNavigation();
                settingsError->clear();
                settingsError->hide();
            } else {
                settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
                settingsError->show();
            }
            closeTapZones();
        });
    }
    card->message(strings::get("settings.tap_one_handed_hint"), "rrTapZonesOneHandedHint");
    card->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    tapZonesPopup->centerCard();
    for (auto *widget : tapZonesPopup->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(tapZonesPopup);
    tapZonesPopup->show();
    tapZonesPopup->raise();
    tapZonesPopup->setFocus();
}

void Reader::showPageNavigation() {
    pageNavigationButton->setCaption(
        navigationLabel(pageNavigation::mode(preferences, services.pageButtons())).toUpper());
}

void Reader::closePageNavigation() {
    if (!pageNavigationPopup) {
        return;
    }
    pageNavigationPopup->hide();
    pageNavigationPopup->deleteLater();
    pageNavigationPopup.clear();
}

void Reader::openPageNavigation() {
    if (pageNavigationPopup) {
        return;
    }
    namespace c = m::chrome;
    pageNavigationPopup = new ui::ModalOverlay(this);
    pageNavigationPopup->setObjectName("rrPageNavigationPopup");
    pageNavigationPopup->setGeometry(rect());
    pageNavigationPopup->installEventFilter(this);
    pageNavigationPopup->dismissed = [this] {
        closePageNavigation();
    };
    auto *card = pageNavigationPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(m::notice::cardMargins.top());
    auto *title = card->title(strings::get("settings.page_navigation"), "rrPageNavigationTitle");
    title->setContentsMargins(c::inset, 0, c::inset, 0);
    auto *hint = card->message(strings::get("settings.navigation_menu_hint"), "rrPageNavigationHint");
    hint->setContentsMargins(c::inset, 0, c::inset, 0);
    card->rows()->addSpacing(m::notice::cardMargins.bottom());
    const int cardWidth = qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrPageNavigationChoices");
    choices->setMinimumRowHeight(c::listActionHeight);
    choices->setTopDivider(true);
    for (const auto mode : pageNavigation::modes) {
        if (mode == pageNavigation::Mode::ButtonsOnly && !services.pageButtons()) {
            continue;
        }
        const bool needsSwipes =
            pageNavigation::tapLayout(preferences) == tapZones::Layout::OneHanded && !pageNavigation::swipes(mode);
        auto *option =
            choices->add("rrNavigation_" + pageNavigation::value(mode), navigationLabel(mode),
                         needsSwipes ? strings::get("settings.navigation_needs_swipes") : navigationExplanation(mode));
        option->setEnabled(!needsSwipes);
        if (mode == pageNavigation::mode(preferences, services.pageButtons())) {
            choices->setCurrent(choices->options().size() - 1);
        }
    }
    choices->chosen = [this](int index) {
        auto next = preferences;
        next[pageNavigation::preferenceKey] = pageNavigation::value(pageNavigation::modes[index]);
        if (writePreferences(next)) {
            preferences = next;
            showPageNavigation();
            settingsError->clear();
            settingsError->hide();
        } else {
            settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
            settingsError->show();
        }
        closePageNavigation();
    };
    card->rows()->addWidget(choices);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    pageNavigationPopup->centerCard();
    for (auto *widget : pageNavigationPopup->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
    textRefresh(pageNavigationPopup);
    pageNavigationPopup->show();
    pageNavigationPopup->raise();
    pageNavigationPopup->setFocus();
}

}
