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
#include "support/strings.h"
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
    choice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    customReplaceDialog->centerCard();
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
                if (!writePreferences(next)) {
                    message->setText(strings::get("reader.could_not_save_reader_settings"));
                    card->fitToWidth(card->width());
                    dialog->centerCard();
                    return;
                }
                preferences = next;
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
    for (auto *button : card->findChildren<QPushButton *>()) {
        button->setFocusPolicy(Qt::StrongFocus);
    }
    card->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    dialog->centerCard();
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
    notice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    notice->centerCard();
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
    return bool(debugDialog) || bool(finishDialog) || bool(kosyncDialog)
            || bool(annotationErrorDialog) || bool(formattingWarningDialog)
            || bool(settingsWriteErrorDialog) || book->deviceTransition() || services.suspended()
            || !isVisible() || session->busy() || sheetOpen() || readingToolOpen()
            || appearanceEdit.applying() || imageViewer->isVisible() || annotationMode
            || bookmarkList->isVisible() || topControls->isVisible();
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
    card->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    syncDialog->centerCard();
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
        updateOrientationBlock();
        save(false, true);
        refresh();
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [choose] { choose(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [choose] { choose(true); });
    card->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    finishDialog->centerCard();
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
    auto *card = choice->card();
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
    card->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    removeDialog->centerCard();
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
    bookmarkList->setGeometry(0, top, width(),
                              (navigation ? navigation->contentBottom(size()) : height()) - top);
    // Bookmarks first, unless the book has annotations and no bookmarks. With
    // only My Words shown, the list opens on its words.
    if (!bookmarksEnabled && !annotationsEnabled) {
        showWords();
        refresh();
        return;
    }
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
    notice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    notice->centerCard();
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
    notice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    notice->centerCard();
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
    const auto contents = session->chapters();
    rows.reserve(list.size());
    for (int i = 0; i < list.size(); ++i) {
        kobalt::BookmarkRow row{ list[i].position, list[i].chapter,        list[i].excerpt,
                                 list[i].created,  bookmarks->bookPage(i), false };
        row.current = pages.value(i, -1) >= 0 && pages.value(i, -1) == session->page();
        // Name the chapter the page is in now; the stored title covers a missing contents list.
        for (const auto &link : contents)
            if (link.page >= 0 && row.page >= 0 && link.page <= row.page)
                row.chapter = link.title;
        row.excerpt = kobalt::excerptAfterChapter(row.excerpt, row.chapter);
        row.pagePending = session->state().contains("book_fraction")
                && session->state().value("rough_pages").toInt() <= 0;
        row.pinName = list[i].name;
        row.pinIcon = list[i].icon;
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const kobalt::BookmarkRow &a, const kobalt::BookmarkRow &b) {
                         return (a.page < 0 ? INT_MAX : a.page) < (b.page < 0 ? INT_MAX : b.page);
                     });
    return rows;
}

void Reader::syncPinnedBookmarks()
{
    if (!pinnedBookmarks)
        return;
    QVector<Bookmark> pins;
    for (const auto &item : bookmarks->bookmarks())
        if (item.pinned() && pins.size() < BookmarkStore::maxPins)
            pins.append(item);
    std::stable_sort(pins.begin(), pins.end(), [this](const Bookmark &a, const Bookmark &b) {
        const int aPage = bookmarks->pageOf(a.position), bPage = bookmarks->pageOf(b.position);
        return (aPage < 0 ? INT_MAX : aPage) < (bPage < 0 ? INT_MAX : bPage);
    });
    const bool visible = features::enabled(preferences, features::Bookmarks) && !pins.isEmpty()
            && topControls->isVisible() && !session->busy() && !lifecycleHidden && !closing
            && !services.suspended() && !annotationMode && !seekOpen && !appearanceEdit.panelOpen()
            && !general->isVisible() && !menu->isVisible() && !choices->isVisible()
            && !lightPopup->isVisible() && !bookmarkList->isVisible()
            && !acknowledgments->isVisible() && !searchPanel->isVisible()
            && !dictionaryPanel->isVisible() && !linksActive() && !lookupPending && !syncDialog
            && !kosyncDialog && !finishDialog && !(fieldChooser && fieldChooser->isVisible())
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
        const auto &pin = pins[i];
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
        button->setEnabled(bookmarks->pageOf(pin.position) >= 0);
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
    refreshButton->setCaption(refreshLabel(screenRefreshMode).toUpper());
}

// Writes the preferences. A failed write keeps the old ones and shows the
// error line in Settings; a write that succeeds clears it.
bool Reader::savePreferences(const QJsonObject &next)
{
    if (!writePreferences(next)) {
        settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
        settingsError->show();
        return false;
    }
    preferences = next;
    settingsError->clear();
    settingsError->hide();
    return true;
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
    sessionTimeoutButton->setCaption(
            sessionTimeoutLabel(sessionTimeout::minutes(preferences)).toUpper());
    sleepScreenButton->setCaption(
            sleepScreenButton->isEnabled()
                    ? SleepGallery::label(sleep::design(preferences)).toUpper()
                    : strings::get("settings.unsupported_caption"));
}

void Reader::closeSleepScreen()
{
    if (sleepGallery) {
        sleepGallery->hide();
        sleepGallery->deleteLater();
        sleepGallery.clear();
    }
    if (!sleepPopup) {
        if (sleep::mode(preferences) != sleep::Mode::Overlay)
            sleepOverlay = {};
        return;
    }
    closePopup(sleepPopup);
}

