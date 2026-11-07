#include "render/highlight_paint.h"
#include "ui/style/theme.h"
#include "support/qt_font.h"
#include "ui/controls/text.h"
#include "ui/controls/icons.h"
#include "reading_page.h"
#include "engine/crengine/engine_document.h"
#include "render/bookmark_paint.h"
#include "render/page_turn_paint.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "ui/controls/panels.h"
#include "support/strings.h"
#include "ui/input/input.h"
#include "render/image_damage.h"
#include "render/page_refresh_regions.h"
#include <QVariant>
#include "platform/api/kobo_refresh.h"
#include "platform/api/safe_area.h"
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QPainter>
#include <QPointer>
#include <QtMath>
#include <QtCore/qnumeric.h>
#include <QTransform>
#include <QTouchEvent>
#include <QMouseEvent>
#include <QTextBoundaryFinder>
#include <QHideEvent>
#include <utility>
#include <algorithm>
#include <cmath>

namespace kobalt {
namespace metrics = ui::metrics;
using platform::textRefresh;

namespace {
// Resolve overlapping targets once when reveal mode changes. Keep the outer
// ellipse as a vector path so its edge remains smooth.
QPainterPath footnoteOutline(const QRect &reference, const QVector<QRect> &rectangles,
                             const QVector<QRect> &footnotes, int padding)
{
    QPainterPath circle;
    const auto target = footnoteTarget(reference, padding);
    circle.addEllipse(target);
    QVector<PageLink> neighbors;
    int own = -1;
    for (const auto &rect : rectangles) {
        const bool note = footnotes.contains(rect);
        if (rect == reference)
            own = neighbors.size();
        else if (!QRectF(rect).intersects(target)
                 && !(note && footnoteTarget(rect, padding).intersects(target)))
            continue;
        neighbors.append({ {}, { rect }, note ? LinkKind::Footnote : LinkKind::Passage });
    }
    if (neighbors.size() <= 1 || own < 0)
        return circle;
    QRegion excluded;
    const auto bounds = target.toAlignedRect();
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        int start = -1;
        for (int x = bounds.left(); x <= bounds.right() + 1; ++x) {
            const int winner = x <= bounds.right()
                    ? linkAtPoint(neighbors, QPoint(x, y), "all", padding).index
                    : -1;
            const bool lost = winner >= 0 && winner != own;
            if (lost && start < 0)
                start = x;
            else if (!lost && start >= 0) {
                excluded += QRect(start, y, x - start, 1);
                start = -1;
            }
        }
    }
    QPainterPath cut;
    cut.addRegion(excluded);
    return circle.subtracted(cut);
}

using headerfooter::Content;
using headerfooter::Field;

// The battery sits beside the text at a little below the text height.
qreal batteryBody(const QFont &font)
{
    return qRound(font.pixelSize() * .75);
}

int textWidth(const QFont &font, Field field, const QString &text)
{
    if (text.isEmpty())
        return 0;
    return headerfooter::pageCount(field) ? qCeil(ui::halfSpacedWidth(font, text))
                                          : ui::interfaceTextWidth(text, font);
}

int iconWidth(const QFont &font, const Content &content)
{
    if (!content.batteryIcon)
        return 0;
    return qCeil(ui::batteryWidth(batteryBody(font)))
            + (content.text.isEmpty() ? 0 : int(metrics::reading::batteryGap));
}

int contentWidth(const QFont &font, Field field, const Content &content, bool sleeping = false)
{
    if (sleeping && field == Field::Time)
        return font.pixelSize();
    const auto textFont = content.batteryIcon && !content.text.isEmpty()
            ? ui::batteryTextFont(font, batteryBody(font), ui::headerFooterTextWeight())
            : font;
    return textWidth(textFont, field, content.text) + iconWidth(font, content);
}

// Draws one slot within `bounds`, eliding its text when it does not fit.
void paintContent(QPainter &painter, const QRect &bounds, Field field, const Content &content,
                  Qt::Alignment align, const QColor &ink, const QColor &paper, bool sleeping)
{
    if (sleeping && field == Field::Time) {
        const int size = qMin(painter.font().pixelSize(), qMin(bounds.width(), bounds.height()));
        if (size <= 0)
            return;
        const int x = align & Qt::AlignRight ? bounds.right() + 1 - size
                : align & Qt::AlignHCenter   ? bounds.x() + (bounds.width() - size) / 2
                                             : bounds.x();
        ui::paintOpeningIcon(painter, ui::ControlIcon::Lock,
                             QRectF(x, bounds.y() + (bounds.height() - size) / 2.0, size, size),
                             ink);
        return;
    }
    if (content.empty() || bounds.width() <= 0)
        return;
    const QFont font = painter.font();
    const auto textFont = content.batteryIcon && !content.text.isEmpty()
            ? ui::batteryTextFont(font, batteryBody(font), ui::headerFooterTextWeight())
            : font;
    const int icon = iconWidth(font, content);
    const int room = qMax(0, bounds.width() - icon);
    QString text = content.text;
    int width = textWidth(textFont, field, text);
    if (width > room) {
        text = ui::headerFooterText(content, textFont, room);
        width = ui::interfaceTextWidth(text, textFont);
        field = Field::Nothing;
    }
    const int total = width + icon;
    const int x = align & Qt::AlignRight ? bounds.right() + 1 - total
            : align & Qt::AlignHCenter   ? bounds.x() + (bounds.width() - total) / 2
                                         : bounds.x();
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setFont(textFont);
    if (!text.isEmpty()) {
        if (content.batteryIcon)
            ui::drawOpticalPrivateText(painter, QRect(x, bounds.y(), width, bounds.height()), text,
                                       Qt::AlignLeft, ui::headerFooterTextWeight());
        else if (headerfooter::pageCount(field))
            ui::drawHalfSpaced(painter, x, bounds, text, ui::headerFooterTextWeight());
        else
            ui::drawInterfaceText(painter, QRect(x, bounds.y(), width, bounds.height()), text,
                                  Qt::AlignLeft | Qt::AlignVCenter, ui::headerFooterTextWeight());
    }
    if (content.batteryIcon) {
        const qreal height = batteryBody(font);
        const qreal left = x + total - ui::batteryWidth(height);
        ui::paintBattery(painter,
                         QRectF(left, bounds.y() + (bounds.height() - height) / 2.0,
                                height * int(metrics::panel::batteryBodyWidth)
                                        / int(metrics::panel::batteryBodyHeight),
                                height),
                         content.battery, content.charging, ink, paper);
    }
    painter.restore();
}
} // namespace

QString ReadingPage::headerText() const
{
    QString text;
    for (int slot = 0; slot < 3; ++slot) {
        const auto &content = slotContents[slot];
        const auto part = !content.text.isEmpty() ? content.text
                : content.batteryIcon && content.battery >= 0
                ? strings::get("common.percent").arg(content.battery)
                : QString();
        if (part.isEmpty())
            continue;
        text = text.isEmpty() ? part : strings::get("reader.chapter_position").arg(text, part);
    }
    return text;
}

ReadingPage::Slots ReadingPage::stripSlots(bool footer) const
{
    const int first = footer ? 3 : 0;
    return { slotContents[first], slotContents[first + 1], slotContents[first + 2] };
}

