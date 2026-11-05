#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <functional>

class QWidget;

namespace kobalt::ui {
// Times a panel from the input that opens it until its first paint has
// finished, and writes one "panel-open" diagnostics event. Stages record the
// time since the previous stage, so an asynchronous lookup and the synchronous
// open call can be told apart. Nothing is recorded while diagnostics are off.
class PanelTiming {
public:
    explicit PanelTiming(const QString &panel);
    void stage(const char *name);
    void field(const char *name, double value);
    // Ends the "open" stage and reports once the panel has painted.
    void finish(QWidget *panel);

private:
    QElapsedTimer total, step;
    QJsonObject fields;
    bool active;
};
// Calls `painted` once `widget` has finished its next paint, including the
// layout that painting can trigger. Nothing is called if it hides first.
void afterNextPaint(QWidget *widget, std::function<void()> painted);
}
