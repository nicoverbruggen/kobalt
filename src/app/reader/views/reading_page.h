#pragma once
#include <QPainterPath>
#include "ui/style/metrics.h"
#include "reading/header_footer.h"
#include "reading/page_turn_animation.h"
#include "reading/highlight_geometry.h"
#include "ui/input/brightness_gesture.h"
#include <QWidget>
#include <QImage>
#include "ui/input/hold_timer.h"
#include <QRegion>
#include <QElapsedTimer>
#include <QTimeLine>
#include <QTimer>
#include <QHash>
#include <array>
#include <functional>
#include <tuple>
class QPainter;
class QTouchEvent;

namespace kobalt {
struct AnnotationGeometry;

class ReadingPage : public QWidget
{
public:
    QImage image;
    int animationDirection = 0;
    void setPageAnimationStyle(pageTurn::Style style);

    struct ReflowLayout
    {
        int fontPixels = 40, lineSpacing = 150;
        QMargins margins;
        int columns = 1, columnSpacing = 0;
    };

    ReflowLayout reflowLayout;
    // A short sample from the displayed page, prepared after its first paint.
    // Lengths are in Unicode characters; transition widths remain estimates.
    void setReflowText(const QString &text);

    const QVector<int> &reflowWordLengths() const { return wordLengths; }

    // Show estimated text lines while the replacement page is laid out.
    void setViewportTransition(bool active);

    bool viewportTransition() const { return changingViewport; }

    const QImage &headerImage() const { return headerPixels; }

    const QImage &footerImage() const { return footerPixels; }

    // What the header and footer show. `notice`, such as an error, replaces
    // the footer's slots while it is set. It never adds a footer, because
    // that would change the page height.
    headerfooter::Layout chrome = headerfooter::presetLayout(headerfooter::Preset::Default);
    std::array<headerfooter::Content, headerfooter::slotCount> slotContents;
    QString notice;
    qreal progressFraction = 0;

    bool showHeader() const { return chrome.header(); }

    bool showFooter() const { return chrome.footer(); }

    bool showProgress() const { return chrome.progressBar; }

    int headerTextPixels() const
    {
        return ui::metrics::reading::textPixels(chrome.headerSize, false);
    }

    int footerTextPixels() const
    {
        return ui::metrics::reading::textPixels(chrome.footerSize, true);
    }

    // The header's accessible text: its slots from left to right.
    QString headerText() const;
    void setPanelBelowHeader(bool visible);
    int topHeight() const;
    int bottomHeight() const;
    QRect headerTextRect() const;
    // The footer's text line, in page coordinates. Empty without a footer.
    QRect footerTextRect() const;
    // Rounded screen corners that touch the page's top or bottom edge.
    void setCornerRadii(int top, int bottom);
    // The space beside a strip's slot text. It grows to keep the text clear of
    // a rounded screen corner.
    int slotSide(bool footer) const;
    // The ribbon that marks a bookmarked page, and the corner that toggles it.
    bool bookmarked = false;
    QRect ribbonRect() const;
    QRect bookmarkCorner() const;
    std::function<void(QPoint)> held;
    // An image hold takes precedence over text selection, including annotation mode.
    std::function<bool(QPoint)> imageHeld;
    // Ends the current touch: no lookup, tap, swipe or light change follows.
    void cancelHold();

    // Invalidates the held text/image, not the contact. Source time decides at
    // release: a long hold is consumed; a short tap or a swipe can still complete.
    void invalidateHoldTarget()
    {
        holdTarget = HoldTarget::Stale;
        holdTimer.stopTimeout();
    }

    // A vertical drag along the left edge changes brightness and one along
    // the right edge changes warmth, when each edge is enabled. `lightStarted`
    // receives true for warmth and may refuse the drag.
    bool brightnessEdge = true, warmthEdge = false;
    std::function<bool(bool)> lightStarted;
    std::function<void(double)> lightChanged;
    std::function<void(bool)> lightFinished;
    static QRegion annotationContour(const QVector<QRect> &rectangles, bool vertical = false);
    void setAnnotationInputEnabled(bool enabled);
    void setAnnotationHandles(const AnnotationGeometry &geometry);
    QRect annotationHandleRect(bool start) const;
    QRegion annotationHandleHitRegion() const;
    // Boundary -1 starts a new drag; 0 and 1 move the source start and end.
    std::function<bool(QPoint, int)> annotationDragStarted;
    std::function<void(QPoint)> annotationDragMoved;
    std::function<void(bool)> annotationDragFinished;

    bool annotationContains(QPoint point) const { return draftContour.contains(point); }

