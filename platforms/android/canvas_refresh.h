#pragma once
#include "boox_frame_queue.h"
#include "boox_refresh.h"
#include "platforms/standalone/canvas.h"
#include <QTimer>

namespace kobalt::android {
// Each app canvas uses the same Surface protocol. Only reading pages add
// REGAL text regions; Library, opening and dialog controls use GU regions.
class CanvasRefresh final : public QObject
{
public:
    CanvasRefresh(standalone::WidgetCanvas *canvas, std::function<bool()> available,
                  std::function<bool(const RefreshFrame &)> submit, bool cleanOnReveal = true)
        : QObject(canvas),
          canvas(canvas),
          available(std::move(available)),
          submit(std::move(submit)),
          cleanOnReveal(cleanOnReveal),
          cleanPending(cleanOnReveal)
    {
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this] {
            if (!queue.pending() || !this->canvas->isVisible() || !this->available() || paused)
                return;
            const auto ratio = this->canvas->viewport()->devicePixelRatioF();
            const auto origin = QPointF(this->canvas->viewport()->mapToGlobal(QPoint())) * ratio;
            const auto pixels = QRectF(queue.nextArea()).translated(-origin);
            const auto logical =
                    QRectF(pixels.topLeft() / ratio, pixels.size() / ratio).toAlignedRect();
            // Preserve the queued region modes. A new clean request must first
            // enter the queue through the paint policy.
            QScopedValueRollback<bool> presenting(presentingRegion, !cleanPending);
            this->canvas->viewport()->repaint(logical);
        });
        connect(qApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    paused = state != Qt::ApplicationActive;
                    if (paused)
                        timer.stop();
                    else
                        schedule();
                });
        canvas->viewport()->installEventFilter(this);
        canvas->framePreparing = [this](const QRegion &region) {
            accepted = false;
            auto *content = this->canvas->content.data();
            if (!content || !this->available() || region.isEmpty())
                return;
            if (presentingRegion) {
                accepted = queue.present(this->submit);
                return;
            }
            auto *page = static_cast<ReadingPage *>(content->findChild<QWidget *>("rrPage"));
            policy.prepare(content, this->canvas->viewport(), page, region,
                           [this](RefreshFrame frame) {
                               frame.clean = frame.clean || cleanPending;
                               accepted = queue.prepare(frame, this->submit);
                               // The queue owns the frame even when its first submission fails.
                               cleanPending = false;
                               return true;
                           });
        };
        canvas->framePainted = [this](const QRegion &) { schedule(); };
    }

    void requestClean() { cleanPending = true; }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Resize
            || event->type() == QEvent::Show) {
            timer.stop();
            queue.reset();
            policy.reset();
            accepted = false;
            cleanPending = cleanOnReveal;
        }
        return false;
    }

private:
    void schedule()
    {
        if (paused || !queue.pending() || !canvas->isVisible() || timer.isActive())
            return;
        // Rejected submissions can mean a Surface is being recreated.
        // Keep retrying without requiring input, at a lower paint rate.
        // Accepted regions leave time for Qt's queued backing-store flush.
        timer.start(accepted ? 34 : 100);
    }

    standalone::WidgetCanvas *canvas;
    std::function<bool()> available;
    std::function<bool(const RefreshFrame &)> submit;
    BooxRefreshPolicy policy;
    BooxFrameQueue queue;
    QTimer timer;
    bool accepted = false, presentingRegion = false;
    bool paused = QGuiApplication::applicationState() != Qt::ApplicationActive;
    bool cleanOnReveal, cleanPending;
};
} // namespace kobalt::android
