#pragma once
#include <QRect>
#include <QRegion>
#include <QList>
class QBackingStore;
class QWindow;

namespace kobalt::platform {
using LookupRefreshFlags = void (*)(void *, quint64 &, const QRect &);
extern LookupRefreshFlags nativeRefreshFlags;
void lookupReaderRefreshFlags(void *screen, quint64 &flags, const QRect &rect);
using FlushFlags = QList<QPair<QRect, quint64>>;
using Flush = void (*)(QBackingStore *, const QRegion &, QWindow *, const QPoint &, const FlushFlags &);
extern Flush nativeFlush;
void flushReader(QBackingStore *store, const QRegion &region, QWindow *target, const QPoint &offset,
                 const FlushFlags &flags);
}
