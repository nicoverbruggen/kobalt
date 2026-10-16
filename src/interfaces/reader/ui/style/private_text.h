#pragma once

#include <QFont>
#include <QImage>
#include <QLabel>
#include <QString>
#include <QVector>

class QPainter;

namespace kobalt {
class AssetBundle;
}
namespace kobalt::ui {
// Owns independent font bytes and faces on the GUI thread. It never shares
// CREngine's worker-owned font manager or changes Qt's font database.
bool initializePrivateInterfaceText(const AssetBundle &assets);
// Count retained font buffers once, including collections with several faces.
qint64 privateInterfaceFontBytes();
enum class PrivateFontRegistration { Failed, Native, Private };
// Retain reading-font bytes before user storage is released. CFF and font
// collections stay on the private path, including their font-picker samples.
// forcePrivate also keeps TrueType previews out of Qt's font loader. Once a
// retained file uses private painting, later registrations keep that choice.
PrivateFontRegistration registerPrivateInterfaceFont(const QString &path, const QString &expectedFamily,
                                                     bool forcePrivate = false);
struct PrivateText {
    QImage image;
    int advance = 0;
    int height = 0;
    int missingGlyphs = 0;
    int lineAscent = 0;
    int lineHeight = 0;
    QPoint origin;
};
enum class PrivateTextEmphasis { Synthetic, FontFace };
// Keep native painting when the bundled primary face covers the whole label.
bool needsPrivateText(const QString &text, const QFont &font);
PrivateText renderPrivateText(const QString &text, const QFont &font, int weight = -1,
                              const QVector<QPair<int, int>> &boldRanges = {},
                              const QVector<QPair<int, int>> &underlineRanges = {},
                              PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                              Qt::LayoutDirection direction = Qt::LayoutDirectionAuto);
int measurePrivateText(const QString &text, const QFont &font, const QVector<QPair<int, int>> &boldRanges = {},
                       PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                       Qt::LayoutDirection direction = Qt::LayoutDirectionAuto);
PrivateText measurePrivateTextLayout(const QString &text, const QFont &font,
                                     const QVector<QPair<int, int>> &boldRanges = {},
                                     PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                                     Qt::LayoutDirection direction = Qt::LayoutDirectionAuto);
QSize measurePrivateTextSize(const QString &text, const QFont &font);
QString elidePrivateText(const QString &text, const QFont &font, int width,
                         const QVector<QPair<int, int>> &boldRanges = {});
int interfaceTextWidth(const QString &text, const QFont &font);
QString elideInterfaceText(const QString &text, const QFont &font, int width);
void drawInterfaceText(QPainter &painter, const QRect &area, const QString &text,
                       Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter, int weight = -1,
                       const QVector<QPair<int, int>> &boldRanges = {});
// Centers the ink of the letters vertically instead of the line box.
void drawOpticalPrivateText(QPainter &painter, const QRect &area, const QString &text,
                            Qt::Alignment alignment = Qt::AlignLeft, int weight = -1,
                            const QVector<QPair<int, int>> &boldRanges = {});
class PrivateTextLabel : public QLabel {
public:
    using QLabel::QLabel;
    int textWidth(const QString &value) const;
    QString elidedText(const QString &value, int width) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
};
}
