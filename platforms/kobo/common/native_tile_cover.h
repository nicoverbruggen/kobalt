#pragma once
#include <QImage>
#include <QString>

class QWidget;

namespace kobalt {
// Reuse native tile covers when all required capture and invalidation hooks exist.
void initializeNativeTileCoverProbe();
bool nativeTileCoverProbeEnabled();
QImage nativeTileCoverProbeImage(const void *volume);
extern void (*nativeTileSetImage)(QWidget *, const QImage &, const QString &);
extern void (*nativeTileSetContent)(QWidget *, const void *, const QString &);
extern void (*nativeTileImageLoaded)(QWidget *, const QString &);
extern void (*nativeTileDefaultCover)(QWidget *);
} // namespace kobalt
