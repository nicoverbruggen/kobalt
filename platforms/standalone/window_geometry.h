#pragma once
#include "canvas.h"
#include <QLabel>
#include <QStackedLayout>
#include <QSize>
#include <QWidget>
#include <QtMath>
#include <utility>

namespace kobalt::standalone {
// Android can finish rotation after Qt chooses its initial fullscreen size.
// Follow the activity's laid-out bounds.
inline void fitWindowToSurface(QWidget &window, QSize pixels)
{
    if (pixels.isEmpty())
        return;
    const auto ratio = window.devicePixelRatioF();
    const QSize logical(qRound(pixels.width() / ratio), qRound(pixels.height() / ratio));
    if (window.size() != logical)
        window.resize(logical);
}

// The activity draws edge to edge. Apply its visible system-bar and cutout
// insets explicitly, including on cold launch before Qt reports safe margins.
inline void fitWindowToSurface(QWidget &window, QSize pixels, QMargins safePixels)
{
    if (pixels.isEmpty())
        return;
    const auto ratio = window.devicePixelRatioF();
    window.setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    // Library and reader share the same centered horizontal bounds even
    // when a cutout or navigation bar occupies just one side.
    const int side = qCeil(qMax(safePixels.left(), safePixels.right()) / ratio);
    window.setContentsMargins(side, qCeil(safePixels.top() / ratio), side,
                              qCeil(safePixels.bottom() / ratio));
    fitWindowToSurface(window, pixels);
}

inline bool contentFitsSurface(const QWidget &window, QSize pixels)
{
    if (pixels.isEmpty())
        return false;
    const auto ratio = window.devicePixelRatioF();
    const auto actual = window.contentsRect().size();
    // Each inset rounds outward by less than a logical pixel; the surface
    // itself rounds to the nearest one. Allow that conversion, not stale bars.
    const int rounding = qCeil(2.5 * ratio);
    return qAbs(qRound(actual.width() * ratio) - pixels.width()) <= rounding
            && qAbs(qRound(actual.height() * ratio) - pixels.height()) <= rounding;
}

// Android can clear or reconfigure its Surface while system bars return.
// Keep a paintable copy of the reader above the Library until its insets settle.
class SurfaceHandoff
{
public:
    explicit SurfaceHandoff(QWidget &window) : window(window) { }

    ~SurfaceHandoff() { delete overlay.data(); }

    bool active() const { return !overlay.isNull(); }

    WidgetCanvas *canvas() const { return overlay; }

    // Capture before reader writeback clears its rendered page.
    void capture()
    {
        if (!overlay)
            captured = window.grab();
    }

    void hold(const std::function<void(WidgetCanvas *)> &configure = {})
    {
        if (overlay)
            return;
        auto pixels = std::exchange(captured, QPixmap());
        if (pixels.isNull())
            pixels = window.grab();
        pixels.setDevicePixelRatio(1);
        auto *image = new QLabel;
        image->setPixmap(pixels);
        image->setFixedSize(pixels.size());
        image->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        overlay = new WidgetCanvas(&window);
        overlay->setAttribute(Qt::WA_OpaquePaintEvent);
        overlay->setGeometry(window.rect());
        overlay->setContent(image);
        if (configure)
            configure(overlay);
        overlay->viewportResized = [this] {
            const auto ratio = overlay->devicePixelRatioF();
            const auto origin = QPointF(overlay->viewport()->mapTo(&window, QPoint())) * ratio;
            overlay->setTransform(QTransform::fromScale(1.0 / ratio, 1.0 / ratio));
            overlay->setSceneRect(QRectF(origin, overlay->pixelSize()));
        };
        auto *layout = qobject_cast<QStackedLayout *>(window.layout());
        Q_ASSERT(layout);
        layout->addWidget(overlay);
        layout->setCurrentWidget(overlay);
        overlay->viewportResized();
        overlay->viewport()->repaint();
        window.repaint();
    }

    bool present(bool geometryReady)
    {
        if (!overlay || !geometryReady)
            return false;
        delete overlay.data();
        window.repaint();
        return true;
    }

private:
    QWidget &window;
    QPointer<WidgetCanvas> overlay;
    QPixmap captured;
};
} // namespace kobalt::standalone
