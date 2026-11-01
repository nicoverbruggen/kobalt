#pragma once
#include "buttons.h"
#include "../style/private_text.h"
#include <QButtonGroup>
#include <QFrame>
#include <QPointer>

namespace kobalt::ui {
// Plain-text lists share line spacing, dividers and accessory geometry.
// Labels remain widgets so callers can keep their accessibility and test names.
class ListRow : public Button {
public:
    struct None {};
    struct Caption {
        Caption(const QString &text = {}, int reservedWidth = 0) : text(text), reservedWidth(reservedWidth) {}
        QString text;
        int reservedWidth;
        bool operator==(const Caption &other) const {
            return text == other.text && reservedWidth == other.reservedWidth;
        }
    };
    struct Chevron {};
    struct Check {};
    class Accessory {
    public:
        enum class Kind { None, Caption, Chevron, Check };
        Accessory(None = {}) : type(Kind::None) {}
        Accessory(const Caption &value) : type(Kind::Caption), captionValue(value) {
            if (captionValue.reservedWidth < 0)
                captionValue.reservedWidth = 0;
        }
        Accessory(Chevron) : type(Kind::Chevron) {}
        Accessory(Check) : type(Kind::Check) {}
        Kind kind() const {
            return type;
        }
        const Caption *caption() const {
            return type == Kind::Caption ? &captionValue : nullptr;
        }
        bool operator==(const Accessory &other) const {
            return type == other.type && (type != Kind::Caption || captionValue == other.captionValue);
        }

    private:
        Kind type;
        Caption captionValue;
    };
    explicit ListRow(QWidget *parent);
    void setTitle(const QString &text);
    // A hidden icon keeps the same text alignment unless alignWithoutIcon is false.
    void setTitleIcon(ControlIcon icon, bool visible = true, bool alignWithoutIcon = true);
    void setDetail(const QString &text);
    void setDetailLines(int lines);
    void setDetailHighlight(const QString &style, QColor color, int lineSpacing);
    void setCaption(const QString &text);
    void setLeading(const QString &text, int indent = 0);
    void setLeadingIcon(ControlIcon icon);
    void setAccessory(Accessory value);
    const Accessory &accessoryValue() const {
        return accessory;
    }
    void setTitlePixels(int pixels);
    void setTitleGap(int pixels);
    void setDetailFont(const QFont &font);
    void setMetadataFont(const QFont &font);
    QPushButton *addAction(const QString &name, const QString &accessibleName);
    void setActionRule(bool value);
    void setActionIcon(ControlIcon icon);
    QSize sizeHint() const override;
    QLabel *titleLabel() const {
        return title;
    }
    QLabel *detailLabel() const {
        return detail;
    }
    QLabel *captionLabel() const {
        return caption;
    }
    QLabel *leadingLabel() const {
        return leading;
    }
    QLabel *trailingLabel() const {
        return trailing;
    }

protected:
    void resizeEvent(QResizeEvent *) override;
    void moveEvent(QMoveEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
    void checkStateSet() override;
    void nextCheckState() override;

private:
    void arrange();
    void syncLabelStyles();
    void setLabelFont(PrivateTextLabel *label, int index, const QFont &font);
    void observeParent();
    PrivateTextLabel *title = nullptr, *detail = nullptr, *caption = nullptr, *leading = nullptr, *trailing = nullptr;
    QPushButton *action = nullptr;
    QString titleText, detailText, captionText, leadingText;
    QString labelStyles[5];
    Accessory accessory = None{};
    ControlIcon leadingIcon{};
    bool hasLeadingIcon = false;
    QPointer<QWidget> observedParent;
    int detailLines = 1;
    int titleGap = 0;
    int indentation = 0;
    bool arranging = false, synchronizing = false;
};
class ListHeader : public QWidget {
public:
    explicit ListHeader(QWidget *parent);
    void setCaptions(const QString &left, const QString &right = {});
    void setTrailingInset(int pixels);
    void setShaded(bool shaded);
    QLabel *leftLabel() const {
        return left;
    }
    QLabel *rightLabel() const {
        return right;
    }
    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *) override;
    void paintEvent(QPaintEvent *) override;

private:
    QLabel *left, *right;
    int trailingInset = 0;
    bool shaded = false;
};
// Rows own spacing and caption geometry; panels supply actions and values.
QHBoxLayout *horizontalRow();
QHBoxLayout *labeledRow(QWidget *parent, QVBoxLayout *rows, const QString &label);
class SegmentedRow {
public:
    SegmentedRow(QWidget *parent, QVBoxLayout *rows, const char *name, const QString &label,
                 const QString &detail = {});
    QPushButton *add(const QString &name, const QString &text = {});

private:
    QFrame *frame;
    QHBoxLayout *layout;
    QButtonGroup *group;
};
}
