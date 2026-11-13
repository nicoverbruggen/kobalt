#pragma once
#include "publication/page_estimate.h"
#include "publication/resource_cache.h"
#include <QHash>
#include <atomic>
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

    // The size in bytes of each spine item, in spine order. Filled when
    // first needed. They decide the spine items of a unit and give the
    // page estimate its sizes.
    QVector<qint64> spineBytes;
    // The plain text of each spine item, folded for a quick check whether a
    // search can match there. Filled when first needed.
    QVector<QString> searchTexts;
    // The part of the text of its spine item before each position that was
    // looked up in the XHTML. It does not depend on the layout.
    QHash<QString, double> textFractions;
    QHash<QString, qint64> textWordPrefixes;
    // Whether a linked target exists, checked on demand without loading its
    // chapter. Unknown or interrupted checks are not retained.
    QHash<QString, bool> linkTargets;

    // The estimated pages of the book, for rough page numbers. They are for
    // the open book only and are not saved. A document prepared ahead
    // shares them with the document on the screen.
    struct PageEstimate
    {
        PageMap map;
        // The spine item that is being measured, and its XHTML.
        std::unique_ptr<PageEstimateMeasurer> measurer;
        LVStreamRef stream;
        int measuring = -1;
        PageAnchor book, item;
        // The page that was placed last: its spine item, the first and the
        // last character of its slice, its page in that part and the pages
        // of the part.
        int lastItem = -1, lastFirst = -1, lastLast = -1, lastPage = -1, lastPages = 0;
        // True when the last placed page came from the kept estimate.
        bool lastKept = false;
        // Set by the reader, read on the engine worker. See
        // Engine::setBackgroundCount().
        std::atomic_bool count{ false };
        // The estimateKey() of the settings that gave the observed pages.
        QString key;
    } pageEstimate;
};

// The spine item that a position names as DocFragment[n], counted from zero,
// or -1.
int spineItemIndex(const QString &position);
} // namespace kobalt