    void annotate(const QVector<QRect> &draft, const QVector<QRect> &saved,
                  const QVector<QRect> &candidate = {}, bool vertical = false);
    void setSavedHighlights(const QVector<HighlightGeometry> &groups);
    // Install destination markup without queuing a separate overlay paint.
    // refreshContent consumes its old and new damage with the page image.
    void installAnnotationFrame(const QVector<QRect> &draft,
                                const QVector<HighlightGeometry> &saved, bool vertical = false,
                                const AnnotationGeometry *geometry = nullptr);
    void mark(const QVector<QRect> &rectangles);
    void revealLinks(const QVector<QRect> &rectangles, const QVector<QRect> &footnotes = {});
    std::function<void(QPoint)> tapped;
    std::function<void(int)> swiped;

    // Available only while a completed input gesture calls tapped or swiped.
    quint32 gestureTimestamp() const { return gestureTime; }

    std::function<void()> annotationRequested;
    ReadingPage(QWidget *parent, std::function<void()> cancelAnimation);
    // Copy the cached page and chrome without widget layout or backing-store work.
    QImage snapshot(bool sleeping = false) const;
    // Cached chrome with the bookmark ribbon, without copying the page body.
    QImage stripSnapshot(bool footer) const;
    void cleanRefresh(bool clean);
    void refreshContent(bool reading = false, bool now = false);
    // Set before refreshContent when the engine prepared the changed rectangle
    // from the image with this cache key to `image`. 0 means not prepared.
    QRect preparedDamage;
    qint64 preparedFrom = 0;

protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    std::function<void()> cancelAnimation;
    void stopPageAnimation();
    pageTurn::Style pageAnimationStyle = pageTurn::Style::Off;
    QTimeLine pageTurnAnimation;
    QImage animationFrame, animationFrom, animationTo;
    int animationTurnDirection = 0, animationStep = -1;
    // Text weight and family changes draw the strips again.
    using Slots = std::array<headerfooter::Content, 3>;
    using HeaderState = std::tuple<Slots, std::array<headerfooter::Field, 3>, bool, bool, int, int,
                                   qint64, int, QString>;
    using FooterState = std::tuple<Slots, std::array<headerfooter::Field, 3>, QString, bool, bool,
                                   qreal, int, int, qint64, int, QString>;
    Slots stripSlots(bool footer) const;
    std::array<headerfooter::Field, 3> stripFields(bool footer) const;
    void paintSlots(QPainter &painter, const QRect &bounds, bool footer,
                    bool sleeping = false) const;
    bool panelBelowHeader = false;
    int cornerTop = 0, cornerBottom = 0;
    HeaderState headerState;
    FooterState footerState;
    QImage renderedBody, headerPixels, footerPixels;
    QSize renderedSize;
    qint64 renderedPalette = 0;
    int renderedTop = -1, renderedBottom = -1;
    QRect renderedRibbon;
    QImage renderStrip(int height, const std::function<void(QPainter &)> &draw);
    void paintHeader(QPainter &painter, bool sleeping = false) const;
    void paintFooter(QPainter &painter, bool sleeping = false) const;
    void paintContents(QPainter &painter, bool sleeping = false) const;
    void paintBody(QPainter &painter) const;
    bool changingViewport = false, viewportNoticeVisible = false;
    QVector<int> wordLengths;
    QTimer viewportNoticeTimer;
    int headerRightWidth() const;
    ui::HoldTimer holdTimer;
    enum class HoldTarget { Current, Stale };
    HoldTarget holdTarget = HoldTarget::Stale;
    QVector<QRect> selection, revealedLinks, draftAnnotations, savedAnnotations,
            candidateAnnotations;
    QRegion draftContour, annotationColorRegion;
    QPainterPath draftOutline, candidateOutline;

    struct AnnotationHandle
    {
        QPoint center, target;
        bool visible = false;
    };

    std::array<AnnotationHandle, 2> annotationHandles;
    bool annotationInputEnabled = false, annotationDragging = false;
    QPoint annotationDragOffset;
    int annotationPointerBoundary = -1;
    void beginPointer(QPointF point, const QInputEvent *event);
    void triggerHold();
    bool annotationVertical = false, installingAnnotationFrame = false;
    QRegion annotationFrameDamage;
    void damageAnnotations(const QRegion &dirty);
    QVector<HighlightGeometry> annotationColorGroups;

    struct PaintedHighlight
    {
        HighlightGeometry mark;
        QRegion visible;
    };

    QVector<PaintedHighlight> annotationTints;
    QVector<QRect> revealedOutlines, revealedFootnotes;
    QVector<QPainterPath> revealedCircles;
    QPainterPath revealedPress;
    void pressRevealedLink(QPoint point);
    void clearRevealedPress();
    bool tracking = false, moved = false, consumed = false;
    QElapsedTimer touchClock;
    QHash<int, QPointF> touchOrigins;
    bool multipleTouch = false, annotationTouch = false, rightPressed = false, rightMoved = false;
    bool annotationCenter(QPointF point) const;
    bool annotationTouchEvent(QTouchEvent *event);
    QPointF origin, latest;
    kobalt::ui::BrightnessGesture brightnessGesture;
    void updatePointer();
    void finishGesture();
    quint32 pressTime = 0, gestureTime = 0;
};

} // namespace kobalt