std::array<headerfooter::Field, 3> ReadingPage::stripFields(bool footer) const
{
    const int first = footer ? 3 : 0;
    return { chrome.slotFields[first], chrome.slotFields[first + 1], chrome.slotFields[first + 2] };
}

int ReadingPage::headerRightWidth() const
{
    QFont header = font();
    header.setPixelSize(headerTextPixels());
    return qMin(contentWidth(header, chrome.slotFields[2], slotContents[2]), width() / 3);
}

// The ribbon hangs just left of the header's top-right slot and runs a
// little past the strip. Without a strip it is shorter and stays in the top
// margin. Its size follows the screen, so it is the same on every density.
QRect ReadingPage::ribbonRect() const
{
    const int ribbonWidth = qRound(width() * .036);
    const int strip = topHeight();
    const int ribbonHeight = strip ? qRound(strip * 1.2) : qRound(height() * .032);
    const int rightWidth = showHeader() ? headerRightWidth() : 0;
    const int right = width() - metrics::reading::headerSide
            - (rightWidth ? rightWidth + metrics::reading::headerTextGap : 0);
    return QRect(right - ribbonWidth, 0, ribbonWidth, ribbonHeight);
}

QRect ReadingPage::bookmarkCorner() const
{
    const int w = qCeil(width() * .33), h = kobalt::ui::px(120);
    return QRect(width() - w, 0, w, h);
}

int ReadingPage::topHeight() const
{
    return showHeader() ? metrics::reading::stripHeight(chrome.headerSize) : 0;
}

int ReadingPage::bottomHeight() const
{
    return (showFooter() ? metrics::reading::stripHeight(chrome.footerSize) : 0)
            + (showProgress() ? ui::metrics::reading::progressHeight : 0);
}

// Keep the old edge alignment through Large. Larger sizes grow the text
// band as well as the reserved strip so glyphs are not clipped.
QRect ReadingPage::headerTextRect() const
{
    if (!showHeader())
        return {};
    return { 0, 0, width(), metrics::reading::textBandHeight(chrome.headerSize) };
}

QRect ReadingPage::footerTextRect() const
{
    if (!showFooter())
        return {};
    // Keep the same edge gap above the progress fill as below the top
    // of the header. The progress strip adds space below the text.
    const int offset = showProgress()
            ? int(metrics::reading::progressHeight) - int(metrics::reading::progressBottom)
                    - int(metrics::reading::progressFillHeight)
            : 0;
    const int band = metrics::reading::textBandHeight(chrome.footerSize);
    const int bottom =
            height() - bottomHeight() + offset + metrics::reading::stripHeight(chrome.footerSize);
    return { 0, bottom - band, width(), band };
}

void ReadingPage::setCornerRadii(int top, int bottom)
{
    if (cornerTop == top && cornerBottom == bottom)
        return;
    cornerTop = top;
    cornerBottom = bottom;
    update();
}

int ReadingPage::slotSide(bool footer) const
{
    const auto text = footer ? footerTextRect() : headerTextRect();
    if (text.isEmpty())
        return metrics::reading::headerSide;
    const int distance = footer ? height() - text.bottom() - 1 : text.top();
    const int corner = cornerInset(footer ? cornerBottom : cornerTop, distance);
    return qMax<int>(metrics::reading::headerSide,
                     corner ? corner + metrics::reading::batteryGap : 0);
}

void ReadingPage::cancelHold()
{
    stopPageAnimation();
    clearRevealedPress();
    cancelAnimation();
    window()->setProperty("reader_animation_direction", 0);
    animationDirection = 0;
    holdTimer.stop();
    tracking = false;
    if (std::exchange(annotationDragging, false) && annotationDragFinished)
        annotationDragFinished(true);
    const bool active = brightnessGesture.active();
    brightnessGesture.cancel();
    if (active && lightFinished)
        lightFinished(false);
}

void ReadingPage::revealLinks(const QVector<QRect> &rectangles, const QVector<QRect> &footnotes)
{
    if (revealedLinks == rectangles && revealedFootnotes == footnotes)
        return;
    cancelHold();
    const bool circlesChanged = revealedFootnotes != footnotes;
    revealedFootnotes = footnotes;
    QRegion dirty;
    for (const auto &outline : revealedOutlines)
        dirty += outline;
    const bool wereShown = !revealedLinks.isEmpty();
    revealedLinks = rectangles;
    // Pad each link, then split the space where padded links meet so that
    // links on consecutive lines, as in a contents page, keep separate outlines.
    const int pad = metrics::links::revealPadding, gap = metrics::links::revealGap;
    revealedOutlines.clear();
    revealedCircles.clear();
    QVector<QRect> ordinary;
    for (const auto &r : rectangles) {
        if (footnotes.contains(r))
            revealedCircles.append(footnoteOutline(r, rectangles, footnotes, pad));
        else
            ordinary.append(r);
    }
    for (const auto &r : ordinary)
        revealedOutlines += r.adjusted(-pad, -pad, pad, pad);
    for (int i = 0; i < ordinary.size(); ++i)
        for (int j = i + 1; j < ordinary.size(); ++j) {
            if (!revealedOutlines[i].intersects(revealedOutlines[j]))
                continue;
            const QRect &a = ordinary[i], &b = ordinary[j];
            const int shared = qMin(a.bottom(), b.bottom()) - qMax(a.top(), b.top());
            const bool sameLine = 2 * shared > qMin(a.height(), b.height());
            const int first = (sameLine ? a.left() <= b.left() : a.top() <= b.top()) ? i : j,
                      second = first == i ? j : i;
            auto &before = revealedOutlines[first], &after = revealedOutlines[second];
            if (sameLine) {
                const int middle = (ordinary[first].right() + ordinary[second].left()) / 2;
                before.setRight(qMin(before.right(), middle - gap / 2));
                after.setLeft(qMax(after.left(), middle + gap - gap / 2));
            } else {
                const int middle = (ordinary[first].bottom() + ordinary[second].top()) / 2;
                before.setBottom(qMin(before.bottom(), middle - gap / 2));
                after.setTop(qMax(after.top(), middle + gap - gap / 2));
            }
        }
    // Showing or hiding the outlines repaints the whole page as one rectangle:
    // MediaTek refreshes each page rectangle on its own, which would flash
    // once per link. A change between two sets of links repaints only their outlines.
    if (circlesChanged || !footnotes.isEmpty() || rectangles.isEmpty() == wereShown) {
        update();
        return;
    }
    for (const auto &outline : revealedOutlines)
        dirty += outline;
    if (!dirty.isEmpty())
        update(dirty.translated(0, topHeight()));
}

void ReadingPage::clearRevealedPress()
{
    if (revealedPress.isEmpty())
        return;
    const auto dirty = revealedPress.boundingRect().toAlignedRect().adjusted(-2, -2, 2, 2);
    revealedPress = {};
    update(dirty.translated(0, topHeight()));
}

