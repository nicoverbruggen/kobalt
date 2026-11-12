#pragma once
#include <atomic>
#include "publication/resource_cache.h"
#include <lvstream.h>

namespace kobalt {
// Private backing storage shared only by engine documents and their previews.
struct ReaderSource
{
    // A preview retains the publication storage even if its source document closes.
    // Containers must die before the buffers backing their streams.
    std::shared_ptr<ReaderSource> parent;
    // The full publication count also governs a preview of a font-free chapter.
    int embeddedFonts = -1;
    QByteArray bytes;
    std::shared_ptr<ResourceCache> resources;
    QMap<QString, qint64> sizes;
    LVContainerRef container;
    // True when `container` already cleans the content that it reads.
    bool contentContainer = false;
    // A number for this source and no other, also after it is destroyed.
    const quint64 identity = nextIdentity();

    static quint64 nextIdentity()
    {
        static std::atomic<quint64> next{ 1 };
        return next.fetch_add(1);
    }
    // The size in bytes of each spine item, in spine order, for rough page
    // numbers. Filled when first needed.
    QVector<qint64> spineBytes;
};
} // namespace kobalt
