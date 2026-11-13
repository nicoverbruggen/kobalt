# Shared contract

This file records the original proposed contract between Kobalt and a rendering engine. Names and gaps below describe that proposal, not the current implementation. The implemented interface is [Engine](../../src/engine/engine.h); [Rendering engines](../engines.md#status) records current status. The contract is derived from the [dependency inventory](inventory.md) and the [compatibility matrix](compatibility.md).

## Ownership

| Part | Owns | Does not know |
| --- | --- | --- |
| Publication model (shared) | Identity, metadata, spine, contents, resources, source text of a spine item | Fonts, layout, pages |
| Engine adapter | Layout, pages, geometry, conversion between shared locations and native positions | Saved data, the interface, the display |
| `ReaderSession` (shared) | The worker, generations, stale results, the frame for the interface | Engine-native types |
| `ReadingBook` and storage (shared) | Saved locations, ranges, settings | The engine |
| Platform | Display refresh, Nickel, input | Publication structure |

The shared publication model reuses the parsing that Kobalt has: `src/publication` for the archive and resources, and the OPF reader of the Library. During the migration the CREngine adapter can still supply contents from its DOM. A new EPUB parser is not a first step.

## Shared models

```cpp
struct PublicationId { QString fingerprint; };       // content hash from Publication::inspect
struct ContentRevision { QString value; };           // changes when the file content changes

struct SpineItem { int index; QString href; bool linear; qint64 bytes; qint64 words; };

struct TocEntry
{
    QString title;
    int depth = 0;
    Location destination;   // always known when the entry exists
};

struct LayoutSettings { QJsonObject values; QSize viewport; int ppi = 0; };

struct Generation { quint64 session = 0; quint64 layout = 0; };
```

A `TocEntry` has no page field. The page of an entry is a query. See [Queries](#queries).

## Location format

### Decision

The shared location is a position in the source XHTML of one spine item. It does not depend on the font, the viewport, the pages or the engine.

The implemented part is `Location` in `src/publication/location.h`:

```cpp
struct Location
{
    int version = 1;
    QString href;         // spine item, as its path in the publication
    QVector<int> path;    // element indexes from <body>, one for each level
    int offset = -1;      // counted code points of the element's text before the position; negative = the element itself
    bool after = true;    // which side of a gap of uncounted characters
};
```

Rules:

- The path counts element children only. Comments, processing instructions and text do not count. This is the same rule as an EPUB CFI step, without the CFI text syntax.
- The offset is relative to the element at the end of the path, over all text below that element. It is not relative to one text node, because engines split text into nodes in different ways.
- The offset counts Unicode code points, and does not count whitespace or zero-width format characters (`locationUncounted`). An engine can collapse or drop these. CREngine drops a U+FEFF in some places.
- `after` selects the side when uncounted characters or markup are at the position: the start of the text that follows, or the end of the text before. A highlight that ends after a word and one that starts at the next word have the same offset and a different `after`.
- An empty path with a negative offset is the start of the spine item.
- A page number or a percent is never a location.

Planned and not implemented: an optional text quote for repair when the path does not resolve, an optional progression in the spine item as the last fallback, the list of native hints, and `Range`.

### Why this format

- CREngine's XPointer and the KOReader path that Crosspoint produces are both paths in the XHTML of one spine item, with a text step and an offset. Both engines already work with this shape.
- Neither native form is portable as it is. The CREngine path counts siblings by name, skips boxing nodes and changes with `dom_version`. Crosspoint counts visible code points over the whole spine item and treats whitespace runs in its own way.
- A path in the source file does not depend on either. Each adapter converts it.
- EPUB CFI was considered. It describes the same structure. Its text form adds parsing work and its spine step uses the package document. The struct above can be written as a CFI later if export is needed.

### Restoration result

Each conversion to a native position reports one of these:

| Result | Meaning |
| --- | --- |
| Exact | The path and the offset resolved. |
| Approximate | The path did not resolve. The quote or the progression was used. The result says which. |
| Unresolved | Nothing resolved. The location is kept and nothing is changed. |

### Hints

A hint is an optional accelerator. The CREngine adapter stores its XPointer and the DOM version as a hint and uses it first when its engine and version match. A location must work when all hints are removed.

### Test result for CREngine

`EngineDocument::locationOf` and `positionOf` convert between an XPointer and a location. `ctest --preset mac -R location-round-trip` checks them on the real EPUB fixtures in `tests/fixtures`. For each sampled text node it converts three positions to a location and back. It also parses the source XHTML and compares the element at the path with CREngine's element: its name and its counted text.

The same executable takes a folder of EPUB files as an argument and reports on each. On a local set of Standard Ebooks editions (novels, verse, drama, books with many notes and tables), the result was:

- Every sampled position returned to the same place in the text. Most returned the identical XPointer string. The rest returned another XPointer at the same place, with only whitespace or markup between the two.
- Every sampled element had the same path and name in the source XHTML.
- The counted text was the same in the source and in CREngine for every sampled element. Before zero-width characters were left out of the count, a small share of elements differed.

Limits of this result:

- All tested books are clean XHTML. Books with malformed markup, with named entities that need a DTD, or from other tool chains were not tested. For such a book the conversion fails or gives another place, and the test does not show how often.
- CREngine wrapper elements (`autoBoxing`, `tabularBox`, `rubyBox`, `mathBox`, `floatBox`, `inlineBox`) are passed through and generated content (`pseudoElem`) is ignored. A position inside generated content has no location.
- An XPointer to an element with a child offset becomes the location of the element.
- Crosspoint was not tested. It snaps a position inside whitespace to the next text run, which this format allows.

## Results

### Queries

Each query that can depend on layout returns a `Query<T>`.

```cpp
enum class QueryState { ReadyExact, ReadyEstimated, PendingLayout, Unsupported, Failed };

template <typename T> struct Query
{
    QueryState state;
    std::optional<T> value;   // set for ReadyExact and ReadyEstimated only
    Basis basis;              // for ReadyEstimated: Pages, Words, Bytes
    Reason reason;            // for Unsupported and Failed: why, in a form the interface can show
    Generation generation;    // the layout that the value belongs to
};
```

No state is expressed as zero, -1, an empty string or a null value.

### Operations

An operation that does work returns an `Outcome<T>`: done, cancelled, superseded or failed. Cancelled and superseded are not errors.

### Page information

```cpp
struct PageInfo
{
    Query<int> chapterPage, chapterPages;   // inside the current spine item or chapter
    Query<int> bookPage, bookPages;         // over the whole book
    Query<double> bookProgress;             // 0 to 1, with its basis
};
```

Chapter values and book values are separate. A chapter can have an exact page count while the book page is pending, because chapters before it have no layout. The contract never starts a layout of the whole book to fill a label.

## Capabilities

Capabilities describe what an engine can do. They are apart from the state of one request.

```cpp
struct Capability { Support support; QString limitation; };   // Support: Full, Limited, None

struct Capabilities
{
    Capability search, characterSelection, wordSelection, noteSheets, embeddedFonts,
               verticalWriting, tables, svg, mathml, externalLinks, bookPageTotal, cancelLayout;
    LayoutUnit layoutUnit;          // WholeBook or SpineItem
    QSet<QString> settings;         // the layout settings that the engine honors
};
```

An engine reports capabilities for the device and for the open publication. A book with vertical text in an engine without vertical writing reports `None` for that book, with the reason.

## Operations

The exact signatures follow when the code is written. This is the set that the inventory requires.

| Group | Operations | Cost |
| --- | --- | --- |
| Session | open, close, retain, capabilities | Open can parse the whole book |
| Publication | metadata, cover, spine, contents, resource | Cheap after open |
| Position | current location, go to location, next, previous, go to book progress | Go to can start a layout |
| Layout | request layout for a spine item, apply settings, set viewport | Expensive |
| Pages | page info, page of a location, locations of the page edges | Cheap. Can be pending. |
| Render | page image for the current, next and previous page, with refresh regions | Expensive |
| Links | links on the page, link target, note content | Cheap after layout |
| Text | token at a point, range rules, range rectangles, text of a range, compare locations | Cheap after layout |
| Search | search in a scope, in batches | Expensive |
| Platform | convert a host location (Nickel) to and from a shared location | Cheap |

Cheap queries never parse, lay out or draw. If a cheap query needs a layout that does not exist, it returns `PendingLayout`. The caller decides whether to request the layout.

## Scheduling

- All engine work runs off the main thread. The main thread never waits for it. This keeps [the page turn rule](../validation.md#main-thread-guard).
- Each engine has one worker. CREngine and Crosspoint keep process-wide state, so one engine never runs two layouts at once.
- Each result carries the session generation and the layout generation. `ReaderSession` drops a result when either is old: after a settings change, a newer navigation, a close or an engine switch.
- Before a new layout, the session reads the current location. After the layout, it goes to that location.
- The visible page comes first. Preparation of the next spine item starts after the visible page is drawn and stops when the user acts. Its memory and time have a limit.
- The work unit is reported, not hidden. CREngine reports `WholeBook` and `cancelLayout = None`: a started layout runs to its end, and its result is dropped if it is stale. Crosspoint reports `SpineItem` and can stop between steps.
- Shutdown waits for the worker only inside a `mainthread::AllowBlocking` scope that exists today for closing.

### Cache identity

A layout cache key contains the content revision, the engine name and version, the fonts, the viewport and the layout settings. The publication model and the contents have their own cache, which stays valid when layout caches are dropped. Each engine has its own cache files. The rules of [Storage compatibility](../storage.md) for cache versions stay as they are.

## Rendering surface

- An engine draws the body of a page into a surface that the session gives it. Today the surface is a `QImage` in viewport pixels.
- Coordinates in the contract are viewport pixels with the origin at the top left of the body. The header and the footer stay in the application.
- The engine returns the refresh regions as data next to the image, not as text keys on the image.
- The session owns the image after the call returns. The engine keeps no reference that it draws to later.
- A later engine can draw to another surface type. The surface type is part of the capabilities. Nothing else in the contract names `QImage`.

## Interface policies

1. A chapter that is known stays in the contents list when it has no layout and no page number.
2. An unknown page count is shown as unknown. It is never shown as zero and never as an exact number.
3. A jump to a known chapter requests the layout of that chapter. It does not require a layout of the chapters before it.
4. A feature can be unavailable for an engine, a book or a layout state. The interface shows the reason from the capability or the query.
5. A change of engine keeps the location. If the restoration is approximate, the interface says so. If it is unresolved, the book opens at the last location that resolved, and the saved location stays as it was.
6. The engine is chosen in a setting, with a documented default for each device. Kobalt never changes the engine without the user.

### Feature classes

This is a proposal from what Kobalt does today. The entries marked with a question are in [Decisions for the user](../engines.md#decisions-for-the-user).

| Class | Features |
| --- | --- |
| Essential: every engine must have it | Open, contents, page turns, go to a chapter, saved location and restore, font size and margins, internal links, images, the page image and refresh regions, bookmarks (question 2) |
| Can degrade, with a reason shown | Book page numbers and total, percent (question 3), the scrubber, chapter page labels, time remaining, highlights at word precision, tables, hyphenation languages, KOSync percentage (question 4), Nickel progress (question 5) |
| Optional: can be unavailable | Search, character selection, note sheets, embedded fonts, vertical writing, SVG, MathML, bold reading, optical size, the beautifier rules (question 1) |