void ReadingPage::pressRevealedLink(QPoint point)
{
    if (point.y() < 0 || point.y() >= height() - topHeight() - bottomHeight())
        return;
    QVector<PageLink> links;
    for (const auto &rect : revealedLinks)
        links.append({ {},
                       { rect },
                       revealedFootnotes.contains(rect) ? LinkKind::Footnote : LinkKind::Passage });
    const auto hit =
            linkAtPoint(links, point, "all", int(metrics::links::revealPadding), ui::px(6));
    if (hit.index < 0)
        return;
    const int note = revealedFootnotes.indexOf(hit.bounds);
    if (note >= 0)
        revealedPress = revealedCircles[note];
    else {
        int ordinary = 0;
        for (int i = 0; i < hit.index; ++i)
            if (!revealedFootnotes.contains(revealedLinks[i]))
                ++ordinary;
        revealedPress.addRect(revealedOutlines[ordinary]);
    }
    update(revealedPress.boundingRect()
                   .toAlignedRect()
                   .adjusted(-2, -2, 2, 2)
                   .translated(0, topHeight()));
}

QRegion ReadingPage::annotationContour(const QVector<QRect> &rectangles, bool vertical)
{
    QVector<QRect> parts;
    for (auto rect : rectangles) {
        if (rect.isEmpty()) {
            continue;
        }
        if (vertical) {
            rect = QRect(rect.y(), rect.x(), rect.height(), rect.width());
        }
        parts.append(rect);
    }
    std::sort(parts.begin(), parts.end(), [](const QRect &a, const QRect &b) {
        return a.top() < b.top() || (a.top() == b.top() && a.left() < b.left());
    });
    QRegion region;
    for (const auto &rect : parts) {
        region += rect;
    }
    for (int i = 0; i < parts.size(); ++i) {
        const auto &a = parts[i];
        for (int j = i + 1; j < parts.size(); ++j) {
            const auto &b = parts[j];
            const int height = qMin(a.height(), b.height());
            if (b.top() - a.bottom() - 1 > a.height() / 2) {
                break;
            }
            const int left = qMax(a.left(), b.left()), right = qMin(a.right(), b.right());
            const int gap = b.top() - a.bottom() - 1;
            if (gap > 0 && right >= left && gap <= height / 2) {
                region += QRect(left, a.bottom() + 1, right - left + 1, gap);
            }
            const int top = qMax(a.top(), b.top()), bottom = qMin(a.bottom(), b.bottom());
            if (bottom < top) {
                continue;
            }
            const auto &first = a.left() <= b.left() ? a : b;
            const auto &last = a.left() <= b.left() ? b : a;
            const int horizontalGap = last.left() - first.right() - 1;
            if (horizontalGap > 0 && horizontalGap <= qMin(a.height(), b.height()) / 2) {
                region += QRect(first.right() + 1, top, horizontalGap, bottom - top + 1);
            }
        }
    }
    if (!vertical) {
        return region;
    }
    return QTransform(0, 1, 1, 0, 0, 0).map(region);
}

void ReadingPage::setAnnotationInputEnabled(bool enabled)
{
    if (annotationInputEnabled == enabled)
        return;
    cancelHold();
    annotationInputEnabled = enabled;
    if (!enabled)
        setAnnotationHandles({});
}

QRect ReadingPage::annotationHandleRect(bool start) const
{
    const auto &handle = annotationHandles[start ? 0 : 1];
    if (!handle.visible)
        return {};
    const int radius = metrics::selection::handleRadius;
    return QRect(handle.center - QPoint(radius, radius) + QPoint(0, topHeight()),
                 QSize(2 * radius + 1, 2 * radius + 1));
}

QRegion ReadingPage::annotationHandleHitRegion() const
{
    QRegion region;
    const int radius = metrics::selection::handleHitRadius;
    for (const auto &handle : annotationHandles) {
        if (handle.visible) {
            const QPoint center = handle.center + QPoint(0, topHeight());
            region += QRegion(
                    QRect(center - QPoint(radius, radius), QSize(2 * radius + 1, 2 * radius + 1)),
                    QRegion::Ellipse);
        }
    }
    return region;
}

void ReadingPage::setAnnotationHandles(const AnnotationGeometry &geometry)
{
    QRegion dirty;
    for (int i = 0; i < 2; ++i) {
        const auto &edge = i == 0 ? geometry.start : geometry.end;
        auto &handle = annotationHandles[i];
        AnnotationHandle next;
        next.visible = annotationInputEnabled && edge.visible && !geometry.rectangles.isEmpty();
        if (next.visible) {
            const QPoint span = edge.last - edge.first;
            const QPoint axis(span.x() == 0 ? 0 : (span.x() > 0 ? 1 : -1),
                              span.y() == 0 ? 0 : (span.y() > 0 ? 1 : -1));
            const int radius = metrics::selection::handleRadius;
            next.center = (i == 0 ? edge.first - axis * radius : edge.last + axis * radius);
            next.center.setX(qBound(radius, next.center.x(), qMax(radius, width() - radius - 1)));
            next.center.setY(
                    qBound(radius, next.center.y(),
                           qMax(radius, height() - topHeight() - bottomHeight() - radius - 1)));
            next.target = (edge.first + edge.last) / 2 - edge.outward * ui::px(4);
        }
        if (handle.visible == next.visible && handle.center == next.center
            && handle.target == next.target)
            continue;
        dirty += annotationHandleRect(i == 0).translated(0, -topHeight());
        handle = next;
        dirty += annotationHandleRect(i == 0).translated(0, -topHeight());
    }
    damageAnnotations(dirty);
}

void ReadingPage::annotate(const QVector<QRect> &draft, const QVector<QRect> &saved,
                           const QVector<QRect> &candidate, bool vertical)
{
    if (draftAnnotations == draft && savedAnnotations == saved && candidateAnnotations == candidate
        && annotationVertical == vertical) {
        return;
    }
    QRegion dirty;
    for (const auto &set :
         { draftAnnotations, savedAnnotations, candidateAnnotations, draft, saved, candidate }) {
        for (const auto &rect : set) {
            dirty += rect.adjusted(-ui::px(6), -ui::px(6), ui::px(6), ui::px(6));
        }
    }
    if (draftAnnotations != draft || annotationVertical != vertical) {
        draftContour = annotationContour(draft, vertical);
        draftOutline = {};
        draftOutline.addRegion(draftContour);
        draftOutline = draftOutline.simplified();
    }
    dirty += draftContour;
    draftAnnotations = draft;
    savedAnnotations = saved;
    if (saved.isEmpty()) {
        setSavedHighlights({});
    }
    candidateAnnotations = candidate;
    candidateOutline = {};
    candidateOutline.addRegion(annotationContour(candidate, vertical));
    candidateOutline = candidateOutline.simplified();
    if (draft.isEmpty())
        setAnnotationHandles({});
    annotationVertical = vertical;
    damageAnnotations(dirty);
}

