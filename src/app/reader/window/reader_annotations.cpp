#include "ui/controls/highlight_palette.h"
#include "reader_p.h"

#include "ui/style/styles.h"
#include "ui/dialogs/action_menu.h"
#include "platform/api/kobo_refresh.h"
#include "support/qt_font.h"
#include "support/strings.h"
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
    notice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    notice->centerCard();
    notice->show();
    notice->raise();
}

void Reader::paintAnnotationDraft()
{
    page->annotate(annotationDraft->geometry().rectangles, savedHighlightRects, {},
                   annotationDraft->geometry().vertical);
    page->setAnnotationHandles(annotationDraft->geometry());
    annotationPopover->setInputExclusions(page->annotationHandleHitRegion().translated(
            page->mapTo(this, QPoint()) - annotationPopover->pos()));
}

void Reader::buildAnnotations()
{
    annotationPopover = new ui::ModalOverlay(this);
    annotationPopover->setObjectName("rrAnnotationPopover");
    annotationPopover->dismissed = [this] {
        if (!annotationMode) {
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

void Reader::enterAnnotations()
{
    if (session->busy() || !highlights->writable() || appearanceEdit.panelOpen()
        || general->isVisible()) {
        return;
    }
    const bool changed = !annotationMode;
    dismissLookup();
    dismissControls();
    annotationMode = true;
    page->setAnnotationInputEnabled(true);
    syncMarkupRequest();
    syncAnnotations();
    if (changed)
        showAnnotationModeNotice();
}

void Reader::leaveAnnotations()
{
    const bool changed = annotationMode;
    const bool discardSelection =
            !annotationDraft->range().start.isEmpty() || !annotationDraft->editingId().isEmpty();
    annotationMode = false;
    annotationDragAwaitingRelease = false;
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
    if (changed)
        dismissControls();
    refresh();
    if (changed)
        showAnnotationModeNotice();
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
                                  std::function<void(QString)> styleChanged)
{
    paintAnnotationDraft();
    annotationPopover->hide();
    auto *card = annotationPopover->card();
    auto *rows = card->rows();
    while (auto *item = rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (highlights->error() == "overlap" && !annotationDraft->range().start.isEmpty())
        card->message(strings::get("annotations.overlap"), "rrAnnotationOverlap");
    if (!highlights->writable() && !highlights->error().isEmpty())
        card->message(strings::get("annotations.load_failed"), "rrAnnotationLoadError");
    int desiredWidth = metrics::links::popupWidth;
    const auto section = [card, rows](const char *name, const char *key, bool divider = false) {
        auto *header = new ui::Panel(card);
        header->setDividers(ui::Edges(Qt::BottomEdge)
                            | (divider ? ui::Edges(Qt::TopEdge) : ui::Edges{}));
        header->setObjectName(name);
        header->setFixedHeight(ui::px(40));
        auto *layout = new QHBoxLayout(header);
        layout->setContentsMargins(metrics::chrome::inset, 0, metrics::chrome::inset, 0);
        auto *label = new ui::InterfaceLabel(strings::get(key), header);
        ui::captionLabel(label);
        layout->addWidget(label);
        rows->addWidget(header);
    };
    if (colorAnnotations() && colorChanged) {
        section("rrAnnotationColorSection", "annotations.color_section");
        auto *palette = new ui::HighlightPalette(paletteColor, colorChanged, card);
        rows->addWidget(palette);
        palette->show();
        desiredWidth = qMax(desiredWidth, ui::px(700));
    }
    if (styleChanged) {
        section("rrAnnotationStyleSection", "annotations.style_section");
        rows->addWidget(new ui::HighlightStylePicker(selectedStyle, styleChanged, card));
        desiredWidth = qMax(desiredWidth, ui::px(700));
    }
    auto menuActions = actions;
    if (!annotationDraft->range().start.isEmpty()) {
        menuActions.append({ "rrAnnotationCancel", ui::ControlIcon::ClearSelection,
                             strings::get("annotations.cancel_selection"),
                             [this] { clearAnnotationDraft(); } });
    }
    if (compactActions && annotationMode && minimalAnnotationMode()) {
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
    const bool compact = compactActions && menuActions.size() <= 4;
    if (styleChanged)
        section("rrAnnotationActionsSection", "annotations.actions_section", true);
    QGridLayout *buttons = nullptr;
    const int columns = menuActions.size() == 4 ? 2 : menuActions.size();
    if (compact) {
        int labelWidth = 0;
        const auto font = ui::interfaceFont(ui::px(23));
        for (const auto &action : menuActions)
            labelWidth = qMax(labelWidth, ui::interfaceTextWidth(action.label, font));
        // Qt can elide at the rounded text advance. Leave room for fractional glyph widths.
        desiredWidth = qMax(desiredWidth,
                            columns * (labelWidth + ui::px(4) + 2 * ui::px(16))
                                    + 2 * int(metrics::control::border));
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
            ui::Edges edges;
            if (!styleChanged || buttons->count() >= columns)
                edges |= Qt::TopEdge;
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
    card->fitToWidth(qMin(desiredWidth, width() - 2 * int(metrics::notice::sideClearance)));
    annotationPopover->anchorTo(anchor);
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
    dismissLookup(true);
    annotationDragAwaitingRelease = false;
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
    if (topControls->isVisible())
        dismissControls();
    dismissLookup(true);
    if (boundary < 0)
        clearAnnotationDraft();
    annotationDraft->invalidate();
    annotationPopover->hide();
    annotationDragging = true;
    annotationDragPending = annotationDragReleased = false;
    annotationDragBoundary = boundary;
    annotationHandleDragging = boundary >= 0;
    annotationDragTurn = 0;
    annotationDragEdges = {};
    annotationDragAwaitingRelease = false;
    annotationDragOrigin = annotationDragPoint = point;
    if (annotationHandleDragging)
        loadAnnotationDragEdges();
    else
        requestAnnotationDrag();
    return true;
}

void Reader::loadAnnotationDragEdges()
{
    annotationDragLoadingEdges = true;
    annotationDragPending = true;
    const auto epoch = annotationDraft->generation();
    QPointer<Reader> guard(this);
    session->annotationPageEdges([guard, epoch](AnnotationPageEdges edges) {
        if (!guard || guard->annotationDraft->generation() != epoch || !guard->annotationDragging)
            return;
        guard->annotationDragLoadingEdges = false;
        guard->annotationDragEdges = edges;
        const int direction = std::exchange(guard->annotationDragTurn, 0);
        guard->annotationDragPending = false;
        if (edges.first.text.isEmpty() || edges.last.text.isEmpty()) {
            guard->annotationDragging = false;
            return;
        }
        if (!direction) {
            guard->requestAnnotationDrag();
            return;
        }
        // Complete this gesture at the adjacent page boundary. Further movement
        // belongs to the old press until the user releases and grabs a handle again.
        guard->annotationDragPending = true;
        const auto token = direction > 0 ? edges.first : edges.last;
        const bool start = guard->annotationDragBoundary == 0;
        guard->resolveAnnotationTokens(
                start ? token : guard->annotationDraft->endpoints().start,
                start ? guard->annotationDraft->endpoints().end : token,
                start ? !token.punctuation : guard->annotationDraft->endpoints().startPolicy,
                start ? guard->annotationDraft->endpoints().endPolicy : !token.punctuation,
                [guard] {
                    guard->annotationDragging = guard->annotationDragPending = false;
                    guard->annotationDragAwaitingRelease = !guard->annotationDragReleased;
                    if (guard->annotationDragReleased && !guard->annotationMode)
                        guard->showAnnotationActions();
                },
                [guard, direction] {
                    // An adjacent saved highlight is a boundary, never an overlapping draft.
                    guard->annotationDragging = guard->annotationDragPending = false;
                    guard->session->turn(-direction);
                });
    });
}

int Reader::annotationDragPageDirection(QPoint point) const
{
    const auto &edges = annotationDragEdges;
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
    if (!annotationDragging || (!annotationMode && !annotationHandleDragging))
        return;
    annotationDragPoint = point;
    requestAnnotationDrag();
}

void Reader::finishAnnotationDrag(bool cancelled)
{
    if (!annotationDragging) {
        if (std::exchange(annotationDragAwaitingRelease, false) && !cancelled && !annotationMode)
            lookupSelection();
        return;
    }
    if (cancelled) {
        annotationDragging = annotationDragPending = false;
        annotationDraft->invalidate();
        paintAnnotationDraft();
        return;
    }
    annotationDragReleased = true;
    requestAnnotationDrag();
}

void Reader::requestAnnotationDrag()
{
    if (!annotationDragging || annotationDragPending || session->busy() || lifecycleHidden
        || services.suspended())
        return;
    if (annotationDragTurn
        || (annotationHandleDragging && annotationDragEdges.first.text.isEmpty()))
        return;
    if (annotationHandleDragging) {
        const int direction = annotationDragPageDirection(annotationDragPoint);
        if (direction != 0) {
            if (!annotationMode
                || !(direction > 0 ? annotationDragEdges.continuesAfter
                                   : annotationDragEdges.continuesBefore)) {
                const auto token =
                        direction > 0 ? annotationDragEdges.last : annotationDragEdges.first;
                const bool start = annotationDragBoundary == 0;
                annotationDragPending = true;
                QPointer<Reader> guard(this);
                const auto stopped = [guard] {
                    if (!guard)
                        return;
                    guard->annotationDragging = guard->annotationDragPending = false;
                    guard->annotationDragAwaitingRelease = !guard->annotationDragReleased;
                    if (guard->annotationDragReleased && !guard->annotationMode)
                        guard->lookupSelection();
                };
                resolveAnnotationTokens(
                        start ? token : annotationDraft->endpoints().start,
                        start ? annotationDraft->endpoints().end : token,
                        start ? !token.punctuation : annotationDraft->endpoints().startPolicy,
                        start ? annotationDraft->endpoints().endPolicy : !token.punctuation,
                        stopped, stopped);
                return;
            }
            annotationDragTurn = direction;
            annotationDragPending = true;
            if (session->turn(direction))
                return;
            annotationDragTurn = 0;
            annotationDragPending = false;
        }
    }
    if (annotationDragBoundary == -2) {
        if (annotationDragReleased) {
            annotationDragging = false;
            const QPoint delta = annotationDragPoint - annotationDragOrigin;
            if (qAbs(delta.x()) >= qMax(100, page->width() / 10)
                && qAbs(delta.x()) > qAbs(delta.y()) * 1.5 && page->swiped)
                page->swiped(delta.x() < 0 ? 1 : -1);
        }
        return;
    }
    annotationDragPending = true;
    const auto epoch = annotationDraft->generation();
    const QPoint point = annotationDragPoint;
    QPointer<Reader> guard(this);
    // At most one hit test and range resolution are in flight. Intermediate
    // pointer positions are replaced by the latest point, including release.
    session->annotationTokenAt(
            point,
            [guard, epoch, point](AnnotationToken token) {
                if (!guard || !guard->annotationDragging
                    || guard->annotationDraft->generation() != epoch || guard->lifecycleHidden
                    || guard->services.suspended())
                    return;
                auto finish = [guard, epoch, point] {
                    if (!guard || guard->annotationDraft->generation() != epoch
                        || !guard->annotationDragging)
                        return;
                    guard->annotationDragPending = false;
                    if (guard->annotationDragBoundary == -2) {
                        guard->requestAnnotationDrag();
                        return;
                    }
                    if (point != guard->annotationDragPoint) {
                        guard->requestAnnotationDrag();
                    } else if (guard->annotationDragReleased) {
                        guard->annotationDragging = false;
                        if (!guard->annotationMode
                            && !guard->annotationDraft->range().start.isEmpty()) {
                            if (!guard->annotationHandleDragging
                                && guard->selectionLinkPoint().x() >= 0)
                                guard->showAnnotationActions();
                            else
                                guard->lookupSelection();
                        }
                    }
                };
                if (token.text.isEmpty()) {
                    // A drag starting on blank paper remains a page gesture. Do not
                    // start a selection later when it happens to cross a word.
                    if (guard->annotationDragBoundary == -1)
                        guard->annotationDragBoundary = -2;
                    finish();
                    return;
                }
                const auto reject = [guard, boundary = guard->annotationDragBoundary, finish] {
                    if (!guard)
                        return;
                    guard->annotationDragBoundary = boundary;
                    finish();
                };
                if (guard->annotationDragBoundary < 0) {
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
                                    || !guard->annotationDragging)
                                    return;
                                if (!hits.isEmpty()) {
                                    guard->annotationDragging = guard->annotationDragPending =
                                            false;
                                    guard->showSavedAnnotation(
                                            ids[hits.first()],
                                            token.rectangles.value(0).translated(guard->page->mapTo(
                                                    guard, QPoint(0, guard->page->topHeight()))));
                                    return;
                                }
                                guard->annotationDragBoundary = 1;
                                guard->resolveAnnotationTokens(token, token, !token.punctuation,
                                                               !token.punctuation, finish, reject);
                            });
                    return;
                }
                const auto endpoints =
                        guard->annotationDraft->moved(token, guard->annotationDragBoundary);
                guard->resolveAnnotationTokens(endpoints.start, endpoints.end,
                                               endpoints.startPolicy, endpoints.endPolicy, finish,
                                               reject);
            },
            annotationDragBoundary == 0           ? annotationDraft->endpoints().start.start
                    : annotationDragBoundary == 1 ? annotationDraft->endpoints().end.end
                                                  : QString(),
            annotationDragBoundary == 0);
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
                if (reversed && guard->annotationDragging)
                    guard->annotationDragBoundary = 1 - guard->annotationDragBoundary;
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
                                             strings::get("reader.link_dictionary"), [this] {
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
            && !general->isVisible() && !appearanceEdit.panelOpen() && !choices->isVisible()
            && !chapterGraph->isVisible() && !bookmarkList->isVisible() && !searchPanel->isVisible()
            && !dictionaryPanel->isVisible() && !lightPopup->isVisible();
}

void Reader::syncAnnotationControls()
{
    if (!annotationPopover)
        return;
    toolbarComposition.annotate->setChecked(annotationMode);
    const bool menuOpen = topControls->isVisible();
    const bool readingOnly = !menuOpen && !general->isVisible() && !appearanceEdit.panelOpen()
            && !choices->isVisible() && !chapterGraph->isVisible() && !bookmarkList->isVisible()
            && !searchPanel->isVisible() && !dictionaryPanel->isVisible()
            && !lightPopup->isVisible();
    const bool selectionEnabled = annotationMenuSelectionEnabled();
    page->setAnnotationInputEnabled(readingOnly || dictionaryPanel->isVisible()
                                    || selectionEnabled);
    page->setAnnotationHandles(annotationDraft->geometry());
    annotationPopover->setInputExclusions(page->annotationHandleHitRegion().translated(
            page->mapTo(this, QPoint()) - annotationPopover->pos()));
}

void Reader::syncMarkupRequest()
{
    ReaderMarkupRequest request;
    for (const auto &item : highlights->highlights()) {
        if (item.id != annotationDraft->editingId()) {
            request.saved.append(
                    { item.start, item.end,
                      colorAnnotations() ? highlightDisplayColor(item.color) : QString("gray"),
                      highlightDisplayStyle(item.style) });
        }
    }
    if (!annotationDraft->range().start.isEmpty()) {
        request.draftStart = annotationDraft->range().start;
        request.draftEnd = annotationDraft->end();
    }
    annotationRequest = std::move(request);
    session->setMarkup(annotationRequest);
    if (auto *preview = appearanceEdit.preview()) {
        preview->setMarkup(annotationRequest);
    }
}

void Reader::preparePresentation(std::function<void(bool)> finished)
{
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
            book->markNativeReading();
            saveWifiPower([this, finished] { finished(!closing && !lifecycleHidden); });
        });
    });
}

