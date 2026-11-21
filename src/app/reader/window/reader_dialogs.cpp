#include "app/reader/settings/sleep_editor.h"
#include "app/reader/settings/sleep_gallery.h"
#include <QLineEdit>
#include "app/reader/views/sleep_screen.h"
#include "reading/remaining_time.h"
#include "ui/controls/buttons.h"
#include "ui/controls/options.h"
#include "ui/controls/rows.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "assets/font_catalog.h"
#include "reader_p.h"
#include "ui/dialogs/choice_sheet.h"
#include "app/reader/settings/settings_panel.h"
#include "support/qt_compat.h"
#include "support/percentage_text.h"
#include "ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include "ui/style/metrics.h"
#include "ui/style/fonts.h"
#include "reading/appearance_settings.h"
#include "reading/page_navigation.h"
#include "reading/sleep_screen.h"
#include "reading/session_timeout.h"
#include "storage/reader_storage.h"
#include <QDateTime>
#include <QGridLayout>
#include <utility>

namespace kobalt::reader {
namespace strings = kobalt::strings;
namespace styles = kobalt::ui::styles;
namespace metrics = kobalt::ui::metrics;
using kobalt::platform::textRefresh;
using kobalt::ui::makeButton;
using kobalt::ui::readerStyle;

void Reader::confirmCustomReplace(const std::function<void()> &proceed,
                                  const std::function<void()> &cancel)
{
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
            { strings::get("header_footer.replace_custom_detail")
                      .arg(kobalt::headerfooter::presetLabel(chromePreset)),
              strings::get("header_footer.replace_custom_prompt"),
              {} },
            { strings::get("common.cancel"), "rrKeepCustomLayout" },
            { strings::get("header_footer.replace_custom"), "rrReplaceCustomLayout" });
    customReplaceDialog = choice;
    customReplaceDialog->setObjectName("rrReplaceCustom");
    auto *text = choice->descriptionLabel();
    text->setObjectName("rrReplaceCustomDetail");
    readerStyle(text,
                styles::font(metrics::chrome::descriptionText)
                        + "color:palette(placeholder-text);");
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
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [finish] { finish(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [finish] { finish(true); });
    // A tap outside the card keeps the layout, as Cancel does.
    choice->dismissed = [finish] { finish(false); };
    for (auto *widget : customReplaceDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(customReplaceDialog);
    customReplaceDialog->show();
    customReplaceDialog->raise();
    customReplaceDialog->setFocus();
}

void Reader::showFontDefaultDialog(const QString &face, bool instructions)
{
    if (session->temporaryTypography())
        return;
    if (fontDefaultDialog || !appearanceEdit.panelOpen())
        return;
    const auto current = preferences.value("default_font_face").toString(defaultFontFace);
    const bool alreadyDefault = face == current;
    const auto label =
            face.isEmpty() ? strings::get("reader.publisher_fonts") : fontDisplayName(face);
    const ui::ModalContent content{
        instructions ? strings::get("reader.default_font_instructions")
                : alreadyDefault
                ? strings::get("reader.already_default_font").arg(label)
                : (face == defaultFontFace ? strings::get("reader.restore_default_font_description")
                                           : strings::get("reader.make_default_font_description")),
        instructions || alreadyDefault ? strings::get("reader.your_default_font")
                                       : strings::get("reader.make_default_font").arg(label),
        {},
        "rrFontDefaultTitle"
    };
    const ui::ModalAction primary{ instructions || alreadyDefault
                                           ? strings::get("common.ok")
                                           : strings::get("reader.make_default"),
                                   "rrFontDefaultConfirm" };
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
        auto *choice = new ui::ChoiceModal(
                this, content, { strings::get("common.cancel"), "rrFontDefaultCancel" }, primary);
        dialog = choice;
        message = choice->descriptionLabel();
        confirm = choice->primaryButton();
        cancel = choice->secondaryButton();
    }
    fontDefaultDialog = dialog;
    dialog->setObjectName("rrFontDefaultDialog");
    auto *card = dialog->card();
    message->setObjectName("rrFontDefaultMessage");
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
                diagnostic("font-default-confirm", {}, false);
                auto next = preferences;
                next["default_font_face"] = face;
                const QPointer<ui::ModalOverlay> pending(dialog);
                writePreferences(next, [this, pending, face, message, card, dismiss](
                                               const QJsonObject &applied, bool saved) {
                    if (saved)
                        preferences = applied;
                    if (!pending || pending != fontDefaultDialog)
                        return;
                    if (!saved) {
                        message->setText(strings::get("reader.could_not_save_reader_settings"));
                        card->fitToWidth(card->width());
                        pending->centerCard();
                        return;
                    }
                    dismiss();
                    mutateAppearance([face](ReaderSession &preview) {
                        const auto patch = appearance::Patch(
                                qt::object({ { "default_font_face",
                                               face.isEmpty() ? QString(defaultFontFace) : face } }));
                        preview.requestAppearance(preview.displayedAppearance().settings.edited(patch));
                    });
                    showFontPicker();
                    diagnostic("font-default-applied", {}, false);
                });
            });
    for (auto *button : card->findChildren<QPushButton *>()) {
        button->setFocusPolicy(Qt::StrongFocus);
    }
    dialog->show();
    dialog->raise();
    confirm->setFocus();
    for (auto *widget : dialog->findChildren<QWidget *>())
        textRefresh(widget);
}

void Reader::showFormattingWarning()
{
    if (formattingWarningDialog || closing || lifecycleHidden || services.suspended()) {
        return;
    }
    session->cancelPendingTurn();
    auto *notice = new kobalt::ui::NoticeModal(
            this,
            { strings::get("reader.formatting_warning_description"),
              strings::get("reader.formatting_warning"),
              {} },
            { strings::get("common.ok"), "rrFormattingWarningConfirm" });
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
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    updateOrientationBlock();
    for (auto *widget : notice->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
}

bool Reader::progressBlocked() const
{
    // The sync prompt is the choice itself, so positionPromptOpen() does not
    // apply here.
    return exitProgress != ExitProgress::Normal || bool(debugDialog) || bool(finishDialog)
            || bool(kosyncDialog) || bool(annotationErrorDialog) || bool(formattingWarningDialog)
            || bool(settingsWriteErrorDialog) || book->deviceTransition() || services.suspended()
            || !isVisible() || session->busy() || sheetOpen() || readingToolOpen()
            || appearanceEdit.applying() || imageViewer->isVisible() || annotationMode
            || bookmarkList->isVisible() || topControls->isVisible();
}

void Reader::clearExitSyncWait()
{
    if (exitProgress == ExitProgress::WaitingForSync) {
        exitProgress = ExitProgress::Normal;
        dismissProgressChoice();
    }
}

void Reader::showExitSyncWait()
{
    if (exitProgress == ExitProgress::WaitingForSync)
        return;
    dismissProgressChoice();
    exitProgress = ExitProgress::WaitingForSync;
    auto *choice = new ui::ChoiceModal(
            this, { strings::get("closing.sync_detail"), strings::get("closing.sync_title"), {} },
            { strings::get("closing.local_only"), "rrExitLocalOnly" },
            { strings::get("closing.wait_for_sync"), "rrExitWaitForSync" });
    syncDialog = choice;
    choice->setGeometry(rect());
    choice->dismissed = [this] { cancelExit(); };
    auto *wait = choice->primaryButton();
    connect(wait, &QPushButton::clicked, choice, [choice, wait] {
        wait->setText(strings::get("closing.waiting_for_sync"));
        wait->setEnabled(false);
        choice->setFocus();
    });
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [this] {
        clearExitSyncWait();
        exitProgress = ExitProgress::LocalOnly;
        finishExit();
    });
    // Some firmware versions do not signal the end of every sync.
    auto *poll = new QTimer(choice);
    connect(poll, &QTimer::timeout, this, [this, choice] {
        if (syncDialog == choice)
            finishExit();
    });
    poll->start(1000);
    for (auto *widget : choice->findChildren<QWidget *>())
        textRefresh(widget);
    choice->show();
    choice->raise();
    choice->primaryButton()->setFocus();
}

void Reader::dismissProgressChoice()
{
    if (syncDialog) {
        syncDialog->hide();
        syncDialog->deleteLater();
        syncDialog.clear();
    }
}

void Reader::showProgressChoice()
{
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
    const auto anchor = session->state().value("position").toString();
    comparingProgress = true;
    session->compareNativePosition(
            shown.location, [this, shown, anchor](const kobalt::PositionComparison &result) {
                using Status = kobalt::PositionComparison::Status;
                comparingProgress = false;
                if (result.status == Status::Stale)
                    return;
                if (progressBlocked())
                    progressCheckDeferred = true;
                if (!book->pendingProgress() || !(book->candidate() == shown) || progressBlocked()
                    || session->state().value("position").toString() != anchor)
                    return;
                buildProgressChoice(shown, result.direction, result.status == Status::Compared);
            });
}