void ReadingPage::setSavedHighlights(const QVector<HighlightGeometry> &groups)
{
    if (annotationColorGroups == groups) {
        return;
    }
    QRegion dirty;
    for (const auto &tint : annotationTints) {
        dirty += tint.visible;
    }
    const QRegion previousColor = annotationColorRegion;
    annotationColorRegion = {};
    annotationColorGroups = groups;
    annotationTints.clear();
    QRegion covered;
    for (int i = groups.size() - 1; i >= 0; --i) {
        QRegion region;
        for (const auto &r : groups[i].rectangles) {
            region += r;
        }
        dirty += region;
        const auto color = kobalt::render::highlightTint(groups[i].color);
        const QRegion visible = region - covered;
        annotationTints.append({ groups[i], visible });
        covered += region;
        if (color.red() != color.green() || color.green() != color.blue()) {
            annotationColorRegion += visible;
        }
    }
    const QPoint offset = mapTo(window(), QPoint(0, topHeight()));
    window()->setProperty("reader_annotation_regions",
                          QVariant::fromValue(annotationColorRegion.translated(offset)));
    window()->setProperty(
            "reader_annotation_repairs",
            QVariant::fromValue(window()->property("reader_annotation_repairs").value<QRegion>()
                                | (previousColor | annotationColorRegion).translated(offset)));
    window()->setProperty("reader_colour_regions",
                          !annotationColorRegion.isEmpty()
                                  || (!image.text("kobalt.colour_regions").isEmpty()
                                      && image.text("kobalt.colour_regions") != "[]"
                                      && image.text("kobalt.animation_text_only") != "true"));
    damageAnnotations(dirty);
}

void ReadingPage::damageAnnotations(const QRegion &dirty)
{
    if (installingAnnotationFrame) {
        annotationFrameDamage += dirty;
    } else if (!dirty.isEmpty()) {
        stopPageAnimation();
        animationFrame = {};
        // MTK refreshes each damage rectangle separately, even after one Qt paint.
        update(dirty.boundingRect().translated(0, topHeight()));
    }
}

void ReadingPage::installAnnotationFrame(const QVector<QRect> &draft,
                                         const QVector<HighlightGeometry> &saved, bool vertical,
                                         const AnnotationGeometry *geometry)
{
    QVector<QRect> rectangles;
    for (const auto &group : saved) {
        rectangles += group.rectangles;
    }
    installingAnnotationFrame = true;
    annotate(draft, rectangles, {}, vertical);
    setSavedHighlights(saved);
    setAnnotationHandles(geometry ? *geometry : AnnotationGeometry{});
    installingAnnotationFrame = false;
}

void ReadingPage::mark(const QVector<QRect> &rectangles)
{
    QRegion dirty;
    for (const auto &r : selection)
        dirty += r;
    selection = rectangles;
    for (const auto &r : selection)
        dirty += r;
    if (!dirty.isEmpty())
        update(dirty.translated(0, topHeight()));
}

ReadingPage::ReadingPage(QWidget *parent, std::function<void()> cancelAnimation)
    : QWidget(parent), cancelAnimation(std::move(cancelAnimation))
{
    Q_ASSERT(this->cancelAnimation);
    viewportNoticeTimer.setParent(this);
    viewportNoticeTimer.setObjectName("rrViewportNoticeTimer");
    viewportNoticeTimer.setSingleShot(true);
    viewportNoticeTimer.setInterval(1000);
    viewportNoticeTimer.setTimerType(Qt::PreciseTimer);
    connect(&viewportNoticeTimer, &QTimer::timeout, this, [this] {
        viewportNoticeVisible = changingViewport;
        update();
    });
    pageTurnAnimation.setObjectName("rrPageTurn");
    pageTurnAnimation.setParent(this);
    pageTurnAnimation.setDuration(180);
    pageTurnAnimation.setEasingCurve(QEasingCurve::InOutSine);
    connect(&pageTurnAnimation, &QTimeLine::valueChanged, this, [this](qreal value) {
        if (pageAnimationStyle == pageTurn::Style::Sweep) {
            const int step = kobalt::render::pageSweepStep(value, animationTo.size());
            if (step == animationStep)
                return;
            animationStep = step;
        }
        update(QRect(0, topHeight(), width(), renderedBody.height()));
    });
    connect(&pageTurnAnimation, &QTimeLine::finished, this, [this] {
        animationFrom = animationTo = {};
        update();
    });
    setObjectName("rrPage");
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    textRefresh(this);
    holdTimer.setParent(this);
    holdTimer.setObjectName("rrHoldTimer");
    holdTimer.setSingleShot(true);
    holdTimer.setInterval(metrics::lookup::holdMilliseconds);
    connect(&holdTimer, &QTimer::timeout, this, [this] { triggerHold(); });
}

void ReadingPage::triggerHold()
{
    if (!tracking || moved || consumed || !revealedLinks.isEmpty())
        return;
    consumed = true;
    brightnessGesture.cancel();
    if (holdTarget == HoldTarget::Stale)
        return;
    if (imageHeld && imageHeld(origin.toPoint() - QPoint(0, topHeight())))
        return;
    if (annotationInputEnabled && annotationDragStarted) {
        QPointer<ReadingPage> guard(this);
        const bool dragging = annotationDragStarted(origin.toPoint() - QPoint(0, topHeight()), -1);
        if (guard)
            annotationDragging = dragging;
        return;
    }
    if (held)
        held(origin.toPoint() - QPoint(0, topHeight()));
}

void ReadingPage::cleanRefresh(bool clean)
{
    kobalt::platform::pageRefresh(this, clean);
}

void ReadingPage::setPanelBelowHeader(bool visible)
{
    if (panelBelowHeader == visible)
        return;
    panelBelowHeader = visible;
    refreshContent();
}

void ReadingPage::stopPageAnimation()
{
    if (pageTurnAnimation.state() != QTimeLine::NotRunning) {
        pageTurnAnimation.stop();
        update();
    }
    animationFrom = animationTo = {};
    animationStep = -1;
}

void ReadingPage::setReflowText(const QString &text)
{
    wordLengths.clear();
    QTextBoundaryFinder words(QTextBoundaryFinder::Word, text);
    int start = -1;
    do {
        const auto reasons = words.boundaryReasons();
        if ((reasons & QTextBoundaryFinder::EndOfItem) && start >= 0) {
            const auto word = text.mid(start, words.position() - start);
            wordLengths.append(qMax(1, int(word.toUcs4().size())));
            start = -1;
        }
        if (reasons & QTextBoundaryFinder::StartOfItem)
            start = words.position();
    } while (words.toNextBoundary() >= 0 && wordLengths.size() < 64);
}

void ReadingPage::setViewportTransition(bool active)
{
    if (changingViewport == active)
        return;
    changingViewport = active;
    if (active) {
        cancelHold();
        multipleTouch = annotationTouch = rightPressed = rightMoved = false;
        touchOrigins.clear();
        viewportNoticeTimer.start();
    } else {
        viewportNoticeTimer.stop();
        viewportNoticeVisible = false;
    }
    update();
}

void ReadingPage::setPageAnimationStyle(pageTurn::Style style)
{
    if (pageAnimationStyle == style)
        return;
    pageAnimationStyle = style;
    stopPageAnimation();
    animationFrame = style != pageTurn::Style::Off && !renderedBody.isNull()
            ? snapshot().copy(0, topHeight(), width(), renderedBody.height())
            : QImage();
}

