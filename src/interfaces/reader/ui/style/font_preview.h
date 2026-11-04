#pragma once
#include <QPainter>
#include <QStringList>
#include <QWidget>
#include <QVariant>

namespace kobalt::ui {
struct FontPreviewSource {
    QString family;
    QStringList files;
    QString identity;
};
FontPreviewSource fontPreviewSource(const QString &family, const QStringList &files);
inline constexpr int fontPreviewBitmapBudget = 4 * 1024 * 1024;
// GUI-thread only. A cache miss queues one bounded worker job and returns false.
// The caller draws its ordinary label until the bitmap is ready.
bool drawFontPreview(QWidget *owner, QPainter &painter, QRect area, const QString &text,
                     const FontPreviewSource &source, int inkHeight, int weight = 0,
                     bool boldReading = false, bool centered = false, bool wrap = false);
// Prepare one page offscreen, then release its paints together. Does not wait on the GUI thread.
void prepareFontPreviewPage(QWidget *page);
int fontPreviewCacheBytes();
int fontPreviewPendingCount();
}
