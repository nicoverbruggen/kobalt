#pragma once
#include "../style/metrics.h"
#include "../style/private_text.h"
#include <QFont>
#include <QLabel>
#include <QVector>

class QPainter;

namespace kobalt::ui {
// Uses the same font backend and wrapping as WholeLineLabel.
qreal excerptTextHeight(const QString &html, const QFont &font, int width);
// Search excerpts end between complete lines, even when their context wraps.
class WholeLineLabel : public QLabel {
public:
    using QLabel::QLabel;

protected:
    void paintEvent(QPaintEvent *) override;
};

struct SampleAppearance {
    int weight = 0;
    QString bionic = QStringLiteral("off");
    bool operator==(const SampleAppearance &other) const {
        return weight == other.weight && bionic == other.bionic;
    }
    bool operator!=(const SampleAppearance &other) const {
        return !(*this == other);
    }
};
// UTF-16 ranges of the first half of each supported word's letters.
QVector<QPair<int, int>> samplePrefixes(const QString &text);
// Font samples align their visible letters, independent of each font's ascent.
class SampleLabel : public PrivateTextLabel {
public:
    using PrivateTextLabel::PrivateTextLabel;
    bool optical = false;
    void setAppearance(const SampleAppearance &value) {
        if (appearance == value)
            return;
        appearance = value;
        update();
    }
    SampleAppearance sampleAppearance() const {
        return appearance;
    }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    SampleAppearance appearance;
};
void drawOpticalText(QPainter &painter, const QRectF &area, const QString &text, bool centered = false,
                     SampleAppearance appearance = {});
QFont fontSample(const QString &face);
// Page counts such as 5 / 33 keep half-width spaces around the slash. A
// Unicode thin space still takes a full cell in mono fonts, so the painter
// draws each word and advances half a space between them.
qreal halfSpacedWidth(const QFontMetrics &metrics, const QString &text);
void drawHalfSpaced(QPainter &painter, qreal x, const QRect &bounds, const QString &text);
void captionLabel(QLabel *label, int pixels = metrics::chrome::captionText);
}