void ReadingPage::refreshContent(bool reading, bool now)
{
    const auto colors = palette();
    const bool geometryChanged = renderedSize != size() || renderedTop != topHeight()
            || renderedBottom != bottomHeight();
    const bool softwareAnimation = pageAnimationStyle != pageTurn::Style::Off;
    const bool animatedTurn = !changingViewport && softwareAnimation && reading
            && animationDirection && !geometryChanged && renderedPalette == colors.cacheKey()
            && !animationFrame.isNull();
    const auto outgoing = animatedTurn ? animationFrame : QImage();
    if (geometryChanged || renderedPalette != colors.cacheKey()
        || (!animatedTurn
            && (renderedBody.cacheKey() != image.cacheKey() || !annotationFrameDamage.isEmpty())))
        stopPageAnimation();
    const int weight = kobalt::ui::interfaceWeight() + kobalt::ui::headerFooterAddedWeight();
    const HeaderState nextHeader{
        stripSlots(false),  stripFields(false), showHeader(), panelBelowHeader, width(),
        headerTextPixels(), colors.cacheKey(),  weight,       font().family()
    };
    const FooterState nextFooter{ stripSlots(true), stripFields(true),  notice,
                                  showFooter(),     showProgress(),     progressFraction,
                                  width(),          footerTextPixels(), colors.cacheKey(),
                                  weight,           font().family() };
    kobalt::platform::pageRefresh(
            this, false,
            reading && !geometryChanged && renderedPalette == colors.cacheKey()
                    && renderedBody.text("kobalt.animation_text_only") == "true"
                    && image.text("kobalt.animation_text_only") == "true");
    QRegion dirty((preparedFrom && preparedFrom == renderedBody.cacheKey()
                                   && renderedBody.size() == image.size()
                           ? preparedDamage
                           : kobalt::render::imageDamage(renderedBody, image))
                          .translated(0, topHeight()));
    preparedFrom = 0;
    dirty += annotationFrameDamage.translated(0, topHeight());
    annotationFrameDamage = {};
    const auto oldRegions = pageRefreshRegions(renderedBody, true);
    const auto newRegions = pageRefreshRegions(image, true);
    window()->setProperty(
            "reader_image_repairs",
            QVariant::fromValue(
                    (oldRegions | newRegions).translated(mapTo(window(), QPoint(0, topHeight())))));
    window()->setProperty(
            "reader_mono_repairs",
            QVariant::fromValue(
                    (pageRefreshRegions(renderedBody, false) | pageRefreshRegions(image, false))
                            .translated(mapTo(window(), QPoint(0, topHeight())))));
    window()->setProperty("reader_colour_regions",
                          !annotationColorRegion.isEmpty()
                                  || (!image.text("kobalt.colour_regions").isEmpty()
                                      && image.text("kobalt.colour_regions") != "[]"
                                      && image.text("kobalt.animation_text_only") != "true"));
    renderedBody = image;
    if (geometryChanged || nextHeader != headerState) {
        auto next = renderStrip(topHeight(), [this](QPainter &p) { paintHeader(p); });
        dirty += kobalt::render::imageDamage(headerPixels, next);
        headerPixels = next;
        headerState = nextHeader;
    }
    if (geometryChanged || nextFooter != footerState) {
        auto next = renderStrip(bottomHeight(), [this](QPainter &p) { paintFooter(p); });
        dirty += kobalt::render::imageDamage(footerPixels, next)
                         .translated(0, height() - bottomHeight());
        footerPixels = next;
        footerState = nextFooter;
    }
    const QRect ribbon = bookmarked ? ribbonRect() : QRect();
    if (ribbon != renderedRibbon) {
        dirty += ribbon;
        dirty += renderedRibbon;
        renderedRibbon = ribbon;
        if (parentWidget() && y() > 0)
            parentWidget()->update(QRect(0, 0, parentWidget()->width(), y()));
    }
    renderedSize = size();
    renderedTop = topHeight();
    renderedBottom = bottomHeight();
    renderedPalette = colors.cacheKey();
    // Keep separate UI strips separate. Reading frames use one rectangle
    // for page turns and completed passage jumps.
    if (reading)
        dirty = dirty.boundingRect();
    const int turn = std::exchange(animationDirection, 0);
    window()->setProperty("reader_page_turn", reading && !geometryChanged);
    window()->setProperty("reader_animation_direction",
                          reading && !geometryChanged && !softwareAnimation ? turn : 0);
    if (softwareAnimation) {
        animationFrame = snapshot().copy(0, topHeight(), width(), renderedBody.height());
        if (animatedTurn && outgoing.size() == animationFrame.size()) {
            stopPageAnimation();
            animationFrom = outgoing;
            animationTo = animationFrame;
            animationTurnDirection = turn;
            // Pace updates to the active display. Recheck on each turn because
            // Android can change refresh rate or move the window to a display.
            const auto *handle = window()->windowHandle();
            const auto *display = handle ? handle->screen() : QGuiApplication::primaryScreen();
            const qreal rate = display ? display->refreshRate() : 60;
            pageTurnAnimation.setUpdateInterval(
                    qMax(1, qRound(1000 / (std::isfinite(rate) && rate > 0 ? rate : 60))));
            pageTurnAnimation.start();
        }
    }
    // `now` paints and flushes before returning, without a trip through the event loop.
    if (geometryChanged) {
        if (now)
            repaint();
        else
            update();
    } else if (!dirty.isEmpty()) {
        if (now)
            repaint(dirty);
        else
            update(dirty);
    }
}

void ReadingPage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (pageTurnAnimation.state() == QTimeLine::NotRunning) {
        paintContents(painter);
        return;
    }
    const QRect body(0, topHeight(), width(), renderedBody.height());
    painter.save();
    painter.setClipRegion(QRegion(rect()) - QRegion(body), Qt::IntersectClip);
    paintContents(painter);
    painter.restore();
    painter.translate(0, topHeight());
    kobalt::render::paintPageTurn(painter, animationFrom, animationTo,
                                  pageTurnAnimation.currentValue(), animationTurnDirection,
                                  pageAnimationStyle);
}

QImage ReadingPage::stripSnapshot(bool footer) const
{
    auto strip = footer ? footerPixels : headerPixels;
    const int top = footer ? height() - bottomHeight() : 0;
    const QRect ribbon = renderedRibbon.translated(0, -top);
    if (!strip.isNull() && ribbon.intersects(strip.rect())) {
        QPainter painter(&strip);
        kobalt::render::paintBookmarkRibbon(painter, ribbon, palette().color(QPalette::WindowText));
    }
    return strip;
}

QImage ReadingPage::snapshot(bool sleeping) const
{
    QImage frame(size(), QImage::Format_RGB32);
    if (!frame.isNull()) {
        QPainter painter(&frame);
        paintContents(painter, sleeping);
    }
    return frame;
}

