#pragma once
#include "ui/style/fonts.h"
#include "buttons.h"
#include "render/highlight_paint.h"
#include "ui/style/theme.h"
#include "ui/style/styles.h"
#include "support/strings.h"
#include <QAbstractButton>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>
#include <QRegion>
#include <QVariant>
#include <functional>

namespace kobalt::ui {
class HighlightSwatch : public QAbstractButton
{
public:
    HighlightSwatch(QString key, QString label, QWidget *parent)
        : QAbstractButton(parent), key(std::move(key))
    {
        setObjectName("rrAnnotationColor_" + this->key);
        setText(label);
        setAccessibleName(label);
        setCheckable(true);
        setFixedHeight(px(88));
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setFont(interfaceFont(px(23)));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(),
                   isEnabled() && isDown() ? themeColors(this).pressedPaper
                                           : palette().color(QPalette::Window));
        p.setRenderHint(QPainter::Antialiasing);
        const auto ink = palette().color(QPalette::WindowText);
        const int pen = px(2), ring = px(5);
        const int inset = ring + (pen + 1) / 2 + px(2);
        const int diameter = qMax(px(24), qMin(px(48), width() - 2 * inset));
        auto labelFont = font();
        labelFont.setPixelSize(qMin(px(23), qMax(px(16), width() / 3)));
        const auto labelLine = interfaceInkBounds(QStringLiteral("Ag"), labelFont);
        const int ringExtent = ring + (pen + 1) / 2;
        const int gap = px(5);
        // Center the visible circle, selection ring and label as one group.
        const int contentHeight = 2 * ringExtent + diameter + gap + labelLine.height();
        const int top = (height() - contentHeight) / 2;
        const QRect chip((width() - diameter) / 2, top + ringExtent, diameter, diameter);
        p.setPen(QPen(ink, pen));
        p.setBrush(render::highlightTint(key));
        p.drawEllipse(chip);
        if (isChecked()) {
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(chip.adjusted(-ring, -ring, ring, ring));
            p.setPen(QPen(Qt::black, px(3)));
            p.drawLine(chip.center() + QPoint(-px(12), 0), chip.center() + QPoint(-px(3), px(10)));
            p.drawLine(chip.center() + QPoint(-px(3), px(10)),
                       chip.center() + QPoint(px(14), -px(13)));
        }
        p.setPen(ink);
        p.setFont(labelFont);
        const int labelTop = chip.y() + diameter + ringExtent + gap;
        drawInterfaceText(p, QRect(0, labelTop, width(), labelLine.height()), text(),
                          Qt::AlignHCenter | Qt::AlignTop);
    }

private:
    QString key;
};

// Presentation only: the owner supplies selection and handles persistence.
class HighlightPalette : public QWidget
{
public:
    HighlightPalette(const QString &selected, std::function<void(QString)> changed, QWidget *parent)
        : QWidget(parent), selectedColor(selected)
    {
        setObjectName("rrAnnotationPalette");
        parent->installEventFilter(this);
        setFixedHeight(compactHeight());
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(px(28), px(12), px(28), px(12));
        layout->setSpacing(px(4));
        auto *row = new QHBoxLayout;
        row->setSpacing(px(12));
        layout->addLayout(row);
        for (const auto &key : { "yellow", "green", "blue", "pink", "gray" }) {
            auto *swatch = new HighlightSwatch(
                    key,
                    (QString(key) == "yellow"          ? strings::get("annotations.color_yellow")
                             : QString(key) == "green" ? strings::get("annotations.color_green")
                             : QString(key) == "blue"  ? strings::get("annotations.color_blue")
                             : QString(key) == "pink"  ? strings::get("annotations.color_pink")
                                                       : strings::get("annotations.color_gray")),
                    this);
            swatch->setChecked(selected == key);
            row->addWidget(swatch);
            connect(swatch, &QAbstractButton::clicked, this, [this, key, changed] {
                setSelectedColor(QString::fromLatin1(key));
                changed(QString::fromLatin1(key));
            });
        }
        error = new kobalt::ui::InterfaceLabel(this);
        error->setObjectName("rrAnnotationPaletteError");
        error->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        error->setWordWrap(true);
        error->setFont(chromeFont(px(23)));
        readerStyle(error,
                    QStringLiteral("font-family:'@ui-caption-family@';") + styles::font(px(23)));
        layout->addWidget(error);
        error->hide();
    }

    static int compactHeight() { return px(112); }

    void setError(const QString &message)
    {
        error->setText(message);
        error->setVisible(!message.isEmpty());
        error->ensurePolished();
        measureError();
    }

    ~HighlightPalette() override { retireRepairBounds(); }

    void setSelectedColor(const QString &selected)
    {
        selectedColor = selected;
        for (auto *button : findChildren<QAbstractButton *>()) {
            button->setChecked(button->objectName() == "rrAnnotationColor_" + selected);
        }
    }

private:
    QString selectedColor;
    QLabel *error = nullptr;

    void measureError()
    {
        int extra = 0;
        if (error && !error->isHidden()) {
            const int room = qMax(1, width() - px(56));
            extra = qMax(error->sizeHint().height(), error->heightForWidth(room)) + px(4);
        }
        const int wanted = compactHeight() + extra;
        if (height() != wanted) {
            setFixedHeight(wanted);
        }
    }

