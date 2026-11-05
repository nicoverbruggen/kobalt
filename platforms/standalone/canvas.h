#pragma once
#include <QAbstractButton>
#include <QApplication>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QInputMethod>
#include <QScopedValueRollback>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QMouseEvent>
#include <QPointer>
#include <QTouchEvent>
#include <QPointingDevice>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QtMath>
#include <functional>
#include "ui/input/input.h"

namespace kobalt::standalone {
// Qt caches each damaged rectangle, but its default proxy paints their bounding
// rectangle. Pass the cache's clip region to QWidget to keep the work local.
class RegionProxy final : public QGraphicsProxyWidget
{
public:
    explicit RegionProxy(bool nativePixels = false) : nativePixels(nativePixels)
    {
        setCacheMode(QGraphicsItem::DeviceCoordinateCache);
    }

    void setNativePixels(bool value)
    {
        if (nativePixels == value)
            return;
        nativePixels = value;
        pixels = {};
        setCacheMode(QGraphicsItem::NoCache);
        setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        update();
    }

    void setRasterScale(qreal value)
    {
        value = qBound(.1, value, 2.0);
        if (rasterScale == value)
            return;
        rasterScale = value;
        pixels = {};
        setCacheMode(QGraphicsItem::NoCache);
        setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        update();
    }

    QSize rasterSize() const { return pixels.size(); }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *) override
    {
        if (!widget() || !widget()->isVisible())
            return;
        // The cache clears a pixel-aligned margin outside exposedRect. Repaint
        // its entire clip, or that margin becomes a blank strip on hover.
        const QRegion region =
                (painter->hasClipping() ? painter->clipRegion()
                                        : QRegion(option->exposedRect.toAlignedRect()))
                & QRegion(rect().toAlignedRect());
        if (region.isEmpty())
            return;
        if (!nativePixels) {
            // Keep images smooth while Qt paints rules directly on the target pixel grid.
            painter->save();
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            widget()->render(painter, region.boundingRect().topLeft(), region);
            painter->restore();
            return;
        }
        // Keep the source layout fixed while choosing the preview's raster resolution.
        // This buffer is at most 64 MiB. The scene owns its separate display cache.
        constexpr qreal maximumPixels = 16 * 1024 * 1024;
        const QSize screen = widget()->size();
        if (screen.isEmpty())
            return;
        const qreal scale =
                qMin(rasterScale, qSqrt(maximumPixels / (qreal(screen.width()) * screen.height())));
        QSize target(qMax(1, qRound(screen.width() * scale)),
                     qMax(1, qRound(screen.height() * scale)));
        if (qreal(target.width()) * target.height() > maximumPixels)
            target.setHeight(qMax(1, int(maximumPixels / target.width())));
        QRegion damage = region;
        if (pixels.size() != target) {
            pixels = QImage(target, QImage::Format_ARGB32_Premultiplied);
            if (pixels.isNull())
                return;
            pixels.fill(Qt::transparent);
            damage = widget()->rect();
        }
        {
            QPainter source(&pixels);
            source.setRenderHint(QPainter::SmoothPixmapTransform);
            source.scale(qreal(target.width()) / screen.width(),
                         qreal(target.height()) / screen.height());
            widget()->render(&source, damage.boundingRect().topLeft(), damage);
        }
        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(QRectF(widget()->rect()), pixels, QRectF(pixels.rect()));
        painter->restore();
    }

private:
    bool nativePixels;
    qreal rasterScale = 1;
    QImage pixels;
};

// Keep the existing UI in physical pixels. Qt maps its damage rectangles to
// the Android surface. Most controls receive mouse input through the proxy.
// A widget that sets kobalt_native_touch receives mapped touch contacts instead.
class WidgetCanvas : public QGraphicsView
{
public:
    explicit WidgetCanvas(QWidget *parent) : QGraphicsView(parent)
    {
        viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        // Use regional updates for separate scene items and popup widgets.
        setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
        setScene(new QGraphicsScene(this));
    }

    void setContent(QWidget *widget, bool nativePixels = false, qreal rasterScale = 1)
    {
        content = widget;
        proxy = new RegionProxy(nativePixels);
        proxy->setRasterScale(rasterScale);
        scene()->addItem(proxy);
        proxy->setWidget(widget);
    }

    void setNativePixels(bool value)
    {
        if (proxy)
            proxy->setNativePixels(value);
    }

    void setRasterScale(qreal value)
    {
        if (proxy)
            proxy->setRasterScale(value);
    }

    QSize rasterSize() const { return proxy ? proxy->rasterSize() : QSize(); }

    QSize pixelSize() const
    {
        const auto dpr = devicePixelRatioF();
        return { qRound(viewport()->width() * dpr), qRound(viewport()->height() * dpr) };
    }

    void fitContent()
    {
        const auto pixels = pixelSize();
        if (pixels.isEmpty())
            return;
        if (content)
            content->resize(pixels);
        const auto dpr = devicePixelRatioF();
        setTransform(QTransform::fromScale(1.0 / dpr, 1.0 / dpr));
        setSceneRect(QRectF(QPointF(), pixels));
    }