void ReadingPage::paintBody(QPainter &painter) const
{
    if (changingViewport) {
        const QRect body(0, topHeight(), width(), qMax(0, height() - topHeight() - bottomHeight()));
        const auto text = body.marginsRemoved(reflowLayout.margins);
        const int columns = qBound(1, reflowLayout.columns, 2);
        const int gap = qMax(0, reflowLayout.columnSpacing);
        const int columnWidth = (text.width() - (columns - 1) * gap) / columns;
        const int fontPixels = qMax(1, reflowLayout.fontPixels);
        const int lineHeight =
                qMax(1, qRound(fontPixels * qMax(100, reflowLayout.lineSpacing) / 100.0));
        const int barHeight = qMax(1, fontPixels / 2);
        painter.save();
        painter.setClipRect(body);
        // Repeat a small sample of the current page's word lengths. The new
        // page may contain different words; this is only a transition texture.
        // Publisher fonts and line breaks remain unknown until reflow.
        const auto ink = ui::themeColors(this).divider;
        if (columnWidth > 0) {
            int word = 0;
            const int space = qMax(1, qRound(fontPixels * .33));
            for (int column = 0; column < columns; ++column) {
                const int left = text.left() + column * (columnWidth + gap);
                int row = 0;
                for (int y = text.top(); y + lineHeight <= text.bottom() + 1;
                     y += lineHeight, ++row) {
                    const int barTop = y + (lineHeight - barHeight) / 2;
                    if (wordLengths.isEmpty()) {
                        const int indent = row % 6 == 0 ? qMin(fontPixels, columnWidth / 4) : 0;
                        const int length =
                                row % 6 == 5 ? columnWidth * 3 / 4 : columnWidth - indent;
                        painter.fillRect(QRect(left + indent, barTop, length, barHeight), ink);
                        continue;
                    }
                    int x = 0;
                    while (x < columnWidth) {
                        const int length = qBound(1, qRound(wordLengths[word] * fontPixels * .45),
                                                  columnWidth);
                        if (x && x + length > columnWidth)
                            break;
                        painter.fillRect(QRect(left + x, barTop, length, barHeight), ink);
                        x += length + space;
                        word = (word + 1) % wordLengths.size();
                    }
                }
            }
        }
        painter.restore();
        // Marks and hit targets belong to the old page coordinates.
        return;
    }
    painter.drawImage(0, topHeight(), renderedBody);
    if (!savedAnnotations.isEmpty()) {
        auto tints = annotationTints;
        if (tints.isEmpty()) {
            QRegion region;
            for (const auto &r : savedAnnotations) {
                region += r;
            }
            tints.append({ { savedAnnotations, "gray" }, region });
        }
        for (const auto &tint : tints) {
            painter.save();
            painter.translate(0, topHeight());
            kobalt::render::paintHighlightMark(painter, tint.mark.rectangles, tint.visible,
                                               kobalt::render::highlightTint(tint.mark.color),
                                               tint.mark.style, ui::darkTheme(this));
            painter.restore();
        }
    }
    painter.save();
    painter.translate(0, topHeight());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette().color(QPalette::WindowText), int(metrics::selection::outline)));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(draftOutline);
    painter.drawPath(candidateOutline);
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::WindowText));
    for (const auto &handle : annotationHandles)
        if (handle.visible)
            painter.drawEllipse(handle.center, int(metrics::selection::handleRadius),
                                int(metrics::selection::handleRadius));
    painter.restore();
    if (!revealedCircles.isEmpty()) {
        painter.save();
        painter.setClipRect(
                QRect(0, topHeight(), width(), height() - topHeight() - bottomHeight()));
        painter.translate(0, topHeight());
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath fill;
        fill.setFillRule(Qt::WindingFill);
        for (const auto &circle : revealedCircles)
            fill.addPath(circle);
        const bool dark = ui::darkTheme(this);
        painter.setCompositionMode(dark ? QPainter::CompositionMode_Screen
                                        : QPainter::CompositionMode_Multiply);
        painter.fillPath(fill, ui::themeColors(dark).pressedPaper);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setPen(
                QPen(palette().color(QPalette::WindowText), int(metrics::links::revealOutline)));
        painter.setBrush(Qt::NoBrush);
        for (const auto &circle : revealedCircles)
            painter.drawPath(circle);
        painter.restore();
    }
    if (!revealedOutlines.isEmpty()) {
        QRegion linkRegion;
        for (const auto &r : revealedOutlines)
            linkRegion += r.translated(0, topHeight());
        const bool dark = ui::darkTheme(this);
        painter.save();
        // Tint the paper without covering the rendered text. The union keeps
        // the tint even where the rectangles of one link overlap.
        painter.setClipRegion(linkRegion);
        painter.setCompositionMode(dark ? QPainter::CompositionMode_Screen
                                        : QPainter::CompositionMode_Multiply);
        painter.fillRect(linkRegion.boundingRect(), ui::themeColors(dark).pressedPaper);
        painter.restore();
        const int outline = metrics::links::revealOutline;
        for (const auto &r : revealedOutlines)
            ui::paintEdges(painter, r.translated(0, topHeight()),
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           palette().color(QPalette::WindowText), outline);
    }
    if (!revealedPress.isEmpty()) {
        painter.save();
        painter.setClipRect(
                QRect(0, topHeight(), width(), height() - topHeight() - bottomHeight()));
        painter.translate(0, topHeight());
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = ui::darkTheme(this);
        painter.setCompositionMode(dark ? QPainter::CompositionMode_Screen
                                        : QPainter::CompositionMode_Multiply);
        painter.fillPath(revealedPress, dark ? QColor(65, 65, 65) : QColor(190, 190, 190));
        painter.restore();
    }
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Difference);
    for (const auto &r : selection)
        painter.fillRect(r.translated(0, topHeight()), Qt::white);
    painter.restore();
}

void ReadingPage::paintContents(QPainter &painter, bool sleeping) const
{
    painter.fillRect(rect(), palette().color(QPalette::Window));
    paintBody(painter);
    if (sleeping) {
        painter.save();
        painter.setFont(font());
        painter.setPen(palette().color(QPalette::WindowText));
        paintHeader(painter, true);
        painter.translate(0, height() - bottomHeight());
        paintFooter(painter, true);
        painter.restore();
    } else {
        painter.drawImage(0, 0, headerPixels);
        painter.drawImage(0, height() - bottomHeight(), footerPixels);
    }
    if (!changingViewport)
        kobalt::render::paintBookmarkRibbon(painter, renderedRibbon,
                                            palette().color(QPalette::WindowText));
    if (viewportNoticeVisible && !sleeping) {
        painter.save();
        painter.setFont(ui::interfaceFont(metrics::chrome::captionText));
        const auto text = strings::get("reader.updating_page_layout");
        const int padding = ui::px(16);
        const int h = ui::interfaceLineHeight(painter.font()) + 2 * padding;
        const int w = qMin(width() - 2 * padding,
                           ui::interfaceTextWidth(text, painter.font()) + 2 * padding);
        const QRect card((width() - w) / 2, height() - bottomHeight() - h - padding, w, h);
        painter.fillRect(card, palette().color(QPalette::Window));
        ui::paintEdges(painter, card,
                       ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                       palette().color(QPalette::WindowText), metrics::control::border);
        painter.setPen(palette().color(QPalette::WindowText));
        ui::drawInterfaceText(painter, card.adjusted(padding, 0, -padding, 0), text,
                              Qt::AlignCenter);
        painter.restore();
    }
}

