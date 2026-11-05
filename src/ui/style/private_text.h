#pragma once

#include <QByteArray>
#include <QFont>
#include <QMap>
#include <QColor>
#include <QImage>
#include <QLabel>
#include <QString>
#include <QVector>

#include <functional>

class QPainter;

namespace kobalt {
class AssetBundle;
}

namespace kobalt::ui {
// Owns independent font bytes and faces on the GUI thread. It never shares
// CREngine's worker-owned font manager or changes Qt's font database.
// `checked` holds font bytes that the caller has already checked against the
// manifest. They are used instead of reading those files again.
bool initializePrivateInterfaceText(const AssetBundle &assets, bool opening = false,
                                    const QMap<QString, QByteArray> &checked = {});
// Font buffers only; glyphs and shaped text have separate bounded caches.
// A ceiling for safety, not memory set aside: one book face, the bundled
// CJK collection and the interface faces fit together without eviction.
inline constexpr qint64 privateInterfaceFontBudget = 100LL * 1024 * 1024;
// Count retained font buffers once, including collections with several faces.
qint64 privateInterfaceFontBytes();
// Shapes `text` once with a throwaway face on the calling thread. HarfBuzz
// sets up its shared tables on first use, so the first interface text drawn
// afterwards does not pay for it. Safe on any thread.
void warmTextShaping(const QByteArray &font, const QString &text);
// Registers metadata only. Each style's bytes load when first rendered and
// may be evicted between layouts. Qt's font loader never sees them.
// Returns whether the file could be registered.
bool registerPrivateInterfaceFont(const QString &path, const QString &expectedFamily);
// A native editor needs a Qt face. Only single-face TrueType files are safe
// to pass to Qt's font loader; private previews keep their existing renderer.
QByteArray nativeEditorFontData(const QFont &font);
// Bundled interface and fallback font files by name, for font previews.
QMap<QString, QString> privateInterfaceFontFiles();

// Sample sizes in device pixels, from the interface metrics.
struct FontPreviewScale
{
    int inkHeight, base, minimum, maximum, lineGap;
};

// Uses isolated file-backed faces and retains no font bytes. Returns an 8-bit coverage bitmap.
// Characters the font lacks use the bundled `fallbacks` from privateInterfaceFontFiles().
QImage renderFontPreviewBitmap(const QStringList &files, const QString &family, const QString &text,
                               QSize size, const FontPreviewScale &scale, int weight,
                               bool boldReading, bool centered, bool wrap,
                               const QMap<QString, QString> &fallbacks = {});

struct PrivateText
{
    QImage image;
    int advance = 0;
    int height = 0;
    int missingGlyphs = 0;
    int lineAscent = 0;
    int lineHeight = 0;
    QPoint origin;
};
enum class PrivateTextEmphasis { Synthetic, FontFace };

struct PrivateTextStyle
{
    int start = 0, length = 0;
    QFont font;
    QColor color;
    int baselineShift = 0;
    QImage image{};
    QColor background{};
};

// All application text uses private painting once font sources are initialized.
bool needsPrivateText(const QString &text, const QFont &font);
bool privateFontFixedPitch(const QFont &font);
PrivateText renderPrivateText(const QString &text, const QFont &font, int weight = -1,
                              const QVector<QPair<int, int>> &boldRanges = {},
                              const QVector<QPair<int, int>> &underlineRanges = {},
                              PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                              Qt::LayoutDirection direction = Qt::LayoutDirectionAuto,
                              const QVector<PrivateTextStyle> &styles = {});
int measurePrivateText(const QString &text, const QFont &font,
                       const QVector<QPair<int, int>> &boldRanges = {},
                       PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                       Qt::LayoutDirection direction = Qt::LayoutDirectionAuto);
PrivateText measurePrivateTextLayout(const QString &text, const QFont &font,
                                     const QVector<QPair<int, int>> &boldRanges = {},
                                     PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                                     Qt::LayoutDirection direction = Qt::LayoutDirectionAuto,
                                     const QVector<PrivateTextStyle> &styles = {});
QSize measurePrivateTextSize(const QString &text, const QFont &font);
// Returns the furthest grapheme end after start and at or before limit whose
// text fits width, or start when none fits. ends lists every grapheme end in
// order, and measure(end) returns the advance of the text from start to end.
// Advances grow with the text, so the search interpolates from the advances
// it measures. unitAdvance carries the advance per UTF-16 unit from one line
// to the next; start it at zero.
int fittingTextEnd(const QVector<int> &ends, int start, int limit, int width,
                   const std::function<int(int end)> &measure, qreal &unitAdvance);
QString elidePrivateText(const QString &text, const QFont &font, int width,
                         const QVector<QPair<int, int>> &boldRanges = {});
int interfaceTextWidth(const QString &text, const QFont &font);
int interfaceLineHeight(const QFont &font);
int interfaceLineAscent(const QFont &font);
QRect interfaceInkBounds(const QString &text, const QFont &font);
QString elideInterfaceText(const QString &text, const QFont &font, int width);
void drawInterfaceText(QPainter &painter, const QRect &area, const QString &text,
                       Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter, int weight = -1,
                       const QVector<QPair<int, int>> &boldRanges = {});
// Centers the ink of the letters vertically instead of the line box.
void drawOpticalPrivateText(QPainter &painter, const QRect &area, const QString &text,
                            Qt::Alignment alignment = Qt::AlignLeft, int weight = -1,
                            const QVector<QPair<int, int>> &boldRanges = {});
// The same for one label of a row, such as a segment: a descender does not
// lift it above the baseline that its neighbours share.
void drawSegmentPrivateText(QPainter &painter, const QRect &area, const QString &text,
                            Qt::Alignment alignment = Qt::AlignHCenter);

class PrivateTextLabel : public QLabel
{
public:
    using QLabel::QLabel;

    void setMaximumLines(int value)
    {
        maximumLines = value;
        update();
    }

    void setHighlight(const QString &style, QColor color, int spacing)
    {
        highlightStyle = style;
        highlightColor = color;
        lineSpacing = spacing;
        update();
    }

    int textWidth(const QString &value) const;
    QString elidedText(const QString &value, int width) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int maximumLines = 0;
    QString highlightStyle;
    QColor highlightColor;
    int lineSpacing = 0;
};
} // namespace kobalt::ui
