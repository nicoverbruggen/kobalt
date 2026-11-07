#pragma once
#include "ui/style/theme.h"
#include "ui/style/font_preview.h"
#include <QCheckBox>
#include <QFont>
#include <QVector>
#include <functional>

namespace kobalt::ui {
enum class ControlIcon;

class DescribedOption : public QCheckBox
{
public:
    DescribedOption(const QString &text, const QString &description, QWidget *parent);
    int heightForWidth(int width) const override;
    void setDescription(const QString &text);

    void setFontPreviewSource(const FontPreviewSource &value)
    {
        fontPreview = value;
        update();
    }
    enum class Indicator { Toggle, Square, Radio };

    struct Presentation
    {
        Presentation();
        Indicator indicator = Indicator::Toggle;
        Edges dividers;
        int horizontalInset = 0, minimumRowHeight = 116;
        int labelPixels, descriptionPixels, descriptionPadding;
        QSize indicatorSize;
        // A held row turns the list's pressed gray and fills its radio.
        bool pressedFill = true;
        // The indicator sits before the label instead of after it.
        bool leadingIndicator = false;
        // Align a font-name preview by its visible ink, like the font panel.
        bool opticalLabel = false;
        // Narrow field lists keep the complete label above its sample value.
        bool valueBelowLabel = false;
        bool operator==(const Presentation &other) const;
    };

    void setPresentation(const Presentation &value);

    const Presentation &presentation() const { return appearance; }

    // A short value on the right of the label line, before the indicator.
    // It can end with a battery and keep half-width spaces, as the strips do.
    struct ValueStyle
    {
        bool battery = false;
        int percent = -1;
        bool charging = false;
        bool halfSpaces = false;
        QFont sampleFont;
        int sampleWeight = -1;

        bool operator==(const ValueStyle &other) const
        {
            return battery == other.battery && percent == other.percent
                    && charging == other.charging && halfSpaces == other.halfSpaces
                    && sampleFont == other.sampleFont && sampleWeight == other.sampleWeight;
        }
    };

    void setValue(const QString &value);
    void setValue(const QString &value, const ValueStyle &style);
    // A non-interactive thumbnail shares the row's click target.
    void setPreview(QWidget *widget, QSize size);
    // An icon before the label, placed like the icons of list action rows.
    void setLeadingIcon(ControlIcon icon);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    bool hitButton(const QPoint &position) const override;
    void paintEvent(QPaintEvent *) override;

private:
    Presentation appearance;
    FontPreviewSource fontPreview;
    QString description, value;
    ValueStyle valueStyle;
    QWidget *preview = nullptr;
    QSize previewSize;
    int leadingIcon = -1;
    int iconSpace() const;
    // Large text makes the title line taller by what it adds to the line
    // height; the description moves down with it.
    int labelHeight() const;
    int valueHeight() const;
    // How far a label that stands alone beside its indicator is raised, so
    // its capitals center on the indicator.
    int labelRaise(const QFont &labelFont) const;
    int descriptionTop() const;
};

// A pick-one list: a row per choice with a radio on the left, its label and
// an optional description or value. By default rows share one height, the
// tallest any row needs and at least the minimum; otherwise each row is as
// tall as it needs. Dividers sit between rows. A held row shows the pressed
// gray. Owners that page the list show a range of rows.
class RadioList : public QWidget
{
public:
    explicit RadioList(QWidget *parent);
    DescribedOption *add(const QString &name, const QString &label,
                         const QString &description = {});

    const QVector<DescribedOption *> &options() const { return rows; }

    void setCurrent(int index);
    void setMinimumRowHeight(int pixels);
    void setTopDivider(bool shown);
    // False lets each row take the height it needs.
    void setUniformRows(bool uniform);
    int rowHeight(int width) const;
    int rowHeight(int index, int width) const;
    // Shows `count` rows from `first`, one under the other.
    void showRows(int first, int count);

    bool hasHeightForWidth() const override { return true; }

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
} // namespace kobalt::ui
