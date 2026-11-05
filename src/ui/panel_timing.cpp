#include "panel_timing.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QEvent>
#include <QPointer>
#include <QWidget>

namespace kobalt::ui {
namespace {
const char *const watchName = "kobaltPanelTiming";
// A queued call runs after the widget's paint returns, so the time includes
// the layout that painting can trigger. A hidden widget never reports.
class PaintWatch final : public QObject {
public:
    PaintWatch(QWidget *widget, std::function<void()> painted) : QObject(widget), painted(std::move(painted)) {
        setObjectName(QString::fromLatin1(watchName));
        widget->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint && !seen) {
            seen = true;
            QPointer<PaintWatch> guard(this);
            qt::invoke(
                this,
                [guard] {
                    if (guard) {
                        guard->painted();
                        guard->deleteLater();
                    }
                },
                Qt::QueuedConnection);
        } else if (event->type() == QEvent::Hide && !seen) {
            deleteLater();
        }
        return false;
    }

private:
    std::function<void()> painted;
    bool seen = false;
};
}
void afterNextPaint(QWidget *widget, std::function<void()> painted) {
    new PaintWatch(widget, std::move(painted));
}
PanelTiming::PanelTiming(const QString &panel) : active(diagnosticsActive()) {
    if (!active)
        return;
    total.start();
    step.start();
    fields["panel"] = panel;
}
void PanelTiming::stage(const char *name) {
    if (!active)
        return;
    fields[QString::fromLatin1(name) + "_ms"] = step.nsecsElapsed() / 1e6;
    step.restart();
}
void PanelTiming::field(const char *name, double value) {
    if (active)
        fields[QString::fromLatin1(name)] = value;
}
void PanelTiming::finish(QWidget *widget) {
    // A hidden panel never paints, so a watch would report a later, unrelated paint.
    if (!active || !widget || !widget->isVisible())
        return;
    stage("open");
    // A newer opening replaces a watch that never saw its paint.
    for (auto *old : widget->findChildren<QObject *>(QString::fromLatin1(watchName), Qt::FindDirectChildrenOnly))
        delete old;
    new PaintWatch(widget, [total = total, step = step, fields = fields]() mutable {
        fields["paint_ms"] = step.nsecsElapsed() / 1e6;
        fields["total_ms"] = total.nsecsElapsed() / 1e6;
        diagnostic("panel-open", fields, false);
    });
}
}
