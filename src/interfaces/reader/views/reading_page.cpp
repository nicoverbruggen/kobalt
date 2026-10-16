#include "interfaces/reader/ui/style/theme.h"
#include "support/qt_font.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "reading_page.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "support/strings.h"
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
using headerfooter::Content;
using headerfooter::Field;
// The battery sits beside the text at a little below the text height.
qreal batteryBody(const QFont &font) {
    return qRound(font.pixelSize() * .75);
}
int textWidth(const QFont &font, Field field, const QString &text) {
    if (text.isEmpty())
        return 0;
    return headerfooter::pageCount(field) ? qCeil(ui::halfSpacedWidth(QFontMetrics(font), text))
                                          : ui::interfaceTextWidth(text, font);
}
int iconWidth(const QFont &font, const Content &content) {
    if (!content.batteryIcon)
        return 0;
    return qCeil(ui::batteryWidth(batteryBody(font))) + (content.text.isEmpty() ? 0 : int(m::reading::batteryGap));
}
int contentWidth(const QFont &font, Field field, const Content &content, bool sleeping = false) {
    if (sleeping && field == Field::Time)
        return font.pixelSize();
    return textWidth(font, field, content.text) + iconWidth(font, content);
}
// Draws one slot within `bounds`, eliding its text when it does not fit.
void paintContent(QPainter &painter, const QRect &bounds, Field field, const Content &content, Qt::Alignment align,
                  const QColor &ink, const QColor &paper, bool sleeping) {
    if (sleeping && field == Field::Time) {
        const int size = qMin(painter.font().pixelSize(), qMin(bounds.width(), bounds.height()));
        if (size <= 0)
            return;
        const int x = align & Qt::AlignRight     ? bounds.right() + 1 - size
                      : align & Qt::AlignHCenter ? bounds.x() + (bounds.width() - size) / 2
                                                 : bounds.x();
        ui::paintOpeningIcon(painter, ui::ControlIcon::Lock,
                             QRectF(x, bounds.y() + (bounds.height() - size) / 2.0, size, size), ink);
        return;
    }
    if (content.empty() || bounds.width() <= 0)
        return;
    const QFont font = painter.font();
    const int icon = iconWidth(font, content);
    const int room = qMax(0, bounds.width() - icon);
    QString text = content.text;
    int width = textWidth(font, field, text);
    if (width > room) {
        text = ui::elideInterfaceText(text, font, room);
        width = ui::interfaceTextWidth(text, font);
        field = Field::Nothing;
    }
    const int total = width + icon;
    const int x = align & Qt::AlignRight     ? bounds.right() + 1 - total
                  : align & Qt::AlignHCenter ? bounds.x() + (bounds.width() - total) / 2
                                             : bounds.x();
    if (!text.isEmpty()) {
        if (headerfooter::pageCount(field))
            ui::drawHalfSpaced(painter, x, bounds, text);
        else
            ui::drawInterfaceText(painter, QRect(x, bounds.y(), width, bounds.height()), text,
                                  Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (content.batteryIcon) {
        const qreal height = batteryBody(font);
        const qreal left = x + total - ui::batteryWidth(height);
        ui::paintBattery(painter,
                         QRectF(left, bounds.y() + (bounds.height() - height) / 2.0,
                                height * int(m::panel::batteryBodyWidth) / int(m::panel::batteryBodyHeight), height),
                         content.battery, content.charging, ink, paper);
    }
}
}
QString ReadingPage::headerText() const {
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
ReadingPage::Slots ReadingPage::stripSlots(bool footer) const {
    const int first = footer ? 3 : 0;
    return {slotContents[first], slotContents[first + 1], slotContents[first + 2]};
}
std::array<headerfooter::Field, 3> ReadingPage::stripFields(bool footer) const {
    const int first = footer ? 3 : 0;
    return {chrome.slotFields[first], chrome.slotFields[first + 1], chrome.slotFields[first + 2]};
}
int ReadingPage::headerRightWidth() const {
    QFont header = font();
    header.setPixelSize(headerTextPixels());
    return qMin(contentWidth(header, chrome.slotFields[2], slotContents[2]), width() / 3);
}
// The ribbon hangs just left of the header's top-right slot and runs a
// little past the strip. Without a strip it is shorter and stays in the top
// margin. Its size follows the screen, so it is the same on every density.
QRect ReadingPage::ribbonRect() const {
    const int ribbonWidth = qRound(width() * .036);
    const int strip = topHeight();
    const int ribbonHeight = strip ? qRound(strip * 1.2) : qRound(height() * .032);
    const int rightWidth = showHeader() ? headerRightWidth() : 0;
    const int right = width() - m::reading::headerSide - (rightWidth ? rightWidth + m::reading::headerTextGap : 0);
    return QRect(right - ribbonWidth, 0, ribbonWidth, ribbonHeight);
}
QRect ReadingPage::bookmarkCorner() const {
    const int w = qCeil(width() * .33), h = kobalt::ui::px(120);
    return QRect(width() - w, 0, w, h);
}
int ReadingPage::topHeight() const {
    return showHeader() ? m::reading::stripHeight(chrome.headerSize) : 0;
}

int ReadingPage::bottomHeight() const {
    return (showFooter() ? m::reading::stripHeight(chrome.footerSize) : 0) +
           (showProgress() ? ui::metrics::reading::progressHeight : 0);
}
// Strip text keeps the Small strip's distance from the page edge at every
// size: it is centred in a band as tall as that strip, at the edge. A larger
// strip only adds space between its text and the book.
QRect ReadingPage::headerTextRect() const {
    if (!showHeader())
        return {};
    return {0, 0, width(), m::reading::stripHeight(QStringLiteral("small"))};
}
QRect ReadingPage::footerTextRect() const {
    if (!showFooter())
        return {};
    // Keep the same edge gap above the progress fill as below the top
    // of the header. The progress strip adds space below the text.
    const int offset = showProgress() ? int(m::reading::progressHeight) - int(m::reading::progressBottom) -
                                            int(m::reading::progressFillHeight)
                                      : 0;
    const int band = m::reading::stripHeight(QStringLiteral("small"));
    const int bottom = height() - bottomHeight() + offset + m::reading::stripHeight(chrome.footerSize);
    return {0, bottom - band, width(), band};
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
    const bool wereShown = !revealedLinks.isEmpty();
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
    // Showing or hiding the outlines repaints the whole page as one rectangle:
    // MediaTek refreshes each page rectangle on its own, which would flash
    // once per link. A change between two sets of links repaints only their outlines.
    if (rectangles.isEmpty() == wereShown) {
        update();
        return;
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
    const HeaderState nextHeader{stripSlots(false), stripFields(false), showHeader(),      panelBelowHeader,
                                 width(),           headerTextPixels(), colors.cacheKey(), weight};
    const FooterState nextFooter{
        stripSlots(true),   stripFields(true), notice, showFooter(), showProgress(), progressFraction, width(),
        footerTextPixels(), colors.cacheKey(), weight};
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
    paintContents(painter);
}

QImage ReadingPage::snapshot(bool sleeping) const {
    QImage frame(size(), QImage::Format_RGB32);
    if (!frame.isNull()) {
        QPainter painter(&frame);
        paintContents(painter, sleeping);
    }
    return frame;
}

void ReadingPage::paintContents(QPainter &painter, bool sleeping) const {
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

// The centre slot is centred on the page, not in a third of it, so a long
// centre keeps its middle while the sides keep their own width. When the
// three do not fit, the sides give way first, down to a third each.
void ReadingPage::paintSlots(QPainter &painter, const QRect &bounds, bool footer, bool sleeping) const {
    const int first = footer ? 3 : 0;
    const QFont font = painter.font();
    int widths[3];
    for (int slot = 0; slot < 3; ++slot)
        widths[slot] = contentWidth(font, chrome.slotFields[first + slot], slotContents[first + slot], sleeping);
    const int side = m::reading::headerSide, gap = m::reading::headerTextGap;
    // The bookmark ribbon hangs left of the top-right slot; keep text off it.
    const int ribbon = footer ? 0 : qRound(width() * .036) + gap;
    int leftRoom = width() - 2 * side, rightRoom = leftRoom - ribbon;
    QRect centre;
    if (widths[1]) {
        int edge = qMax(side + (widths[0] ? widths[0] + gap : 0), side + (widths[2] ? widths[2] + gap : 0) + ribbon);
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
    const auto ink = palette().color(QPalette::WindowText), paper = palette().color(QPalette::Window);
    paintContent(painter, QRect(side, bounds.y(), qMax(0, leftRoom), bounds.height()), chrome.slotFields[first],
                 slotContents[first], Qt::AlignLeft, ink, paper, sleeping);
    if (!centre.isEmpty())
        paintContent(painter, centre, chrome.slotFields[first + 1], slotContents[first + 1], Qt::AlignHCenter, ink,
                     paper, sleeping);
    paintContent(painter, QRect(width() - side - qMax(0, rightRoom), bounds.y(), qMax(0, rightRoom), bounds.height()),
                 chrome.slotFields[first + 2], slotContents[first + 2], Qt::AlignRight, ink, paper, sleeping);
}

void ReadingPage::paintHeader(QPainter &painter, bool sleeping) const {
    QFont font = painter.font();
    font.setPixelSize(headerTextPixels());
    painter.setFont(font);
    if (showHeader())
        paintSlots(painter, headerTextRect(), false, sleeping);
    if (panelBelowHeader)
        painter.fillRect(0, topHeight() - m::control::border, width(), m::control::border, ui::dividerColor(this));
}

void ReadingPage::paintFooter(QPainter &painter, bool sleeping) const {
    if (showFooter()) {
        QFont font = painter.font();
        font.setPixelSize(footerTextPixels());
        painter.setFont(font);
        const auto bounds = footerTextRect().translated(0, bottomHeight() - height());
        if (!notice.isEmpty()) {
            const int room = width() - 2 * m::reading::headerSide;
            ui::drawInterfaceText(painter, bounds, ui::elideInterfaceText(notice, font, room), Qt::AlignCenter);
        } else
            paintSlots(painter, bounds, true, sleeping);
    }
    if (showProgress()) {
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