void Reader::buildProgressChoice(const kobalt::ReadingProgress &shown, int direction, bool located)
{
    session->cancelPendingTurn();
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
    auto *choice = new ui::ChoiceModal(
            this, { prompt, strings::get("sync.title"), {} },
            { strings::get("reader.keep_reading_here"), "rrKeepPosition" },
            { strings::get("reader.go_to_synced_position"), "rrGoToSyncedPosition" });
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
    }
    connect(keep, &QPushButton::clicked, this, [this, shown] {
        if (progressBlocked())
            return;
        book->acknowledge(shown, false);
        savedNativeAnchor.clear();
        dismissProgressChoice();
        save(false, true);
        refresh();
        progressTick();
    });
    connect(go, &QPushButton::clicked, this, [this, shown, message, go, keep] {
        if (progressBlocked())
            return;
        importingProgress = true;
        go->setEnabled(false);
        keep->setEnabled(false);
        session->goToNative(shown.location, [this, shown, message, go, keep](const QString &error) {
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
            savedNativeAnchor.clear();
            dismissProgressChoice();
            save(false, true);
        });
    });
    for (auto *widget : syncDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(syncDialog);
    syncDialog->show();
    syncDialog->raise();
    syncDialog->setFocus();
}

void Reader::showProgressDebug()
{
    if (!services.development.simulateProgress)
        return;
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
    auto *message = card->message(services.development.progressDescription);
    auto dismiss = [this, dialog] {
        dialog->hide();
        dialog->deleteLater();
        debugDialog.clear();
    };
    const bool available = services.development.progressAvailable
            && services.development.progressAvailable(book->identity());
    auto action = [this, dialog, card, rows, message, available,
                   dismiss](const char *name, const QString &label, ProgressSimulation kind) {
        auto *button = new kobalt::ui::Button(label, card);
        button->setObjectName(name);
        button->setEnabled(available);
        rows->addWidget(button);
        connect(button, &QPushButton::clicked, dialog, [this, message, kind, dismiss] {
            const auto current = book->readProgress();
            if (!services.development.simulateProgress(kind, current)) {
                message->setText(strings::get(
                        "reader.the_simulation_failed_no_successful_update_was_reported"));
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
    action("rrDebugNewPosition", strings::get("reader.new_synced_position"),
           ProgressSimulation::NewPosition);
    action("rrDebugDuplicate", strings::get("reader.duplicate_current_update"),
           ProgressSimulation::Duplicate);
    action("rrDebugStale", strings::get("reader.older_synced_position"),
           ProgressSimulation::OlderPosition);
    action("rrDebugFinished", strings::get("reader.book_marked_finished"),
           ProgressSimulation::Finished);
    action("rrDebugSaveFailure", strings::get("reader.fail_next_native_save"),
           ProgressSimulation::SaveFailure);
    auto *done = new kobalt::ui::Button(strings::get("common.back"), card);
    done->setObjectName("rrDebugBack");
    rows->addWidget(done);
    connect(done, &QPushButton::clicked, dialog, dismiss);
    card->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    dialog->centerCard();
    for (auto *widget : dialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(dialog);
    dialog->show();
    dialog->raise();
    dialog->setFocus();
}

void Reader::showFinishChoice()
{
    if (finishDialog || !book->offerFinish())
        return;
    session->cancelPendingTurn();
    auto *choice = new ui::ChoiceModal(
            this, { {}, strings::get("finish.prompt"), {}, "rrFinishBookPrompt" },
            { strings::get("finish.still_reading"), "rrStillReading" },
            { strings::get("finish.mark_finished"), "rrMarkFinished" });
    finishDialog = choice;
    finishChoiceForExit = exitPending;
    finishDialog->setObjectName("rrFinishBook");
    finishDialog->setGeometry(rect());
    auto *card = finishDialog->card();
    auto *suppress = new ui::DescribedOption(strings::get("finish.dont_ask_again"), {}, card);
    suppress->setObjectName("rrSuppressFinishPrompt");
    auto look = suppress->presentation();
    look.indicator = ui::DescribedOption::Indicator::Square;
    look.indicatorSize = QSize(metrics::chrome::checkSize, metrics::chrome::checkSize);
    look.labelPixels = metrics::chrome::labelText;
    look.minimumRowHeight = metrics::chrome::actionHeight;
    look.dividers = Qt::TopEdge;
    look.horizontalInset = metrics::chrome::inset;
    suppress->setPresentation(look);
    // Keep the checkbox above the connected confirmation footer.
    card->rows()->insertWidget(card->rows()->count() - 1, suppress);
    auto choose = [this, suppress](bool finished) {
        if (!finishDialog)
            return;
        if (services.suspended() || book->deviceTransition() || session->busy())
            return;
        if (finished)
            book->markFinished();
        if (suppress->isChecked())
            book->suppressFinishPrompt();
        finishDialog->hide();
        finishDialog->deleteLater();
        finishDialog.clear();
        finishChoiceResolved = exitPending || finished;
        exitPending = exitPending || finished;
        if (exitPending)
            session->pauseHybridTurnPreparation(true);
        updateOrientationBlock();
        save(false, true);
        refresh();
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [choose] { choose(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [choose] { choose(true); });
    for (auto *widget : finishDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(finishDialog);
    finishDialog->show();
    finishDialog->raise();
    finishDialog->setFocus();
    updateOrientationBlock();
}

void Reader::guardCloseTap()
{
    closeGuarded = true;
    closeGuardPressTime = lastPressTime;
    const int generation = ++closeGuardGeneration;
    kobalt::qt::later(0, this, [this, generation] {
        kobalt::qt::later(metrics::contents::closeGuardMilliseconds, this, [this, generation] {
            if (generation == closeGuardGeneration)
                closeGuarded = false;
        });
    });
}

bool Reader::closeGuardElapsed() const
{
    // The timer above starts once the reader has handled the close. Source
    // timestamps tell a tap pressed later apart from one that only waited.
    // Without them, the timer alone decides. Subtraction permits clock wrap.
    if (!closeGuardPressTime || !lastPressTime)
        return false;
    const quint32 elapsed = lastPressTime - closeGuardPressTime;
    return elapsed < 0x80000000u && elapsed >= quint32(metrics::contents::closeGuardMilliseconds);
}

void Reader::askRemoveBookmark(const QString &position)
{
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
    const int number = bookmarks->bookPage(index);
    // One line of the page's first words, cut at a word. The row's excerpt
    // leaves out the chapter title, as in the list.
    BookmarkChapterTitles titles(session->chapters());
    const auto chapter = titles.title(number, list[index].chapter);
    const auto excerpt =
            elideBookmarkExcerpt(excerptAfterChapter(list[index].excerpt, chapter), 40, 80);
    const auto detail = number >= 0
            ? strings::get("bookmarks.remove_detail").arg(number + 1).arg(excerpt)
            : excerpt;
    const bool pinned = list[index].pinned();
    auto description = strings::get(pinned ? "bookmarks.remove_pinned_description"
                                           : "bookmarks.remove_description");
    if (!list[index].name.isEmpty())
        description += "\n\n" + list[index].name;
    description += "\n\n" + detail;
    auto *choice = new ui::ChoiceModal(
            this,
            { description,
              strings::get(pinned ? "bookmarks.remove_pinned_prompt" : "bookmarks.remove_prompt"),
              {},
              "rrRemoveBookmarkPrompt" },
            { strings::get("common.cancel"), "rrKeepBookmark" },
            { strings::get("bookmarks.remove"), "rrConfirmRemoveBookmark" });
    removeDialog = choice;
    removeDialog->setObjectName("rrRemoveBookmark");
    auto *text = choice->descriptionLabel();
    text->setObjectName("rrRemoveBookmarkDetail");
    auto finish = [this, position](bool remove) {
        if (!removeDialog)
            return;
        removeDialog->hide();
        removeDialog->deleteLater();
        removeDialog.clear();
        if (remove) {
            bookmarks->remove(position);
            page->bookmarked = bookmarks->marked(session->page());
            if (bookmarkList->isVisible() && !bookmarkList->annotationsShown())
                bookmarkList->showBookmarks(bookmarkRows());
        }
        refresh();
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [finish] { finish(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [finish] { finish(true); });
    for (auto *widget : removeDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(removeDialog);
    removeDialog->show();
    removeDialog->raise();
    removeDialog->setFocus();
}

void Reader::openBookmarks()
{
    const bool bookmarksEnabled = features::enabled(preferences, features::Bookmarks);
    const bool annotationsEnabled = features::enabled(preferences, features::Annotations);
    const bool wordsEnabled = savedWords && features::enabled(preferences, features::Words);
    if (!bookmarksEnabled && !annotationsEnabled && !wordsEnabled)
        return;
    bookmarkList->setFeatures(bookmarksEnabled, annotationsEnabled, wordsEnabled);
    annotationPopover->hide();
    if (!bookmarks->available())
        return;
    if (!bookmarks->writable()) {
        showBookmarkError();
        return;
    }
    session->cancelPendingTurn();
    if (settingsVisible() || appearanceEdit.panelOpen()) {
        appearanceEdit.request(AppearanceDestination::Bookmarks);
        if (settingsVisible())
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
    bookmarkList->setGeometry(0, top, width(),
                              (navigation ? navigation->contentBottom(size()) : height()) - top);
    // Bookmarks first, unless the book has annotations and no bookmarks. With
    // only My Words shown, the list opens on its words.
    if (!bookmarksEnabled && !annotationsEnabled) {
        showWords();
        refresh();
        return;
    }
    bookmarkList->setExcerptFont(bookFace());
    if (annotationsEnabled
        && (!bookmarksEnabled
            || (bookmarks->bookmarks().isEmpty() && !highlights->highlights().isEmpty()))) {
        const bool ready = highlights->resolved() || highlights->sync();
        bookmarkList->showAnnotations(ready ? annotationRows() : QVector<BookmarkRow>(), !ready);
    } else {
        bookmarkList->showBookmarks(bookmarkRows());
    }
    refresh();
}

void Reader::showSettingsWriteError()
{
    if (settingsWriteErrorDialog) {
        settingsWriteErrorDialog->raise();
        return;
    }
    session->cancelPendingTurn();
    auto *notice =
            new ui::NoticeModal(this,
                                { strings::get("reader.settings_write_error_description"),
                                  strings::get("reader.settings_write_error"),
                                  {} },
                                { strings::get("common.ok"), "rrSettingsWriteErrorConfirm" });
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
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    updateOrientationBlock();
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
}

void Reader::showBookmarkError()
{
    if (!bookmarks->error().isEmpty())
        showAnnotationError(bookmarks->error(), strings::get("bookmarks.title"), "rrBookmarkError");
}

void Reader::showHighlightError()
{
    showAnnotationError(strings::get("annotations.change_not_saved"),
                        strings::get("annotations.title"), "rrHighlightError");
}

bool Reader::showNotice(const ReaderNotice &notice)
{
    if (notice.message.isEmpty() || annotationErrorDialog || closing || lifecycleHidden
        || services.suspended() || !isVisible())
        return false;
    showAnnotationError(notice.message, notice.title, notice.name);
    return true;
}

void Reader::showStartupNotice()
{
    if (showNotice(services.startupNotice))
        services.startupNotice = {};
}

// `name` names the notice; its message and button add `Message` and `Confirm`.
void Reader::showAnnotationError(const QString &message, const QString &title, const QString &name)
{
    if (annotationErrorDialog || closing || lifecycleHidden || services.suspended() || !isVisible())
        return;
    session->cancelPendingTurn();
    auto *notice = new ui::NoticeModal(this, { message, title, {} },
                                       { strings::get("common.ok"), name + "Confirm" });
    annotationErrorDialog = notice;
    notice->setObjectName(name);
    notice->descriptionLabel()->setObjectName(name + "Message");
    const auto dismiss = [this, notice] {
        notice->hide();
        notice->deleteLater();
        annotationErrorDialog.clear();
        updateOrientationBlock();
        refresh();
    };
    notice->dismissed = dismiss;
    connect(notice->primaryButton(), &QPushButton::clicked, this, dismiss);
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
    updateOrientationBlock();
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(notice);
}

QVector<kobalt::BookmarkRow> Reader::bookmarkRows() const
{
    QVector<kobalt::BookmarkRow> rows;
    const auto &list = bookmarks->bookmarks();
    const auto pages = bookmarks->resolvedPages();
    const auto state = session->state();
    const bool pagePending =
            state.contains("book_fraction") && state.value("rough_pages").toInt() <= 0;
    rows.reserve(list.size());
    for (int i = 0; i < list.size(); ++i) {
        kobalt::BookmarkRow row{ list[i].position, list[i].chapter,        list[i].excerpt,
                                 list[i].created,  bookmarks->bookPage(i), false };
        row.current = pages.value(i, -1) >= 0 && pages.value(i, -1) == session->page();
        row.pagePending = pagePending;
        row.pinName = list[i].name;
        row.pinIcon = list[i].icon;
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const kobalt::BookmarkRow &a, const kobalt::BookmarkRow &b) {
                         return (a.page < 0 ? INT_MAX : a.page) < (b.page < 0 ? INT_MAX : b.page);
                     });
    BookmarkChapterTitles titles(session->chapters());
    for (int i = rows.size() - 1; i >= 0; --i) {
        auto &row = rows[i];
        row.chapter = titles.title(row.page, row.chapter);
        row.excerpt = excerptAfterChapter(row.excerpt, row.chapter);
    }
    return rows;
}

void Reader::syncPinnedBookmarks()
{
    if (!pinnedBookmarks)
        return;
    const auto &items = bookmarks->bookmarks();
    QVector<int> pins;
    for (int i = 0; i < items.size() && pins.size() < BookmarkStore::maxPins; ++i)
        if (items[i].pinned())
            pins.append(i);
    std::stable_sort(pins.begin(), pins.end(), [this](int a, int b) {
        const int aPage = bookmarks->bookPage(a), bPage = bookmarks->bookPage(b);
        return (aPage < 0 ? INT_MAX : aPage) < (bPage < 0 ? INT_MAX : bPage);
    });
    const bool visible = features::enabled(preferences, features::Bookmarks) && !pins.isEmpty()
            && topControls->isVisible() && !session->busy() && !lifecycleHidden && !closing
            && !services.suspended() && !annotationMode && !seekOpen && !appearanceEdit.panelOpen()
            && !activityOpen() && !settingsVisible() && !menu->isVisible() && !choices->isVisible()
            && !lightPopup->isVisible() && !bookmarkList->isVisible() && !acknowledgmentsVisible()
            && !searchPanel->isVisible() && !dictionaryPanel->isVisible() && !linksActive()
            && !lookupPending && !syncDialog && !kosyncDialog && !finishDialog
            && !(fieldChooser && fieldChooser->isVisible())
            && !(headerFooterView && headerFooterView->isVisible());
    pinnedBookmarks->setVisible(visible);
    if (!visible)
        return;
    const int padding = ui::px(12), edgeGap = ui::px(20);
    const int top = topControls->geometry().bottom() + 1;
    int bottom = navigation               ? navigation->navigationTop()
            : bottomControls->isVisible() ? bottomControls->y()
                                          : height();
    // A short screen, such as a phone in landscape, cannot stack every
    // shortcut at a usable size. Add columns instead of shrinking below
    // the buttons' minimum size, including with large interface text.
    int minimumSide = 0;
    for (auto *button : pinnedButtons) {
        button->ensurePolished();
        minimumSide = qMax(minimumSide, qMax(button->minimumWidth(), button->minimumHeight()));
    }
    const int cell = qMax<int>(metrics::chrome::actionHeight, minimumSide + 2 * padding);
    const int gap = ui::px(16);
    const bool stats = chapterGraph->isVisible();
    // Prefer the exposed paper above Stats, keeping its text at full width.
    // Short landscape screens instead leave room beside it for one column.
    const bool besideStats = stats && chapterGraph->y() - top < cell + 2 * padding;
    pinnedBookmarks->setAttribute(Qt::WA_OpaquePaintEvent, besideStats);
    if (stats && !besideStats)
        bottom = chapterGraph->y();
    const int room = qMax(1, bottom - top);
    const int capacity = qBound(1, (room - 2 * padding) / cell, BookmarkStore::maxPins);
    const int columns = besideStats ? 1 : qMax(1, (width() - 2 * edgeGap + gap) / (cell + gap));
    // Bookmarks retains any pins that do not fit at their usual button size.
    const int count = stats ? qMin(int(pins.size()), capacity * columns) : int(pins.size());
    const int used = (count + capacity - 1) / capacity;
    const int columnRows = (count + used - 1) / used;
    const int side = qMax(1, cell - 2 * padding);
    const int trayHeight = cell * columnRows + 2 * padding;
    const int trayWidth = cell * used + gap * (used - 1);
    if (besideStats) {
        if (room < cell + 2 * padding) {
            pinnedBookmarks->hide();
            return;
        }
        const int panelWidth = qMax(1, width() - trayWidth - 2 * edgeGap);
        chapterGraph->setGeometry(0, bottom - chapterGraph->height(), panelWidth,
                                  chapterGraph->height());
    }
    if (besideStats)
        pinnedBookmarks->setGeometry(chapterGraph->geometry().right() + 1, top,
                                     width() - chapterGraph->width(), room);
    else
        pinnedBookmarks->setGeometry(width() - trayWidth - edgeGap, top + (room - trayHeight) / 2,
                                     trayWidth, trayHeight);
    const int trayLeft = besideStats ? edgeGap : 0;
    const int trayTop = (pinnedBookmarks->height() - trayHeight) / 2;
    for (int i = 0; i < pinnedButtons.size(); ++i) {
        auto *button = pinnedButtons[i];
        button->setVisible(i < count);
        if (i >= count)
            continue;
        const auto &pin = items[pins[i]];
        const int column = i / columnRows;
        const int rows = qMin(columnRows, count - column * columnRows);
        const int columnTop = (trayHeight - (cell * rows + 2 * padding)) / 2;
        button->setGeometry(trayLeft + padding + column * (cell + gap),
                            trayTop + columnTop + padding + i % columnRows * cell
                                    + (cell - side) / 2,
                            side, side);
        button->setGlyph(ui::bookmarkIcon(pin.icon));
        const auto *custom = session->customPins().find(pin.icon);
        button->setGlyphMask(custom ? custom->mask : QImage());
        const auto name = pin.name.isEmpty() ? pin.chapter : pin.name;
        button->setAccessibleName(name);
        button->setToolTip(name);
        button->setProperty("bookmark_position", pin.position);
        button->setEnabled(bookmarks->bookPage(pins[i]) >= 0
                           || (session->state().contains("book_fraction")
                               && session->state().value("rough_pages").toInt() <= 0));
    }
    pinnedBookmarks->update();
    pinnedBookmarks->raise();
}

// Only the choices whose name does not say what they do get a description,
// so the pop-up also fits a landscape screen.
QString Reader::refreshExplanation(kobalt::ScreenRefresh mode)
{
    if (mode == kobalt::ScreenRefresh::Smart)
        return strings::get("settings.refresh_smart_description").arg(kobalt::smartRefreshTurns);
    if (mode == kobalt::ScreenRefresh::Manual)
        return strings::get("settings.refresh_manually_description");
    return {};
}

QString Reader::refreshLabel(kobalt::ScreenRefresh mode)
{
    switch (mode) {
    case kobalt::ScreenRefresh::Smart:
        return strings::get("settings.refresh_smart");
    case kobalt::ScreenRefresh::EveryPage:
        return strings::get("settings.refresh_every_page");
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
void Reader::showScreenRefresh()
{
    if (refreshButton)
        refreshButton->setCaption(refreshLabel(screenRefreshMode).toUpper());
}

// Writes the preferences. A failed write keeps the old ones and shows the
// error line in Settings; a write that succeeds clears it.
void Reader::savePreferences(const QJsonObject &next, storage::RecordWriter::Done done)
{
    writePreferences(next, [this, done = std::move(done)](const QJsonObject &applied, bool saved) {
        if (saved)
            preferences = applied;
        if (settingsError) {
            if (saved) {
                settingsError->clear();
                settingsError->hide();
            } else {
                settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
                settingsError->show();
            }
        }
        if (done)
            done(saved);
    });
}

void Reader::closePopup(QPointer<ui::ModalOverlay> &slot)
{
    if (!slot)
        return;
    slot->hide();
    slot->deleteLater();
    slot.clear();
}

// A titled pop-up with one row for each option, kept in `slot`. Picking a row
// calls `chosen` with its index; `chosen` saves and closes. A tap on the
// backdrop closes the pop-up. Its widgets are named `name` + "Popup",
// "Title", "Hint" and "Choices"; each option brings its own name.
void Reader::openChoicePopup(QPointer<ui::ModalOverlay> &slot, const QString &name,
                             const QString &title, const QString &hint,
                             const QVector<ChoiceOption> &options, int current,
                             std::function<void(int)> chosen, bool focus)
{
    if (slot)
        return;
    const auto sheet = ui::makeChoiceSheet(this, name, title, hint, name + "Title", name + "Hint");
    slot = sheet.overlay;
    slot->dismissed = [this, popup = &slot] { closePopup(*popup); };
    for (const auto &option : options)
        sheet.list->add(option.name, option.label, option.description)->setEnabled(option.enabled);
    if (current >= 0)
        sheet.list->setCurrent(current);
    sheet.list->chosen = std::move(chosen);
    ui::fitChoiceSheet(slot, sheet.list);
    for (auto *widget : slot->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(slot);
    slot->show();
    slot->raise();
    if (focus)
        slot->setFocus();
}

void Reader::openStatsWordScope()
{
    openChoicePopup(wordScopePopup, "rrStatsWordScope", strings::get("stats.count_words_in_title"),
                    strings::get("stats.scope_hint"),
                    { { "rrStatsWordMain", strings::get("stats.scope_main"),
                        strings::get("stats.scope_main_hint") },
                      { "rrStatsWordVolume", strings::get("stats.scope_volume"),
                        strings::get("stats.scope_volume_hint") } },
                    statsUseWholeVolume() ? 1 : 0, [this](int index) {
                        auto next = preferences;
                        next["stats_word_scope"] = index ? QStringLiteral("volume") : QStringLiteral("main");
                        savePreferences(next, [this, popup = wordScopePopup](bool saved) {
                            if (saved) {
                                if (popup && popup == wordScopePopup)
                                    closePopup(wordScopePopup);
                                updateGraph();
                            }
                        });
                    });
}

void Reader::openScreenRefresh()
{
    QVector<ChoiceOption> options;
    int current = -1;
    for (const auto mode : kobalt::screenRefreshModes) {
        if (mode == screenRefreshMode)
            current = options.size();
        options.append({ QString("rrRefresh_" + kobalt::screenRefreshValue(mode)),
                         refreshLabel(mode), refreshExplanation(mode) });
    }
    // The mode is saved when Settings closes. The pop-up has never taken the
    // focus.
    openChoicePopup(
            refreshPopup, "rrScreenRefresh", strings::get("reader.refresh_the_screen"), {}, options,
            current,
            [this](int index) {
                screenRefreshMode = kobalt::screenRefreshModes[index];
                showScreenRefresh();
                closePopup(refreshPopup);
            },
            false);
}

namespace {
QString sessionTimeoutLabel(int minutes)
{
    return strings::get(QString("settings.session_timeout_%1").arg(minutes).toLatin1().constData());
}
} // namespace

void Reader::showSleepScreen()
{
    if (!sleepScreenButton)
        return;
    sessionTimeoutButton->setCaption(
            sessionTimeoutLabel(sessionTimeout::minutes(preferences)).toUpper());
    sleepScreenButton->setCaption(
            sleepScreenButton->isEnabled()
                    ? SleepGallery::label(sleep::design(preferences)).toUpper()
                    : strings::get("settings.unsupported_caption"));
}

void Reader::closeSleepScreen()
{
    closeSleepEditor();
    if (sleepGallery) {
        sleepGallery->hide();
        sleepGallery->deleteLater();
        sleepGallery.clear();
    }
    if (!sleepPopup) {
        if (!sleep::uses(preferences, sleep::Mode::Overlay))
            sleepOverlay = {};
        // The gallery and the editor keep the cover while they are open.
        // Release it once no sleep look draws it.
        if (!sleep::uses(preferences, sleep::Mode::Cover)) {
            ++sleepCoverRequest;
            sleepCover = {};
            sleepCoverSource = {};
        }
        return;
    }
    closePopup(sleepPopup);
}

void Reader::saveSleepPreferences(const QJsonObject &next, storage::RecordWriter::Done done)
{
    savePreferences(next, [this, done = std::move(done)](bool saved) {
        if (saved)
            showSleepScreen();
        if (done)
            done(saved);
    });
}

sleep::Snapshot Reader::sleepPreviewSample(bool poweredOff)
{
    sleep::Snapshot sample;
    const int percent = book && session
            ? (book->finishedHere() ? 100 : session->statsPercent(statsUseWholeVolume()))
            : 0;
    sample.percent = percent;
    sample.progress =
            strings::get("sleep.percent_read").arg(strings::percentArgument(percent, QLocale::c()));
    if (book) {
        sample.remaining = sleepTimeLeft(book->estimates(statsUseWholeVolume()).bookMilliseconds,
                                         remainingTime::approximate(preferences));
    }
    sample.dark = darkTheme;
    sample.dimPage = false;
    sample.tile = sleep::savedTile(preferences);
    if (session && book)
        sample.details = sleepDetails(poweredOff);
    sample.privateBook = book && book->hideBookOnSleepScreen();
    return sample;
}

void Reader::openSleepScreen()
{
    if (sleepGallery || sleepPopup)
        return;
    QVector<SleepGallery::Entry> entries;
    for (const auto design : sleep::designs)
        entries.append({ design, design != sleep::Design::Overlay || !sleepOverlay.isNull() });
    sleepGallery = new SleepGallery(this, entries, sleep::design(preferences), sleepPreviewSample(),
                                    page ? page->size() : size(),
                                    services.nativeSleepPreview ? services.nativeSleepPreview()
                                                                : sleep::NativePreview{},
                                    sleep::followsReading(preferences));
    sleepGallery->setGeometry(general ? general->geometry() : rect());
    sleepGallery->followOrientationChanged = [this](bool checked,
                                                     storage::RecordWriter::Done done) {
        auto next = preferences;
        next[sleep::followOrientationKey] = checked;
        saveSleepPreferences(next, std::move(done));
    };
    sleepGallery->setCustom(sleep::custom(preferences));
    sleepGallery->setPlaces(sleepPlaces());
    sleepGallery->setWallpapers(sleepWallpaper, sleepOffWallpaper);
    sleepGallery->chosen = [this](sleep::Design design) {
        // Custom is chosen when its editor closes.
        if (design == sleep::Design::Custom) {
            openSleepEditor(design);
            return;
        }
        auto next = preferences;
        sleep::setDesign(next, design);
        next.remove("kobalt_sleep_screen");
        saveSleepPreferences(next, [this, gallery = sleepGallery](bool saved) {
            if (saved)
                prepareSleepCover();
            if (gallery && gallery == sleepGallery)
                closeSleepScreen();
        });
    };
    sleepGallery->unavailable = [this](sleep::Design design) {
        if (design == sleep::Design::Overlay)
            showSleepOverlayHelp();
    };
    sleepGallery->closed = [this] { closeSleepScreen(); };
    sleepGallery->customize = [this](sleep::Design design) {
        if (design == sleep::Design::Night)
            editSleepGreeting();
        else
            openSleepEditor(design);
    };
    if (!sleepCover.isNull())
        sleepGallery->setCover(sleepCover);
    else
        loadGalleryCover();
    if (page)
        sleepGallery->setPage(page->snapshot(true));
    if (!sleepOverlay.isNull())
        sleepGallery->setOverlay(sleepOverlay);
    for (auto *widget : sleepGallery->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(sleepGallery);
    sleepGallery->show();
    sleepGallery->raise();
    if (sleepOverlay.isNull())
        prepareSleepOverlay();
    prepareSleepWallpapers();
}

QHash<int, sleep::Place> Reader::sleepPlaces() const
{
    QHash<int, sleep::Place> places;
    const auto own = book ? book->sleepPositions() : QJsonObject();
    for (const auto design : sleep::customStatuses)
        places.insert(int(design), sleep::place(preferences, own, design));
    return places;
}

void Reader::openSleepEditor(sleep::Design design)
{
    if (sleepEditor || !sleepGallery)
        return;
    SleepEditor::Setup setup;
    setup.arrange = design != sleep::Design::Custom;
    setup.custom = sleep::custom(preferences);
    setup.preset = sleepGallery->look(design);
    const auto status = sleep::statusDesign(setup.preset);
    setup.bookOnly = book && book->sleepPositions().contains(sleep::value(status));
    setup.sample = sleepPreviewSample();
    setup.screen = sleep::followsReading(preferences) || !page
            ? (page ? page->size() : size())
            : QSize(qMin(page->width(), page->height()), qMax(page->width(), page->height()));
    setup.places = sleepPlaces();
    // Holding Custom before it was ever saved starts from the chosen design,
    // when that design is a backdrop with one status.
    if (!setup.arrange && !preferences.contains(sleep::customKey)) {
        sleep::Layer start;
        if (sleep::asLayer(sleep::design(preferences), sleep::savedTile(preferences), start))
            setup.custom.sleep = setup.custom.off = start;
    }
    auto *editor = new SleepEditor(this, setup);
    sleepEditor = editor;
    editor->setGeometry(sleepGallery->geometry());
    editor->setImages(sleepCover, page ? page->snapshot(true) : QImage(), sleepOverlay);
    if (sleepCover.isNull())
        loadGalleryCover();
    editor->setWallpapers(sleepWallpaperNames);
    const QPointer<SleepEditor> guard(editor);
    const auto requestWallpapers = [this, guard](const QStringList &wanted, QSize bound) {
        if (!session || services.sleepWallpaperDirectories.isEmpty())
            return;
        session->sleepWallpapers(
                services.sleepWallpaperDirectories, wanted, bound,
                [this, guard, wanted, bound](QStringList names, QVector<QImage> images) {
                    sleepWallpaperNames = names;
                    if (!guard)
                        return;
                    guard->setWallpapers(names);
                    if (!wanted.isEmpty())
                        guard->setWallpaper(wanted.first(), bound, images.value(0));
                });
    };
    editor->wallpaperWanted = [requestWallpapers](const QString &name, QSize bound) {
        requestWallpapers({ name }, bound);
    };
    // List the choices before the first paint. Only a visible wallpaper needs decoding.
    if (!setup.arrange)
        requestWallpapers({}, {});
    editor->help = [this] { showSleepWallpaperHelp(); };
    editor->done = [this, guard] {
        if (!guard)
            return;
        auto next = preferences;
        auto global = next.value(sleep::placesKey).toObject();
        auto own = book ? book->sleepPositions() : QJsonObject();
        const auto ownBefore = own;
        const auto keep = [&](sleep::Design design, sleep::Place place, bool forBook) {
            if (forBook) {
                // A book's own place is kept even when it is the default, so
                // it still wins over a moved global one.
                own[sleep::value(design)] = sleep::value(
                        place == sleep::Place::Default ? sleep::defaultPlace(design) : place);
            } else {
                sleep::setPlace(global, design, place);
                own.remove(sleep::value(design));
            }
        };
        const auto moved = guard->moved();
        for (auto it = moved.constBegin(); it != moved.constEnd(); ++it) {
            const auto design = sleep::Design(it.key());
            const bool forBook =
                    guard->arranging() ? guard->bookOnly() : own.contains(sleep::value(design));
            keep(design, it.value(), forBook && book);
        }
        // Ticking "Only for this book" without moving keeps the current place
        // for this book; clearing it returns the book to the global place.
        if (guard->arranging() && book && !moved.contains(int(guard->arranged()))) {
            const auto design = guard->arranged();
            const auto key = sleep::value(design);
            if (!guard->bookOnly())
                own.remove(key);
            else if (!own.contains(key))
                keep(design, sleepPlaces().value(int(design)), true);
        }
        if (global.isEmpty())
            next.remove(sleep::placesKey);
        else
            next[sleep::placesKey] = global;
        const bool custom = !guard->arranging();
        if (custom) {
            sleep::setCustom(next, guard->custom());
            sleep::setDesign(next, sleep::Design::Custom);
            next.remove("kobalt_sleep_screen");
        }
        saveSleepPreferences(next, [this, guard, own, ownBefore, custom](bool saved) {
            if (!saved)
                return;
            if (book && own != ownBefore) {
                book->setSleepPositions(own);
                QJsonObject snapshot;
                book->checkpoint(&snapshot, true);
            }
            if (sleepEditor == guard)
                closeSleepEditor();
            if (custom) {
                // The gallery stays open and may open the editor again, so keep a
                // cover that is already decoded; closing the gallery releases it.
                if (sleepCover.isNull())
                    prepareSleepCover();
                if (sleep::uses(preferences, sleep::Mode::Overlay))
                    prepareSleepOverlay();
                prepareSleepWallpapers();
                // Back to the list of lock screens, with Custom chosen.
                if (sleepGallery) {
                    sleepGallery->setCustom(sleep::custom(preferences));
                    sleepGallery->setCurrent(sleep::Design::Custom);
                }
            }
            if (sleepGallery)
                sleepGallery->setPlaces(sleepPlaces());
        });
    };
    for (auto *widget : editor->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(editor);
    editor->show();
    editor->raise();
}

void Reader::closeSleepEditor()
{
    if (!sleepEditor)
        return;
    sleepEditor->hide();
    sleepEditor->deleteLater();
    sleepEditor.clear();
}

void Reader::updateSleepWallpaperOption()
{
    if (sleepGallery)
        sleepGallery->setWallpapers(sleepWallpaper, sleepOffWallpaper);
    if (sleepEditor)
        sleepEditor->setWallpapers(sleepWallpaperNames);
}

void Reader::loadGalleryCover()
{
    if (!session)
        return;
    const auto request = ++sleepCoverRequest;
    QPointer<Reader> guard(this);
    session->coverImage(
            { 2048, 2048 },
            [guard, request](QImage image, QFileInfo source) {
                if (!guard || guard->sleepCoverRequest != request)
                    return;
                // Keep it for sleep too when the cover designs are chosen.
                guard->sleepCover = image;
                guard->sleepCoverSource = std::move(source);
                if (guard->sleepGallery)
                    guard->sleepGallery->setCover(image);
                // An editor opened before the cover arrived shows it now.
                if (guard->sleepEditor)
                    guard->sleepEditor->setCover(image);
            },
            services.coverImagePath);
}

// A hidden option: long-pressing Night in the gallery sets its greeting.
void Reader::editSleepGreeting()
{
    if (greetingEditor || !services.keyboard)
        return;
    auto *dialog = new ui::ChoiceModal(
            this, { strings::get("sleep.greeting_hint"), strings::get("sleep.greeting_title"), {} },
            { strings::get("common.cancel"), "rrSleepGreetingCancel" },
            { strings::get("common.ok"), "rrSleepGreetingSave" });
    greetingEditor = dialog;
    dialog->setObjectName("rrSleepGreeting");
    dialog->setDimmed(true);
    dialog->setGeometry(rect());
    auto *card = dialog->card();
    auto *editor = new QLineEdit(card);
    ui::prepareNativeEditor(editor);
    editor->setObjectName("rrSleepGreetingInput");
    dialog->setFocusProxy(editor);
    editor->setMaxLength(sleep::greetingLength);
    editor->setText(preferences.value(sleep::greetingKey).toString());
    editor->setPlaceholderText(strings::get("sleep.goodnight"));
    editor->setAccessibleName(strings::get("sleep.greeting_title"));
    editor->setInputMethodHints(Qt::ImhNoPredictiveText);
    ui::readerStyle(
            editor,
            QString("QLineEdit{border:%1px solid palette(mid);border-radius:0;padding:0;background:palette(window);color:palette(window-text);")
                            .arg(int(metrics::control::border))
                    + ui::styles::font(metrics::chrome::labelText) + "}");
    editor->setTextMargins(metrics::notice::cardMargins.left(), 0,
                           metrics::notice::cardMargins.right(), 0);
    editor->setMinimumHeight(metrics::chrome::actionHeight);
    auto *inputRow = new QWidget(card);
    auto *inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(metrics::notice::cardMargins.left(),
                                    metrics::notice::cardMargins.top(),
                                    metrics::notice::cardMargins.right(), metrics::notice::cardGap);
    inputLayout->addWidget(editor);
    card->rows()->insertWidget(1, inputRow);
    const auto close = [this] {
        if (!greetingEditor)
            return;
        if (greetingKeyboard) {
            greetingKeyboard->hide();
            greetingKeyboard->submitted = {};
        }
        greetingEditor->hide();
        greetingKeyboard.reset();
        greetingEditor->deleteLater();
        greetingEditor.clear();
    };
    const auto submit = [this, editor, close] {
        auto next = preferences;
        const auto text = editor->text().simplified();
        // An empty greeting brings back "Goodnight".
        if (text.isEmpty())
            next.remove(sleep::greetingKey);
        else
            next[sleep::greetingKey] = text;
        // Native submission runs inside the controller; close after it returns.
        greetingEditor->hide();
        qt::later(0, this, close);
        saveSleepPreferences(next, [this](bool saved) {
            if (saved && sleepGallery)
                sleepGallery->setSample(sleepPreviewSample());
        });
    };
    connect(dialog->secondaryButton(), &QPushButton::clicked, this, close);
    connect(dialog->primaryButton(), &QPushButton::clicked, this, submit);
    connect(editor, &QLineEdit::returnPressed, this, submit);
    dialog->dismissed = close;
    greetingKeyboard = services.keyboard(dialog, editor);
    greetingKeyboard->setSubmitText(strings::get("common.ok"));
    greetingKeyboard->submitted = submit;
    dialog->show();
    dialog->raise();
    editor->setFocus();
    greetingKeyboard->show();
    // Keep the card above the keyboard.
    const auto bounds = window()->property("reader_keyboard_bounds").toRect();
    if (!bounds.isEmpty()) {
        const int bottom = dialog->mapFrom(window(), bounds.topLeft()).y();
        if (card->height() > bottom && dialog->width() > dialog->height())
            card->fitToWidth(dialog->width() - 2 * int(metrics::notice::sideClearance));
        // Keep the input and actions above the keyboard on short landscapes.
        // The placeholder still shows the default when the hint does not fit.
        if (card->height() > bottom && dialog->descriptionLabel()) {
            dialog->descriptionLabel()->hide();
            inputLayout->setContentsMargins(metrics::notice::cardMargins.left(), 0,
                                            metrics::notice::cardMargins.right(), 0);
            card->fitToWidth(card->width());
        }
        const QRect area(0, 0, dialog->width(), qMax(1, bottom));
        dialog->setCardArea(area);
        // Hiding the hint invalidates nested layout constraints on the next turn.
        qt::later(0, dialog, [dialog, area] {
            dialog->card()->fitToWidth(dialog->card()->width());
            dialog->setCardArea(area);
        });
    }
    for (auto *widget : dialog->findChildren<QWidget *>())
        textRefresh(widget);
}

void Reader::showSleepOverlayHelp()
{
    if (sleepPopup)
        return;
    // The card only explains, so it closes with one button like other notices.
    // A tap on the card itself keeps it open.
    auto *notice = new ui::NoticeModal(this,
                                       { strings::get("settings.sleep_overlay_description"),
                                         SleepGallery::label(sleep::Design::Overlay),
                                         {},
                                         "rrSleepOverlayHelpTitle" },
                                       { strings::get("common.ok"), "rrSleepOverlayHelpClose" });
    sleepPopup = notice;
    notice->setObjectName("rrSleepOverlayHelpPopup");
    notice->descriptionLabel()->setObjectName("rrSleepOverlayHelpHint");
    notice->setGeometry(rect());
    notice->dismissed = [this] { closePopup(sleepPopup); };
    connect(notice->primaryButton(), &QPushButton::clicked, this,
            [this] { closePopup(sleepPopup); });
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(notice);
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
}

void Reader::showSleepWallpaperHelp()
{
    if (sleepPopup)
        return;
    // The same explaining card as the PNG help, above the Custom editor.
    auto *notice = new ui::NoticeModal(this,
                                       { strings::get("sleep.wallpaper_help"),
                                         strings::get("sleep.wallpaper_help_title"),
                                         {},
                                         "rrSleepWallpaperHelpTitle" },
                                       { strings::get("common.ok"), "rrSleepWallpaperHelpClose" });
    sleepPopup = notice;
    notice->setObjectName("rrSleepWallpaperHelpPopup");
    notice->setGeometry(rect());
    notice->dismissed = [this] { closePopup(sleepPopup); };
    connect(notice->primaryButton(), &QPushButton::clicked, this,
            [this] { closePopup(sleepPopup); });
    for (auto *widget : notice->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(notice);
    notice->show();
    notice->raise();
    notice->primaryButton()->setFocus();
}

void Reader::updateSleepOverlayOption()
{
    if (!sleepGallery)
        return;
    sleepGallery->setEnabled(sleep::Design::Overlay, !sleepOverlay.isNull());
    if (!sleepOverlay.isNull())
        sleepGallery->setOverlay(sleepOverlay);
}

void Reader::openSessionTimeout()
{
    QVector<SleepOption> options;
    int current = 0;
    for (const auto minutes : sessionTimeout::choices) {
        if (minutes == sessionTimeout::minutes(preferences))
            current = options.size();
        options.append({ QString::number(minutes), sessionTimeoutLabel(minutes),
                         minutes == 0 ? strings::get("settings.session_timeout_never_description")
                                      : QString() });
    }
    openSleepChoice(
            "rrSessionTimeout", "rrSessionTimeout_", strings::get("settings.session_timeout"),
            options, current,
            [this](int index) {
                auto next = preferences;
                next[sessionTimeout::preferenceKey] = sessionTimeout::choices[index];
                saveSleepPreferences(next);
            },
            strings::get("settings.session_timeout_description"));
}

void Reader::openSleepChoice(const QString &name, const QString &optionPrefix,
                             const QString &heading, const QVector<SleepOption> &options,
                             int current, std::function<void(int)> chosen,
                             const QString &description)
{
    if (sleepPopup) {
        return;
    }
    const auto sheet =
            ui::makeChoiceSheet(this, name, heading, description, name + "Title", name + "Hint");
    sleepPopup = sheet.overlay;
    sleepPopup->dismissed = [this] { closeSleepScreen(); };
    auto *choices = sheet.list;
    choices->setUniformRows(false);
    for (const auto &option : options)
        choices->add(optionPrefix + option.value, option.label, option.explanation)
                ->setEnabled(option.enabled);
    choices->setCurrent(current);
    choices->chosen = [this, chosen = std::move(chosen)](int index) {
        chosen(index);
        closeSleepScreen();
    };
    ui::fitChoiceSheet(sleepPopup, choices);
    for (auto *widget : sleepPopup->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
    textRefresh(sleepPopup);
    sleepPopup->show();
    sleepPopup->raise();
    sleepPopup->setFocus();
}

namespace {
QString barButtonsLabel(pageNavigation::BarButtons mode)
{
    return strings::get(mode == pageNavigation::BarButtons::Chapters
                                ? "settings.navigation_bar_chapters"
                                : "settings.navigation_bar_pages");
}

QString navigationLabel(pageNavigation::Mode mode)
{
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

QString tapZoneLabel(tapZones::Layout layout)
{
    switch (layout) {
    case tapZones::Layout::LeftRight:
        return strings::get("settings.tap_left_right");
    case tapZones::Layout::Reversed:
        return strings::get("settings.tap_reversed");
    case tapZones::Layout::TopBottom:
        return strings::get("settings.tap_top_bottom");
    case tapZones::Layout::OneHanded:
        return strings::get("settings.tap_one_handed");
    }
    return {};
}

QString navigationExplanation(pageNavigation::Mode mode)
{
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
} // namespace

void Reader::showTapZones()
{
    if (tapZonesButton)
        tapZonesButton->setCaption(tapZoneLabel(pageNavigation::tapLayout(preferences)).toUpper());
}

void Reader::openTapZones()
{
    if (tapZonesPopup)
        return;
    namespace chrome = metrics::chrome;
    tapZonesPopup = new ui::ModalOverlay(this);
    tapZonesPopup->setObjectName("rrTapZonesPopup");
    tapZonesPopup->setDimmed(true);
    tapZonesPopup->setGeometry(rect());
    tapZonesPopup->dismissed = [this] { closePopup(tapZonesPopup); };
    auto *card = tapZonesPopup->card();
    card->heading(strings::get("settings.tap_zones"), strings::get("settings.tap_zones_hint"),
                  "rrTapZonesTitle", "rrTapZonesHint");
    auto *grid = new QGridLayout;
    grid->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    grid->setSpacing(metrics::control::itemGap);
    card->rows()->addLayout(grid);
    auto *group = new QButtonGroup(card);
    const bool rtl = session->state().value("page_progression_rtl").toBool();
    int index = 0;
    for (const auto layout : tapZones::layouts) {
        auto *button =
                new ui::TapZoneButton(layout, rtl && layout != tapZones::Layout::Reversed, card);
        button->setObjectName("rrTapZones_" + pageNavigation::tapValue(layout));
        button->setText(tapZoneLabel(layout));
        button->setAccessibleName(button->text());
        button->setChecked(layout == pageNavigation::tapLayout(preferences));
        group->addButton(button);
        grid->addWidget(button, index / 2, index % 2);
        ++index;
        connect(button, &QPushButton::clicked, this, [this, layout] {
            auto next = preferences;
            next[pageNavigation::tapPreferenceKey] = pageNavigation::tapValue(layout);
            if (layout == tapZones::Layout::OneHanded)
                next[pageNavigation::preferenceKey] =
                        pageNavigation::value(pageNavigation::Mode::TapsAndSwipes);
            savePreferences(next, [this, popup = tapZonesPopup](bool saved) {
                if (saved) {
                    showTapZones();
                    showPageNavigation();
                }
                if (popup && popup == tapZonesPopup)
                    closePopup(tapZonesPopup);
            });
        });
    }
    auto *note =
            card->message(strings::get("settings.tap_one_handed_hint"), "rrTapZonesOneHandedHint");
    note->setContentsMargins(chrome::inset, metrics::notice::cardGap, chrome::inset,
                             metrics::notice::cardMargins.bottom());
    card->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    tapZonesPopup->centerCard();
    for (auto *widget : tapZonesPopup->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(tapZonesPopup);
    tapZonesPopup->show();
    tapZonesPopup->raise();
    tapZonesPopup->setFocus();
}

void Reader::showPageNavigation()
{
    if (!pageNavigationButton)
        return;
    pageNavigationButton->setCaption(
            navigationLabel(pageNavigation::mode(preferences, services.pageButtons())).toUpper());
}

void Reader::openPageNavigation()
{
    QVector<ChoiceOption> options;
    QVector<pageNavigation::Mode> modes;
    int current = -1;
    for (const auto mode : pageNavigation::modes) {
        if (mode == pageNavigation::Mode::ButtonsOnly && !services.pageButtons())
            continue;
        const bool needsSwipes =
                pageNavigation::tapLayout(preferences) == tapZones::Layout::OneHanded
                && !pageNavigation::swipes(mode);
        if (mode == pageNavigation::mode(preferences, services.pageButtons()))
            current = options.size();
        modes.append(mode);
        options.append({ "rrNavigation_" + pageNavigation::value(mode), navigationLabel(mode),
                         needsSwipes ? strings::get("settings.navigation_needs_swipes")
                                     : navigationExplanation(mode),
                         !needsSwipes });
    }
    if (pageNavigationPopup)
        return;
    const auto sheet =
            ui::makeChoiceSheet(this, "rrPageNavigation", strings::get("settings.page_navigation"),
                                strings::get("settings.navigation_menu_hint"),
                                "rrPageNavigationTitle", "rrPageNavigationHint");
    // Leave room for the section strips on short landscape screens. Text still
    // determines each row's height when it needs more than this compact minimum.
    if (width() > height())
        sheet.list->setMinimumRowHeight(metrics::settings::optionHeight);
    pageNavigationPopup = sheet.overlay;
    pageNavigationPopup->dismissed = [this] { closePopup(pageNavigationPopup); };
    for (const auto &option : options)
        sheet.list->add(option.name, option.label, option.description)->setEnabled(option.enabled);
    sheet.list->setCurrent(current);
    sheet.list->chosen = [this, modes](int index) {
        auto next = preferences;
        next[pageNavigation::preferenceKey] = pageNavigation::value(modes[index]);
        pageNavigationPopup->setEnabled(false);
        savePreferences(next, [this, popup = pageNavigationPopup](bool saved) {
            if (saved)
                showPageNavigation();
            if (popup && popup == pageNavigationPopup)
                closePopup(pageNavigationPopup);
        });
    };
    auto *card = pageNavigationPopup->card();
    // Each header pages with its controls on short screens.
    const auto section = [card](const QString &caption, const QString &name, QWidget *control) {
        card->rows()->removeWidget(control);
        auto *group = new QWidget(card);
        auto *rows = new QVBoxLayout(group);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        auto *header = new ui::ListHeader(group);
        header->setObjectName(name);
        header->setShaded(true);
        header->setCaptions(caption.toUpper());
        header->setFixedHeight(header->sizeHint().height());
        rows->addWidget(header);
        rows->addWidget(control);
        card->rows()->addWidget(group);
    };
    section(strings::get("settings.navigation_choice"), "rrNavigationChoiceSection", sheet.list);
    auto *reverse =
            new ui::DescribedOption(strings::get("settings.reverse_swipe_direction"),
                                    strings::get("settings.reverse_swipe_direction_hint"), card);
    reverse->setObjectName("rrReverseSwipeDirection");
    auto look = sheet.list->options().first()->presentation();
    look.indicator = ui::DescribedOption::Indicator::Square;
    look.dividers = {};
    reverse->setPresentation(look);
    reverse->setChecked(preferences.value(pageNavigation::reverseSwipesKey).toBool());
    // Direction is independent of the navigation mode, including when swipes are disabled.
    connect(reverse, &QCheckBox::clicked, this, [this, reverse](bool checked) {
        auto next = preferences;
        next[pageNavigation::reverseSwipesKey] = checked;
        pageNavigationPopup->setEnabled(false);
        savePreferences(next,
                        [popup = pageNavigationPopup, guard = QPointer<QCheckBox>(reverse),
                         checked](bool saved) {
                            if (popup)
                                popup->setEnabled(true);
                            if (!saved && guard)
                                guard->setChecked(!checked);
                        });
    });
    section(strings::get("settings.swipe_direction"), "rrSwipeDirectionSection", reverse);
    const int width = qMin<int>(metrics::notice::cardWidth,
                                this->width() - 2 * metrics::notice::sideClearance);
    card->fitToWidth(width);
    pageNavigationPopup->centerCard();
    for (auto *widget : pageNavigationPopup->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(pageNavigationPopup);
    pageNavigationPopup->show();
    pageNavigationPopup->raise();
    pageNavigationPopup->setFocus();
}

void Reader::showChapterGroups()
{
    if (!chapterGroupsButton)
        return;
    chapterGroupsButton->setCaption(
            strings::get(preferences.value("chapter_groups").toString() == "off"
                                 ? "settings.chapter_groups_off"
                                 : preferences.value("chapter_groups").toString() == "all"
                                 ? "settings.chapter_groups_all"
                                 : "settings.chapter_groups_current")
                    .toUpper());
}

void Reader::openChapterGroups()
{
    const QStringList modes{ "current", "all", "off" };
    QVector<ChoiceOption> options;
    for (const auto &mode : modes)
        options.append({ "rrChapterGroups_" + mode,
                         strings::get(mode == "off"           ? "settings.chapter_groups_off"
                                              : mode == "all" ? "settings.chapter_groups_all"
                                                              : "settings.chapter_groups_current"),
                         strings::get(mode == "off" ? "settings.chapter_groups_off_description"
                                              : mode == "all"
                                              ? "settings.chapter_groups_all_description"
                                              : "settings.chapter_groups_current_description") });
    openChoicePopup(chapterGroupsPopup, "rrChapterGroups", strings::get("settings.chapter_groups"),
                    strings::get("settings.chapter_groups_hint"), options,
                    modes.indexOf(preferences.value("chapter_groups").toString("current")),
                    [this, modes](int index) {
                        auto next = preferences;
                        next["chapter_groups"] = modes[index];
                        savePreferences(next, [this, popup = chapterGroupsPopup](bool saved) {
                            if (saved)
                                showChapterGroups();
                            if (popup && popup == chapterGroupsPopup)
                                closePopup(chapterGroupsPopup);
                        });
                    });
}

void Reader::showNavigationBarButtons()
{
    const auto mode = pageNavigation::barButtons(preferences);
    if (navigationBarButton)
        navigationBarButton->setCaption(barButtonsLabel(mode).toUpper());
    pageScrubber->setChapterJumps(mode == pageNavigation::BarButtons::Chapters);
}

void Reader::openNavigationBarButtons()
{
    QVector<ChoiceOption> options;
    int current = -1;
    for (const auto mode : pageNavigation::barButtonsModes) {
        if (mode == pageNavigation::barButtons(preferences))
            current = options.size();
        options.append(
                { "rrNavigationBar_" + pageNavigation::value(mode), barButtonsLabel(mode),
                  strings::get(
                          mode == pageNavigation::BarButtons::Chapters
                                  ? strings::key("settings.navigation_bar_chapters_description")
                                  : strings::key("settings.navigation_bar_pages_description")) });
    }
    openChoicePopup(
            navigationBarPopup, "rrNavigationBar", strings::get("settings.navigation_bar_buttons"),
            strings::get("settings.navigation_bar_hint"), options, current, [this](int index) {
                auto next = preferences;
                next[pageNavigation::barButtonsKey] =
                        pageNavigation::value(pageNavigation::barButtonsModes[index]);
                savePreferences(next, [this, popup = navigationBarPopup](bool saved) {
                    if (saved)
                        showNavigationBarButtons();
                    if (popup && popup == navigationBarPopup)
                        closePopup(navigationBarPopup);
                });
            });
}

namespace {
QString menuViewLabel(pageNavigation::MenuView mode)
{
    switch (mode) {
    case pageNavigation::MenuView::Seek:
        return strings::get("settings.menu_view_seek");
    case pageNavigation::MenuView::Stats:
        return strings::get("settings.menu_view_stats");
    default:
        return strings::get("settings.menu_view_none");
    }
}
} // namespace

namespace {
QString tappableLinksLabel(const QString &mode)
{
    return strings::get(mode == "footnotes"     ? "settings.tappable_links_footnotes"
                                : mode == "all" ? "settings.tappable_links_all"
                                                : "settings.tappable_links_off");
}

QString tappableLinksDescription(const QString &mode)
{
    return strings::get(mode == "footnotes"     ? "settings.tappable_links_footnotes_description"
                                : mode == "all" ? "settings.tappable_links_all_description"
                                                : "settings.tappable_links_off_description");
}
} // namespace

void Reader::showTappableLinks()
{
    const auto mode = preferences.value("tappable_links").toString("footnotes");
    if (tappableLinksButton)
        tappableLinksButton->setCaption(tappableLinksLabel(mode).toUpper());
}

void Reader::openTappableLinks()
{
    const QStringList modes{ "footnotes", "all", "off" };
    QVector<ChoiceOption> options;
    for (const auto &mode : modes)
        options.append({ "rrTappableLinks_" + mode, tappableLinksLabel(mode),
                         tappableLinksDescription(mode) });
    openChoicePopup(tappableLinksPopup, "rrTappableLinks", strings::get("settings.tappable_links"),
                    strings::get("settings.tappable_links_description"), options,
                    modes.indexOf(preferences.value("tappable_links").toString("footnotes")),
                    [this, modes](int index) {
                        auto next = preferences;
                        next["tappable_links"] = modes[index];
                        savePreferences(next, [this, popup = tappableLinksPopup](bool saved) {
                            if (saved) {
                                showTappableLinks();
                                syncLinkDiscovery();
                            }
                            if (popup && popup == tappableLinksPopup)
                                closePopup(tappableLinksPopup);
                        });
                    });
}

namespace {
QString annotationInteractionLabel(const QString &mode)
{
    return strings::get(mode == "quick" ? "settings.annotation_interaction_quick"
                                        : "settings.annotation_interaction_toggle");
}
} // namespace

void Reader::showAnnotationInteraction()
{
    const auto mode = preferences.value("annotation_interaction").toString("toggle");
    if (annotationInteractionButton)
        annotationInteractionButton->setCaption(annotationInteractionLabel(mode).toUpper());
}

void Reader::openAnnotationInteraction()
{
    const QStringList modes{ "toggle", "quick" };
    QVector<ChoiceOption> options;
    for (const auto &mode : modes)
        options.append(
                { "rrAnnotationInteraction_" + mode, annotationInteractionLabel(mode),
                  strings::get(mode == "quick"
                                       ? "settings.annotation_interaction_quick_description"
                                       : "settings.annotation_interaction_toggle_description") });
    openChoicePopup(annotationInteractionPopup, "rrAnnotationInteraction",
                    strings::get("settings.annotation_interaction"),
                    strings::get("settings.annotation_interaction_description"), options,
                    modes.indexOf(preferences.value("annotation_interaction").toString("toggle")),
                    [this, modes](int index) {
                        auto next = preferences;
                        next["annotation_interaction"] = modes[index];
                        savePreferences(next, [this, popup = annotationInteractionPopup](bool saved) {
                            if (saved) {
                                showAnnotationInteraction();
                                // A mode that the toggle opened ends with the toggle.
                                if (quickAnnotations() && annotationMode)
                                    leaveAnnotations(false);
                                syncReadingControls();
                            }
                            if (popup && popup == annotationInteractionPopup)
                                closePopup(annotationInteractionPopup);
                        });
                    });
}

void Reader::showMenuView()
{
    const auto mode = pageNavigation::menuView(preferences);
    if (menuViewButton)
        menuViewButton->setCaption(menuViewLabel(mode).toUpper());
}

void Reader::openMenuView()
{
    QVector<ChoiceOption> options;
    QVector<pageNavigation::MenuView> modes;
    int current = -1;
    const auto featurePreferences = static_cast<SettingsPanel *>(general)->featurePreferences;
    for (const auto mode : pageNavigation::menuViews) {
        if ((mode == pageNavigation::MenuView::Seek
             && !features::enabled(featurePreferences, features::Navigate))
            || (mode == pageNavigation::MenuView::Stats
                && !features::enabled(featurePreferences, features::Stats)))
            continue;
        const auto description = mode == pageNavigation::MenuView::None
                ? strings::key("settings.menu_view_none_description")
                : mode == pageNavigation::MenuView::Seek
                ? strings::key("settings.menu_view_seek_description")
                : strings::key("settings.menu_view_stats_description");
        if (mode == pageNavigation::menuView(preferences))
            current = options.size();
        modes.append(mode);
        options.append({ "rrMenuView_" + pageNavigation::value(mode), menuViewLabel(mode),
                         strings::get(description) });
    }
    openChoicePopup(menuViewPopup, "rrMenuView", strings::get("settings.default_menu_view"),
                    strings::get("settings.default_menu_view_description"), options, current,
                    [this, modes](int index) {
                        auto next = preferences;
                        next[pageNavigation::menuViewKey] = pageNavigation::value(modes[index]);
                        next.remove("pin_stats");
                        next.remove(pageNavigation::thumbnailsKey);
                        savePreferences(next, [this, popup = menuViewPopup](bool saved) {
                            if (saved)
                                showMenuView();
                            if (popup && popup == menuViewPopup)
                                closePopup(menuViewPopup);
                        });
                    });
}

} // namespace kobalt::reader
