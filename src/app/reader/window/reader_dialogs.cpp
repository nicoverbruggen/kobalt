#include "ui/controls/buttons.h"
#include "ui/controls/options.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "assets/font_catalog.h"
#include "reader_p.h"
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
        {}
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
                    auto settings = preview.state();
                    settings["default_font_face"] =
                            face.isEmpty() ? QString(defaultFontFace) : face;
                    preview.apply(settings);
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
    return bool(debugDialog) || bool(finishDialog) || bool(kosyncDialog)
            || bool(annotationErrorDialog) || bool(formattingWarningDialog)
            || bool(settingsWriteErrorDialog) || book->deviceTransition() || services.suspended()
            || !isVisible() || lightPopup->isVisible() || session->busy()
            || chapterGraph->isVisible() || dictionaryPanel->isVisible() || searchPanel->isVisible()
            || linksActive() || lookupPending || appearanceEdit.panelOpen()
            || appearanceEdit.applying() || general->isVisible() || choices->isVisible()
            || acknowledgments->isVisible() || imageViewer->isVisible();
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

#ifndef KOBALT_READER_HARDWARE
void Reader::showProgressDebug()
{
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
    auto *message = card->message(strings::get(
            "reader.simulate_nickel_progress_these_controls_change_only_the_synthetic"));
    auto dismiss = [this, dialog] {
        dialog->hide();
        dialog->deleteLater();
        debugDialog.clear();
    };
    const bool fixture = book->identity() == "file:///mnt/onboard/progress-integration.kepub.epub";
    auto action = [this, dialog, card, rows, message, fixture,
                   dismiss](const char *name, const QString &label, const QString &kind) {
        auto *button = new kobalt::ui::Button(label, card);
        button->setObjectName(name);
        button->setEnabled(fixture && bool(services.debugProgressAction));
        rows->addWidget(button);
        connect(button, &QPushButton::clicked, dialog, [this, message, kind, dismiss] {
            const auto current = book->readProgress();
            QJsonObject command =
                    kobalt::qt::object({ { "book", book->identity() }, { "action", kind } });
            if (kind != "fail-next-save") {
                const bool duplicate = kind == "duplicate", stale = kind == "stale";
                const QString next = current.location.startsWith("OEBPS/c2.xhtml")
                        ? "OEBPS/c3.xhtml#kobo.9.1:9"
                        : "OEBPS/c2.xhtml#kobo.8.1:7";
                const qint64 stamp = duplicate ? current.modified
                        : stale
                        ? current.modified - 1000
                        : qMax(current.modified, QDateTime::currentMSecsSinceEpoch() / 1000 * 1000)
                                + 1000;
                command["action"] = QStringLiteral("persist");
                command["location"] = duplicate ? current.location : next;
                command["modified"] = QString::number(stamp);
                command["status"] = kind == "finished" ? 2 : duplicate ? current.status : 1;
            }
            const auto result = services.debugProgressAction ? services.debugProgressAction(command)
                                                             : QJsonObject();
            if (!result.value("ok").toBool()) {
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
    action("rrDebugNewPosition", strings::get("reader.new_synced_position"), "new");
    action("rrDebugDuplicate", strings::get("reader.duplicate_current_update"), "duplicate");
    action("rrDebugStale", strings::get("reader.older_synced_position"), "stale");
    action("rrDebugFinished", strings::get("reader.book_marked_finished"), "finished");
    action("rrDebugSaveFailure", strings::get("reader.fail_next_native_save"), "fail-next-save");
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
#endif

void Reader::showFinishChoice()
{
    if (finishDialog || !book->offerFinish())
        return;
    session->cancelPendingTurn();
    auto *choice = new ui::ChoiceModal(this, { {}, strings::get("finish.prompt"), {} },
                                       { strings::get("finish.still_reading"), "rrStillReading" },
                                       { strings::get("finish.mark_finished"), "rrMarkFinished" });
    finishDialog = choice;
    finishDialog->setObjectName("rrFinishBook");
    finishDialog->setGeometry(rect());
    auto *card = finishDialog->card();
    card->findChild<QLabel *>("rrModalTitle")->setObjectName("rrFinishBookPrompt");
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

void Reader::guardExit()
{
    exitGuarded = true;
    exitGuardPressTime = lastPressTime;
    const int generation = ++exitGuardGeneration;
    kobalt::qt::later(0, this, [this, generation] {
        kobalt::qt::later(metrics::contents::exitGuardMilliseconds, this, [this, generation] {
            if (generation == exitGuardGeneration)
                exitGuarded = false;
        });
    });
}

bool Reader::exitGuardElapsed() const
{
    // The timer above starts once the reader has handled the close. Source
    // timestamps tell a tap pressed later apart from one that only waited.
    // Without them, the timer alone decides. Subtraction permits clock wrap.
    if (!exitGuardPressTime || !lastPressTime)
        return false;
    const quint32 elapsed = lastPressTime - exitGuardPressTime;
    return elapsed < 0x80000000u && elapsed >= quint32(metrics::contents::exitGuardMilliseconds);
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
              {} },
            { strings::get("common.cancel"), "rrKeepBookmark" },
            { strings::get("bookmarks.remove"), "rrConfirmRemoveBookmark" });
    removeDialog = choice;
    removeDialog->setObjectName("rrRemoveBookmark");
    auto *card = choice->card();
    card->findChild<QLabel *>("rrModalTitle")->setObjectName("rrRemoveBookmarkPrompt");
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
    if (!highlights->highlights().isEmpty()) {
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
        kobalt::BookmarkRow row{ list[i].position, list[i].chapter,    list[i].excerpt,
                                 list[i].created,  pages.value(i, -1), false };
        row.current = row.page >= 0 && row.page == session->page();
        // Name the chapter the page is in now; the stored title covers a missing contents list.
        for (const auto &link : contents)
            if (link.page >= 0 && row.page >= 0 && link.page <= row.page)
                row.chapter = link.title;
        row.excerpt = kobalt::excerptAfterChapter(row.excerpt, row.chapter);
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
    const bool visible = !pins.isEmpty() && topControls->isVisible() && !session->busy()
            && !lifecycleHidden && !closing && !services.suspended() && !annotationMode && !seekOpen
            && !appearanceEdit.panelOpen() && !general->isVisible() && !menu->isVisible()
            && !choices->isVisible() && !chapterGraph->isVisible() && !lightPopup->isVisible()
            && !bookmarkList->isVisible() && !acknowledgments->isVisible()
            && !searchPanel->isVisible() && !dictionaryPanel->isVisible() && !linksActive()
            && !lookupPending && !syncDialog && !kosyncDialog && !finishDialog
            && !(fieldChooser && fieldChooser->isVisible())
            && !(headerFooterView && headerFooterView->isVisible());
    pinnedBookmarks->setVisible(visible);
    if (!visible)
        return;
    const int padding = ui::px(12), edgeGap = ui::px(20);
    const int top = topControls->geometry().bottom() + 1;
    const int bottom = navigation         ? navigation->navigationTop()
            : bottomControls->isVisible() ? bottomControls->y()
                                          : height();
    const int room = qMax(1, bottom - top);
    // A short screen, such as a phone in landscape, cannot stack every
    // shortcut at a usable size. Add columns instead of shrinking below
    // the buttons' minimum size, including with large interface text.
    int minimumSide = 0;
    for (auto *button : pinnedButtons) {
        button->ensurePolished();
        minimumSide = qMax(minimumSide, qMax(button->minimumWidth(), button->minimumHeight()));
    }
    const int cell = qMax<int>(metrics::chrome::actionHeight, minimumSide + 2 * padding);
    const int capacity = qBound(1, (room - 2 * padding) / cell, BookmarkStore::maxPins);
    const int used = (pins.size() + capacity - 1) / capacity;
    const int columnRows = (pins.size() + used - 1) / used;
    const int side = qMax(1, cell - 2 * padding);
    const int gap = ui::px(16);
    const int trayHeight = cell * columnRows + 2 * padding;
    const int trayWidth = cell * used + gap * (used - 1);
    pinnedBookmarks->setGeometry(width() - trayWidth - edgeGap, top + (room - trayHeight) / 2,
                                 trayWidth, trayHeight);
    for (int i = 0; i < pinnedButtons.size(); ++i) {
        auto *button = pinnedButtons[i];
        button->setVisible(i < pins.size());
        if (i >= pins.size())
            continue;
        const auto &pin = pins[i];
        const int column = i / columnRows;
        const int rows = qMin(columnRows, int(pins.size()) - column * columnRows);
        const int columnTop = (trayHeight - (cell * rows + 2 * padding)) / 2;
        button->setGeometry(padding + column * (cell + gap),
                            columnTop + padding + i % columnRows * cell + (cell - side) / 2, side,
                            side);
        button->setGlyph(ui::bookmarkIcon(pin.icon));
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

void Reader::closeScreenRefresh()
{
    if (!refreshPopup)
        return;
    refreshPopup->hide();
    refreshPopup->deleteLater();
    refreshPopup.clear();
}

// A titled pop-up with one row per mode. Picking a mode or tapping
// outside the card closes it.
void Reader::openScreenRefresh()
{
    if (refreshPopup)
        return;
    namespace chrome = metrics::chrome;
    refreshPopup = new kobalt::ui::ModalOverlay(this);
    refreshPopup->setObjectName("rrScreenRefreshPopup");
    refreshPopup->setDimmed(true);
    refreshPopup->setGeometry(rect());
    refreshPopup->installEventFilter(this);
    auto *card = refreshPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(metrics::notice::cardMargins.top());
    auto *title = card->title(strings::get("reader.refresh_the_screen"), "rrScreenRefreshTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::notice::cardMargins.bottom());
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    // The shared pick-one list: rows share the tallest height a choice needs.
    auto *choices = new kobalt::ui::RadioList(card);
    choices->setObjectName("rrScreenRefreshChoices");
    choices->setMinimumRowHeight(chrome::listActionHeight);
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
QString sleepLabel(sleep::Mode mode)
{
    switch (mode) {
    case sleep::Mode::Stock:
        return strings::get("settings.sleep_stock");
    case sleep::Mode::Cover:
        return strings::get("settings.sleep_cover");
    case sleep::Mode::Page:
        return strings::get("settings.sleep_page");
    case sleep::Mode::Overlay:
        return strings::get("settings.sleep_overlay");
    }
    return {};
}

QString sleepExplanation(sleep::Mode mode)
{
    switch (mode) {
    case sleep::Mode::Stock:
        return strings::get("settings.sleep_stock_description");
    case sleep::Mode::Cover:
        return strings::get("settings.sleep_cover_description");
    case sleep::Mode::Page:
        return strings::get("settings.sleep_page_description");
    case sleep::Mode::Overlay:
        return strings::get("settings.sleep_overlay_description");
    }
    return {};
}

QString sleepTileLabel(sleep::Tile tile)
{
    switch (tile) {
    case sleep::Tile::Card:
        return strings::get("settings.sleep_tile_card");
    case sleep::Tile::None:
        return strings::get("settings.sleep_tile_none");
    case sleep::Tile::Bookmark:
        return strings::get("settings.sleep_tile_bookmark");
    }
    return {};
}

QString sleepTileExplanation(sleep::Tile tile)
{
    switch (tile) {
    case sleep::Tile::Card:
        return strings::get("settings.sleep_tile_card_description");
    case sleep::Tile::None:
        return strings::get("settings.sleep_tile_none_description");
    case sleep::Tile::Bookmark:
        return strings::get("settings.sleep_tile_bookmark_description");
    }
    return {};
}
} // namespace

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
    sleepScreenButton->setCaption(sleepScreenButton->isEnabled()
                                          ? sleepLabel(sleep::mode(preferences)).toUpper()
                                          : strings::get("settings.unsupported_caption"));
    sleepTileButton->setVisible(sleep::mode(preferences) != sleep::Mode::Stock);
    sleepTileButton->setCaption(sleepTileButton->isEnabled()
                                        ? sleepTileLabel(sleep::tile(preferences)).toUpper()
                                        : strings::get("settings.unsupported_caption"));
}

void Reader::closeSleepScreen()
{
    if (!sleepPopup) {
        return;
    }
    if (sleep::mode(preferences) != sleep::Mode::Overlay)
        sleepOverlay = {};
    sleepPopup->hide();
    sleepPopup->deleteLater();
    sleepPopup.clear();
}

bool Reader::saveSleepPreferences(const QJsonObject &next)
{
    if (!writePreferences(next)) {
        settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
        settingsError->show();
        return false;
    }
    preferences = next;
    showSleepScreen();
    settingsError->clear();
    settingsError->hide();
    return true;
}

void Reader::openSleepScreen()
{
    QVector<SleepOption> options;
    int current = 0;
    for (const auto mode : sleep::modes) {
        if (mode == sleep::mode(preferences))
            current = int(options.size());
        const bool enabled = mode != sleep::Mode::Overlay || !sleepOverlay.isNull();
        options.append({ sleep::value(mode), sleepLabel(mode), sleepExplanation(mode), enabled });
    }
    openSleepChoice("rrSleepScreen", "rrSleep_", strings::get("settings.sleep_screen"), options,
                    current, [this](int index) {
                        auto next = preferences;
                        next[sleep::preferenceKey] = sleep::value(sleep::modes[index]);
                        next.remove("kobalt_sleep_screen");
                        if (saveSleepPreferences(next))
                            prepareSleepCover();
                    });
    if (sleepOverlay.isNull())
        prepareSleepOverlay();
}

void Reader::updateSleepOverlayOption()
{
    if (auto *option = sleepPopup->findChild<QWidget *>("rrSleep_overlay"))
        option->setEnabled(!sleepOverlay.isNull());
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

void Reader::openSleepTile()
{
    QVector<SleepOption> options;
    int current = 0;
    for (const auto tile : sleep::tiles) {
        if (tile == sleep::tile(preferences))
            current = int(options.size());
        options.append({ sleep::value(tile), sleepTileLabel(tile), sleepTileExplanation(tile) });
    }
    openSleepChoice("rrSleepTile", "rrSleepTile_", strings::get("settings.sleep_tile"), options,
                    current, [this](int index) {
                        auto next = preferences;
                        next[sleep::tileKey] = sleep::value(sleep::tiles[index]);
                        saveSleepPreferences(next);
                    });
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
    sleepPopup->installEventFilter(this);
    sleepPopup->dismissed = [this] { closeSleepScreen(); };
    auto *card = sleepPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(metrics::notice::cardMargins.top());
    auto *title = card->title(heading, name + "Title");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    if (!description.isEmpty()) {
        card->rows()->addSpacing(metrics::spacing::row);
        auto *hint = card->message(description, name + "Hint");
        readerStyle(hint, styles::font(chrome::descriptionText));
        hint->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    }
    card->rows()->addSpacing(metrics::notice::cardMargins.bottom());
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName(name + "Choices");
    choices->setUniformRows(false);
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
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

void Reader::closeTapZones()
{
    if (!tapZonesPopup)
        return;
    tapZonesPopup->hide();
    tapZonesPopup->deleteLater();
    tapZonesPopup.clear();
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
    tapZonesPopup->installEventFilter(this);
    tapZonesPopup->dismissed = [this] { closeTapZones(); };
    auto *card = tapZonesPopup->card();
    card->rows()->setContentsMargins(chrome::inset, metrics::notice::cardMargins.top(),
                                     chrome::inset, metrics::notice::cardMargins.bottom());
    card->title(strings::get("settings.tap_zones"), "rrTapZonesTitle");
    card->message(strings::get("settings.tap_zones_hint"), "rrTapZonesHint");
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
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

void Reader::closePageNavigation()
{
    if (!pageNavigationPopup) {
        return;
    }
    pageNavigationPopup->hide();
    pageNavigationPopup->deleteLater();
    pageNavigationPopup.clear();
}

void Reader::openPageNavigation()
{
    if (pageNavigationPopup) {
        return;
    }
    namespace chrome = metrics::chrome;
    pageNavigationPopup = new ui::ModalOverlay(this);
    pageNavigationPopup->setObjectName("rrPageNavigationPopup");
    pageNavigationPopup->setDimmed(true);
    pageNavigationPopup->setGeometry(rect());
    pageNavigationPopup->installEventFilter(this);
    pageNavigationPopup->dismissed = [this] { closePageNavigation(); };
    auto *card = pageNavigationPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(metrics::notice::cardMargins.top());
    auto *title = card->title(strings::get("settings.page_navigation"), "rrPageNavigationTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    auto *hint =
            card->message(strings::get("settings.navigation_menu_hint"), "rrPageNavigationHint");
    hint->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::notice::cardMargins.bottom());
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrPageNavigationChoices");
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
    for (const auto mode : pageNavigation::modes) {
        if (mode == pageNavigation::Mode::ButtonsOnly && !services.pageButtons()) {
            continue;
        }
        const bool needsSwipes =
                pageNavigation::tapLayout(preferences) == tapZones::Layout::OneHanded
                && !pageNavigation::swipes(mode);
        auto *option =
                choices->add("rrNavigation_" + pageNavigation::value(mode), navigationLabel(mode),
                             needsSwipes ? strings::get("settings.navigation_needs_swipes")
                                         : navigationExplanation(mode));
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

void Reader::showNavigationBarButtons()
{
    const auto mode = pageNavigation::barButtons(preferences);
    navigationBarButton->setCaption(barButtonsLabel(mode).toUpper());
    pageScrubber->setChapterJumps(mode == pageNavigation::BarButtons::Chapters);
}

void Reader::closeNavigationBarButtons()
{
    if (!navigationBarPopup) {
        return;
    }
    navigationBarPopup->hide();
    navigationBarPopup->deleteLater();
    navigationBarPopup.clear();
}

void Reader::openNavigationBarButtons()
{
    if (navigationBarPopup) {
        return;
    }
    namespace chrome = metrics::chrome;
    navigationBarPopup = new ui::ModalOverlay(this);
    navigationBarPopup->setObjectName("rrNavigationBarPopup");
    navigationBarPopup->setDimmed(true);
    navigationBarPopup->setGeometry(rect());
    navigationBarPopup->installEventFilter(this);
    navigationBarPopup->dismissed = [this] { closeNavigationBarButtons(); };
    auto *card = navigationBarPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(metrics::notice::cardMargins.top());
    auto *title =
            card->title(strings::get("settings.navigation_bar_buttons"), "rrNavigationBarTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    auto *hint = card->message(strings::get("settings.navigation_bar_hint"), "rrNavigationBarHint");
    hint->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::notice::cardMargins.bottom());
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrNavigationBarChoices");
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
    for (const auto mode : pageNavigation::barButtonsModes) {
        choices->add(
                "rrNavigationBar_" + pageNavigation::value(mode), barButtonsLabel(mode),
                strings::get(mode == pageNavigation::BarButtons::Chapters
                                     ? strings::key("settings.navigation_bar_chapters_description")
                                     : strings::key("settings.navigation_bar_pages_description")));
        if (mode == pageNavigation::barButtons(preferences)) {
            choices->setCurrent(choices->options().size() - 1);
        }
    }
    choices->chosen = [this](int index) {
        auto next = preferences;
        next[pageNavigation::barButtonsKey] =
                pageNavigation::value(pageNavigation::barButtonsModes[index]);
        if (writePreferences(next)) {
            preferences = next;
            showNavigationBarButtons();
            settingsError->clear();
            settingsError->hide();
        } else {
            settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
            settingsError->show();
        }
        closeNavigationBarButtons();
    };
    card->rows()->addWidget(choices);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    navigationBarPopup->centerCard();
    for (auto *widget : navigationBarPopup->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
    textRefresh(navigationBarPopup);
    navigationBarPopup->show();
    navigationBarPopup->raise();
    navigationBarPopup->setFocus();
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

void Reader::closeTappableLinks()
{
    if (!tappableLinksPopup) {
        return;
    }
    tappableLinksPopup->hide();
    tappableLinksPopup->deleteLater();
    tappableLinksPopup.clear();
}

void Reader::openTappableLinks()
{
    if (tappableLinksPopup) {
        return;
    }
    namespace chrome = metrics::chrome;
    tappableLinksPopup = new ui::ModalOverlay(this);
    tappableLinksPopup->setObjectName("rrTappableLinksPopup");
    tappableLinksPopup->setDimmed(true);
    tappableLinksPopup->setGeometry(rect());
    tappableLinksPopup->installEventFilter(this);
    tappableLinksPopup->dismissed = [this] { closeTappableLinks(); };
    auto *card = tappableLinksPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(metrics::notice::cardMargins.top());
    auto *title = card->title(strings::get("settings.tappable_links"), "rrTappableLinksTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::spacing::row);
    auto *hint = card->message(strings::get("settings.tappable_links_description"),
                               "rrTappableLinksHint");
    readerStyle(hint, styles::font(chrome::descriptionText));
    hint->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::notice::cardMargins.bottom());
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrTappableLinksChoices");
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
    const QStringList modes{ "footnotes", "all", "off" };
    const auto current = preferences.value("tappable_links").toString("footnotes");
    for (const auto &mode : modes) {
        choices->add("rrTappableLinks_" + mode, tappableLinksLabel(mode),
                     tappableLinksDescription(mode));
        if (mode == current)
            choices->setCurrent(choices->options().size() - 1);
    }
    choices->chosen = [this, modes](int index) {
        auto next = preferences;
        next["tappable_links"] = modes[index];
        if (writePreferences(next)) {
            preferences = next;
            showTappableLinks();
            syncLinkDiscovery();
            settingsError->clear();
            settingsError->hide();
        } else {
            settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
            settingsError->show();
        }
        closeTappableLinks();
    };
    card->rows()->addWidget(choices);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    tappableLinksPopup->centerCard();
    for (auto *widget : tappableLinksPopup->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
    textRefresh(tappableLinksPopup);
    tappableLinksPopup->show();
    tappableLinksPopup->raise();
    tappableLinksPopup->setFocus();
}

void Reader::showMenuView()
{
    const auto mode = pageNavigation::menuView(preferences);
    menuViewButton->setCaption(menuViewLabel(mode).toUpper());
}

void Reader::closeMenuView()
{
    if (!menuViewPopup) {
        return;
    }
    menuViewPopup->hide();
    menuViewPopup->deleteLater();
    menuViewPopup.clear();
}

void Reader::openMenuView()
{
    if (menuViewPopup) {
        return;
    }
    namespace chrome = metrics::chrome;
    menuViewPopup = new ui::ModalOverlay(this);
    menuViewPopup->setObjectName("rrMenuViewPopup");
    menuViewPopup->setDimmed(true);
    menuViewPopup->setGeometry(rect());
    menuViewPopup->installEventFilter(this);
    menuViewPopup->dismissed = [this] { closeMenuView(); };
    auto *card = menuViewPopup->card();
    card->rows()->setContentsMargins(2, 2, 2, 2);
    card->rows()->setSpacing(0);
    card->rows()->addSpacing(metrics::notice::cardMargins.top());
    auto *title = card->title(strings::get("settings.default_menu_view"), "rrMenuViewTitle");
    title->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::spacing::row);
    auto *hint =
            card->message(strings::get("settings.default_menu_view_description"), "rrMenuViewHint");
    readerStyle(hint, styles::font(chrome::descriptionText));
    hint->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    card->rows()->addSpacing(metrics::notice::cardMargins.bottom());
    const int cardWidth =
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance);
    auto *choices = new ui::RadioList(card);
    choices->setObjectName("rrMenuViewChoices");
    choices->setMinimumRowHeight(chrome::listActionHeight);
    choices->setTopDivider(true);
    for (const auto mode : pageNavigation::menuViews) {
        const auto description = mode == pageNavigation::MenuView::None
                ? strings::key("settings.menu_view_none_description")
                : mode == pageNavigation::MenuView::Seek
                ? strings::key("settings.menu_view_seek_description")
                : strings::key("settings.menu_view_stats_description");
        choices->add("rrMenuView_" + pageNavigation::value(mode), menuViewLabel(mode),
                     strings::get(description));
        if (mode == pageNavigation::menuView(preferences)) {
            choices->setCurrent(choices->options().size() - 1);
        }
    }
    choices->chosen = [this](int index) {
        auto next = preferences;
        next[pageNavigation::menuViewKey] = pageNavigation::value(pageNavigation::menuViews[index]);
        next.remove("pin_stats");
        next.remove(pageNavigation::thumbnailsKey);
        if (writePreferences(next)) {
            preferences = next;
            showMenuView();
            settingsError->clear();
            settingsError->hide();
        } else {
            settingsError->setText(strings::get("reader.could_not_save_reader_settings"));
            settingsError->show();
        }
        closeMenuView();
    };
    card->rows()->addWidget(choices);
    choices->setFixedHeight(choices->heightForWidth(cardWidth - 4));
    card->fitToWidth(cardWidth);
    menuViewPopup->centerCard();
    for (auto *widget : menuViewPopup->findChildren<QWidget *>()) {
        textRefresh(widget);
    }
    textRefresh(menuViewPopup);
    menuViewPopup->show();
    menuViewPopup->raise();
    menuViewPopup->setFocus();
}

} // namespace kobalt::reader