bool ReadingPage::annotationCenter(QPointF point) const
{
    const QRect body(0, topHeight(), width(), height() - topHeight() - bottomHeight());
    return QRectF(body.left() + body.width() / 3., body.top() + body.height() / 3.,
                  body.width() / 3., body.height() / 3.)
            .contains(point);
}

bool ReadingPage::annotationTouchEvent(QTouchEvent *event)
{
    const auto contacts = ui::touchContacts(event);
    if (event->type() == QEvent::TouchBegin) {
        touchClock.start();
        touchOrigins.clear();
        multipleTouch = false;
        annotationTouch = true;
    }
    int active = 0;
    for (const auto &contact : contacts) {
        if (!touchOrigins.contains(contact.id)) {
            if (touchClock.elapsed() > 150 || !annotationCenter(contact.position)) {
                annotationTouch = false;
            }
            if (touchOrigins.size() < 3) {
                touchOrigins.insert(contact.id, contact.position);
            }
        }
        if ((contact.position - touchOrigins.value(contact.id)).manhattanLength()
            > metrics::lookup::moveTolerance) {
            annotationTouch = false;
        }
        if (!contact.released) {
            ++active;
        }
    }
    if (touchOrigins.size() > 1 && !multipleTouch) {
        multipleTouch = true;
        if (active != 2) {
            annotationTouch = false;
        }
        cancelHold();
    }
    if (touchOrigins.size() > 2 || touchClock.elapsed() > 350) {
        annotationTouch = false;
    }
    // A first contact alone is still eligible when the second arrives shortly after it.
    if (!multipleTouch && touchOrigins.size() == 1 && event->type() == QEvent::TouchBegin) {
        annotationTouch = annotationCenter(contacts.first().position);
    }
    if (!multipleTouch) {
        return false;
    }
    if (event->type() == QEvent::TouchEnd || active == 0) {
        const bool request = annotationTouch;
        multipleTouch = annotationTouch = false;
        touchOrigins.clear();
        if (request && revealedLinks.isEmpty() && annotationRequested) {
            annotationRequested();
        }
    }
    event->accept();
    return true;
}

bool ReadingPage::event(QEvent *event)
{
    if (changingViewport) {
        switch (event->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
            event->accept();
            return true;
        default:
            break;
        }
    }
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
        || event->type() == QEvent::TouchEnd) {
        if (annotationTouchEvent(static_cast<QTouchEvent *>(event))) {
            return true;
        }
        const auto points = kobalt::ui::touchPositions(static_cast<QTouchEvent *>(event));
        if (event->type() == QEvent::TouchBegin) {
            if (points.size() == 1)
                beginPointer(points.first(), static_cast<QTouchEvent *>(event));
            else
                cancelHold();
        }
        if (points.size() != 1)
            cancelHold();
        if (tracking) {
            latest = points.first();
            updatePointer();
        }
        if (!tracking || moved)
            holdTimer.stop();
        if (event->type() == QEvent::TouchEnd) {
            QPointer<ReadingPage> guard(this);
            if (holdTimer.finish(static_cast<QTouchEvent *>(event)))
                triggerHold();
            if (!guard)
                return true;
            finishGesture();
        }
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        multipleTouch = annotationTouch = false;
        touchOrigins.clear();
        cancelHold();
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void ReadingPage::beginPointer(QPointF point, const QInputEvent *event)
{
    cancelHold();
    tracking = true;
    pressTime = quint32(event->timestamp());
    moved = consumed = false;
    holdTarget = HoldTarget::Current;
    origin = latest = point;
    if (!revealedLinks.isEmpty()) {
        pressRevealedLink(point.toPoint() - QPoint(0, topHeight()));
        return;
    }
    annotationDragOffset = {};
    annotationPointerBoundary = -1;
    if (annotationInputEnabled) {
        int nearest = -1;
        qreal distance = qreal(metrics::selection::handleHitRadius)
                * int(metrics::selection::handleHitRadius);
        const QPoint body = point.toPoint() - QPoint(0, topHeight());
        for (int i = 0; i < 2; ++i) {
            const auto &handle = annotationHandles[i];
            const QPoint delta = body - handle.center;
            const qreal squared = QPoint::dotProduct(delta, delta);
            if (handle.visible && squared <= distance) {
                nearest = i;
                distance = squared;
            }
        }
        if (nearest >= 0 && annotationDragStarted) {
            const auto &handle = annotationHandles[nearest];
            annotationDragOffset = body - handle.target;
            // Resolve a press as a drag only after movement. The larger hit
            // area overlaps nearby words, which must still accept taps.
            annotationPointerBoundary = nearest;
            return;
        }
    }
    brightnessGesture.begin(origin, size(), brightnessEdge, warmthEdge);
    holdTimer.start(event);
}

void ReadingPage::mousePressEvent(QMouseEvent *event)
{
#if defined(Q_OS_MAC) || defined(KOBALT_DESKTOP_POINTER)
    if (ui::isPointerMouse(event) && event->button() == Qt::RightButton) {
        cancelHold();
        origin = ui::mousePosition(event);
        rightPressed = event->buttons() == Qt::RightButton;
        rightMoved = false;
        event->accept();
        return;
    }
#endif
    rightPressed = false;
    if (!kobalt::ui::isPointerMouse(event) || event->button() != Qt::LeftButton)
        return;
    beginPointer(kobalt::ui::mousePosition(event), event);
}

void ReadingPage::mouseMoveEvent(QMouseEvent *event)
{
#if defined(Q_OS_MAC) || defined(KOBALT_DESKTOP_POINTER)
    if (rightPressed && ui::isPointerMouse(event)) {
        rightMoved |= event->buttons() != Qt::RightButton
                || (ui::mousePosition(event) - origin).manhattanLength()
                        > metrics::lookup::moveTolerance;
        event->accept();
        return;
    }
#endif
    if (!tracking || !kobalt::ui::isPointerMouse(event))
        return;
    latest = kobalt::ui::mousePosition(event);
    updatePointer();
}

void ReadingPage::mouseReleaseEvent(QMouseEvent *event)
{
#if defined(Q_OS_MAC) || defined(KOBALT_DESKTOP_POINTER)
    if (ui::isPointerMouse(event) && event->button() == Qt::RightButton) {
        const bool request = rightPressed && !rightMoved && event->buttons() == Qt::NoButton
                && (ui::mousePosition(event) - origin).manhattanLength()
                        <= metrics::lookup::moveTolerance;
        rightPressed = false;
        if (request && revealedLinks.isEmpty() && annotationRequested) {
            annotationRequested();
        }
        event->accept();
        return;
    }
#endif
    rightPressed = false;
    if (!kobalt::ui::isPointerMouse(event) || event->button() != Qt::LeftButton)
        return;
    latest = kobalt::ui::mousePosition(event);
    updatePointer();
    QPointer<ReadingPage> guard(this);
    if (holdTimer.finish(event))
        triggerHold();
    if (!guard)
        return;
    finishGesture();
}

void ReadingPage::hideEvent(QHideEvent *event)
{
    setViewportTransition(false);
    multipleTouch = annotationTouch = rightPressed = false;
    touchOrigins.clear();
    cancelHold();
    QWidget::hideEvent(event);
}

QImage ReadingPage::renderStrip(int height, const std::function<void(QPainter &)> &draw)
{
    if (!height)
        return {};
    QImage result(width(), height, QImage::Format_RGB32);
    result.fill(palette().color(QPalette::Window));
    QPainter painter(&result);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::WindowText));
    draw(painter);
    return result;
}

