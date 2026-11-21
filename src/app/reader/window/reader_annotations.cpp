#include "ui/controls/highlight_palette.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/style/private_text.h"
#include <QLabel>
#include <QSet>
#include "reader_p.h"

#include "ui/style/styles.h"
#include "ui/dialogs/action_menu.h"
#include "platform/api/kobo_refresh.h"
#include "support/qt_font.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include "storage/annotation_export.h"

#include <QHBoxLayout>
#include <QGridLayout>
#include "support/qt_compat.h"
#include <QVBoxLayout>
#include <QtMath>
#include <limits>
#include <utility>

namespace kobalt::reader {
namespace metrics = kobalt::ui::metrics;
namespace strings = kobalt::strings;

namespace {
const char *exportFormatName(AnnotationExportFormat format)
{
    return format == AnnotationExportFormat::Markdown ? "Markdown" : "HTML";
}

QByteArray exportFont(const EngineFontInputs &fonts)
{
    for (const auto &path : fonts.sources.bundled) {
        if (!path.endsWith("/Libron-Regular.ttf"))
            continue;
        QFile font(path);
        if (!font.open(QIODevice::ReadOnly))
            return {};
        return font.readAll();
    }
    return {};
}
} // namespace

void Reader::closeMarksActions()
{
    if (!marksActions)
        return;
    marksActions->hide();
    marksActions->deleteLater();
    marksActions.clear();
}

void Reader::openMarksActions()
{
    if (marksActions || removeDialog || (annotationExporter && annotationExporter->active())
        || session->busy() || closing || lifecycleHidden || services.suspended()
        || book->deviceTransition())
        return;
    ui::ActionMenu *menu;
    if (bookmarkList->annotationsShown()) {
        const ui::ActionMenuHeader header{ {},
                                           strings::get("annotations.export"),
                                           strings::get("annotations.export_compression"),
                                           {} };
        menu = new ui::ActionMenu(
                this, header,
                { ui::actionItem(strings::get("annotations.export_markdown"),
                                 "rrAnnotationExportMarkdown", ui::ControlIcon::Export,
                                 [this] { askExportMarks(AnnotationExportFormat::Markdown); }),
                  ui::actionItem(strings::get("annotations.export_html"), "rrAnnotationExportHtml",
                                 ui::ControlIcon::Export,
                                 [this] { askExportMarks(AnnotationExportFormat::Html); }) });
        menu->setObjectName("rrAnnotationActions");
        const bool enabled = highlights->writable() && highlights->resolved()
                && !highlights->highlights().isEmpty();
        for (auto *action : menu->actions())
            action->setEnabled(enabled);
    } else {
        const int current = session->page();
        const bool marked = bookmarks->marked(current);
        menu = new ui::ActionMenu(
                this, {},
                { ui::actionItem(
                        strings::get(marked ? "bookmarks.remove_current" : "bookmarks.add_current"),
                        marked ? "rrBookmarkRemoveCurrent" : "rrBookmarkAddCurrent",
                        marked ? ui::ControlIcon::BookmarkRemove : ui::ControlIcon::BookmarkAdd,
                        [this, current, marked] {
                            if (session->page() == current && bookmarks->marked(current) == marked)
                                toggleBookmark();
                        }) });
        menu->setObjectName("rrBookmarkActions");
        menu->actions().first()->setEnabled(bookmarks->writable() && bookmarks->resolved()
                                            && session->pages() > 0);
    }
    marksActions = menu;
    menu->dismissed = [this] { closeMarksActions(); };
    menu->setDimmed(true);
    menu->setProperty("reader_standard_modal", true);
    menu->setGeometry(rect());
    menu->fitCard();
    menu->show();
    menu->raise();
    menu->setFocus();
}

QByteArray Reader::marksExportArchive(AnnotationExportFormat format)
{
    QVector<Highlight> rows;
    return highlights->exportDetails(&rows)
            ? annotationExportArchive(session->title(), session->author(), rows, format,
                                      QDateTime::currentDateTimeUtc(),
                                      format == AnnotationExportFormat::Html
                                              ? exportFont(session->fontInputs())
                                              : QByteArray())
            : QByteArray();
}

void Reader::askExportMarks(AnnotationExportFormat format)
{
    if (removeDialog || (annotationExporter && annotationExporter->active()) || session->busy()
        || closing || lifecycleHidden || services.suspended() || book->deviceTransition())
        return;
    if (!highlights->writable() || !highlights->resolved() || highlights->highlights().isEmpty())
        return;
    if (!annotationExporter) {
        finishMarksExport(format, false);
        return;
    }
    annotationExporter->start(
            { annotationExportFileName(book->identity(), session->title(), format),
              session->title(), QString::fromLatin1(exportFormatName(format)),
              [this, format] { return marksExportArchive(format); },
              [this] {
                  return !session->busy() && !closing && !lifecycleHidden && !services.suspended()
                          && !book->deviceTransition();
              },
              [this] { return !closing && !book->deviceTransition(); } },
            [this, format](AnnotationExportResult result) {
                if (closing || lifecycleHidden || book->deviceTransition()
                    || result.status == AnnotationExportStatus::Cancelled)
                    return;
                finishMarksExport(format, result.status == AnnotationExportStatus::Saved,
                                  result.location);
            });
}

void Reader::finishMarksExport(AnnotationExportFormat format, bool saved, const QString &location)
{
    auto *notice = new ui::NoticeModal(
            this,
            { !saved                       ? strings::get("annotations.export_failed")
                      : location.isEmpty() ? strings::get("annotations.export_complete")
                                           : strings::get("annotations.export_saved").arg(location),
              strings::get("annotations.export_format_title").arg(exportFormatName(format)),
              {} },
            { strings::get("common.ok"), "rrAnnotationExportDone" });
    removeDialog = notice;
    const auto done = [this, notice] {
        notice->hide();
        notice->deleteLater();
        removeDialog.clear();
    };
    notice->dismissed = done;
    connect(notice->primaryButton(), &QPushButton::clicked, this, done);
    notice->show();
    notice->raise();
}

void Reader::paintAnnotationDraft()
{
    page->annotate(annotationDraft->geometry().rectangles, savedHighlightRects, {},
                   annotationDraft->geometry().vertical);
    page->setAnnotationHandles(annotationDraft->geometry());
    syncPopoverExclusions();
}

void Reader::syncPopoverExclusions()
{
    // Handles keep their full press area outside the card. Over the card only
    // the drawn dots pass presses on, so the card's controls stay usable.
    const QPoint offset = page->mapTo(this, QPoint()) - annotationPopover->pos();
    auto *card = annotationPopover->card();
    const QRegion cardArea(QRect(card->mapTo(annotationPopover, QPoint()), card->size()));
    annotationPopover->setInputExclusions(
            page->annotationHandleHitRegion().translated(offset).subtracted(cardArea)
            | page->annotationHandleDotRegion().translated(offset));
}

void Reader::buildAnnotations()
{
    annotationPopover = new ui::ModalOverlay(this);
    annotationPopover->setObjectName("rrAnnotationPopover");
    annotationPopover->dismissed = [this] {
        // Quick annotation mode keeps no draft without its menu.
        if (!annotationMode || quickAnnotations()) {
            clearAnnotationDraft();
            return;
        }
        annotationDraft->invalidate();
        annotationPopover->hide();
        paintAnnotationDraft();
    };
    auto *rows = annotationPopover->card()->rows();
    rows->setContentsMargins(metrics::control::border, metrics::control::border,
                             metrics::control::border, metrics::control::border);
    rows->setSpacing(0);
    annotationPopover->hide();
}

void Reader::enterAnnotations(bool notice)
{
    if (!features::enabled(preferences, features::Annotations))
        return;
    if (session->busy() || !highlights->writable() || appearanceEdit.panelOpen()
        || settingsVisible()) {
        return;
    }
    const bool changed = !annotationMode;
    // A quick entry happens inside a hold; keep that gesture.
    dismissLookup(!notice);
    dismissControls();
    annotationMode = true;
    page->setAnnotationInputEnabled(true);
    syncMarkupRequest();
    syncAnnotations();
    if (changed && notice)
        showAnnotationModeNotice();
}

void Reader::leaveAnnotations(bool notice)
{
    const bool changed = annotationMode;
    const bool discardSelection =
            !annotationDraft->range().start.isEmpty() || !annotationDraft->editingId().isEmpty();
    annotationMode = false;
    annotationReopen.stop();
    setAnnotationDragRefresh(false);
    annotationDrag.forgetAwaitedRelease();
    annotationDraft->clear();
    page->setAnnotationHandles({});
    page->setAnnotationInputEnabled(true);
    annotationColor = annotationSelectedColor;
    annotationStyle = annotationSelectedStyle;
    annotationColorChosen = annotationStyleChosen = false;
    annotationActionAnchor = {};
    annotationShowActionsPending = false;
    syncMarkupRequest();
    // Discarding a selection clears draft geometry and restores any saved range
    // hidden by editing. Ordinary dismissal keeps the resolved saved marks.
    if (discardSelection) {
        annotationFrame.clear();
    }
    toolbarComposition.annotate->setChecked(false);
    annotationPopover->hide();
    page->annotate({}, savedHighlightRects);
    if (changed && notice)
        dismissControls();
    refresh();
    if (changed && notice)
        showAnnotationModeNotice();
}

bool Reader::quickAnnotations() const
{
    return features::enabled(preferences, features::Annotations)
            && preferences.value("annotation_interaction").toString() == QLatin1String("quick");
}

bool Reader::usesAnnotationToggle() const
{
    return features::enabled(preferences, features::Annotations) && !quickAnnotations();
}

void Reader::settleQuickAnnotations()
{
    // Wait for the current event: a new hold clears the old draft before it
    // starts its own.
    qt::later(0, this, [this] {
        if (!quickAnnotations() || !annotationMode || annotationSaving || annotationDrag.active()
            || annotationPopover->isVisible() || noteEditor || removeDialog
            || !annotationDraft->range().start.isEmpty() || !annotationDraft->editingId().isEmpty())
            return;
        leaveAnnotations(false);
    });
}

void Reader::showAnnotationModeNotice()
{
    if (!isVisible() || lifecycleHidden || closing || services.suspended() || !annotationModeNotice)
        return;
    brightnessHide.stop();
    brightnessNotice->hide();
    annotationModeNotice->setModeEnabled(annotationMode);
    annotationModeNotice->centerIn(size());
    annotationModeNotice->show();
    annotationModeNotice->raise();
    annotationModeHide.start();
}

void Reader::annotationActionsFor(const QVector<AnnotationAction> &actions, QRect anchor,
                                  bool compactActions, const QString &paletteColor,
                                  std::function<void(QString)> colorChanged,
                                  const QString &selectedStyle,
                                  std::function<void(QString)> styleChanged, const QString &note)
{
    annotationReopen.stop();
    paintAnnotationDraft();
    annotationPopover->hide();
    auto *card = annotationPopover->card();
    auto *rows = card->rows();
    while (auto *item = rows->takeAt(0)) {
        if (auto *row = item->widget()) {
            // Actions can rebuild the card from their own clicked signal.
            // Detach the old rows so widget lookups find the replacement.
            row->hide();
            row->setParent(nullptr);
            row->deleteLater();
        }
        delete item;
    }
    if (highlights->error() == "overlap" && !annotationDraft->range().start.isEmpty())
        card->message(strings::get("annotations.overlap"), "rrAnnotationOverlap");
    if (!highlights->writable() && !highlights->error().isEmpty())
        card->message(strings::get("annotations.load_failed"), "rrAnnotationLoadError");
    int desiredWidth = metrics::links::popupWidth;
    // The choices have no section captions, so that the card covers less of
    // the page. A saved note comes first.
    ui::PrivateTextLabel *noteText = nullptr;
    if (!note.isEmpty()) {
        auto *noteRow = new ui::Panel(card);
        // The action buttons below draw the divider.
        noteRow->setObjectName("rrAnnotationNote");
        auto *layout = new QHBoxLayout(noteRow);
        layout->setContentsMargins(metrics::chrome::inset, ui::px(20), metrics::chrome::inset,
                                   ui::px(20));
        layout->setSpacing(ui::px(18));
        auto *icon = new QLabel(noteRow);
        const int side = ui::px(32);
        icon->setPixmap(
                ui::controlIcon(ui::ControlIcon::Note, palette().color(QPalette::WindowText))
                        .pixmap(side, side));
        icon->setFixedSize(side, side);
        layout->addWidget(icon, 0, Qt::AlignTop);
        auto *text = new ui::PrivateTextLabel(noteRow);
        text->setObjectName("rrAnnotationNoteText");
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setMaximumLines(3);
        text->setFont(ui::interfaceFont(ui::px(23)));
        ui::readerStyle(text, ui::styles::font(ui::px(23)));
        text->setText(note);
        text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        noteText = text;
        layout->addWidget(text, 1);
        rows->addWidget(noteRow);
        desiredWidth = qMax(desiredWidth, ui::px(700));
    }
    if (colorAnnotations() && colorChanged) {
        auto *palette = new ui::HighlightPalette(paletteColor, colorChanged, card);
        rows->addWidget(palette);
        palette->show();
        desiredWidth = qMax(desiredWidth, ui::px(700));
    }
    if (styleChanged) {
        rows->addWidget(new ui::HighlightStylePicker(selectedStyle, styleChanged, card));
        desiredWidth = qMax(desiredWidth, ui::px(700));
    }
    auto menuActions = actions;
    if (!annotationDraft->range().start.isEmpty()) {
        // While a saved highlight is adjusted, this drops the changes and
        // keeps the saved highlight, as Discard does in the Color step.
        const bool adjusting = !annotationDraft->editingId().isEmpty();
        menuActions.append(
                { "rrAnnotationCancel",
                  adjusting ? ui::ControlIcon::Undo : ui::ControlIcon::ClearSelection,
                  strings::get(adjusting ? "annotations.discard" : "annotations.cancel_selection"),
                  [this] { clearAnnotationDraft(); } });
    }
    if (compactActions && annotationMode && minimalAnnotationMode() && !quickAnnotations()) {
        menuActions.append({ "rrAnnotationCloseMode", ui::ControlIcon::Close,
                             strings::get("annotations.close"), [this] { leaveAnnotations(); } });
    }
    const int paletteHeight =
            colorAnnotations() && colorChanged ? ui::HighlightPalette::compactHeight() : 0;
    const int rowHeight =
            qBound(ui::px(56),
                   (height() - 2 * int(metrics::notice::sideClearance) - paletteHeight)
                           / qMax(1, int(menuActions.size())),
                   int(metrics::links::actionHeight));
    const bool compact = compactActions && menuActions.size() <= 6;
    QGridLayout *buttons = nullptr;
    int columns = menuActions.size();
    if (compact) {
        int labelWidth = 0;
        const auto font = ui::interfaceFont(ui::px(23));
        for (const auto &action : menuActions)
            labelWidth = qMax(labelWidth, ui::interfaceTextWidth(action.label, font));
        // Qt can elide at the rounded text advance. Leave room for fractional glyph widths.
        const auto stripWidth = [&](int count) {
            return count * (labelWidth + ui::px(4) + 2 * ui::px(16))
                    + 2 * int(metrics::control::border);
        };
        // Four actions share one row when their labels fit, and two rows
        // otherwise. Five or six use rows of three.
        if (columns > 4)
            columns = 3;
        else if (columns == 4 && stripWidth(4) > width() - 2 * int(metrics::notice::sideClearance))
            columns = 2;
        desiredWidth = qMax(desiredWidth, stripWidth(columns));
        auto *strip = new QWidget(card);
        strip->setFixedHeight(ui::px(104) * ((menuActions.size() + columns - 1) / columns));
        buttons = new QGridLayout(strip);
        buttons->setContentsMargins(0, 0, 0, 0);
        buttons->setSpacing(0);
        rows->addWidget(strip);
    }
    for (const auto &action : menuActions) {
        if (compact) {
            auto *button =
                    new ui::CaptionButton(ui::CaptionButton::Presentation::ToolbarAction, card);
            button->setText(action.label);
            button->setObjectName(action.name);
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            button->setFixedHeight(ui::px(104));
            button->setIconVisible(true);
            button->setIconAboveLabel(true);
            button->setChevron(false);
            ui::Edges edges(Qt::TopEdge);
            if ((buttons->count() + 1) % columns != 0)
                edges |= Qt::RightEdge;
            button->setRule(edges);
            ui::buttonIcon(button, action.icon);
            ui::readerStyle(
                    button,
                    QString("QPushButton{%1border:none;padding:0;min-height:%2px;max-height:%2px;}")
                            .arg(ui::styles::font(ui::px(23)))
                            .arg(ui::px(104)));
            connect(button, &QPushButton::clicked, this, action.run);
            const int index = buttons->count();
            buttons->addWidget(button, index / columns, index % columns);
        } else {
            auto *button = ui::makeCaptionButton(card, rows, action.name.toLatin1().constData(),
                                                 action.label, action.run);
            button->setRule(rows->count() > 1 ? ui::Edges(Qt::TopEdge) : ui::Edges{});
            button->setIconVisible(true);
            button->setChevron(false);
            ui::buttonIcon(button, action.icon);
            ui::readerStyle(
                    button,
                    QString("QPushButton{min-height:%1px;max-height:%1px;padding:0;border:none;}")
                            .arg(rowHeight));
            button->ensurePolished();
            const int rowWidth = metrics::chrome::inset + metrics::chrome::cellIcon + ui::px(20)
                    + ui::interfaceTextWidth(action.label, button->font())
                    + metrics::chrome::actionRight;
            desiredWidth = qMax(desiredWidth, rowWidth + 2 * int(metrics::control::border));
        }
    }
    for (int i = 0; i < rows->count(); ++i)
        if (auto *widget = rows->itemAt(i)->widget()) {
            widget->ensurePolished();
            widget->show();
        }
    rows->activate();
    const int cardWidth = qMin(desiredWidth, width() - 2 * int(metrics::notice::sideClearance));
    card->fitToWidth(cardWidth);
    if (noteText) {
        // The wrapped height of the note is known once the card has its width.
        const int lineHeight = ui::interfaceLineHeight(noteText->font());
        const int lines = qBound(
                1,
                (ui::wrappedInterfaceTextHeight(note, noteText->font(), qMax(1, noteText->width()))
                 + lineHeight - 1)
                        / qMax(1, lineHeight),
                3);
        noteText->setFixedHeight(lines * lineHeight);
        rows->activate();
        card->fitToWidth(cardWidth);
    }
    annotationPopover->anchorTo(anchor);
    syncPopoverExclusions();
    for (auto *widget : annotationPopover->findChildren<QWidget *>()) {
        widget->setPalette(palette());
        platform::textRefresh(widget);
    }
    annotationPopover->show();
    annotationPopover->raise();
    if (auto *palette = dynamic_cast<ui::HighlightPalette *>(
                annotationPopover->findChild<QWidget *>("rrAnnotationPalette"))) {
        palette->syncRepairBounds();
    }
}

void Reader::clearAnnotationDraft()
{
    annotationReopen.stop();
    setAnnotationDragRefresh(false);
    dismissLookup(true);
    annotationDrag.forgetAwaitedRelease();
    annotationDraft->clear();
    page->setAnnotationHandles({});
    annotationColorChosen = annotationStyleChosen = false;
    annotationColor = annotationSelectedColor;
    annotationStyle = annotationSelectedStyle;
    annotationShowActionsPending = false;
    annotationActionAnchor = {};
    page->annotate({}, savedHighlightRects);
    annotationPopover->hide();
    syncMarkupRequest();
    annotationFrame.clear();
    syncAnnotations();
    settleQuickAnnotations();
}

void Reader::annotationTap(QPoint point)
{
    if (annotationSaving || session->busy() || lifecycleHidden || services.suspended()
        || annotationDraft->range().start.isEmpty() || !page->annotationContains(point))
        return;
    dismissLookup(true);
    showAnnotationActions();
}

bool Reader::beginAnnotationDrag(QPoint point, int boundary)
{
    if (annotationSaving || session->busy() || lifecycleHidden || closing || services.suspended()
        || (boundary >= 0 && !annotationMode
            && (annotationDraft->range().start.isEmpty() || annotationDraft->range().end.isEmpty()))
        || (topControls->isVisible() && !annotationMenuSelectionEnabled()))
        return false;
    // Quick annotation mode enters annotation mode for each held selection,
    // so that the hold can drag across words, pages and saved highlights.
    annotationReopen.stop();
    if (boundary < 0 && quickAnnotations() && !annotationMode)
        enterAnnotations(false);
    if (topControls->isVisible())
        dismissControls();
    dismissLookup(true);
    if (boundary < 0)
        clearAnnotationDraft();
    annotationDraft->invalidate();
    annotationPopover->hide();
    annotationDrag.begin(AnnotationDrag::Boundary(boundary), point);
    if (annotationDrag.handle)
        loadAnnotationDragEdges();
    else
        requestAnnotationDrag();
    return true;
}

void Reader::loadAnnotationDragEdges()
{
    annotationDrag.loadingEdges = true;
    annotationDrag.requestPending = true;
    const auto epoch = annotationDraft->generation();
    QPointer<Reader> guard(this);
    session->annotationPageEdges([guard, epoch](AnnotationPageEdges edges) {
        if (!guard || guard->annotationDraft->generation() != epoch
            || !guard->annotationDrag.active())
            return;
        guard->annotationDrag.loadingEdges = false;
        guard->annotationDrag.edges = edges;
        const int direction = std::exchange(guard->annotationDrag.turn, 0);
        guard->annotationDrag.requestPending = false;
        if (edges.first.text.isEmpty() || edges.last.text.isEmpty()) {
            guard->annotationDrag.end();
            return;
        }
        if (!direction) {
            guard->requestAnnotationDrag();
            return;
        }
        // Complete this gesture at the adjacent page boundary. Further movement
        // belongs to the old press until the user releases and grabs a handle again.
        guard->annotationDrag.requestPending = true;
        const auto token = direction > 0 ? edges.first : edges.last;
        const bool start = guard->annotationDrag.boundary == AnnotationDrag::Boundary::Start;
        guard->resolveAnnotationTokens(
                start ? token : guard->annotationDraft->endpoints().start,
                start ? guard->annotationDraft->endpoints().end : token,
                start ? !token.punctuation : guard->annotationDraft->endpoints().startPolicy,
                start ? guard->annotationDraft->endpoints().endPolicy : !token.punctuation,
                [guard] {
                    guard->annotationDrag.endBeforeRelease();
                    // A lookup handle opens its menu at once. An annotation
                    // handle reopens the menu after a pause.
                    if (guard->annotationDrag.released && !guard->annotationMode)
                        guard->showAnnotationActions();
                    else if (guard->annotationDrag.released)
                        guard->annotationReopen.start();
                },
                [guard, direction] {
                    // An adjacent saved highlight is a boundary, never an overlapping draft.
                    guard->annotationDrag.end();
                    guard->session->turn(-direction);
                });
    });
}

int Reader::annotationDragPageDirection(QPoint point) const
{
    const auto &edges = annotationDrag.edges;
    if (edges.first.text.isEmpty() || edges.last.text.isEmpty())
        return 0;
    const auto before = [&](const SelectionEndpoint &edge, const AnnotationToken &token,
                            bool start) {
        if (!edge.visible || token.rectangles.isEmpty())
            return false;
        const auto rect = start ? token.rectangles.first() : token.rectangles.last();
        const int tolerance = ui::px(8);
        if (edges.columns > 1
            && ((point.x() < page->width() / 2) != (rect.center().x() < page->width() / 2)))
            return false;
        if (edges.vertical) {
            // Vertical lines progress in the direction between the terminal words.
            const int flow = edges.last.rectangles.last().center().x()
                            >= edges.first.rectangles.first().center().x()
                    ? 1
                    : -1;
            if ((point.x() - rect.center().x()) * flow * (start ? -1 : 1)
                > rect.width() / 2 + tolerance)
                return true;
        } else if (start ? point.y() < rect.top() - tolerance
                         : point.y() > rect.bottom() + tolerance) {
            return true;
        }
        const QPoint middle = (edge.first + edge.last) / 2;
        const bool onLine = edges.vertical ? point.x() >= rect.left() && point.x() <= rect.right()
                                           : point.y() >= rect.top() && point.y() <= rect.bottom();
        return onLine && QPoint::dotProduct(point - middle, edge.outward) > tolerance;
    };
    if (before(edges.start, edges.first, true))
        return -1;
    if (before(edges.end, edges.last, false))
        return 1;
    return 0;
}

void Reader::moveAnnotationDrag(QPoint point)
{
    // The initial lookup hold keeps its word. Lookup handles can adjust the
    // range after release without enabling annotation mode.
    if (!annotationDrag.active() || (!annotationMode && !annotationDrag.handle))
        return;
    setAnnotationDragRefresh(true);
    annotationDrag.point = point;
    requestAnnotationDrag();
}

void Reader::reopenAnnotationActions()
{
    if (!annotationMode || annotationSaving || annotationDrag.active()
        || annotationDraft->range().start.isEmpty() || annotationPopover->isVisible() || noteEditor
        || removeDialog || lifecycleHidden || closing || services.suspended() || session->busy()
        || topControls->isVisible())
        return;
    // One full cleanup refresh replaces the fast drawing of the drag with the
    // reading waveform, then the menu returns over a clean selection.
    requestFlash("annotation-adjusted");
    showAnnotationActions();
    update();
}

void Reader::setAnnotationDragRefresh(bool dragging)
{
    if (property("reader_annotation_drag").toBool() == dragging)
        return;
    setProperty("reader_annotation_drag", dragging);
    // The next flush redraws the dragged area with the text waveform.
    if (!dragging)
        page->update();
}

void Reader::finishAnnotationDrag(bool cancelled)
{
    setAnnotationDragRefresh(false);
    if (!annotationDrag.active()) {
        if (annotationDrag.takeAwaitedRelease() && !cancelled && !annotationMode)
            lookupSelection();
        return;
    }
    if (cancelled) {
        annotationDrag.end();
        annotationDraft->invalidate();
        paintAnnotationDraft();
        return;
    }
    annotationDrag.released = true;
    requestAnnotationDrag();
}

void Reader::requestAnnotationDrag()
{
    if (!annotationDrag.active() || annotationDrag.requestPending || session->busy()
        || lifecycleHidden || services.suspended())
        return;
    if (annotationDrag.turn || (annotationDrag.handle && annotationDrag.edges.first.text.isEmpty()))
        return;
    if (annotationDrag.handle) {
        const int direction = annotationDragPageDirection(annotationDrag.point);
        if (direction != 0) {
            if (!annotationMode
                || !(direction > 0 ? annotationDrag.edges.continuesAfter
                                   : annotationDrag.edges.continuesBefore)) {
                const auto token =
                        direction > 0 ? annotationDrag.edges.last : annotationDrag.edges.first;
                const bool start = annotationDrag.boundary == AnnotationDrag::Boundary::Start;
                annotationDrag.requestPending = true;
                QPointer<Reader> guard(this);
                const auto stopped = [guard] {
                    if (!guard)
                        return;
                    guard->annotationDrag.endBeforeRelease();
                    if (guard->annotationDrag.released && !guard->annotationMode)
                        guard->lookupSelection();
                    else if (guard->annotationDrag.released)
                        guard->annotationReopen.start();
                };
                resolveAnnotationTokens(
                        start ? token : annotationDraft->endpoints().start,
                        start ? annotationDraft->endpoints().end : token,
                        start ? !token.punctuation : annotationDraft->endpoints().startPolicy,
                        start ? annotationDraft->endpoints().endPolicy : !token.punctuation,
                        stopped, stopped);
                return;
            }
            annotationDrag.turn = direction;
            annotationDrag.requestPending = true;
            if (session->turn(direction))
                return;
            annotationDrag.turn = 0;
            annotationDrag.requestPending = false;
        }
    }
    if (annotationDrag.boundary == AnnotationDrag::Boundary::Anchored) {
        if (annotationDrag.released) {
            annotationDrag.end();
            const QPoint delta = annotationDrag.point - annotationDrag.origin;
            if (qAbs(delta.x()) >= qMax(100, page->width() / 10)
                && qAbs(delta.x()) > qAbs(delta.y()) * 1.5 && page->swiped)
                page->swiped(delta.x() < 0 ? 1 : -1);
        }
        return;
    }
    annotationDrag.requestPending = true;
    const auto epoch = annotationDraft->generation();
    const QPoint point = annotationDrag.point;
    QPointer<Reader> guard(this);
    // At most one hit test and range resolution are in flight. Intermediate
    // pointer positions are replaced by the latest point, including release.
    session->annotationTokenAt(
            point,
            [guard, epoch, point](AnnotationToken token) {
                if (!guard || !guard->annotationDrag.active()
                    || guard->annotationDraft->generation() != epoch || guard->lifecycleHidden
                    || guard->services.suspended())
                    return;
                auto finish = [guard, epoch, point] {
                    if (!guard || guard->annotationDraft->generation() != epoch
                        || !guard->annotationDrag.active())
                        return;
                    guard->annotationDrag.requestPending = false;
                    if (guard->annotationDrag.boundary == AnnotationDrag::Boundary::Anchored) {
                        guard->requestAnnotationDrag();
                        return;
                    }
                    if (point != guard->annotationDrag.point) {
                        guard->requestAnnotationDrag();
                    } else if (guard->annotationDrag.released) {
                        guard->annotationDrag.end();
                        if (!guard->annotationMode
                            && !guard->annotationDraft->range().start.isEmpty()) {
                            if (!guard->annotationDrag.handle
                                && guard->selectionLinkPoint().x() >= 0)
                                guard->showAnnotationActions();
                            else
                                guard->lookupSelection();
                        } else if (!guard->annotationDraft->range().start.isEmpty()) {
                            // A new selection opens the menu once the finger
                            // lifts, so it never appears while the finger
                            // moves. After a handle adjustment the menu waits
                            // for a pause, in case the finger grabs a handle
                            // again.
                            if (guard->annotationDrag.handle)
                                guard->annotationReopen.start();
                            else
                                guard->showAnnotationActions();
                        }
                    }
                };
                if (token.text.isEmpty()) {
                    // A drag starting on blank paper remains a page gesture. Do not
                    // start a selection later when it happens to cross a word.
                    if (guard->annotationDrag.boundary == AnnotationDrag::Boundary::None)
                        guard->annotationDrag.boundary = AnnotationDrag::Boundary::Anchored;
                    finish();
                    return;
                }
                const auto reject = [guard, boundary = guard->annotationDrag.boundary, finish] {
                    if (!guard)
                        return;
                    guard->annotationDrag.boundary = boundary;
                    finish();
                };
                if (!guard->annotationDrag.movesEnd()) {
                    QVector<QPair<QString, QString>> ranges;
                    QStringList ids;
                    if (guard->annotationMode)
                        for (const auto &item : guard->highlights->highlights()) {
                            ranges.append({ item.start, item.end });
                            ids.append(item.id);
                        }
                    guard->session->intersectingRanges(
                            token.start, token.end, ranges,
                            [guard, epoch, token, ids, finish, reject](QVector<int> hits) {
                                if (!guard || guard->annotationDraft->generation() != epoch
                                    || !guard->annotationDrag.active())
                                    return;
                                if (!hits.isEmpty()) {
                                    guard->annotationDrag.end();
                                    guard->showSavedAnnotation(
                                            ids[hits.first()],
                                            token.rectangles.value(0).translated(guard->page->mapTo(
                                                    guard, QPoint(0, guard->page->topHeight()))));
                                    return;
                                }
                                guard->annotationDrag.boundary = AnnotationDrag::Boundary::End;
                                guard->resolveAnnotationTokens(token, token, !token.punctuation,
                                                               !token.punctuation, finish, reject);
                            });
                    return;
                }
                const auto endpoints =
                        guard->annotationDraft->moved(token, int(guard->annotationDrag.boundary));
                guard->resolveAnnotationTokens(endpoints.start, endpoints.end,
                                               endpoints.startPolicy, endpoints.endPolicy, finish,
                                               reject);
            },
            annotationDrag.boundary == AnnotationDrag::Boundary::Start
                    ? annotationDraft->endpoints().start.start
                    : annotationDrag.boundary == AnnotationDrag::Boundary::End
                    ? annotationDraft->endpoints().end.end
                    : QString(),
            annotationDrag.boundary == AnnotationDrag::Boundary::Start);
}

void Reader::resolveAnnotationTokens(AnnotationToken start, AnnotationToken end, bool startPolicy,
                                     bool endPolicy, std::function<void()> after,
                                     std::function<void()> reject)
{
    AnnotationDraft::Ranges others;
    if (annotationMode)
        for (const auto &item : highlights->highlights())
            if (item.id != annotationDraft->editingId())
                others.append({ item.start, item.end });
    QPointer<Reader> guard(this);
    annotationDraft->resolve(
            { start, end, startPolicy, endPolicy }, std::move(others),
            [guard](bool reversed) {
                if (!guard)
                    return;
                if (reversed && guard->annotationDrag.active())
                    guard->annotationDrag.swapEnd();
                guard->syncMarkupRequest();
            },
            [guard, after] {
                if (!guard)
                    return;
                const auto &rectangles = guard->annotationDraft->geometry().rectangles;
                if (!rectangles.isEmpty())
                    guard->annotationActionAnchor = rectangles.last().translated(
                            guard->page->mapTo(guard, QPoint(0, guard->page->topHeight())));
                guard->paintAnnotationDraft();
                if (after)
                    after();
            },
            std::move(reject));
}

void Reader::prepareAnnotation(bool leave, std::function<void()> after)
{
    annotationPopover->hide();
    annotationSaving = true;
    QPointer<Reader> guard(this);
    annotationDraft->prepareSave([guard, leave, after](AnnotationDraft::Prepared result, quint64) {
        if (!guard)
            return;
        guard->annotationSaving = false;
        saves::settled();
        if (!guard->annotationMode || result == AnnotationDraft::Prepared::Stale)
            return;
        if (result == AnnotationDraft::Prepared::Rejected) {
            guard->annotationShowActionsPending = true;
            guard->annotationFrame.clear();
            guard->syncAnnotations();
            return;
        }
        guard->syncMarkupRequest();
        guard->saveAnnotation(leave, after);
    });
}

QPoint Reader::selectionLinkPoint() const
{
    for (const auto &link : session->pageLinks()) {
        for (const auto &rect : link.rectangles)
            for (const auto &selection : annotationDraft->geometry().rectangles)
                if (rect.intersects(selection))
                    return link.rectangles.first().center();
    }
    return { -1, -1 };
}

void Reader::showAnnotationActions()
{
    QRect bounds;
    for (const auto &rect : annotationDraft->geometry().rectangles)
        bounds = bounds.united(rect);
    if (!bounds.isEmpty())
        bounds.adjust(-ui::px(24), -ui::px(24), ui::px(24), ui::px(24));
    bounds.translate(page->mapTo(this, QPoint(0, page->topHeight())));
    if (bounds.isEmpty())
        bounds = topControls->geometry();
    if (!annotationMode) {
        QVector<AnnotationAction> actions{ { "rrSelectionLookup", ui::ControlIcon::Dictionary,
                                             strings::get("annotations.look_up"), [this] {
                                                 annotationPopover->hide();
                                                 lookupSelection();
                                             } } };
        const auto target = selectionLinkPoint();
        if (target.x() >= 0) {
            actions.insert(1,
                           { "rrSelectionLink", ui::ControlIcon::Link,
                             strings::get("reader.follow_link"), [this, target] {
                                 clearAnnotationDraft();
                                 openLink(target);
                             } });
        }
        annotationActionsFor(actions, bounds);
        return;
    }
    QVector<AnnotationAction> actions{ { "rrAnnotationSave", ui::ControlIcon::SaveHighlight,
                                         strings::get(annotationDraft->editingId().isEmpty()
                                                              ? "annotations.mark_text"
                                                              : "annotations.apply_changes"),
                                         [this] { prepareAnnotation(); } } };
    const bool quickDraft = quickAnnotations() && annotationDraft->editingId().isEmpty();
    if (annotationDraft->editingId().isEmpty()) {
        // Add note saves the highlight first, then writes the note on it.
        actions.append({ "rrAnnotationAddNote", ui::ControlIcon::Note,
                         strings::get("annotations.add_note"), [this] {
                             QSet<QString> before;
                             for (const auto &item : highlights->highlights())
                                 before.insert(item.id);
                             prepareAnnotation(false, [this, before] {
                                 syncAnnotations();
                                 for (const auto &item : highlights->highlights())
                                     if (!before.contains(item.id)) {
                                         openNoteEditor(item.id);
                                         return;
                                     }
                             });
                         } });
    }
    if (!annotationDraft->editingId().isEmpty()) {
        // Adjusting a saved highlight: the note action applies the new range,
        // then opens the note, so the order matches a new selection.
        const auto id = annotationDraft->editingId();
        bool noted = false;
        for (const auto &item : highlights->highlights())
            if (item.id == id)
                noted = hasHighlightNote(item);
        actions.append({ noted ? "rrAnnotationEditNote" : "rrAnnotationAddNote",
                         ui::ControlIcon::Note,
                         strings::get(noted ? "annotations.edit_note" : "annotations.add_note"),
                         [this, id] {
                             prepareAnnotation(false, [this, id] {
                                 syncAnnotations();
                                 openNoteEditor(id);
                             });
                         } });
    }
    if (quickDraft) {
        // Quick annotation mode also offers the actions of an ordinary
        // selection. Both leave annotation mode first, as the hold had not
        // entered it in toggleable mode.
        actions.append({ "rrSelectionLookup", ui::ControlIcon::Dictionary,
                         strings::get("annotations.look_up"), [this] {
                             annotationPopover->hide();
                             annotationMode = false;
                             annotationColor = annotationSelectedColor;
                             annotationStyle = annotationSelectedStyle;
                             annotationColorChosen = annotationStyleChosen = false;
                             syncMarkupRequest();
                             lookupSelection();
                         } });
        const auto target = selectionLinkPoint();
        if (target.x() >= 0)
            actions.append({ "rrSelectionLink", ui::ControlIcon::Link,
                             strings::get("reader.follow_link"), [this, target] {
                                 clearAnnotationDraft();
                                 leaveAnnotations(false);
                                 openLink(target);
                             } });
    }
    annotationActionsFor(
            actions, bounds, true, annotationColor,
            [this](QString color) {
                annotationColor = color;
                annotationColorChosen = true;
                annotationDraft->markDirty();
                syncAnnotationAppearance();
            },
            annotationStyle,
            [this](QString style) {
                annotationStyle = style;
                annotationStyleChosen = true;
                annotationDraft->markDirty();
                syncAnnotationAppearance();
            });
    syncAnnotationAppearance();
}

void Reader::syncAnnotationAppearance()
{
    if (auto *picker = dynamic_cast<ui::HighlightStylePicker *>(
                annotationPopover->findChild<QWidget *>("rrAnnotationStyles"))) {
        picker->setSelectedStyle(annotationStyle);
        picker->setColor(colorAnnotations() ? annotationColor : QString("gray"));
    }
}

} // namespace kobalt::reader