    QPointer<QWidget> content;
    std::function<void()> cancelGesture;
    std::function<void()> viewportResized;
    std::function<void(QRegion)> framePreparing;
    std::function<void(QRegion)> framePainted;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QGraphicsView::resizeEvent(event);
        if (viewportResized)
            viewportResized();
    }

    bool event(QEvent *event) override
    {
        const auto type = event->type();
        const bool handled = QGraphicsView::event(event);
        bool scaleChanged = type == QEvent::ScreenChangeInternal;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        scaleChanged = scaleChanged || type == QEvent::DevicePixelRatioChange;
#endif
        if (scaleChanged && viewportResized)
            viewportResized();
        if (scaleChanged || type == QEvent::InputMethodQuery || type == QEvent::FocusIn
            || type == QEvent::Move || type == QEvent::Resize) {
            updateInputGeometry();
        }
        return handled;
    }

    bool viewportEvent(QEvent *event) override
    {
        const auto type = event->type();
        if (type == QEvent::Paint) {
            const auto region = static_cast<QPaintEvent *>(event)->region();
            if (framePreparing)
                framePreparing(region);
            const bool handled = QGraphicsView::viewportEvent(event);
            if (framePainted)
                framePainted(region);
            return handled;
        }
        if (type == QEvent::TouchBegin || type == QEvent::TouchUpdate || type == QEvent::TouchEnd
            || type == QEvent::TouchCancel) {
            const auto *touch = static_cast<QTouchEvent *>(event);
            // Trackpads already send pointer events. Their finger positions
            // are not screen coordinates and must not become a second mouse gesture.
            if (touch->pointingDevice()
                && touch->pointingDevice()->type() == QInputDevice::DeviceType::TouchPad) {
                event->accept();
                return true;
            }
        }
        if (type == QEvent::TouchCancel) {
            cancel();
            event->accept();
            return true;
        }
        if (type != QEvent::TouchBegin && type != QEvent::TouchUpdate && type != QEvent::TouchEnd)
            return QGraphicsView::viewportEvent(event);
        const auto *touch = static_cast<QTouchEvent *>(event);
        if (content && type == QEvent::TouchBegin && !touch->points().isEmpty()) {
            const auto position =
                    mapToScene(touch->points().first().position().toPoint()).toPoint();
            target = content->childAt(position);
            if (!target)
                target = content;
            nativeTouch = target->property("kobalt_native_touch").toBool();
        }
        if (nativeTouch && target && content) {
            QVector<ui::TouchContact> contacts;
            for (const auto &point : touch->points()) {
                const auto scene = mapToScene(point.position().toPoint()).toPoint();
                contacts.append({ point.id(), target->mapFrom(content, scene),
                                  point.state() == QEventPoint::Released });
            }
            ui::MappedTouchEvent mapped(*touch, std::move(contacts));
            QApplication::sendEvent(target, &mapped);
            if (type == QEvent::TouchEnd) {
                target.clear();
                nativeTouch = false;
            }
            event->accept();
            return true;
        }
        if (!content || touch->points().size() != 1) {
            cancel();
            event->accept();
            return true;
        }
        const auto position = mapToScene(touch->points().first().position().toPoint()).toPoint();
        if (type == QEvent::TouchBegin) {
            target = content->childAt(position);
            if (!target)
                target = content;
        }
        if (target) {
            const bool release = type == QEvent::TouchEnd;
            const auto mouseType = type == QEvent::TouchBegin ? QEvent::MouseButtonPress
                    : release                                 ? QEvent::MouseButtonRelease
                                                              : QEvent::MouseMove;
            const QPoint local = target->mapFrom(content, position);
            QMouseEvent mouse(mouseType, local, target->mapToGlobal(local),
                              type == QEvent::TouchUpdate ? Qt::NoButton : Qt::LeftButton,
                              release ? Qt::NoButton : Qt::LeftButton, touch->modifiers());
            mouse.setTimestamp(touch->timestamp());
            QApplication::sendEvent(target, &mouse);
            if (release)
                target.clear();
        }
        event->accept();
        return true;
    }

private:
    QPointer<RegionProxy> proxy;
    QPointer<QWidget> target;
    bool nativeTouch = false;
    bool updatingInputGeometry = false;

    void updateInputGeometry()
    {
        auto *focus = QGuiApplication::focusObject();
        if (updatingInputGeometry || (focus != this && focus != viewport()) || !proxy
            || !proxy->hasFocus() || !QGraphicsView::inputMethodQuery(Qt::ImEnabled).toBool()) {
            return;
        }
        auto *editor = proxy && proxy->widget() ? proxy->widget()->focusWidget() : nullptr;
        if (!editor) {
            return;
        }
        const auto clip = editor->inputMethodQuery(Qt::ImInputItemClipRectangle);
        if (!clip.isValid() || clip.toRectF().isEmpty()) {
            return;
        }
        const QRectF item = clip.toRectF().translated(proxy->subWidgetRect(editor).topLeft());
        const QRectF rectangle = viewportTransform().mapRect(proxy->mapRectToScene(item));
        QScopedValueRollback<bool> updating(updatingInputGeometry, true);
        // Qt's QWidget focus and resize events cache either the unscaled child
        // rectangle or the whole view. Map the editor's clip without rounding
        // through QRect, which can add a physical pixel at fractional scales.
        auto *input = QGuiApplication::inputMethod();
        input->setInputItemRectangle(rectangle);
        const QPoint offset = viewport()->mapTo(window(), QPoint());
        input->setInputItemTransform(QTransform::fromTranslate(offset.x(), offset.y()));
    }

    void cancel()
    {
        // Cancellation handlers can delete the target or start another gesture.
        QPointer<QWidget> cancelled = target;
        target.clear();
        nativeTouch = false;
        if (cancelled) {
            QTouchEvent event(QEvent::TouchCancel);
            QApplication::sendEvent(cancelled, &event);
        }
        if (auto *button = qobject_cast<QAbstractButton *>(cancelled.data()))
            button->setDown(false);
        if (content && cancelGesture)
            cancelGesture();
    }
};

} // namespace kobalt::standalone