void Reader::prepareAnnotationPresentation(std::function<void(bool)> finished)
{
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
    return QString::number(session->layoutRevision()) + ':' + layoutKey() + ':'
            + QString::number(session->page()) + ':'
            + session->state().value("position").toString();
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
    if (annotationDragTurn && annotationDragging && !annotationDragLoadingEdges) {
        annotationDragLoadingEdges = true;
        qt::later(0, this, [this] {
            if (annotationDragging && annotationDragTurn && !session->busy())
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
        BookmarkRow row{ item.id, {}, {}, item.created, locations.pages.value(i * 2, -1), false };
        row.endPage = locations.pages.value(i * 2 + 1, -1);
        row.style = item.style;
        row.color = item.color;
        row.sourceOrder = locations.order.value(i * 2, -1);
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
    for (const auto &item : highlights->highlights()) {
        if (item.id == id) {
            annotationColor = item.color;
            annotationStyle = highlightDisplayStyle(item.style);
            break;
        }
    }
    annotationActionsFor(
            { { "rrAnnotationApply", ui::ControlIcon::SaveHighlight,
                strings::get("annotations.apply_changes"),
                [this, id, anchor] {
                    if (highlights->updateAppearance(
                                id, colorAnnotations() ? annotationColor : QString(),
                                annotationStyle)) {
                        annotationSelectedColor = annotationColor;
                        annotationSelectedStyle = annotationStyle;
                        annotationPopover->hide();
                        annotationFrame.clear();
                        syncAnnotations();
                    } else {
                        auto *picker = dynamic_cast<ui::HighlightStylePicker *>(
                                annotationPopover->findChild<QWidget *>("rrAnnotationStyles"));
                        if (picker) {
                            picker->setError(strings::get("annotations.save_failed"),
                                             annotationStyle);
                            auto *card = annotationPopover->card();
                            card->rows()->activate();
                            card->fitToWidth(card->width());
                            annotationPopover->anchorTo(anchor);
                        }
                    }
                } },
              { "rrAnnotationEdit", ui::ControlIcon::EditHighlight,
                strings::get("annotations.edit"),
                [this, id, navigate, anchor] {
                    annotationPopover->hide();
                    annotationActionAnchor = anchor;
                    const auto color = annotationColor, style = annotationStyle;
                    editAnnotation(id, navigate);
                    annotationColor = color;
                    annotationStyle = style;
                    annotationColorChosen = annotationStyleChosen = true;
                } },
              { "rrAnnotationRemove", ui::ControlIcon::RemoveHighlight,
                strings::get("annotations.remove"),
                [this, id] {
                    annotationPopover->hide();
                    removeAnnotation(id);
                } } },
            anchor, true, annotationColor,
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
            QString excerpt = item.text.simplified();
            if (excerpt.size() > 36) {
                excerpt = excerpt.left(36) + QString::fromUtf8("…");
            }
            excerpt = QString::number(i + 1) + ". " + excerpt;
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
    if (excerpt.size() > 80) {
        excerpt = excerpt.left(80);
        excerpt = excerpt.left(qMax(40, excerpt.lastIndexOf(' '))) + QString::fromUtf8("…");
    }
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
    };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [finish] { finish(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [finish] { finish(true); });
    choice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    choice->centerCard();
    choice->show();
    choice->raise();
}
} // namespace kobalt::reader
