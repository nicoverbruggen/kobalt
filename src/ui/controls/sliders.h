#pragma once
#include <QSlider>
#include <QVBoxLayout>
#include <QVector>
#include <QStringList>
#include <functional>

namespace kobalt::ui {
class SettingSlider : public QSlider
{
public:
    SettingSlider(QWidget *parent, int minimum, int maximum, int step = 1);
    std::function<void(int)> committed;
    // Runs before a drag or a plus or minus press changes the value.
    std::function<void()> adjusting;

    bool interacting() const { return tracking; }
    enum class MarkerMode { Positions, Stops, Checkpoints };

    void setDotMarkers(bool enabled)
    {
        dotMarkers = enabled;
        update();
    }

    // Stops are evenly spaced and snap pointer gestures. Buttons retain singleStep().
    void setMarkers(const QVector<int> &values, MarkerMode mode = MarkerMode::Positions);

    // Extra Stops-mode targets use the existing rail positions without drawing marks or labels.
    void setAdditionalStops(const QVector<int> &values) { additionalStops = values; }

    // Checkpoints use equal spacing but allow values between the marks.
    void setMarkerLabels(const QStringList &labels);
    void nudge(int steps);

protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    bool tracking = false;
    bool dotMarkers = false;
    int original = 0;
    QVector<int> markers;
    QVector<int> additionalStops;
    QStringList markerLabels;
    MarkerMode markerMode = MarkerMode::Positions;
    int positionForValue(int value) const;
    void moveTo(qreal x);
    void cancel();
    void finish();
};

// A full-range rail with a draggable viewport. Presentation updates never emit
// moved; pointer input reports the first visible value while preserving the span.
class RangeOverview final : public QWidget
{
public:
    explicit RangeOverview(QWidget *parent);
    void setRange(int count, int first, int last, int current);
    QRect windowBounds() const;
    std::function<void(int)> moved;

protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    int count = 1, first = 0, last = 0, current = 0;
    int original = 0;
    qreal grab = 0;
    bool tracking = false;
    QRect trackBounds() const;
    qreal valueAt(qreal x) const;
    int xAt(int value) const;
    void begin(qreal x);
    void move(qreal x);
    void cancel();
};

SettingSlider *sliderRow(QWidget *parent, QVBoxLayout *rows, const char *name,
                         const char *labelName, const QString &label, const char *lessName,
                         const char *moreName, int minimum, int maximum, int step,
                         std::function<void(int)> apply, const QString &prefix = {},
                         const QString &suffix = {}, const QStringList &markerLabels = {});
} // namespace kobalt::ui
