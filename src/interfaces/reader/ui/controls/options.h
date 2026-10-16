#pragma once
#include "../style/theme.h"
#include <QCheckBox>
#include <QVector>
#include <functional>

namespace kobalt::ui {
class DescribedOption : public QCheckBox {
public:
    DescribedOption(const QString &text, const QString &description, QWidget *parent);
    int heightForWidth(int width) const override;
    enum class Indicator { Toggle, Square, Radio };
    struct Presentation {
        Presentation();
        Indicator indicator = Indicator::Toggle;
        Edges dividers;
        int horizontalInset = 0, minimumRowHeight = 116;
        int labelPixels, descriptionPixels;
        QSize indicatorSize;
        // A held row turns the list's pressed gray and fills its radio.
        bool pressedFill = false;
        // The indicator sits before the label instead of after it.
        bool leadingIndicator = false;
        bool operator==(const Presentation &other) const;
    };
    void setPresentation(const Presentation &value);
    const Presentation &presentation() const {
        return appearance;
    }
    // A short value on the right of the label line, before the indicator.
    // It can end with a battery and keep half-width spaces, as the strips do.
    struct ValueStyle {
        bool battery = false;
        int percent = -1;
        bool charging = false;
        bool halfSpaces = false;
        bool operator==(const ValueStyle &other) const {
            return battery == other.battery && percent == other.percent && charging == other.charging &&
                   halfSpaces == other.halfSpaces;
        }
    };
    void setValue(const QString &value);
    void setValue(const QString &value, const ValueStyle &style);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    bool hitButton(const QPoint &position) const override;
    void paintEvent(QPaintEvent *) override;

private:
    Presentation appearance;
    QString description, value;
    ValueStyle valueStyle;
};

// A pick-one list: a row per choice with a radio on the left, its label and
// an optional description or value. By default rows share one height, the
// tallest any row needs and at least the minimum; otherwise each row is as
// tall as it needs. Dividers sit between rows. A held row shows the pressed
// gray. Owners that page the list show a range of rows.
class RadioList : public QWidget {
public:
    explicit RadioList(QWidget *parent);
    DescribedOption *add(const QString &name, const QString &label, const QString &description = {});
    const QVector<DescribedOption *> &options() const {
        return rows;
    }
    void setCurrent(int index);
    void setMinimumRowHeight(int pixels);
    void setTopDivider(bool shown);
    // False lets each row take the height it needs.
    void setUniformRows(bool uniform);
    int rowHeight(int width) const;
    int rowHeight(int index, int width) const;
    // Shows `count` rows from `first`, one under the other.
    void showRows(int first, int count);
    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    std::function<void(int)> chosen;

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    QVector<DescribedOption *> rows;
    int minimum = 0, first = 0, count = -1;
    bool topDivider = false, uniformRows = true;
    void arrange();
};
}