namespace kobalt::reader {
void Reader::saveAnnotation(bool leave, std::function<void()> after)
{
    if (annotationDraft->range().start.isEmpty() || annotationDraft->range().end.isEmpty()) {
        return;
    }
    annotationPopover->hide();
    annotationSaving = true;
    annotationDraft->invalidate();
    const auto transaction = annotationDraft->saveRevision();
    QPointer<Reader> guard(this);
    highlights->save(
            annotationDraft->range().start, annotationDraft->range().end,
            annotationDraft->editingId(),
            [guard, transaction, leave, after](bool saved) {
                if (!guard) {
                    return;
                }
                guard->annotationSaving = false;
                saves::settled();
                if (!guard->annotationMode
                    || !guard->annotationDraft->finishSave(transaction, saved)) {
                    guard->annotationShowActionsPending = false;
                    guard->annotationFrame.clear();
                    guard->syncAnnotations();
                    return;
                }
                if (!saved) {
                    guard->annotationShowActionsPending = true;
                    guard->annotationFrame.clear();
                    guard->syncAnnotations();
                    return;
                }
                guard->annotationSelectedColor = guard->annotationColor;
                guard->annotationSelectedStyle = guard->annotationStyle;
                guard->annotationShowActionsPending = false;
                guard->annotationColor = guard->annotationSelectedColor;
                guard->annotationStyle = guard->annotationSelectedStyle;
                guard->annotationColorChosen = guard->annotationStyleChosen = false;
                guard->page->setAnnotationHandles({});
                guard->annotationActionAnchor = {};
                guard->syncMarkupRequest();
                guard->annotationFrame.clear();
                if (after && guard->isVisible() && !guard->lifecycleHidden
                    && !guard->services.suspended()) {
                    after();
                } else if (leave) {
                    guard->leaveAnnotations();
                } else {
                    guard->syncAnnotations();
                }
                guard->settleQuickAnnotations();
            },
            annotationDraft->range().chosenEnd, annotationDraft->endpoints().endPolicy,
            annotationDraft->range().chosenStart, annotationDraft->endpoints().startPolicy,
            colorAnnotations() && (annotationDraft->editingId().isEmpty() || annotationColorChosen)
                    ? annotationColor
                    : QString(),
            annotationDraft->editingId().isEmpty() || annotationStyleChosen ? annotationStyle
                                                                            : QString());
}

void Reader::updateAnnotationRects(bool showActions)
{
    if (annotationDraft->range().start.isEmpty()) {
        annotationDraft->hideRectangles();
        page->annotate({}, savedHighlightRects);
        return;
    }
    QPointer<Reader> guard(this);
    annotationDraft->refreshGeometry([guard, showActions] {
        if (!guard)
            return;
        guard->paintAnnotationDraft();
        if (showActions)
            guard->showAnnotationActions();
    });
}

bool Reader::annotationMenuSelectionEnabled() const
{
    return annotationMode && topControls->isVisible() && (navigation || bottomControls->isVisible())
            && !settingsVisible() && !appearanceEdit.panelOpen() && !choices->isVisible()
            && !chapterGraph->isVisible() && !bookmarkList->isVisible() && !searchPanel->isVisible()
            && !dictionaryPanel->isVisible() && !lightPopup->isVisible();
}

void Reader::syncAnnotationControls()
{
    if (!annotationPopover)
        return;
    toolbarComposition.annotate->setChecked(annotationMode);
    const bool menuOpen = topControls->isVisible();
    const bool readingOnly = !menuOpen && !settingsVisible() && !appearanceEdit.panelOpen()
            && !choices->isVisible() && !chapterGraph->isVisible() && !bookmarkList->isVisible()
            && !searchPanel->isVisible() && !dictionaryPanel->isVisible()
            && !lightPopup->isVisible();
    const bool selectionEnabled = annotationMenuSelectionEnabled();
    page->setAnnotationInputEnabled(readingOnly || dictionaryPanel->isVisible()
                                    || selectionEnabled);
    page->setAnnotationHandles(annotationDraft->geometry());
    syncPopoverExclusions();
}

void Reader::syncMarkupRequest()
{
    const quint64 generation = highlights->generation();
    const auto &editingId = annotationDraft->editingId();
    const auto &items = highlights->highlights();
    const bool inColor = !items.isEmpty() && colorAnnotations();
    const bool savedChanged = !savedMarkupReady || savedMarkupGeneration != generation
            || savedMarkupEditingId != editingId || savedMarkupInColor != inColor;
    const auto &start = annotationDraft->range().start;
    const QString end = start.isEmpty() ? QString() : annotationDraft->end();
    // A new appearance preview inherits the current request in ReaderSession::openPreview.
    if (!savedChanged && annotationRequest.draftStart == start && annotationRequest.draftEnd == end)
        return;

    if (savedChanged) {
        QVector<ReaderMarkupRange> saved;
        saved.reserve(items.size());
        for (const auto &item : items) {
            if (item.id != editingId) {
                saved.append({ item.start, item.end,
                               inColor ? highlightDisplayColor(item.color) : QStringLiteral("gray"),
                               highlightDisplayStyle(item.style) });
            }
        }
        // A note edit changes the highlight generation but keeps the displayed ranges.
        if (annotationRequest.saved != saved)
            annotationRequest.saved = std::move(saved);
        savedMarkupReady = true;
        savedMarkupGeneration = generation;
        savedMarkupEditingId = editingId;
        savedMarkupInColor = inColor;
    }
    annotationRequest.draftStart = start;
    annotationRequest.draftEnd = end;
    session->setMarkup(annotationRequest);
    if (auto *preview = appearanceEdit.preview()) {
        preview->setMarkup(annotationRequest);
    }
}

void Reader::preparePresentation(std::function<void(bool)> finished)
{
    DiagnosticOperation setup("reader-presentation-setup");
    auto waiting = std::make_shared<DiagnosticOperation>("reader-presentation-wait");
    finished = [waiting, finished = std::move(finished)](bool ready) { finished(ready); };
    prepareKosyncPresentation([this, finished = std::move(finished)](bool ready) mutable {
        if (!ready) {
            finished(false);
            return;
        }
        prepareAnnotationPresentation([this, finished = std::move(finished)](bool ready) {
            if (!ready) {
                finished(false);
                return;
            }
            const auto present = [this, finished] {
                book->markNativeReading();
                saveWifiPower([this, finished] { finished(!closing && !lifecycleHidden); });
            };
            present();
        });
    });
}

void Reader::prepareAnnotationPresentation(std::function<void(bool)> finished)
{
    DiagnosticOperation setup("reader-annotation-presentation");
    syncMarkupRequest();
    if (annotationRequest.saved.isEmpty()) {
        finished(true);
        return;
    }
    annotationPresentationReady = std::move(finished);
    refresh();
}

void Reader::finishAnnotationPresentation()
{
    if (!annotationPresentationReady || annotationPresentationCapture) {
        return;
    }
    if (closing || lifecycleHidden || !session->error().isEmpty()) {
        auto finished = std::exchange(annotationPresentationReady, {});
        finished(false);
        return;
    }
    if (session->busy() || session->image().isNull()) {
        return;
    }
    syncMarkupRequest();
    const auto &markup = session->markup();
    if (markup.imageKey == session->image().cacheKey()
        && markup.request.saved == annotationRequest.saved) {
        syncAnnotations();
        auto finished = std::exchange(annotationPresentationReady, {});
        finished(true);
        return;
    }
    annotationPresentationCapture = true;
    QPointer<Reader> guard(this);
    session->captureMarkup([guard](bool ready) {
        if (!guard) {
            return;
        }
        guard->annotationPresentationCapture = false;
        if (!ready) {
            if (guard->session->busy()) {
                return;
            }
            auto finished = std::exchange(guard->annotationPresentationReady, {});
            if (finished) {
                finished(false);
            }
            return;
        }
        guard->refresh();
        if (guard) {
            guard->finishAnnotationPresentation();
        }
    });
}

QString Reader::annotationViewportKey() const
{
    // Preview frames have separate geometry and cannot start a lookup.
    if (appearanceEdit.panelOpen())
        return "preview:" + QString::number(page->image.cacheKey());
    // Book page estimates can change without replacing the visible text layout.
    const auto &state = session->state();
    const auto slice = state.value("slice_start").toString();
    return QString::number(session->layoutRevision()) + ':' + slice + ':'
            + QString::number(session->page()) + ':' + state.value("position").toString();
}

void Reader::syncAnnotations()
{
    syncAnnotationControls();
    if (!annotationPopover || session->busy()) {
        return;
    }
    // A hidden loading panel cancels its request. Wake can reveal it again
    // without opening a tab or changing the displayed page.
    if (bookmarkList->isVisible() && bookmarkList->annotationsShown() && !highlights->sync())
        bookmarkList->showAnnotations({}, true);

    if (annotationMode) {
        if (annotationPopover->isVisible()) {
            annotationPopover->raise();
        }
    }
    const QString frame = annotationViewportKey();
    if (annotationDraft->setViewport(frame)) {
        annotationActionAnchor = {};
        annotationPopover->hide();
    }
    if (annotationDrag.turn && annotationDrag.active() && !annotationDrag.loadingEdges) {
        annotationDrag.loadingEdges = true;
        qt::later(0, this, [this] {
            if (annotationDrag.active() && annotationDrag.turn && !session->busy())
                loadAnnotationDragEdges();
        });
    }
    syncMarkupRequest();
    const QString tintFrame = frame + (colorAnnotations() ? ":color" : ":mono");
    const auto imageKey = page->image.cacheKey();
    // Decoding an image changes pixels, not text geometry. Keep a pending
    // selection and its actions; the viewport key above invalidates stale text.
    const ReaderSession *displayed = session;
    if (const auto *preview = appearanceEdit.preview()) {
        if (preview->image().cacheKey() == imageKey) {
            displayed = preview;
        }
    }
    const auto &markup = displayed->markup();
    const bool canShowActions =
            annotationMode && isVisible() && !lifecycleHidden && !services.suspended();
    const bool showActions = canShowActions && annotationShowActionsPending;
    const bool unchangedMarkup =
            annotationFrame != tintFrame && markup.request == annotationRequest;
    if ((annotationImageKey != imageKey || annotationPresentationReady || unchangedMarkup)
        && markup.imageKey == imageKey && markup.request.saved == annotationRequest.saved) {
        const bool unchangedViewport = annotationFrame == tintFrame;
        ++savedHighlightEpoch;
        annotationImageKey = imageKey;
        annotationFrame = tintFrame;
        savedHighlightRects.clear();
        for (const auto &group : markup.saved) {
            savedHighlightRects += group.rectangles;
        }
        annotationDraft->acceptFrame(markup.request.draftStart, markup.request.draftEnd,
                                     markup.draft, unchangedViewport);
        page->installAnnotationFrame(annotationDraft->geometry().rectangles, markup.saved,
                                     annotationDraft->geometry().vertical,
                                     &annotationDraft->geometry());
        if (showActions) {
            annotationShowActionsPending = false;
            showAnnotationActions();
        }
        return;
    }
    annotationImageKey = imageKey;
    if (annotationPresentationReady) {
        return;
    }
    if (annotationFrame == tintFrame) {
        if (showActions) {
            annotationShowActionsPending = false;
            updateAnnotationRects(true);
        }
        return;
    }
    if (!highlights->sync()) {
        return;
    }
    annotationFrame = tintFrame;
    if (showActions) {
        annotationShowActionsPending = false;
    }
    updateAnnotationRects(showActions);
    savedHighlightRects.clear();
    annotationDraft->hideRectangles();
    page->annotate({}, {});
    const auto epoch = ++savedHighlightEpoch;
    QVector<QPair<QString, QString>> ranges;
    QVector<QString> colors, styles;
    const auto &items = highlights->highlights();
    const auto locations = highlights->locations();
    for (int i = 0; i < items.size(); ++i) {
        const int start = locations.pages.value(i * 2, -1),
                  end = locations.pages.value(i * 2 + 1, -1);
        if (items[i].id != annotationDraft->editingId() && start >= 0 && start <= session->page()
            && end >= session->page()) {
            ranges.append(qMakePair(items[i].start, items[i].end));
            colors.append(colorAnnotations() ? highlightDisplayColor(items[i].color)
                                             : QString("gray"));
            styles.append(highlightDisplayStyle(items[i].style));
        }
    }
    if (ranges.isEmpty()) {
        return;
    }
    QPointer<Reader> guard(this);
    session->rangeRectGroups(ranges,
                             [guard, epoch, colors, styles](QVector<QVector<QRect>> groups) {
                                 if (!guard || guard->savedHighlightEpoch != epoch) {
                                     return;
                                 }
                                 QVector<QRect> rects;
                                 QVector<HighlightGeometry> tinted;
                                 for (int i = 0; i < groups.size(); ++i) {
                                     rects += groups[i];
                                     tinted.append({ groups[i], colors.value(i, "gray"),
                                                     styles.value(i, "shaded") });
                                 }
                                 guard->savedHighlightRects = rects;
                                 guard->paintAnnotationDraft();
                                 guard->page->setSavedHighlights(tinted);
                             });
}

QVector<BookmarkRow> Reader::annotationRows() const
{
    QVector<BookmarkRow> rows;
    const auto &items = highlights->highlights();
    const auto locations = highlights->locations();
    rows.reserve(items.size());
    for (int i = 0; i < items.size(); ++i) {
        const auto &item = items[i];
        BookmarkRow row{
            item.id, {}, {}, item.created, locations.bookPages.value(i * 2, -1), false
        };
        row.endPage = locations.bookPages.value(i * 2 + 1, -1);
        row.pagePending = session->state().contains("book_fraction")
                && session->state().value("rough_pages").toInt() <= 0;
        row.style = item.style;
        row.color = item.color;
        row.sourceOrder = locations.order.value(i * 2, -1);
        row.hasNote = hasHighlightNote(item);
        rows.append(row);
    }
    return rows;
}

void Reader::editAnnotation(const QString &id, bool navigate)
{
    if (annotationDraft->dirty() && !annotationDraft->range().start.isEmpty()
        && annotationDraft->editingId() != id) {
        QRect anchor =
                annotationActionAnchor.isEmpty() ? topControls->geometry() : annotationActionAnchor;
        annotationActionsFor({ { "rrAnnotationSwitchSave", ui::ControlIcon::SaveHighlight,
                                 strings::get("annotations.save_then_edit"),
                                 [this, id, navigate] {
                                     prepareAnnotation(false, [this, id, navigate] {
                                         editAnnotation(id, navigate);
                                     });
                                 } },
                               { "rrAnnotationSwitchDiscard", ui::ControlIcon::ClearSelection,
                                 strings::get("annotations.discard_then_edit"),
                                 [this, id, navigate] {
                                     clearAnnotationDraft();
                                     editAnnotation(id, navigate);
                                 } } },
                             anchor);
        annotationPopover->findChild<QPushButton *>("rrAnnotationSwitchSave")
                ->setEnabled(!annotationDraft->range().end.isEmpty());
        return;
    }
    for (const auto &item : highlights->highlights()) {
        if (item.id != id) {
            continue;
        }
        Highlight detail;
        if (!highlights->detail(id, &detail)) {
            annotationActionsFor({}, topControls->geometry());
            return;
        }
        bookmarkList->hide();
        enterAnnotations();
        annotationDraft->edit(item);
        annotationColorChosen = annotationStyleChosen = false;
        annotationColor = item.color;
        annotationStyle = highlightDisplayStyle(item.style);
        annotationShowActionsPending = false;
        annotationPopover->hide();
        if (navigate) {
            session->goTo(item.start);
        }
        syncMarkupRequest();
        annotationFrame.clear();
        syncAnnotations();
        return;
    }
}

void Reader::showSavedAnnotation(const QString &id, QRect anchor, bool navigate)
{
    if (!features::enabled(preferences, features::Annotations))
        return;
    const auto epoch = annotationDraft->generation();
    for (const auto &item : highlights->highlights()) {
        if (item.id != id)
            continue;
        QPointer<Reader> guard(this);
        session->annotationGeometry(
                item.start, item.end,
                [guard, epoch, id, anchor, navigate](AnnotationGeometry geometry) {
                    if (!guard || guard->annotationDraft->generation() != epoch
                        || guard->lifecycleHidden || guard->services.suspended())
                        return;
                    QRect bounds;
                    for (const auto &rect : geometry.rectangles)
                        bounds = bounds.united(rect);
                    if (!bounds.isEmpty())
                        bounds.translate(
                                guard->page->mapTo(guard, QPoint(0, guard->page->topHeight())));
                    guard->showSavedAnnotationAt(id, bounds.isEmpty() ? anchor : bounds, navigate);
                });
        return;
    }
}

void Reader::showSavedAnnotationAt(const QString &id, QRect anchor, bool navigate)
{
    if (!features::enabled(preferences, features::Annotations))
        return;
    const Highlight *item = nullptr;
    for (const auto &row : highlights->highlights())
        if (row.id == id)
            item = &row;
    if (!item)
        return;
    if (!hasHighlightNote(*item)) {
        showSavedAppearance(id, anchor, navigate, false);
        return;
    }
    // A highlight with a note shows the note and its actions. Its color and
    // style wait behind Color.
    Highlight detail;
    if (!highlights->detail(id, &detail)) {
        annotationActionsFor({}, anchor);
        return;
    }
    annotationColor = item->color;
    annotationStyle = highlightDisplayStyle(item->style);
    annotationActionsFor(
            { { "rrAnnotationEditNote", ui::ControlIcon::Note,
                strings::get("annotations.edit_note"), [this, id] { openNoteEditor(id); } },
              { "rrAnnotationAppearance", ui::ControlIcon::Styling,
                strings::get(colorAnnotations() ? "annotations.color" : "annotations.style"),
                [this, id, anchor, navigate] { showSavedAppearance(id, anchor, navigate, true); } },
              { "rrAnnotationEdit", ui::ControlIcon::EditHighlight,
                strings::get("annotations.edit"),
                [this, id, navigate, anchor] {
                    annotationPopover->hide();
                    annotationActionAnchor = anchor;
                    editAnnotation(id, navigate);
                } },
              { "rrAnnotationRemove", ui::ControlIcon::RemoveHighlight,
                strings::get("annotations.remove"),
                [this, id] {
                    annotationPopover->hide();
                    removeAnnotation(id);
                } } },
            anchor, true, {}, {}, {}, {}, detail.note);
}

void Reader::showSavedAppearance(const QString &id, QRect anchor, bool navigate, bool colorStep)
{
    for (const auto &item : highlights->highlights()) {
        if (item.id == id) {
            annotationColor = item.color;
            annotationStyle = highlightDisplayStyle(item.style);
            break;
        }
    }
    QVector<AnnotationAction> actions{
        { "rrAnnotationApply", ui::ControlIcon::SaveHighlight,
          strings::get("annotations.apply_changes"),
          [this, id, anchor] {
              if (highlights->updateAppearance(id, colorAnnotations() ? annotationColor : QString(),
                                               annotationStyle)) {
                  annotationSelectedColor = annotationColor;
                  annotationSelectedStyle = annotationStyle;
                  annotationPopover->hide();
                  annotationFrame.clear();
                  syncAnnotations();
                  settleQuickAnnotations();
              } else {
                  auto *picker = dynamic_cast<ui::HighlightStylePicker *>(
                          annotationPopover->findChild<QWidget *>("rrAnnotationStyles"));
                  if (picker) {
                      picker->setError(strings::get("annotations.save_failed"), annotationStyle);
                      auto *card = annotationPopover->card();
                      card->rows()->activate();
                      card->fitToWidth(card->width());
                      annotationPopover->anchorTo(anchor);
                  }
              }
          } }
    };
    if (colorStep) {
        // Discard leaves the choices unsaved and returns to the note.
        actions.append(
                { "rrAnnotationDiscard", ui::ControlIcon::Undo, strings::get("annotations.discard"),
                  [this, id, anchor, navigate] { showSavedAnnotationAt(id, anchor, navigate); } });
    } else {
        actions.append({ "rrAnnotationAddNote", ui::ControlIcon::Note,
                         strings::get("annotations.add_note"),
                         [this, id] { openNoteEditor(id); } });
        actions.append({ "rrAnnotationEdit", ui::ControlIcon::EditHighlight,
                         strings::get("annotations.edit"), [this, id, navigate, anchor] {
                             annotationPopover->hide();
                             annotationActionAnchor = anchor;
                             const auto color = annotationColor, style = annotationStyle;
                             editAnnotation(id, navigate);
                             annotationColor = color;
                             annotationStyle = style;
                             annotationColorChosen = annotationStyleChosen = true;
                         } });
        actions.append({ "rrAnnotationRemove", ui::ControlIcon::RemoveHighlight,
                         strings::get("annotations.remove"), [this, id] {
                             annotationPopover->hide();
                             removeAnnotation(id);
                         } });
    }
    annotationActionsFor(
            actions, anchor, true, annotationColor,
            [this](QString color) {
                annotationColor = color;
                syncAnnotationAppearance();
            },
            annotationStyle,
            [this](QString style) {
                annotationStyle = style;
                syncAnnotationAppearance();
            });
    syncAnnotationAppearance();
}

void Reader::openNoteEditor(const QString &id)
{
    if (noteEditor)
        delete noteEditor.data();
    Highlight detail;
    if (!highlights->detail(id, &detail)) {
        annotationActionsFor({}, topControls->geometry());
        return;
    }
    annotationPopover->hide();
    bookmarkList->setExcerptFont(bookFace());
    const NotePassage passage{ detail.text, detail.chapter, highlightDisplayStyle(detail.style),
                               colorAnnotations() ? highlightDisplayColor(detail.color)
                                                  : QStringLiteral("gray"),
                               bookmarkList->excerptFont() };
    noteEditor = new NoteEditor(
            this, services.keyboard,
            strings::get(detail.note.isEmpty() ? "annotations.add_note" : "annotations.edit_note"),
            passage, detail.note);
    noteEditor->saveRequested = [this, id](const QString &text) {
        if (text.size() > HighlightStore::maxNoteUnits)
            noteEditor->setError(strings::get("annotations.note_too_long"));
        else if (!highlights->setNote(id, text, true))
            noteEditor->setError(strings::get("annotations.note_save_failed"));
        else if (!highlights->saving())
            noteEditor->saved();
    };
    noteEditor->closed = [this] {
        if (noteEditor)
            noteEditor->deleteLater();
        noteEditor.clear();
        annotationFrame.clear();
        syncAnnotations();
        settleQuickAnnotations();
    };
}

void Reader::showAnnotationMatches(QVector<QString> ids, QRect anchor, int listPage)
{
    const int reservedRows =
            annotationMode ? (annotationDraft->range().start.isEmpty() ? 4 : 5) : 3;
    const int pageSize = qBound(1,
                                (height() - 2 * int(metrics::notice::sideClearance))
                                                / int(metrics::links::actionHeight)
                                        - reservedRows,
                                3);
    const int pages = qMax(1, (ids.size() + pageSize - 1) / pageSize);
    listPage = qBound(0, listPage, pages - 1);
    QVector<AnnotationAction> actions;
    QStringList requested;
    for (int i = listPage * pageSize; i < qMin(ids.size(), (listPage + 1) * pageSize); ++i)
        requested.append(ids[i]);
    QVector<HighlightExcerpt> excerpts;
    if (!highlights->excerpts(requested, &excerpts)) {
        annotationActionsFor({}, anchor);
        return;
    }
    for (int i = listPage * pageSize; i < qMin(ids.size(), (listPage + 1) * pageSize); ++i) {
        for (const auto &item : excerpts) {
            if (item.id != ids[i]) {
                continue;
            }
            const auto excerpt = strings::get("common.numbered_title")
                                         .arg(i + 1)
                                         .arg(elideBookmarkExcerpt(item.text.simplified(), 36, 36));
            const QString name = QString("rrAnnotationMatch%1").arg(i);
            const QString id = item.id;
            actions.append({ name, ui::ControlIcon::Annotate, excerpt,
                             [this, id, anchor] { showSavedAnnotation(id, anchor); } });
        }
    }
    if (pages > 1) {
        actions.append({ "rrAnnotationMatchPrevious", ui::ControlIcon::Previous,
                         strings::get("annotations.previous_matches"),
                         [this, ids, anchor, listPage, pages] {
                             showAnnotationMatches(ids, anchor, (listPage + pages - 1) % pages);
                         } });
        actions.append({ "rrAnnotationMatchNext", ui::ControlIcon::Next,
                         strings::get("annotations.next_matches"),
                         [this, ids, anchor, listPage, pages] {
                             showAnnotationMatches(ids, anchor, (listPage + 1) % pages);
                         } });
    }
    annotationActionsFor(actions, anchor);
}

void Reader::removeAnnotation(const QString &id)
{
    if (removeDialog) {
        return;
    }
    QVector<HighlightExcerpt> details;
    if (!highlights->excerpts({ id }, &details) || details.size() != 1) {
        annotationActionsFor({}, topControls->geometry());
        return;
    }
    QString excerpt = details.first().text;
    int number = -1;
    for (const auto &row : annotationRows()) {
        if (row.position == id) {
            number = row.page;
            break;
        }
    }
    if (excerpt.isEmpty())
        return;
    excerpt = elideBookmarkExcerpt(excerpt, 40, 80);
    const auto detail = number >= 0
            ? strings::get("bookmarks.remove_detail").arg(number + 1).arg(excerpt)
            : excerpt;
    auto *choice = new ui::ChoiceModal(
            this, { detail, strings::get("annotations.remove_prompt"), {} },
            { strings::get("common.cancel"), "rrKeepAnnotation" },
            { strings::get("annotations.remove"), "rrConfirmRemoveAnnotation" });
    removeDialog = choice;
    choice->setObjectName("rrRemoveAnnotation");
    auto *text = choice->descriptionLabel();
    text->setObjectName("rrRemoveAnnotationDetail");
    ui::readerStyle(text,
                    ui::styles::font(metrics::chrome::descriptionText)
                            + "color:palette(placeholder-text);");
    const auto finish = [this, id, choice](bool remove) {
        choice->hide();
        choice->deleteLater();
        removeDialog.clear();
        if (remove) {
            highlights->remove(id);
            // The session's changed callback updates the list and markup once.
        }
        settleQuickAnnotations();
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [finish] { finish(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [finish] { finish(true); });
    choice->show();
    choice->raise();
}
} // namespace kobalt::reader
