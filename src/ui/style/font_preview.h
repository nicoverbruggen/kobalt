#pragma once
#include <QPainter>
#include <QStringList>

namespace kobalt::ui {
struct FontPreviewSource
{
    QString family;
    QStringList files;
    QString identity;
};

FontPreviewSource fontPreviewSource(const QString &family, const QStringList &files);
inline constexpr int fontPreviewBitmapBudget = 4 * 1024 * 1024;
// GUI thread only. Draws the sample from the bounded bitmap cache, rendering
// it first on a miss. Returns false when the font cannot draw the text; the
// caller then draws its ordinary label. `weight` is the reader's thickness
// steps. `axisWeight` selects a variable font's weight; 0 keeps its default.
bool drawFontPreview(QPainter &painter, QRect area, const QString &text,
                     const FontPreviewSource &source, int inkHeight, int weight = 0,
                     bool boldReading = false, bool centered = false, bool wrap = false,
                     int axisWeight = 0);
int fontPreviewCacheBytes();
} // namespace kobalt::ui
