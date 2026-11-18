#pragma once
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QSize>
#include <QVector>
#include <functional>

// Plain values that cross between a rendering engine and the application.
// They name no engine. A position is a string that only its engine reads.
namespace kobalt {
struct PagePreview
{
    QImage image;
    QString position;
    bool bookmarked = false;
    // Geometry of the requested ranges in the original page viewport.
    QVector<QVector<QRect>> ranges;
};

enum class OpeningStage { Content, Text, Styles, Pages, Ready };
using OpeningProgress = std::function<void(OpeningStage, int)>;

inline constexpr int openingBarEngine = 94;
inline constexpr int openingBarWindow = 98;

inline int openingProgressPercent(OpeningStage stage, int percent)
{
    // Whole-book pagination takes most of the engine's opening time.
    constexpr int starts[] = { 0, 3, 14, 19, 90, openingBarEngine };
    const int index = int(stage);
    return starts[index] + (starts[index + 1] - starts[index]) * qBound(0, percent, 100) / 100;
}

struct ChapterProgress
{
    QString title;
    int page = 0, pages = 0;
};

struct SearchHit
{
    QString start, end, before, text, after;
    // The page of the layout, or in chapter mode an estimated page of the
    // book. Negative when there is none.
    int page = -1;
    // In chapter mode, the title of the chapter of the hit.
    QString chapter = {};
};

enum class SearchScope { Book, Chapter, Before, After };

// A few lines of text drawn with the reading fonts, without a document: a
// centred heading, then paragraphs. For previews of text settings.
struct TextSampleRequest
{
    // The reading font, or empty for the default one.
    QString face;
    // The text size in pixels of the canvas.
    int pixels = 0;
    // The line height in percent of the text size, as the reader sets it.
    int lineSpacing = 100;
    // "left", "justify", "center" or "right".
    QString alignment;
    // Breaks words at line ends, as the reader does with hyphenation on.
    bool hyphenate = false;
    // The value of a variable weight axis, or 400.
    int weight = 400;
    // The reader's weight_steps: added thickness in steps of 1/256 em.
    int extraWeight = 0;
    // In points; 0 follows the text size at `ppi`.
    double opticalSize = 0;
    int ppi = 300;
    QSize canvas;
    int margin = 0;
    QString heading;
    QStringList paragraphs;
};

struct SearchCursor
{
    int block = 0, offset = 0;
    // In chapter mode, the spine item that the search is in. The block and
    // offset count in the document that has its text.
    int item = 0;
    // The range of spine items in the document whose blocks the cursor counts.
    // Background preparation can replace that document with an overlapping unit.
    int firstItem = -1, lastItem = -1;
    // In chapter mode a long spine item is laid out in parts, and only the
    // part with a layout has text to search. Where the part that the search
    // is in starts, as a count of the text before it.
    int slice = 0;
    // Normalization can map adjacent matches to the same source range.
    QString previousStart, previousEnd;
    bool prepareWholeBook = false;
};

struct SearchBatch
{
    QVector<SearchHit> hits;
    int nextPage = 0;
    SearchCursor cursor;
    bool done = false, limited = false;
    // A restarted scan replaces results from the previous layout.
    bool reset = false;
};

struct TextRange
{
    QString start, end, text;
    QString chosenEnd = {};
    QString chosenStart = {};
    bool sentenceStartPolicy = false;
    bool sentenceEndPolicy = false;
    // The sentence around the range, when asked for.
    QString context = {};
};

struct WordSelection
{
    QString text, start, end;
    QVector<QRect> rectangles;
};

struct SelectionEndpoint
{
    // True terminal edge span in viewport pixels, never a clipped range edge.
    QPoint first, last;
    // Axis unit normal pointing away from selected text. Caps point inward.
    QPoint outward;
    bool visible = false; // False for endpoints outside the current page/viewport.
};

struct AnnotationGeometry
{
    QVector<QRect> rectangles;
    SelectionEndpoint start, end;
    bool vertical = false; // Current page's writing axis, independent of range shape.
};

struct AnnotationToken : WordSelection
{
    // Source boundaries for automatic paired punctuation. No whitespace is included.
    QString startBeforePunctuation, endAfterPunctuation;
    bool punctuation = false;
};

struct AnnotationPageEdges
{
    AnnotationToken first, last;
    SelectionEndpoint start, end;
    int columns = 1;
    bool vertical = false;
    bool continuesBefore = false, continuesAfter = false;
};
} // namespace kobalt
