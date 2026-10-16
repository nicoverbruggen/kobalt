#pragma once
#include "../style/theme.h"
#include <QFrame>
#include <QLabel>

namespace kobalt::ui {
void paintBrandIcon(QPainter &painter, const QRectF &bounds, const QColor &ink);
class BrandIcon final : public QWidget {
public:
    explicit BrandIcon(QWidget *parent) : QWidget(parent) {}

protected:
    void paintEvent(QPaintEvent *) override;
};
class Panel : public QFrame {
public:
    explicit Panel(QWidget *parent = nullptr, Edges separators = {});
    void setEdges(Edges edges) {
        separators = edges;
        update();
    }
    void setEdgeWidth(int value) {
        edgeWidth = value;
        update();
    }
    void setSoftEdges(bool value) {
        softEdges = value;
        update();
    }
    // Density-scaled rules in the divider color, drawn after the separators.
    void setDividers(Edges edges) {
        dividers = edges;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Edges separators, dividers;
    bool softEdges = false;
    int edgeWidth = 0; // Zero follows the current density; positive values are explicit widths.
};

class LightSwipeNotice final : public Panel {
public:
    explicit LightSwipeNotice(QWidget *parent);
    void setLevel(bool warmth, int percentage);
    void centerIn(const QSize &viewport);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QLabel *percentage, *caption;
    int level = 0;
    bool warm = false;
};

// Draws a battery whose body fills `body`; the terminal sits outside it. A
// negative percent draws an empty battery. Insets scale with the body height.
void paintBattery(QPainter &painter, const QRectF &body, int percent, bool charging, const QColor &ink,
                  const QColor &paper);
// The width of the battery drawn for a body of this height, terminal included.
qreal batteryWidth(qreal bodyHeight);
class BatteryIndicator : public QLabel {
public:
    explicit BatteryIndicator(QWidget *parent);
    void setStatus(int percent, bool charging);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int percent = -2;
    bool charging = false;
};
class BluetoothIndicator final : public QWidget {
public:
    explicit BluetoothIndicator(QWidget *parent);
    bool active() const {
        return enabled;
    }
    void setActive(bool value) {
        enabled = value;
    }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    bool enabled = false;
};
}
