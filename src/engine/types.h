#pragma once
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <functional>

// Plain values that cross between a rendering engine and the application.
// They name no engine. A position is a string that only its engine reads.
namespace kobalt {
enum class OpeningStage { Content, Text, Styles, Pages, Ready };
using OpeningProgress = std::function<void(OpeningStage, int)>;

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

struct SearchCursor
{
    int block = 0, offset = 0;
    // In chapter mode, the spine item that the search is in. The block and
    // offset count in the document that has its text.
    int item = 0;
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
