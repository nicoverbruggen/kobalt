#pragma once
#include "publication/page_estimate.h"
#include "publication/resource_cache.h"
#include <QCache>
#include <QHash>
#include <QJsonObject>
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
    // -1 before checking the spine for in-page note bodies.
    int hasLinkedPageNotes = -1;
    QByteArray bytes;
    std::shared_ptr<ResourceCache> resources;
    QMap<QString, qint64> sizes;
    LVContainerRef container;
    // True when `container` already cleans the content that it reads.
    bool contentContainer = false;
    // A number for this source and no other, also after it is destroyed.
    const quint64 identity = nextIdentity();
    // The open book. The documents that replace one another while it is
    // open share it, such as the whole book that Hybrid builds. Embedded
    // fonts and the Nickel position cache are kept per open book.
    quint64 fontIdentity = identity;

    static quint64 nextIdentity()
    {
        static std::atomic<quint64> next{ 1 };
        return next.fetch_add(1);
    }

    // The size in bytes of each spine item, in spine order. Filled when
    // first needed. They decide the spine items of a unit and give the
    // page estimate its sizes.
    QVector<qint64> spineBytes;
    // Positive and negative image-only chapter lookups survive chapter-window changes.
    QHash<QString, QString> chapterImages;
    int chapterImageProbes = 0;

    struct TocAnchor
    {
        QString path, id, position;
    };

    QVector<TocAnchor> tocAnchors;
    QHash<QString, int> boundaryElementOrder;
    bool mainTextChecked = false;
    QPair<QString, QString> mainTextHints;
    // EPUB spine flags, including fragments whose text the chapter window omitted.
    QVector<bool> nonLinearItems;
    // Folded chapter text used to skip items during search. Recent keys share
    // one byte budget across the documents of an open book.
    std::shared_ptr<QCache<int, QString>> searchTexts;
    // The part of the text of its spine item before each position that was
    // looked up in the XHTML. It does not depend on the layout.
    QHash<QString, double> textFractions;

    // One entry per saved endpoint, shared by chapter windows for the same appearance.
    struct SelectionEndpoints
    {
        QString appearance;
        QCache<QString, bool> valid{ 4096 };
    };

    std::shared_ptr<SelectionEndpoints> selectionEndpoints;
    QHash<QString, qint64> textWordPrefixes;
    // Whether a linked target exists, checked on demand without loading its
    // chapter. Unknown or interrupted checks are not retained.
    enum class LinkTarget { Unknown, Missing, Passage, Footnote };
    QHash<QString, LinkTarget> linkTargets;

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
        // How many chapter positions have their place in the text measured,
        // for the book pages of the chapter lists.
        int chapterPlaces = 0;
        PageAnchor book, item;
        // The page that was placed last: its spine item, the first and the
        // last character of its slice, its page in that part and the pages
        // of the part.
        int lastItem = -1, lastFirst = -1, lastLast = -1, lastPage = -1, lastPages = 0;
        // True when the last placed page came from a layout of the whole
        // book. Its page then counts from the start of the spine item.
        bool lastWhole = false;
        // True when the last placed page came from the kept estimate.
        bool lastKept = false;
        // The estimateKey() of the settings that gave the observed pages.
        QString key;
        // A layout of the whole book: the first page of each spine item,
        // for the settings with the estimateKey() `wholeKey`, and its
        // record without the place, which does not change with the page.
        QString wholeKey;
        QVector<int> wholeStarts;
        QJsonObject wholeRecord;
        // The spine item that `wholeRecord` keeps its nearest spine items
        // around, in a book with more than the record holds.
        int wholeRecordItem = -1;
        // The record of chapter mode without the place, for the map
        // generation `recordGeneration`, the estimateKey() `recordKey` and
        // the spine item `recordItem`, which decides what a full record
        // leaves out.
        QJsonObject record;
        quint64 recordGeneration = 0;
        QString recordKey;
        int recordItem = -1;
    } pageEstimate;
};

// The spine item that a position names as DocFragment[n], counted from zero,
// or -1. atStart reports whether the position names the fragment itself at its start.
int spineItemIndex(const QString &position, bool *atStart = nullptr);
// The pages of one slice in chapter mode. Zero lays out three spine items
// as a whole.
int chapterSlicePages();
} // namespace kobalt