    void retireRepairBounds()
    {
        window()->setProperty(
                "reader_palette_repairs",
                QVariant::fromValue(
                        window()->property("reader_palette_repairs").value<QRegion>()
                        | window()->property("reader_palette_regions").value<QRegion>()));
        window()->setProperty("reader_palette_regions", QVariant::fromValue(QRegion()));
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        QPainter painter(this);
        paintEdges(painter, rect(), Edges(Qt::BottomEdge), dividerColor(this));
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (isVisible() && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
            syncRepairBounds();
        }
        return QWidget::eventFilter(object, event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        measureError();
        if (isVisible()) {
            syncRepairBounds();
        }
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        syncRepairBounds();
    }

    void hideEvent(QHideEvent *event) override
    {
        retireRepairBounds();
        QWidget::hideEvent(event);
    }

public:
    void syncRepairBounds()
    {
        const QRegion region(QRect(mapTo(window(), QPoint()), size()));
        window()->setProperty(
                "reader_palette_repairs",
                QVariant::fromValue(window()->property("reader_palette_repairs").value<QRegion>()
                                    | window()->property("reader_palette_regions").value<QRegion>()
                                    | region));
        window()->setProperty("reader_palette_regions", QVariant::fromValue(region));
    }
};

class HighlightStyleSample : public Button
{
public:
    HighlightStyleSample(const QString &style, const QString &label, bool last, QWidget *parent)
        : Button(parent), style(style), label(label), last(last)
    {
        setObjectName("rrAnnotationStyle_" + style);
        setAccessibleName(label);
        setCheckable(true);
        setPaintMode(ButtonPaintMode::ToolbarCell);
        setFixedHeight(px(104));
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setFont(interfaceFont(px(23)));
        readerStyle(this, styles::font(px(23)));
    }

    void setColor(const QString &key)
    {
        color = render::highlightTint(key);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), isDown() ? sectionColor(this) : palette().color(QPalette::Window));
        const auto ink = palette().color(QPalette::WindowText);
        const QRect sample((width() - px(90)) / 2, px(8), px(90), px(44));
        p.setFont(interfaceFont(px(32)));
        p.setPen(ink);
        drawInterfaceText(p, sample, QStringLiteral("Abc"), Qt::AlignCenter);
        render::paintHighlightMark(p, { sample }, QRegion(sample), color, style, darkTheme(this));
        p.setFont(font());
        drawInterfaceText(p, QRect(px(6), px(62), width() - px(12), px(30)), label,
                          Qt::AlignCenter);
        if (isChecked()) {
            const QRect check(width() - px(40), px(10), px(24), px(24));
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(ink, px(3), Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            QPolygonF mark;
            mark << check.topLeft() + QPointF(check.width() * .18, check.height() * .5)
                 << check.topLeft() + QPointF(check.width() * .42, check.height() * .75)
                 << check.topLeft() + QPointF(check.width() * .82, check.height() * .22);
            p.drawPolyline(mark);
        }
        paintEdges(p, rect(), last ? Edges{} : Edges(Qt::RightEdge), dividerColor(this));
    }

private:
    QString style, label;
    QColor color = render::highlightTint("gray");
    bool last;
};

class HighlightStylePicker : public QWidget
{
public:
    HighlightStylePicker(const QString &selected, std::function<void(QString)> changed,
                         QWidget *parent)
        : QWidget(parent)
    {
        setObjectName("rrAnnotationStyles");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *row = new QHBoxLayout;
        layout->addLayout(row);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        for (const auto &style : { "shaded", "underline", "dotted" }) {
            const auto label = QString(style) == "shaded" ? strings::get("annotations.style_shaded")
                    : QString(style) == "underline" ? strings::get("annotations.style_underline")
                                                    : strings::get("annotations.style_dotted");
            auto *button = new HighlightStyleSample(style, label, QString(style) == "dotted", this);
            button->setChecked(selected == style);
            row->addWidget(button);
            connect(button, &QAbstractButton::clicked, this, [this, changed, style] {
                setSelectedStyle(QString::fromLatin1(style));
                changed(QString::fromLatin1(style));
            });
        }
        error = new kobalt::ui::InterfaceLabel(this);
        error->setObjectName("rrAnnotationStyleError");
        error->setFont(interfaceFont(px(23)));
        error->setWordWrap(true);
        error->setContentsMargins(px(20), px(4), px(20), px(12));
        layout->addWidget(error);
        error->hide();
        setFixedHeight(px(104));
    }

    void setSelectedStyle(const QString &selected)
    {
        for (auto *button : findChildren<QAbstractButton *>())
            button->setChecked(button->objectName() == "rrAnnotationStyle_" + selected);
    }

    void setColor(const QString &color)
    {
        for (auto *button : findChildren<QAbstractButton *>())
            if (auto *sample = dynamic_cast<HighlightStyleSample *>(button))
                sample->setColor(color);
    }

    void setError(const QString &message, const QString &selected)
    {
        setSelectedStyle(selected);
        error->setText(message);
        error->show();
        setFixedHeight(px(104)
                       + qMax(error->sizeHint().height(), error->heightForWidth(qMax(1, width()))));
    }

private:
    QLabel *error = nullptr;
};

} // namespace kobalt::ui
