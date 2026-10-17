#pragma once
#include "icons.h"
#include "ui/tap_zones.h"
#include "text.h"
#include "../style/theme.h"
#include "../style/metrics.h"
#include <QBoxLayout>
#include <QEvent>
#include <QPushButton>
#include <functional>

namespace kobalt::ui {
inline bool isHoverEvent(const QEvent *event) {
    return event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave ||
           event->type() == QEvent::HoverMove;
}
class Button : public QPushButton {
public:
    using QPushButton::QPushButton;
    void setPaintMode(ButtonPaintMode mode);
    ButtonPaintMode paintMode() const {
        return mode;
    }
    void setIconConfiguration(ButtonIcon configuration);
    const ButtonIcon *iconConfiguration() const {
        return hasConfiguredIcon ? &configuredIcon : nullptr;
    }
    void refreshIcon();
    void setIconLabelGap(int pixels) {
        if (iconLabelGap == pixels) {
            return;
        }
        iconLabelGap = pixels;
        update();
    }
    // With a gap set, draw the icon centred above the label, as the bottom
    // tabs do, instead of beside it.
    void setIconAboveLabel(bool above) {
        if (iconAboveLabel == above) {
            return;
        }
        iconAboveLabel = above;
        update();
    }

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void initStyleOption(QStyleOptionButton *option) const override;
#else
    void initStyleOption(QStyleOptionButton *option) const;
#endif
    bool event(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    ButtonPaintMode mode = ButtonPaintMode::Native;
    ButtonIcon configuredIcon;
    int iconLabelGap = 0;
    bool iconAboveLabel = false;
    bool hasConfiguredIcon = false, hasIconTheme = false, iconDark = false;
};

// A selectable diagram of the actual page hit regions, with a label below it.
class TapZoneButton : public Button {
public:
    TapZoneButton(tapZones::Layout layout, bool rightToLeft, QWidget *parent);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    tapZones::Layout layout;
    bool rightToLeft;
};

class BackButton : public Button {
public:
    using Button::Button;
    void setLabelOffset(int pixels) {
        labelOffset = pixels;
        update();
    }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int labelOffset = 0;
};

class ElidingButton : public Button {
public:
    using Button::Button;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
};
class StepButton : public Button {
public:
    StepButton(ControlIcon icon, QWidget *parent);

protected:
    bool hitButton(const QPoint &point) const override;
    void paintEvent(QPaintEvent *) override;

private:
    QIcon normal, pressed;
};
class DropdownButton : public Button {
public:
    explicit DropdownButton(QWidget *parent);
    // A forward button opens another page instead of a list, so it shows a
    // right chevron in place of the arrow.
    void setForward(bool value) {
        if (forward == value)
            return;
        forward = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override;
    virtual QFont labelFont() const {
        return font();
    }
    virtual bool opticalLabel() const {
        return false;
    }
    virtual SampleAppearance sampleAppearance() const {
        return {};
    }

private:
    bool forward = false;
};
class FontButton : public DropdownButton {
public:
    explicit FontButton(QWidget *parent);
    void setFace(const QString &face);
    QString resolvedFace() const;
    void setAppearance(const SampleAppearance &value) {
        if (appearance == value)
            return;
        appearance = value;
        update();
    }

protected:
    QFont labelFont() const override {
        return sample;
    }
    bool opticalLabel() const override {
        return true;
    }
    SampleAppearance sampleAppearance() const override {
        return appearance;
    }

private:
    QFont sample;
    SampleAppearance appearance;
};
// Caption rows and list actions retain their different text and icon placement.
class CaptionButton : public Button {
public:
    enum class Presentation { Caption, ListAction };
    CaptionButton(ControlIcon icon, QWidget *parent);
    CaptionButton(Presentation presentation, QWidget *parent);
    Presentation presentation() const {
        return mode;
    }
    void setCaption(const QString &caption) {
        if (note == caption)
            return;
        note = caption;
        update();
    }
    QString caption() const {
        return note;
    }
    void setRule(Edges edges) {
        if (rule == edges)
            return;
        rule = edges;
        update();
    }
    void setChevron(bool shown) {
        if (chevron == shown)
            return;
        chevron = shown;
        update();
    }
    // List actions have an optional icon; caption rows retain their required glyph.
    void setIconVisible(bool shown) {
        if (mode != Presentation::ListAction || iconVisible == shown)
            return;
        iconVisible = shown;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Presentation mode;
    ControlIcon glyph = ControlIcon::Link;
    QString note;
    Edges rule;
    bool chevron = false, iconVisible = true;
};
CaptionButton *makeCaptionButton(QWidget *parent, QBoxLayout *layout, const char *name, const QString &text,
                                 std::function<void()> action);
// A header or pager cell: an icon, or an uppercase label with a chevron. It has
// a rule on its inner side and fills with ink while pressed, like a tab.
class CellButton : public Button {
public:
    CellButton(ControlIcon icon, QWidget *parent, int iconPixels = metrics::chrome::cellIcon);
    void setRule(Edges edges) {
        rule = edges;
        update();
    }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    ControlIcon glyph;
    int iconPixels;
    Edges rule;
};
QPushButton *makeButton(QWidget *parent, QBoxLayout *layout, const char *name, const QString &text,
                        std::function<void()> action);

}