bool Reader::saveSleepPreferences(const QJsonObject &next)
{
    if (!savePreferences(next))
        return false;
    showSleepScreen();
    return true;
}

sleep::Snapshot Reader::sleepPreviewSample()
{
    sleep::Snapshot sample;
    const int percent =
            book && session ? (book->finishedHere() ? 100 : session->progressPercent()) : 0;
    sample.percent = percent;
    sample.progress = strings::get("sleep.percent_read").arg(percent);
    if (book) {
        sample.remaining = sleepTimeLeft(book->estimates().bookMilliseconds,
                                         remainingTime::approximate(preferences));
    }
    sample.dark = darkTheme;
    sample.dimPage = false;
    sample.tile = sleep::savedTile(preferences);
    if (session && book)
        sample.details = sleepDetails();
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
    sleepGallery->followOrientationChanged = [this](bool checked) {
        auto next = preferences;
        next[sleep::followOrientationKey] = checked;
        return saveSleepPreferences(next);
    };
    sleepGallery->chosen = [this](sleep::Design design) {
        auto next = preferences;
        sleep::setDesign(next, design);
        next.remove("kobalt_sleep_screen");
        if (saveSleepPreferences(next))
            prepareSleepCover();
        closeSleepScreen();
    };
    sleepGallery->unavailable = [this](sleep::Design design) {
        if (design == sleep::Design::Overlay)
            showSleepOverlayHelp();
    };
    sleepGallery->closed = [this] { closeSleepScreen(); };
    sleepGallery->customize = [this](sleep::Design design) {
        if (design == sleep::Design::Night)
            editSleepGreeting();
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
        if (saveSleepPreferences(next) && sleepGallery)
            sleepGallery->setSample(sleepPreviewSample());
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
    dialog->fitCard();
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
    notice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    notice->centerCard();
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
    namespace chrome = metrics::chrome;
    sleepPopup = new ui::ModalOverlay(this);
    sleepPopup->setObjectName(name + "Popup");
    sleepPopup->setDimmed(true);
    sleepPopup->setGeometry(rect());
    sleepPopup->dismissed = [this] { closeSleepScreen(); };
    auto *card = sleepPopup->card();
    card->heading(heading, description, name + "Title", name + "Hint");
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName(name + "Choices");
    choices->setUniformRows(false);
    choices->setMinimumRowHeight(chrome::listActionHeight);
    for (const auto &option : options)
        choices->add(optionPrefix + option.value, option.label, option.explanation)
                ->setEnabled(option.enabled);
    choices->setCurrent(current);
    choices->chosen = [this, chosen = std::move(chosen)](int index) {
        chosen(index);
        closeSleepScreen();
    };
    card->rows()->addWidget(choices);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    const int available = height() - 2 * metrics::notice::sideClearance;
    if (card->height() > available) {
        auto *pager = new ui::PagerBar(card, name);
        card->rows()->addWidget(pager);
        const int fixed = card->height() - choices->height();
        const int room = qMax(1, available - fixed - int(chrome::actionHeight));
        QVector<int> starts{ 0 };
        int used = 0;
        for (int row = 0; row < choices->options().size(); ++row) {
            const int rowHeight = choices->rowHeight(row, cardWidth - 4);
            if (used && used + rowHeight > room) {
                starts.append(row);
                used = 0;
            }
            used += rowHeight;
        }
        auto page = std::make_shared<int>(0);
        for (int index = 0; index < starts.size(); ++index)
            if (starts[index] <= current)
                *page = index;
        const auto showPage = [this, choices, pager, card, cardWidth, starts, room, page] {
            const int first = starts[*page];
            const int last =
                    *page + 1 < starts.size() ? starts[*page + 1] : int(choices->options().size());
            choices->showRows(first, last - first);
            choices->setFixedHeight(room);
            pager->setPage(*page, starts.size());
            card->fitToWidth(cardWidth);
            sleepPopup->centerCard();
        };
        connect(pager->previous, &QPushButton::clicked, this, [page, pager, showPage] {
            *page = pager->turnTarget(-1);
            showPage();
        });
        connect(pager->next, &QPushButton::clicked, this, [page, pager, showPage] {
            *page = pager->turnTarget(1);
            showPage();
        });
        showPage();
    }
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
            if (savePreferences(next)) {
                showTapZones();
                showPageNavigation();
            }
            closePopup(tapZonesPopup);
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
        if (savePreferences(next))
            showPageNavigation();
        closePopup(pageNavigationPopup);
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
        if (!savePreferences(next))
            reverse->setChecked(!checked);
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
                        if (savePreferences(next))
                            showChapterGroups();
                        closePopup(chapterGroupsPopup);
                    });
}

void Reader::showNavigationBarButtons()
{
    const auto mode = pageNavigation::barButtons(preferences);
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
                if (savePreferences(next))
                    showNavigationBarButtons();
                closePopup(navigationBarPopup);
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
                        if (savePreferences(next)) {
                            showTappableLinks();
                            syncLinkDiscovery();
                        }
                        closePopup(tappableLinksPopup);
                    });
}

void Reader::showMenuView()
{
    const auto mode = pageNavigation::menuView(preferences);
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
                        if (savePreferences(next))
                            showMenuView();
                        closePopup(menuViewPopup);
                    });
}

} // namespace kobalt::reader
