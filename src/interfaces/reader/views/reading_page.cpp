#include "interfaces/reader/ui/style/theme.h"
#include "support/qt_font.h"
#include "reading_page.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "ui/input.h"
#include "ui/image_damage.h"
#include "model/page_refresh_regions.h"
#include <QVariant>
#include "platform/api/kobo_refresh.h"
#include <QPainter>
#include <QtMath>
#include <QTouchEvent>
#include <QMouseEvent>
#include <QHideEvent>
#include <utility>

namespace kobalt {
namespace m = ui::metrics;
using platform::textRefresh;
namespace {
qreal progressWidth(const QFontMetrics &metrics, const QString &text) {
    return qt::textWidth(metrics, text) - text.count(' ') * qt::textWidth(metrics, QStringLiteral(" ")) / 2.0;
}
void paintProgress(QPainter &painter, const QRect &bounds, const QString &text, bool rightAligned) {
    const auto metrics = painter.fontMetrics();
    qreal x = rightAligned ? bounds.x() + bounds.width() - progressWidth(metrics, text)
                           : bounds.x() + (bounds.width() - progressWidth(metrics, text)) / 2;
    const qreal baseline = bounds.y() + (bounds.height() - metrics.height()) / 2.0 + metrics.ascent();
    // A Unicode thin space still occupies a full cell in some mono fonts.
    const qreal space = qt::textWidth(metrics, QStringLiteral(" ")) / 2.0;
    for (const auto &word : text.split(' ')) {
        painter.drawText(QPointF(x, baseline), word);
        x += qt::textWidth(metrics, word) + space;
    }
}
}
int ReadingPage::headerProgressWidth() const {
    QFont header = font();
    header.setPixelSize(headerTextPixels);
    return qCeil(progressWidth(QFontMetrics(header), chapterProgress));
}
// The ribbon hangs just left of the header's chapter progress and runs a
// little past the strip. Without a strip it is shorter and stays in the top
// margin. Its size follows the screen, so it is the same on every density.
QRect ReadingPage::ribbonRect() const {
    const int ribbonWidth = qRound(width() * .036);
    const int strip = topHeight();
    const int ribbonHeight = strip ? qRound(strip * 1.2) : qRound(height() * .032);
    const int right = showHeader ? width() - m::reading::headerSide - headerProgressWidth() - m::reading::headerTextGap
                                 : width() - m::reading::headerSide;
    return QRect(right - ribbonWidth, 0, ribbonWidth, ribbonHeight);
}
QRect ReadingPage::bookmarkCorner() const {
    const int w = qCeil(width() * .33), h = kobalt::ui::px(120);
    return QRect(width() - w, 0, w, h);
}
int ReadingPage::topHeight() const {
    return showHeader ? m::reading::stripHeight(headerSize) : clockVisible() ? m::reading::clockHeight : 0;
}

int ReadingPage::bottomHeight() const {
    return (showFooter ? m::reading::stripHeight(footerSize) : 0) +
           (showProgress ? ui::metrics::reading::progressHeight : 0);
}
QRect ReadingPage::footerTextRect() const {
    if (!showFooter)
        return {};
    // Keep the same edge gap above the progress fill as below the top
    // of the header. The progress strip adds space below the text.
    const int offset = showProgress ? int(m::reading::progressHeight) - int(m::reading::progressBottom) -
                                          int(m::reading::progressFillHeight)
                                    : 0;
    return {0, height() - bottomHeight() + offset, width(), m::reading::stripHeight(footerSize)};
}

void ReadingPage::cancelHold() {
    cancelAnimation();
    window()->setProperty("reader_animation_direction", 0);
    animationDirection = 0;
    holdTimer.stop();
    tracking = false;
    const bool active = brightnessGesture.active();
    brightnessGesture.cancel();
    if (active && lightFinished)
        lightFinished(false);
}

void ReadingPage::revealLinks(const QVector<QRect> &rectangles) {
    if (revealedLinks == rectangles)
        return;
    QRegion dirty;
    for (const auto &outline : revealedOutlines)
        dirty += outline;
    revealedLinks = rectangles;
    // Pad each link, then split the space where padded links meet so that
    // links on consecutive lines, as in a contents page, keep separate outlines.
    const int pad = m::links::revealPadding, gap = m::links::revealGap;
    revealedOutlines.clear();
    for (const auto &r : rectangles)
        revealedOutlines += r.adjusted(-pad, -pad, pad, pad);
    for (int i = 0; i < rectangles.size(); ++i)
        for (int j = i + 1; j < rectangles.size(); ++j) {
            if (!revealedOutlines[i].intersects(revealedOutlines[j]))
                continue;
            const QRect &a = rectangles[i], &b = rectangles[j];
            const int shared = qMin(a.bottom(), b.bottom()) - qMax(a.top(), b.top());
            const bool sameLine = 2 * shared > qMin(a.height(), b.height());
            const int first = (sameLine ? a.left() <= b.left() : a.top() <= b.top()) ? i : j,
                      second = first == i ? j : i;
            auto &before = revealedOutlines[first], &after = revealedOutlines[second];
            if (sameLine) {
                const int middle = (rectangles[first].right() + rectangles[second].left()) / 2;
                before.setRight(qMin(before.right(), middle - gap / 2));
                after.setLeft(qMax(after.left(), middle + gap - gap / 2));
            } else {
                const int middle = (rectangles[first].bottom() + rectangles[second].top()) / 2;
                before.setBottom(qMin(before.bottom(), middle - gap / 2));
                after.setTop(qMax(after.top(), middle + gap - gap / 2));
            }
        }
    for (const auto &outline : revealedOutlines)
        dirty += outline;
    if (!dirty.isEmpty())
        update(dirty.translated(0, topHeight()));
}

void ReadingPage::mark(const QVector<QRect> &rectangles) {
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
    : QWidget(parent), cancelAnimation(std::move(cancelAnimation)) {
    Q_ASSERT(this->cancelAnimation);
    setObjectName("rrPage");
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    textRefresh(this);
    holdTimer.setSingleShot(true);
    holdTimer.setInterval(m::lookup::holdMilliseconds);
    connect(&holdTimer, &QTimer::timeout, this, [this] {
        if (!tracking || moved)
            return;
        consumed = true;
        brightnessGesture.cancel();
        if (held)
            held(origin.toPoint() - QPoint(0, topHeight()));
    });
}

void ReadingPage::cleanRefresh(bool clean) {
    kobalt::platform::pageRefresh(this, clean);
}

void ReadingPage::setPanelBelowHeader(bool visible) {
    if (panelBelowHeader == visible)
        return;
    panelBelowHeader = visible;
    refreshContent();
}

void ReadingPage::refreshContent(bool reading, bool now) {
    const auto colors = palette();
    const bool geometryChanged =
        renderedSize != size() || renderedTop != topHeight() || renderedBottom != bottomHeight();
    const int weight = kobalt::ui::interfaceWeight();
    const HeaderState nextHeader{chapterTitle,     chapterProgress, clockText,        showHeader,        clockVisible(),
                                 panelBelowHeader, width(),         headerTextPixels, colors.cacheKey(), weight};
    const FooterState nextFooter{progress, showFooter,       showProgress,      progressFraction,
                                 width(),  footerTextPixels, colors.cacheKey(), weight};
    kobalt::platform::pageRefresh(this, false,
                                  reading && !geometryChanged && renderedPalette == colors.cacheKey() &&
                                      renderedBody.text("kobalt.animation_text_only") == "true" &&
                                      image.text("kobalt.animation_text_only") == "true");
    QRegion dirty((preparedFrom && preparedFrom == renderedBody.cacheKey() && renderedBody.size() == image.size()
                       ? preparedDamage
                       : kobalt::ui::imageDamage(renderedBody, image))
                      .translated(0, topHeight()));
    preparedFrom = 0;
    const auto oldRegions = pageRefreshRegions(renderedBody, true);
    const auto newRegions = pageRefreshRegions(image, true);
    window()->setProperty(
        "reader_image_repairs",
        QVariant::fromValue((oldRegions | newRegions).translated(mapTo(window(), QPoint(0, topHeight())))));
    window()->setProperty(
        "reader_mono_repairs",
        QVariant::fromValue((pageRefreshRegions(renderedBody, false) | pageRefreshRegions(image, false))
                                .translated(mapTo(window(), QPoint(0, topHeight())))));
    window()->setProperty("reader_colour_regions", image.text("kobalt.colour_regions") != "[]" &&
                                                       image.text("kobalt.animation_text_only") != "true");
    renderedBody = image;
    if (geometryChanged || nextHeader != headerState) {
        auto next = renderStrip(topHeight(), [this](QPainter &p) {
            paintHeader(p);
        });
        dirty += kobalt::ui::imageDamage(headerPixels, next);
        headerPixels = next;
        headerState = nextHeader;
    }
    if (geometryChanged || nextFooter != footerState) {
        auto next = renderStrip(bottomHeight(), [this](QPainter &p) {
            paintFooter(p);
        });
        dirty += kobalt::ui::imageDamage(footerPixels, next).translated(0, height() - bottomHeight());
        footerPixels = next;
        footerState = nextFooter;
    }
    const QRect ribbon = bookmarked ? ribbonRect() : QRect();
    if (ribbon != renderedRibbon) {
        dirty += ribbon;
        dirty += renderedRibbon;
        renderedRibbon = ribbon;
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
    window()->setProperty("reader_animation_direction", reading && !geometryChanged ? turn : 0);
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

void ReadingPage::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    painter.drawImage(0, topHeight(), renderedBody);
    if (!revealedOutlines.isEmpty()) {
        QRegion linkRegion;
        for (const auto &r : revealedOutlines)
            linkRegion += r.translated(0, topHeight());
        const bool dark = ui::darkTheme(this);
        painter.save();
        // Tint the paper without covering the rendered text. The union keeps
        // the tint even where the rectangles of one link overlap.
        painter.setClipRegion(linkRegion);
        painter.setCompositionMode(dark ? QPainter::CompositionMode_Screen : QPainter::CompositionMode_Multiply);
        painter.fillRect(linkRegion.boundingRect(), QColor(dark ? m::links::revealDark : m::links::revealLight));
        painter.restore();
        const int outline = m::links::revealOutline;
        for (const auto &r : revealedOutlines)
            ui::paintEdges(painter, r.translated(0, topHeight()),
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           palette().color(QPalette::WindowText), outline);
    }
    painter.save();
    painter.setCompositionMode(QPainter::CompositionMode_Difference);
    for (const auto &r : selection)
        painter.fillRect(r.translated(0, topHeight()), Qt::white);
    painter.restore();
    painter.drawImage(0, 0, headerPixels);
    painter.drawImage(0, height() - bottomHeight(), footerPixels);
    if (!renderedRibbon.isEmpty()) {
        const QRectF r = renderedRibbon;
        QPolygonF ribbon;
        ribbon << r.topLeft() << r.topRight() << r.bottomRight() << QPointF(r.center().x(), r.top() + r.height() * .78)
               << r.bottomLeft();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::WindowText));
        painter.drawPolygon(ribbon);
    }
}

bool ReadingPage::event(QEvent *event) {
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate ||
        event->type() == QEvent::TouchEnd) {
        const auto points = kobalt::ui::touchPositions(static_cast<QTouchEvent *>(event));
        if (event->type() == QEvent::TouchBegin) {
            cancelHold();
            tracking = points.size() == 1;
            moved = consumed = false;
            if (tracking)
                holdTimer.start();
            if (tracking) {
                origin = latest = points.first();
                brightnessGesture.begin(origin, size(), brightnessEdge, warmthEdge);
            }
        }
        if (points.size() != 1)
            cancelHold();
        if (tracking) {
            latest = points.first();
            updatePointer();
        }
        if (!tracking || moved)
            holdTimer.stop();
        if (event->type() == QEvent::TouchEnd)
            finishGesture();
        event->accept();
        return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        cancelHold();
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void ReadingPage::mousePressEvent(QMouseEvent *event) {
    if (!kobalt::ui::isPointerMouse(event) || event->button() != Qt::LeftButton)
        return;
    cancelHold();
    tracking = true;
    moved = consumed = false;
    origin = latest = kobalt::ui::mousePosition(event);
    brightnessGesture.begin(origin, size(), brightnessEdge, warmthEdge);
    holdTimer.start();
}

void ReadingPage::mouseMoveEvent(QMouseEvent *event) {
    if (!tracking || !kobalt::ui::isPointerMouse(event))
        return;
    latest = kobalt::ui::mousePosition(event);
    updatePointer();
}

void ReadingPage::mouseReleaseEvent(QMouseEvent *event) {
    if (!kobalt::ui::isPointerMouse(event) || event->button() != Qt::LeftButton)
        return;
    latest = kobalt::ui::mousePosition(event);
    updatePointer();
    finishGesture();
}

void ReadingPage::hideEvent(QHideEvent *event) {
    cancelHold();
    QWidget::hideEvent(event);
}

QImage ReadingPage::renderStrip(int height, const std::function<void(QPainter &)> &draw) {
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

void ReadingPage::paintHeader(QPainter &painter) const {
    QFont font = painter.font();
    font.setPixelSize(headerTextPixels);
    painter.setFont(font);
    const int clockWidth = showClock ? kobalt::qt::textWidth(painter.fontMetrics(), clockText) +
                                           m::reading::headerSide + m::reading::headerTextGap
                                     : m::reading::headerSide;
    if (showHeader) {
        const int ribbonSpace = qRound(width() * .036) + m::reading::headerTextGap;
        const int sideWidth = qMax(clockWidth, qCeil(progressWidth(painter.fontMetrics(), chapterProgress)) +
                                                   m::reading::headerSide + m::reading::headerTextGap + ribbonSpace);
        const QRect titleBounds(sideWidth, 0, qMax(0, width() - 2 * sideWidth), m::reading::stripHeight(headerSize));
        ui::drawInterfaceText(painter, titleBounds,
                              ui::elideInterfaceText(chapterTitle.toUpper(), painter.font(), titleBounds.width()),
                              Qt::AlignCenter);
        paintProgress(
            painter,
            QRect(m::reading::headerSide, 0, width() - 2 * m::reading::headerSide, m::reading::stripHeight(headerSize)),
            chapterProgress, true);
    }
    if (clockVisible())
        painter.drawText(QRect(m::reading::headerSide, 0, width() - 2 * m::reading::headerSide, topHeight()),
                         Qt::AlignLeft | Qt::AlignVCenter, clockText);
    if (panelBelowHeader)
        painter.fillRect(0, topHeight() - m::control::border, width(), m::control::border, ui::dividerColor(this));
}

void ReadingPage::paintFooter(QPainter &painter) const {
    if (showFooter) {
        QFont font = painter.font();
        font.setPixelSize(footerTextPixels);
        painter.setFont(font);
        paintProgress(painter, footerTextRect().translated(0, bottomHeight() - height()), progress, false);
    }
    if (showProgress) {
        const auto ink = palette().color(QPalette::WindowText);
        painter.fillRect(QRect(0, bottomHeight() - m::reading::progressBottom, width(), m::control::border), ink);
        painter.fillRect(QRect(0, bottomHeight() - m::reading::progressBottom - m::reading::progressFillHeight,
                               qRound(width() * progressFraction), m::reading::progressFillHeight),
                         ink);
    }
}

void ReadingPage::updatePointer() {
    moved |= (latest - origin).manhattanLength() > m::lookup::moveTolerance;
    if (moved)
        holdTimer.stop();
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

void ReadingPage::finishGesture() {
    holdTimer.stop();
    if (!tracking)
        return;
    tracking = false;
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
}