// The centre slot is centred on the page, not in a third of it, so a long
// centre keeps its middle while the sides keep their own width. When the
// three do not fit, the sides give way first, down to a third each.
void ReadingPage::paintSlots(QPainter &painter, const QRect &bounds, bool footer,
                             bool sleeping) const
{
    const int first = footer ? 3 : 0;
    const QFont font = painter.font();
    int widths[3];
    for (int slot = 0; slot < 3; ++slot)
        widths[slot] = contentWidth(font, chrome.slotFields[first + slot],
                                    slotContents[first + slot], sleeping);
    const int side = slotSide(footer), gap = metrics::reading::headerTextGap;
    // The bookmark ribbon hangs left of the top-right slot; keep text off it.
    const int ribbon = footer ? 0 : qRound(width() * .036) + gap;
    int leftRoom = width() - 2 * side, rightRoom = leftRoom - ribbon;
    QRect centre;
    if (widths[1]) {
        int edge = qMax(side + (widths[0] ? widths[0] + gap : 0),
                        side + (widths[2] ? widths[2] + gap : 0) + ribbon);
        int centreWidth = width() - 2 * edge;
        const int least = qMin(widths[1], width() / 3);
        if (centreWidth < least) {
            centreWidth = least;
            edge = (width() - centreWidth) / 2;
        }
        centre = QRect(edge, bounds.y(), centreWidth, bounds.height());
        leftRoom = edge - side - gap;
        rightRoom = edge - side - gap - ribbon;
    } else if (widths[0] && widths[2] && widths[0] + widths[2] + gap > leftRoom - ribbon) {
        const int shared = leftRoom - ribbon - gap;
        leftRoom = shared * widths[0] / (widths[0] + widths[2]);
        rightRoom = shared - leftRoom;
    }
    const auto ink = palette().color(QPalette::WindowText),
               paper = palette().color(QPalette::Window);
    paintContent(painter, QRect(side, bounds.y(), qMax(0, leftRoom), bounds.height()),
                 chrome.slotFields[first], slotContents[first], Qt::AlignLeft, ink, paper,
                 sleeping);
    if (!centre.isEmpty())
        paintContent(painter, centre, chrome.slotFields[first + 1], slotContents[first + 1],
                     Qt::AlignHCenter, ink, paper, sleeping);
    paintContent(painter,
                 QRect(width() - side - qMax(0, rightRoom), bounds.y(), qMax(0, rightRoom),
                       bounds.height()),
                 chrome.slotFields[first + 2], slotContents[first + 2], Qt::AlignRight, ink, paper,
                 sleeping);
}

void ReadingPage::paintHeader(QPainter &painter, bool sleeping) const
{
    QFont font = painter.font();
    font.setPixelSize(headerTextPixels());
    painter.setFont(font);
    if (showHeader())
        paintSlots(painter, headerTextRect(), false, sleeping);
    if (panelBelowHeader)
        painter.fillRect(0, topHeight() - metrics::control::border, width(),
                         metrics::control::border, ui::dividerColor(this));
}

void ReadingPage::paintFooter(QPainter &painter, bool sleeping) const
{
    if (showFooter()) {
        QFont font = painter.font();
        font.setPixelSize(footerTextPixels());
        painter.setFont(font);
        const auto bounds = footerTextRect().translated(0, bottomHeight() - height());
        if (!notice.isEmpty()) {
            const int room = width() - 2 * slotSide(true);
            ui::drawInterfaceText(painter, bounds, ui::elideInterfaceText(notice, font, room),
                                  Qt::AlignCenter, ui::headerFooterTextWeight());
        } else
            paintSlots(painter, bounds, true, sleeping);
    }
    if (showProgress()) {
        const auto ink = palette().color(QPalette::WindowText);
        painter.fillRect(QRect(0, bottomHeight() - metrics::reading::progressBottom, width(),
                               metrics::control::border),
                         ink);
        painter.fillRect(QRect(0,
                               bottomHeight() - metrics::reading::progressBottom
                                       - metrics::reading::progressFillHeight,
                               qRound(width() * progressFraction),
                               metrics::reading::progressFillHeight),
                         ink);
    }
}

void ReadingPage::updatePointer()
{
    moved |= (latest - origin).manhattanLength() > metrics::lookup::moveTolerance;
    if (!revealedLinks.isEmpty()) {
        if (moved)
            clearRevealedPress();
        return;
    }
    if (moved)
        holdTimer.stop();
    if (annotationInputEnabled && annotationPointerBoundary >= 0 && !annotationDragging && !consumed
        && moved && annotationDragStarted) {
        annotationDragging = annotationDragStarted(origin.toPoint() - QPoint(0, topHeight())
                                                           - annotationDragOffset,
                                                   annotationPointerBoundary);
        if (annotationDragging) {
            consumed = true;
            brightnessGesture.cancel();
        }
    }
    if (annotationDragging) {
        if (annotationDragMoved)
            annotationDragMoved(latest.toPoint() - QPoint(0, topHeight()) - annotationDragOffset);
        return;
    }
    const bool active = brightnessGesture.active();
    if ((!consumed || active) && brightnessGesture.move(latest)) {
        const bool warmth = brightnessGesture.edge() == kobalt::ui::BrightnessGesture::Edge::Right;
        if (!active && (!lightStarted || !lightStarted(warmth))) {
            brightnessGesture.cancel();
            return;
        }
        consumed = true;
        holdTimer.stop();
        if (lightChanged)
            lightChanged(brightnessGesture.moved(latest));
    }
}

void ReadingPage::finishGesture()
{
    holdTimer.stop();
    if (!tracking)
        return;
    tracking = false;

    // A navigation callback can destroy the reader.
    struct SourceTime
    {
        QPointer<ReadingPage> page;

        ~SourceTime()
        {
            if (page)
                page->gestureTime = 0;
        }
    } sourceTime{ this };

    gestureTime = pressTime;
    if (!revealedLinks.isEmpty()) {
        clearRevealedPress();
        if (!moved && !consumed && tapped)
            tapped(origin.toPoint());
        return;
    }
    if (std::exchange(annotationDragging, false)) {
        brightnessGesture.cancel();
        if (annotationDragFinished)
            annotationDragFinished(false);
        return;
    }
    const bool brightnessActive = brightnessGesture.active();
    brightnessGesture.cancel();
    if (brightnessActive && lightFinished)
        lightFinished(true);
    if (consumed)
        return;
    const auto delta = latest - origin;
    if (qAbs(delta.x()) >= qMax(100, width() / 10) && qAbs(delta.x()) > qAbs(delta.y()) * 1.5) {
        if (swiped)
            swiped(delta.x() < 0 ? 1 : -1);
    } else if (!moved && tapped)
        tapped(origin.toPoint());
}
} // namespace kobalt
