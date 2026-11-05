#pragma once
#include <QList>
#include <QPair>
#include <QRect>
#include <QRegion>
class QBackingStore;
class QWindow;

namespace kobalt::platform {
// Firmware 4's Qt 5.2 flush carries one refresh policy per rectangle.
using FlushFlags = QList<QPair<QRect, quint64>>;
using Flush = void (*)(QBackingStore *, const QRegion &, QWindow *, const QPoint &, const FlushFlags &);
extern Flush nativeFlush;
// Older firmware uses a different QList element layout for 32-bit policies.
using FlushFlags32 = QList<QPair<QRect, quint32>>;
using Flush32 = void (*)(QBackingStore *, const QRegion &, QWindow *, const QPoint &, const FlushFlags32 &);
extern Flush32 nativeFlush32;
void flushReader(QBackingStore *store, const QRegion &region, QWindow *target, const QPoint &offset,
                 const FlushFlags &flags);
void flushReader32(QBackingStore *store, const QRegion &region, QWindow *target, const QPoint &offset,
                   const FlushFlags32 &flags);
}
