#pragma once
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/font_preview.h"
#include <QFont>
#include <QLabel>
#include <QVector>

class QPainter;

namespace kobalt::headerfooter {
struct Content;
}

namespace kobalt::ui {
// Elides only the title in a combined chapter field.
QString headerFooterText(const headerfooter::Content &content, const QFont &font, int width);

// Interface labels use the same private font metrics and painting as the reader controls.
class InterfaceLabel : public QLabel
{
public:
    using QLabel::QLabel;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;
};

int wrappedInterfaceTextHeight(const QString &text, const QFont &font, int width,
                               int maximumLines = 0);
void drawWrappedInterfaceText(QPainter &painter, const QRect &bounds, const QString &text,
                              Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignTop,
                              int maximumLines = 0, int lineSpacing = 0,
                              QVector<QRect> *lineRectangles = nullptr);
// Uses the same font backend and wrapping as WholeLineLabel.
qreal excerptTextHeight(const QString &html, const QFont &font, int width);

// Search excerpts end between complete lines, even when their context wraps.
class WholeLineLabel : public QLabel
{
public:
    using QLabel::QLabel;

protected:
    void paintEvent(QPaintEvent *) override;
};

struct SampleAppearance
{
    int weight = 0;
    QString bionic = QStringLiteral("off");

    bool operator==(const SampleAppearance &other) const
    {
        return weight == other.weight && bionic == other.bionic;
    }

    bool operator!=(const SampleAppearance &other) const { return !(*this == other); }
};

// UTF-16 ranges of the first half of each supported word's letters.
QVector<QPair<int, int>> samplePrefixes(const QString &text);

// Font samples align their visible letters, independent of each font's ascent.
class SampleLabel : public PrivateTextLabel
{
public:
    using PrivateTextLabel::PrivateTextLabel;
    bool optical = false;

    void setPreviewSource(const FontPreviewSource &value)
    {
        if (previewSource.identity == value.identity && previewSource.family == value.family)
            return;
        previewSource = value;
        update();
    }

    void setAppearance(const SampleAppearance &value)
    {
        if (appearance == value)
            return;
        appearance = value;
        update();
    }

    SampleAppearance sampleAppearance() const { return appearance; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    SampleAppearance appearance;
    FontPreviewSource previewSource;
};

// UI labels inherit the interface weight. Font samples pass their own appearance.
void drawOpticalText(QPainter &painter, const QRectF &area, const QString &text,
                     bool centered = false);
void drawOpticalText(QPainter &painter, const QRectF &area, const QString &text, bool centered,
                     SampleAppearance appearance);
QFont fontSample(const QString &face);
// Page counts such as 5 / 33 keep half-width spaces around the slash. A
// Unicode thin space still takes a full cell in mono fonts, so the painter
// draws each word and advances half a space between them.
qreal halfSpacedWidth(const QFont &font, const QString &text);
void drawHalfSpaced(QPainter &painter, qreal x, const QRect &bounds, const QString &text,
                    int weight = -1);
void captionLabel(QLabel *label, int pixels = metrics::chrome::captionText);
} // namespace kobalt::ui
