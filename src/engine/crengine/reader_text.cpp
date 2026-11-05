#include "column_view.h"
#include "support/qt_compat.h"
#include "engine_document.h"
#include "engine_activity.h"
#include <lvdocview.h>
#include <QTextBoundaryFinder>
#include <QSet>
#include <QHash>
#include <vector>

namespace kobalt {
namespace {
QString qstring(const lString32 &s)
{
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}

lString32 cre(const QString &s)
{
    return Utf8ToUnicode(s.toUtf8().constData());
}

// Collapse whitespace in search context, but keep one space at an edge that
// had one. The match joins its context without guessing at word boundaries.
QString contextText(const QString &text)
{
    auto result = text.simplified();
    if (result.isEmpty())
        return result;
    if (text.at(0).isSpace())
        result.prepend(' ');
    if (text.at(text.size() - 1).isSpace())
        result.append(' ');
    return result;
}
} // namespace

// Engine pointers belong to one document and one rendered DOM. Reflow can
// change boxing nodes, so even source-only comparisons use the layout epoch.
struct EngineDocument::SourceIndex
{
    struct Position
    {
        ldomXPointerEx pointer;
        int page[2] = { -2, -2 }; // Normal anchor and exclusive passage end.
        int textEndpoint = -1;

        bool selectable()
        {
            if (textEndpoint < 0)
                textEndpoint = !pointer.isNull() && pointer.isText() && pointer.getOffset() >= 0
                        && pointer.getOffset() <= pointer.getNode()->getText().length()
                        && pointer.isVisible();
            return textEndpoint;
        }
    };

    struct Range
    {
        int start, end, input;
    };

    struct Node
    {
        int range, left = -1, right = -1, maximumEnd;
    };

    quint64 layout = 0;
    qint64 retainedBytes = 0;
    QHash<QString, int> lookup;
    std::vector<Position> positions;
    QVector<QPair<QString, QString>> ranges;
    QVector<Range> compiled;
    QVector<Node> tree;
    bool ordered = true;
    int root = -1;

    int buildTree(QVector<int> &order, int first, int last)
    {
        if (first == last)
            return -1;
        const int middle = first + (last - first) / 2;
        const int node = tree.size();
        tree.append({ order[middle], -1, -1, compiled[order[middle]].end });
        const int left = buildTree(order, first, middle),
                  right = buildTree(order, middle + 1, last);
        tree[node].left = left;
        tree[node].right = right;
        for (const int child : { left, right }) {
            if (child >= 0
                && positions[tree[node].maximumEnd].pointer.compare(
                           positions[tree[child].maximumEnd].pointer)
                        < 0)
                tree[node].maximumEnd = tree[child].maximumEnd;
        }
        return node;
    }

    void query(int node, const ldomXPointerEx &first, const ldomXPointerEx &last,
               QVector<int> &result) const
    {
        if (node < 0 || positions[tree[node].maximumEnd].pointer.compare(first) <= 0)
            return;
        const auto &entry = compiled[tree[node].range];
        query(tree[node].left, first, last, result);
        if (positions[entry.start].pointer.compare(last) >= 0)
            return;
        if (first.compare(positions[entry.end].pointer) < 0)
            result.append(entry.input);
        query(tree[node].right, first, last, result);
    }

    QVector<int> intersections(const ldomXPointerEx &first, const ldomXPointerEx &last) const
    {
        QVector<int> result;
        if (ordered && !(first.isElement() && first.getOffset() > 0)
            && !(last.isElement() && last.getOffset() > 0)) {
            query(root, first, last, result);
            std::sort(result.begin(), result.end());
        } else {
            for (const auto &range : compiled)
                if (positions[range.start].pointer.compare(last) < 0
                    && first.compare(positions[range.end].pointer) < 0)
                    result.append(range.input);
        }
        return result;
    }
};

QPair<int, qint64> EngineDocument::sourceIndexUsage() const
{
    if (!sourceIndex_)
        return {};
    const auto &index = *sourceIndex_;
    return { int(index.positions.size()),
             index.retainedBytes + qint64(index.ranges.capacity()) * sizeof(QPair<QString, QString>)
                     + qint64(index.compiled.capacity()) * sizeof(SourceIndex::Range)
                     + qint64(index.tree.capacity()) * sizeof(SourceIndex::Node) };
}

std::shared_ptr<EngineDocument::SourceIndex>
EngineDocument::sourceIndex(const QStringList &anchors, const std::function<bool()> &current) const
{
    // Allow room for highlight endpoints, bookmarks and a changing draft.
    // The byte budget includes UTF-16 key payloads, pointer records and a
    // conservative hash allocation allowance. Range/tree arrays have their
    // separate fixed 2001-entry bound and fit in the reserved 256 KiB.
    constexpr int maximumPositions = 8192;
    constexpr qint64 maximumBytes = 4 * 1024 * 1024 - 256 * 1024;
    const auto layout = layoutSignature();
    if (!sourceIndex_ || sourceIndex_->layout != layout) {
        sourceIndex_ = std::make_shared<SourceIndex>();
        sourceIndex_->layout = layout;
    }
    auto index = sourceIndex_;
    QSet<QString> missing;
    qint64 missingBytes = 0;
    const auto collect = [&] {
        missing.clear();
        missingBytes = 0;
        int processed = 0;
        for (const auto &anchor : anchors) {
            if (processed++ % 64 == 0 && current && !current())
                return false;
            if (!index->lookup.contains(anchor) && !missing.contains(anchor)) {
                missing.insert(anchor);
                missingBytes +=
                        sizeof(SourceIndex::Position) + 96 + qint64(anchor.size()) * sizeof(QChar);
            }
        }
        return true;
    };
    if (!collect())
        return {};
    if (index->positions.size() + size_t(missing.size()) > maximumPositions
        || index->retainedBytes + missingBytes > maximumBytes) {
        // Replace the whole cache, including its interval tree. Its integer
        // references must never survive eviction of their source positions.
        index = std::make_shared<SourceIndex>();
        index->layout = layout;
        if (!collect())
            return {};
        if (missing.size() <= maximumPositions && missingBytes <= maximumBytes)
            sourceIndex_ = index;
        else
            sourceIndex_.reset(); // Oversized caller data is used only for this call.
    }
    index->positions.reserve(index->positions.size() + missing.size());
    auto *doc = engine_->getDocument();
    int processed = 0;
    for (const auto &anchor : missing) {
        if (processed++ % 64 == 0) {
            engineActivity().checkpoint();
            if (current && !current())
                return {};
        }
        SourceIndex::Position entry;
        if (!anchor.isEmpty())
            entry.pointer = ldomXPointerEx(doc->createXPointer(cre(anchor)));
        // Do not keep a caller's oversized QString allocation alive when its
        // logical key is short. Range snapshots share these compact key copies.
        index->lookup.insert(QString(anchor.constData(), anchor.size()),
                             int(index->positions.size()));
        index->positions.push_back(std::move(entry));
        index->retainedBytes +=
                sizeof(SourceIndex::Position) + 96 + qint64(anchor.size()) * sizeof(QChar);
    }
    return index;
}

QVector<int> EngineDocument::pagesOf(const QStringList &positions, bool rangeEndpoints,
                                     QVector<int> *sourceOrder, std::function<bool()> current) const
{
    QVector<int> pages;
    if (sourceOrder)
        sourceOrder->fill(-1, positions.size());
    if (!engine_) {
        pages.fill(-1, positions.size());
        return pages;
    }
    if (current && !current())
        return {};
    const auto index = sourceIndex(positions, current);
    if (!index)
        return {};
    QVector<int> resolved;
    resolved.reserve(positions.size());
    for (int i = 0; i < positions.size(); ++i) {
        if (i % 64 == 0) {
            engineActivity().checkpoint();
            if (current && !current())
                return {};
        }
        const int positionIndex = index->lookup.value(positions[i]);
        resolved.append(positionIndex);
        auto &entry = index->positions[positionIndex];
        const bool exclusive = rangeEndpoints && i % 2 == 1;
        // Passage ends are exclusive. Locate their preceding character so a
        // boundary exactly at the next page does not add an empty final page.
        if (entry.page[exclusive] == -2) {
            if (current && !current())
                return {};
            auto pointer = entry.pointer;
            if (exclusive && !pointer.isNull())
                pointer.prevVisibleChar(false);
            entry.page[exclusive] = pointer.isNull() ? -1 : engine_->getBookmarkPage(pointer);
        }
        pages.append(entry.page[exclusive]);
    }
    if (sourceOrder) {
        QVector<int> indices;
        for (int i = 0; i < positions.size(); ++i)
            if (!index->positions[resolved[i]].pointer.isNull())
                indices.append(i);
        std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
            return index->positions[resolved[a]].pointer.compare(
                           index->positions[resolved[b]].pointer)
                    < 0;
        });
        for (int rank = 0; rank < indices.size(); ++rank)
            (*sourceOrder)[indices[rank]] = rank;
    }
    return current && !current() ? QVector<int>() : pages;
}

QString EngineDocument::pageStart(int page) const
{
    if (!engine_ || page < 0 || page >= pages())
        return {};
    return qstring(engine_->getPageBookmark(page).toString());
}

QString EngineDocument::pageExcerpt(int page) const
{
    if (!engine_ || page < 0 || page >= pages())
        return {};
    return qstring(engine_->getPageText(false, page)).simplified().left(200);
}

namespace {
AnnotationToken sourceWord(ColumnView *engine, QSize size, ldomXPointerEx hit,
                           bool annotation = false)
{
    AnnotationToken result;
    if (hit.isNull() || !hit.isText() || !hit.isVisible()) {
        return result;
    }
    // Collect a bounded window in this rendered block, retaining a pointer for
    // every UTF-16 unit. Inline markup must not introduce word boundaries.
    auto previous = [](ldomXPointerEx &p) { return p.prevVisibleChar(true); };
    auto next = [](ldomXPointerEx &p) {
        if (!p.nextVisibleChar(true))
            return false;
        if (p.getOffset() == p.getNode()->getText().length())
            return p.nextVisibleChar(true);
        return true;
    };
    if (hit.getOffset() >= hit.getNode()->getText().length() && !previous(hit))
        return result;
    auto first = hit;
    for (int i = 0; i < 256; ++i) {
        auto p = first;
        if (!previous(p))
            break;
        first = p;
    }
    QString text;
    QVector<ldomXPointerEx> positions;
    int offset = -1;
    auto cursor = first;
    for (int count = 0; count < 513; ++count) {
        const auto nodeText = cursor.getNode()->getText();
        if (cursor.getOffset() < 0 || cursor.getOffset() >= nodeText.length())
            break;
        if (cursor == hit)
            offset = text.size();
        const char32_t character = nodeText[cursor.getOffset()];
        // A discretionary hyphen is layout, not part of the dictionary key.
        if (character != 0xad) {
            const auto piece = kobalt::qt::character(character);
            text += piece;
            for (int i = 0; i < piece.size(); ++i)
                positions.append(cursor);
        }
        if (!next(cursor))
            break;
    }
    if (offset < 0 || offset >= text.size())
        return result;
    auto scalar = [&text](int at) {
        const auto points = text.mid(at, 2).toUcs4();
        return points.isEmpty() ? char32_t(0) : char32_t(points.first());
    };
    auto prior = [&text](int at) {
        --at;
        if (at > 0 && text.at(at).isLowSurrogate() && text.at(at - 1).isHighSurrogate()) {
            --at;
        }
        return at;
    };
    auto following = [&text](int at) { return at + (text.at(at).isHighSurrogate() ? 2 : 1); };
    auto wordBounds = [&] {
        QTextBoundaryFinder words(QTextBoundaryFinder::Word, text);
        words.setPosition(offset);
        // Qt reports Han and Thai boundaries without word-item flags. Keep
        // these boundaries so their text can still be selected. Thai selection
        // follows these boundaries; it does not use a dictionary segmenter.
        const auto script = QChar::script(scalar(offset));
        const bool unflaggedScript = script == QChar::Script_Han || script == QChar::Script_Thai;
        if (!(words.boundaryReasons() & QTextBoundaryFinder::StartOfItem)
            && !(unflaggedScript && words.isAtBoundary())) {
            if (words.toPreviousBoundary() < 0) {
                return qMakePair(-1, -1);
            }
        }
        if (!(words.boundaryReasons() & QTextBoundaryFinder::StartOfItem) && !unflaggedScript) {
            return qMakePair(-1, -1);
        }
        const int begin = words.position(), end = words.toNextBoundary();
        if (end <= offset || end - begin > 256
            || (!(words.boundaryReasons() & QTextBoundaryFinder::EndOfItem) && !unflaggedScript)) {
            return qMakePair(-1, -1);
        }
        return qMakePair(begin, end);
    };
    const auto bounds = wordBounds();
    const bool punctuation = annotation && bounds.first < 0 && QChar::isPunct(scalar(offset));
    int begin = bounds.first, end = bounds.second;
    if (punctuation) {
        begin = offset;
        end = following(offset);
        while (begin > 0 && QChar::isPunct(scalar(prior(begin)))) {
            begin = prior(begin);
        }
        while (end < text.size() && QChar::isPunct(scalar(end))) {
            end = following(end);
        }
    } else if (begin < 0) {
        return result;
    }
    if (end - begin > 256) {
        return result;
    }
    // Do not silently truncate a word at the bounded window's edges.
    auto before = first;
    auto after = positions.back();
    if ((begin == 0 && previous(before)) || (end == text.size() && next(after)))
        return result;
    auto startPointer = positions[begin], endPointer = positions[end - 1];
    endPointer.setOffset(endPointer.getOffset() + 1);
    ldomXRange range(startPointer, endPointer);
    LVArray<lvRect> rectangles;
    range.getSegmentRects(rectangles);
    for (int i = 0; i < rectangles.length(); ++i) {
        const auto &r = rectangles[i];
        lvPoint top(r.left, r.top), bottom(r.right, r.bottom);
        if (!engine->docToWindowPoint(top) || !engine->docToWindowPoint(bottom, true))
            continue;
        const QRect rect(qMin(top.x, bottom.x), qMin(top.y, bottom.y), qAbs(bottom.x - top.x),
                         qAbs(bottom.y - top.y));
        const auto clipped = rect.intersected(QRect(QPoint(), size));
        if (clipped.isEmpty())
            continue;
        result.rectangles.append(clipped);
    }
    result.punctuation = punctuation;
    result.text = text.mid(begin, end - begin);
    result.start = qstring(startPointer.toString());
    result.end = qstring(endPointer.toString());
    if (annotation && !punctuation) {
        auto opening = [&scalar](int at) {
            const auto c = scalar(at);
            const auto category = QChar::category(c);
            return category == QChar::Punctuation_Open
                    || category == QChar::Punctuation_InitialQuote || c == '\'' || c == '"';
        };
        auto closing = [&scalar](int at) {
            const auto c = scalar(at);
            const auto category = QChar::category(c);
            return QChar::isPunct(c) && category != QChar::Punctuation_Open
                    && category != QChar::Punctuation_InitialQuote;
        };
        int outerStart = begin, outerEnd = end;
        while (outerStart > 0 && opening(prior(outerStart))) {
            outerStart = prior(outerStart);
        }
        while (outerEnd < text.size() && closing(outerEnd)) {
            outerEnd = following(outerEnd);
        }
        // Never expose a boundary truncated by the bounded source window.
        if (outerStart < begin && !(outerStart == 0 && previous(before))) {
            result.startBeforePunctuation = qstring(positions[outerStart].toString());
        }
        if (outerEnd > end && !(outerEnd == text.size() && next(after))) {
            auto pointer = positions[outerEnd - 1];
            pointer.setOffset(pointer.getOffset() + 1);
            result.endAfterPunctuation = qstring(pointer.toString());
        }
    }
    return result;
}

bool sentenceTerminal(char32_t c)
{
    return c == '.' || c == '!' || c == '?' || c == 0x3002 || c == 0xff01 || c == 0xff1f
            || c == 0x061f || c == 0x06d4 || c == 0x0964 || c == 0x0965;
}

bool ambiguousPeriod(const QString &text, int position)
{
    if (position <= 0 || text[position] != '.') {
        return false;
    }
    int begin = position;
    while (begin > 0 && (text[begin - 1].isLetterOrNumber() || text[begin - 1] == '.')) {
        --begin;
    }
    const auto word = text.mid(begin, position - begin).toLower();
    static const QSet<QString> abbreviations = { "mr",  "mrs", "ms",  "dr",  "prof",
                                                 "st",  "sr",  "jr",  "vs",  "etc",
                                                 "fig", "no",  "inc", "e.g", "i.e" };
    return word.size() == 1 || word.contains('.') || abbreviations.contains(word)
            || (!word.isEmpty() && word.at(word.size() - 1).isDigit());
}

QChar openingFor(QChar closing)
{
    switch (closing.unicode()) {
    case ')':
        return '(';
    case ']':
        return '[';
    case '}':
        return '{';
    case 0x201d:
        return QChar(0x201c);
    case 0x2019:
        return QChar(0x2018);
    case 0x300d:
        return QChar(0x300c);
    case 0x300f:
        return QChar(0x300e);
    case 0xff09:
        return QChar(0xff08);
    case 0x00bb:
        return QChar(0x00ab);
    case '"':
        return '"';
    case '\'':
        return '\'';
    default:
        return {};
    }
}

struct SentenceContext
{
    QString text;
    QVector<ldomXPointerEx> positions;
    int wordStart = -1, wordEnd = -1;
    bool blockStart = false, blockEnd = false;
};

SentenceContext sentenceContext(ColumnView *engine, const WordSelection &word,
                                const std::function<bool()> &current, int followingLimit = 512)
{
    SentenceContext result;
    auto *doc = engine->getDocument();
    ldomXPointerEx end(doc->createXPointer(cre(word.end)));
    auto hit = end;
    if (!hit.prevVisibleChar(true)) {
        return {};
    }
    // Context is temporary and bounded. A truncated leading sentence is never
    // treated as complete. Keep source pointers for every UTF-16 text unit.
    const ldomXPointerEx wordPointer(doc->createXPointer(cre(word.start)));
    auto first = hit;
    auto &blockStart = result.blockStart;
    for (int i = 0; i < 8192; ++i) {
        if (i % 128 == 0) {
            engineActivity().checkpoint();
            if (current && !current()) {
                return {};
            }
        }
        auto before = first;
        if (!before.prevVisibleChar(true)) {
            blockStart = true;
            break;
        }
        first = before;
    }
    auto &text = result.text;
    auto &positions = result.positions;
    auto &wordStart = result.wordStart;
    auto &wordEnd = result.wordEnd;
    auto &blockEnd = result.blockEnd;
    auto cursor = first;
    for (int i = 0; i < 16384; ++i) {
        if (i % 128 == 0) {
            engineActivity().checkpoint();
            if (current && !current()) {
                return {};
            }
        }
        const auto source = cursor.getNode()->getText();
        if (cursor.getOffset() < 0 || cursor.getOffset() >= source.length()) {
            break;
        }
        if (cursor == wordPointer) {
            wordStart = text.size();
        }
        const auto c = source[cursor.getOffset()];
        if (c != 0xad) {
            const auto piece = qt::character(c);
            text += piece;
            for (int n = 0; n < piece.size(); ++n) {
                positions.append(cursor);
            }
        }
        auto after = cursor;
        after.setOffset(after.getOffset() + 1);
        if (after == end) {
            wordEnd = text.size();
        }
        if (wordEnd >= 0 && text.size() - wordEnd > followingLimit) {
            break;
        }
        if (!cursor.nextVisibleChar(true)) {
            blockEnd = true;
            break;
        }
        if (cursor.getOffset() == cursor.getNode()->getText().length()
            && !cursor.nextVisibleChar(true)) {
            blockEnd = true;
            break;
        }
    }
    if (wordStart < 0 || wordEnd < 0 || wordEnd >= text.size()) {
        return {};
    }
    return result;
}

QString sentenceEnding(ColumnView *engine, QSize size, const TextRange &range,
                       const std::function<bool()> &current)
{
    auto *doc = engine->getDocument();
    ldomXPointerEx start(doc->createXPointer(cre(range.start))),
            end(doc->createXPointer(cre(range.end)));
    auto hit = end;
    if (!hit.prevVisibleChar(true)) {
        return {};
    }
    const auto word = sourceWord(engine, size, hit, true);
    if (word.punctuation || word.end != range.end || word.endAfterPunctuation.isEmpty()) {
        return {};
    }
    const auto context = sentenceContext(engine, word, current);
    const auto &text = context.text;
    const auto &positions = context.positions;
    const int wordStart = context.wordStart, wordEnd = context.wordEnd;
    const bool blockStart = context.blockStart, blockEnd = context.blockEnd;
    if (wordStart < 0 || wordEnd < 0) {
        return {};
    }
    const auto suffix = qstring(
            ldomXRange(end, ldomXPointerEx(doc->createXPointer(cre(word.endAfterPunctuation))))
                    .getRangeText());
    const auto marks = suffix.toUcs4();
    if (marks.isEmpty() || !sentenceTerminal(marks.first()) || suffix.contains(QChar(0x2026))
        || suffix.contains("..")) {
        return {};
    }
    if (ambiguousPeriod(text, wordEnd)) {
        return {};
    }
    QTextBoundaryFinder sentences(QTextBoundaryFinder::Sentence, text);
    sentences.setPosition(wordStart);
    int sentenceStart = sentences.isAtBoundary() ? wordStart : sentences.toPreviousBoundary();
    if (sentenceStart < 0 || (sentenceStart == 0 && !blockStart)) {
        return {};
    }
    // Do not mistake an abbreviation or an initial for the preceding sentence.
    int preceding = sentenceStart - 1;
    while (preceding >= 0
           && (text[preceding].isSpace() || text[preceding] == '"' || text[preceding] == '\'')) {
        --preceding;
    }
    if (preceding >= 0 && ambiguousPeriod(text, preceding)) {
        return {};
    }
    sentences.setPosition(wordEnd);
    const int sentenceEnd = sentences.toNextBoundary();
    if (sentenceEnd < 0 || (sentenceEnd == text.size() && !blockEnd)) {
        return {};
    }
    const auto tail = text.mid(wordEnd, sentenceEnd - wordEnd).trimmed();
    if (tail != suffix) {
        return {};
    }
    // Find the sentence's first annotation word, including CJK/Thai boundaries,
    // rather than assuming that words are separated by spaces.
    int firstWord = sentenceStart;
    while (firstWord < wordStart && (text[firstWord].isSpace() || text[firstWord].isPunct())) {
        ++firstWord;
    }
    if (firstWord >= positions.size() || start.compare(positions[firstWord]) > 0) {
        return {};
    }
    int included = 0;
    while (included < suffix.size() && sentenceTerminal(suffix[included].unicode())) {
        ++included;
    }
    // Only unmatched openers inside the bounded selected context can justify
    // closers. This includes a quotation spanning sentences, but an earlier
    // completed quotation cannot supply an opener.
    QVector<QChar> opened;
    for (int i = 0; i < wordEnd; ++i) {
        if (start.compare(positions[i]) > 0) {
            continue;
        }
        const auto c = text[i];
        if ((c == '\'' || c == QChar(0x2019)) && i > 0 && i + 1 < text.size()
            && text[i - 1].isLetterOrNumber() && text[i + 1].isLetterOrNumber()) {
            continue;
        }
        const auto match = openingFor(c);
        if (!match.isNull() && !opened.isEmpty() && opened.back() == match) {
            opened.removeLast();
        } else if (c == '(' || c == '[' || c == '{' || c == QChar(0x201c) || c == QChar(0x2018)
                   || c == QChar(0x300c) || c == QChar(0x300e) || c == QChar(0xff08)
                   || c == QChar(0x00ab) || c == '"' || c == '\'') {
            opened.append(c);
        }
    }
    for (; included < suffix.size(); ++included) {
        const auto opening = openingFor(suffix[included]);
        if (opening.isNull() || opened.isEmpty() || opened.back() != opening) {
            break;
        }
        opened.removeLast();
    }
    if (included <= 0 || wordEnd + included > positions.size()) {
        return {};
    }
    auto pointer = positions[wordEnd + included - 1];
    pointer.setOffset(pointer.getOffset() + 1);
    return qstring(pointer.toString());
}

struct SentenceOpening
{
    QString start, completeEnd;
};

SentenceOpening sentenceOpening(ColumnView *engine, QSize size, const TextRange &range,
                                const std::function<bool()> &current)
{
    auto *doc = engine->getDocument();
    const ldomXPointerEx start(doc->createXPointer(cre(range.start)));
    const auto word = sourceWord(engine, size, start, true);
    if (word.punctuation || word.start != range.start || word.startBeforePunctuation.isEmpty()) {
        return {};
    }
    const auto context = sentenceContext(engine, word, current, 8192);
    if (context.wordStart < 0 || context.wordEnd < 0) {
        return {};
    }
    QTextBoundaryFinder sentences(QTextBoundaryFinder::Sentence, context.text);
    sentences.setPosition(context.wordStart);
    const int beginning =
            sentences.isAtBoundary() ? context.wordStart : sentences.toPreviousBoundary();
    if (beginning < 0 || (beginning == 0 && !context.blockStart)) {
        return {};
    }
    int firstWord = beginning;
    while (firstWord < context.wordStart
           && (context.text[firstWord].isSpace() || context.text[firstWord].isPunct())) {
        ++firstWord;
    }
    if (firstWord != context.wordStart) {
        return {};
    }
    sentences.setPosition(context.wordEnd);
    const int sentenceEnd = sentences.toNextBoundary();
    if (sentenceEnd < 0 || (sentenceEnd == context.text.size() && !context.blockEnd)) {
        return {};
    }
    int terminal = sentenceEnd - 1;
    while (terminal >= context.wordEnd
           && (context.text[terminal].isSpace() || !openingFor(context.text[terminal]).isNull())) {
        --terminal;
    }
    while (terminal >= context.wordEnd && sentenceTerminal(context.text[terminal].unicode())) {
        --terminal;
    }
    ++terminal;
    if (terminal < context.wordEnd || terminal >= context.positions.size()
        || !sentenceTerminal(context.text[terminal].unicode())) {
        return {};
    }
    auto lastHit = context.positions[terminal];
    if (!lastHit.prevVisibleChar(true)) {
        return {};
    }
    const auto lastWord = sourceWord(engine, size, lastHit, true);
    if (lastWord.punctuation || lastWord.text.isEmpty()) {
        return {};
    }
    const QString rawEnd = lastWord.end;
    const auto provisionalText =
            qstring(ldomXRange(
                            ldomXPointerEx(doc->createXPointer(cre(word.startBeforePunctuation))),
                            ldomXPointerEx(doc->createXPointer(cre(rawEnd))))
                            .getRangeText())
                    .simplified();
    const TextRange provisional{ word.startBeforePunctuation, rawEnd, provisionalText };
    const auto complete = sentenceEnding(engine, size, provisional, current);
    if (complete.isEmpty()) {
        return {};
    }
    const ldomXPointerEx openingStart(doc->createXPointer(cre(word.startBeforePunctuation)));
    QVector<QChar> opened;
    for (int i = 0; i < context.text.size(); ++i) {
        if (i % 128 == 0) {
            engineActivity().checkpoint();
            if (current && !current()) {
                return {};
            }
        }
        if (context.positions[i].compare(openingStart) < 0) {
            continue;
        }
        const auto c = context.text[i];
        if ((c == '\'' || c == QChar(0x2019)) && i > 0 && i + 1 < context.text.size()
            && context.text[i - 1].isLetterOrNumber() && context.text[i + 1].isLetterOrNumber()) {
            continue;
        }
        const auto match = openingFor(c);
        if (!match.isNull() && !opened.isEmpty() && opened.back() == match) {
            opened.removeLast();
            if (opened.isEmpty() && i >= context.wordStart) {
                auto pairedEnd = context.positions[i];
                pairedEnd.setOffset(pairedEnd.getOffset() + 1);
                const ldomXPointerEx completeEnd(doc->createXPointer(cre(complete)));
                return { word.startBeforePunctuation,
                         pairedEnd.compare(completeEnd) > 0 ? qstring(pairedEnd.toString())
                                                            : complete };
            }
        } else if (c == '(' || c == '[' || c == '{' || c == QChar(0x201c) || c == QChar(0x2018)
                   || c == QChar(0x300c) || c == QChar(0x300e) || c == QChar(0xff08)
                   || c == QChar(0x00ab) || c == '"' || c == '\'') {
            opened.append(c);
        }
    }
    return {};
}

} // namespace

WordSelection EngineDocument::wordAt(QPoint point)
{
    if (!engine_ || !QRect(QPoint(), size_).contains(point)) {
        return {};
    }
    auto result = sourceWord(
            engine_.get(), size_,
            ldomXPointerEx(engine_->getNodeByPoint(lvPoint(point.x(), point.y()), true)));
    for (const auto &rect : result.rectangles) {
        if (rect.contains(point)) {
            return result;
        }
    }
    return {};
}

AnnotationPageEdges EngineDocument::annotationPageEdges()
{
    AnnotationPageEdges result;
    if (!engine_ || pages() <= 0)
        return result;
    auto *doc = engine_->getDocument();
    const ldomXPointerEx first(engine_->getPageBookmark(page()));
    const ldomXPointerEx last = page() + 1 < pages()
            ? ldomXPointerEx(engine_->getPageBookmark(page() + 1))
            : ldomXRange(doc->getRootNode(), true).getEnd();
    if (first.isNull() || last.isNull())
        return result;
    result.columns = columns_;
    QString firstPosition, lastPosition;
    auto find = [&](ldomXPointerEx cursor, bool forward) {
        AnnotationToken punctuation;
        for (int n = 0; n < 8192; ++n) {
            if (n % 64 == 0)
                engineActivity().checkpoint();
            if (cursor.isText() && cursor.compare(first) >= 0 && cursor.compare(last) < 0) {
                const auto text = cursor.getNode()->getText();
                const int offset = cursor.getOffset();
                if (offset >= 0 && offset < text.length() && !QChar::isSpace(text[offset])) {
                    auto token = sourceWord(engine_.get(), size_, cursor, true);
                    if (!token.text.isEmpty() && !token.rectangles.isEmpty()) {
                        if (forward) {
                            firstPosition = qstring(cursor.toString());
                        } else if (lastPosition.isEmpty()) {
                            auto end = cursor;
                            end.setOffset(offset + 1);
                            lastPosition = qstring(end.toString());
                        }
                        if (forward || !token.punctuation)
                            return token;
                        // Extend onto the last word, while keeping the physical
                        // page edge after its punctuation. Punctuation-only pages
                        // retain their selectable endpoint when no word exists.
                        if (punctuation.text.isEmpty())
                            punctuation = token;
                    }
                }
            }
            if (!(forward ? cursor.nextVisibleChar(false) : cursor.prevVisibleChar(false)))
                break;
            if (cursor.compare(first) < 0 || cursor.compare(last) >= 0)
                break;
        }
        return punctuation;
    };
    result.first = find(first, true);
    auto tail = last;
    if (tail.prevVisibleChar(false))
        result.last = find(tail, false);
    if (!result.first.text.isEmpty() && !result.last.text.isEmpty()) {
        const auto geometry = annotationGeometry(firstPosition, lastPosition);
        result.start = geometry.start;
        result.end = geometry.end;
        result.vertical = geometry.vertical;
        const auto sameSection = [&](ldomXPointerEx before, ldomXPointerEx after) {
            const auto section = [](ldomNode *node) {
                for (; node; node = node->getParentNode())
                    if (node->isNodeName("DocFragment") || node->isNodeName("section")
                        || node->isNodeName("article"))
                        return node;
                return static_cast<ldomNode *>(nullptr);
            };
            if (section(before.getNode()) != section(after.getNode()))
                return false;
            // A table-of-contents chapter can begin inside one spine document.
            std::vector<LVTocItem *> pending{ engine_->getToc() };
            while (!pending.empty()) {
                auto *item = pending.back();
                pending.pop_back();
                if (!item)
                    continue;
                const ldomXPointerEx marker(item->getXPointer());
                if (!marker.isNull() && marker.compare(before) > 0 && marker.compare(after) <= 0)
                    return false;
                for (int i = 0; i < item->getChildCount(); ++i)
                    pending.push_back(item->getChild(i));
            }
            return true;
        };
        const auto continuation = [&](bool forward) {
            if (forward ? page() + 1 >= pages() : page() == 0)
                return false;
            ldomXPointerEx edge(doc->createXPointer(cre(forward ? lastPosition : firstPosition)));
            auto adjacent = forward ? last : edge;
            if (!forward && !adjacent.prevVisibleChar(false))
                return false;
            for (int n = 0; n < 8192; ++n) {
                if (n % 64 == 0)
                    engineActivity().checkpoint();
                if (adjacent.isText() && adjacent.isVisible()) {
                    const auto text = adjacent.getNode()->getText();
                    const int offset = adjacent.getOffset();
                    if (offset >= 0 && offset < text.length() && !QChar::isSpace(text[offset])) {
                        if (engine_->getBookmarkPage(adjacent) != page() + (forward ? 1 : -1))
                            return false;
                        return forward ? sameSection(edge, adjacent) : sameSection(adjacent, edge);
                    }
                }
                if (!(forward ? adjacent.nextVisibleChar(false) : adjacent.prevVisibleChar(false)))
                    break;
            }
            return false;
        };
        result.continuesBefore = continuation(false);
        result.continuesAfter = continuation(true);
    }
    return result;
}

AnnotationToken EngineDocument::annotationTokenAt(QPoint point)
{
    if (!engine_ || !QRect(QPoint(), size_).contains(point)) {
        return {};
    }
    const ldomXPointerEx hit(engine_->getNodeByPoint(lvPoint(point.x(), point.y()), true));
    auto result = sourceWord(engine_.get(), size_, hit, true);
    for (const auto &rect : result.rectangles) {
        if (rect.contains(point)) {
            return result;
        }
    }
    // A space belongs to the preceding word when marking text. Verify the
    // actual whitespace cell first, so margins and paragraph gaps stay empty.
    auto space = hit;
    for (int candidate = 0; candidate < 2 && !space.isNull() && space.isText(); ++candidate) {
        const auto text = space.getNode()->getText();
        const int offset = space.getOffset();
        if (offset >= 0 && offset < text.length() && QChar::isSpace(text[offset])) {
            auto end = space;
            end.setOffset(offset + 1);
            bool contains = false;
            for (const auto &rect : rangeRects(qstring(space.toString()), qstring(end.toString())))
                contains |= rect.contains(point);
            if (contains) {
                auto before = space;
                for (int n = 0; n < 256 && before.prevVisibleChar(true); ++n) {
                    const auto value = before.getNode()->getText();
                    if (before.getOffset() < 0 || before.getOffset() >= value.length())
                        return {};
                    const auto character = value[before.getOffset()];
                    if (!QChar::isSpace(character) && !QChar::isPunct(character))
                        return sourceWord(engine_.get(), size_, before, true);
                }
                return {};
            }
        }
        if (!space.prevVisibleChar(true))
            break;
    }
    return {};
}

AnnotationToken EngineDocument::annotationDragTokenAt(QPoint point, const QString &boundary,
                                                      bool start)
{
    auto token = annotationTokenAt(point);
    if (boundary.isEmpty() || !engine_ || !QRect(QPoint(), size_).contains(point))
        return token;
    const auto crossedEdge = [&](const AnnotationToken &word) {
        if (word.text.isEmpty())
            return false;
        const auto geometry = annotationGeometry(word.start, word.end);
        const auto &edge = start ? geometry.start : geometry.end;
        const QPoint middle = (edge.first + edge.last) / 2;
        const QPoint delta = point - middle;
        const int beyond = QPoint::dotProduct(delta, edge.outward);
        const bool onLine = edge.outward.x() != 0 ? point.y() >= qMin(edge.first.y(), edge.last.y())
                        && point.y() <= qMax(edge.first.y(), edge.last.y())
                                                  : point.x() >= qMin(edge.first.x(), edge.last.x())
                        && point.x() <= qMax(edge.first.x(), edge.last.x());
        return edge.visible && beyond >= 0 && onLine;
    };
    if (token.text.isEmpty()) {
        // At a line end the pointer must pass the last glyph, into the margin.
        // CREngine's selection hit mode finds the adjacent text there. Keep
        // ordinary taps strict and reject hits outside the word's actual line.
        ldomXPointerEx hit(engine_->getNodeByPoint(lvPoint(point.x(), point.y()), false, true));
        token = sourceWord(engine_.get(), size_, hit, true);
        if (!crossedEdge(token)) {
            token = {};
            for (int n = 0; n < 256 && hit.isText(); ++n) {
                if (!(start ? hit.nextVisibleChar(true) : hit.prevVisibleChar(true)))
                    break;
                const auto text = hit.getNode()->getText();
                const int offset = hit.getOffset();
                if (offset >= 0 && offset < text.length() && !QChar::isSpace(text[offset])) {
                    const auto adjacent = sourceWord(engine_.get(), size_, hit, true);
                    if (crossedEdge(adjacent))
                        token = adjacent;
                    break;
                }
            }
        }
        if (token.text.isEmpty())
            return {};
    }
    auto *doc = engine_->getDocument();
    ldomXPointerEx current(doc->createXPointer(cre(boundary)));
    ldomXPointerEx candidate(doc->createXPointer(cre(start ? token.start : token.end)));
    if (current.isNull() || candidate.isNull())
        return {};
    const int order = candidate.compare(current);
    if (start ? order >= 0 : order <= 0)
        return token;
    // Expanding requires crossing the word's far edge, including on RTL and
    // vertical lines. Entering its whitespace or near half is not enough.
    if (crossedEdge(token))
        return token;
    // Pointer events can skip a narrow space. Keep the last complete word
    // even when the pointer has already entered the following word.
    ldomXPointerEx adjacent(doc->createXPointer(cre(start ? token.end : token.start)));
    if (!start && !adjacent.prevVisibleChar(true))
        return {};
    for (int n = 0; n < 256 && adjacent.isText(); ++n) {
        const auto text = adjacent.getNode()->getText();
        const int offset = adjacent.getOffset();
        if (offset >= 0 && offset < text.length() && !QChar::isSpace(text[offset])
            && !QChar::isPunct(text[offset])) {
            const auto complete = sourceWord(engine_.get(), size_, adjacent, true);
            ldomXPointerEx completed(
                    doc->createXPointer(cre(start ? complete.start : complete.end)));
            if (!completed.isNull()
                && (start ? completed.compare(current) < 0 : completed.compare(current) > 0))
                return complete;
            return {};
        }
        if (!(start ? adjacent.nextVisibleChar(true) : adjacent.prevVisibleChar(true)))
            break;
    }
    return {};
}

TextRange EngineDocument::annotationRange(const QString &startTokenStart,
                                          const QString &startTokenEnd,
                                          const QString &endTokenStart, const QString &endTokenEnd,
                                          bool endTokenPolicy, bool startTokenPolicy,
                                          std::function<bool()> current)
{
    if (!engine_) {
        return {};
    }
    auto *doc = engine_->getDocument();
    auto token = [&](const QString &start, const QString &end, bool exclusiveEnd) {
        if (start != end) {
            return textRange(start, end);
        }
        if (start.isEmpty() || start.size() > 4096) {
            return TextRange();
        }
        ldomXPointerEx pointer(doc->createXPointer(cre(start)));
        if (pointer.isNull() || !pointer.isText() || pointer.getOffset() < 0
            || pointer.getOffset() > pointer.getNode()->getText().length()) {
            return TextRange();
        }
        if (exclusiveEnd && !pointer.prevVisibleChar(true)) {
            return TextRange();
        }
        const auto word = sourceWord(engine_.get(), size_, pointer, true);
        return TextRange{ word.start, word.end, word.text };
    };
    const auto first = token(startTokenStart, startTokenEnd, false);
    const auto last = token(endTokenStart, endTokenEnd, true);
    if (first.text.isEmpty() || last.text.isEmpty()) {
        return {};
    }
    ldomXPointerEx a(doc->createXPointer(cre(first.start))),
            b(doc->createXPointer(cre(last.start)));
    const bool forward = a.compare(b) <= 0;
    auto result = forward ? textRange(first.start, last.end) : textRange(last.start, first.end);
    result.chosenStart = result.start;
    result.chosenEnd = result.end;
    result.sentenceStartPolicy = forward ? startTokenPolicy : endTokenPolicy;
    result.sentenceEndPolicy = forward ? endTokenPolicy : startTokenPolicy;
    const auto opening = result.sentenceStartPolicy
            ? sentenceOpening(engine_.get(), size_, result, current)
            : SentenceOpening();
    auto effective = result;
    if (!opening.start.isEmpty()) {
        effective.start = opening.start;
    }
    if (result.sentenceEndPolicy) {
        const auto expanded = sentenceEnding(engine_.get(), size_, effective, current);
        if (!expanded.isEmpty()) {
            effective.end = expanded;
        }
    }
    if (!opening.start.isEmpty()) {
        const ldomXPointerEx required(doc->createXPointer(cre(opening.completeEnd))),
                actual(doc->createXPointer(cre(effective.end)));
        if (required.compare(actual) > 0) {
            // Reject the provisional opener and recompute the end without it.
            effective.start = result.start;
            effective.end = result.end;
            if (result.sentenceEndPolicy) {
                const auto expanded = sentenceEnding(engine_.get(), size_, effective, current);
                if (!expanded.isEmpty()) {
                    effective.end = expanded;
                }
            }
        }
    }
    const auto text = textRange(effective.start, effective.end);
    result.start = text.start;
    result.end = text.end;
    result.text = text.text;
    return result;
}

TextRange EngineDocument::annotationRange(const QString &start, const QString &end)
{
    if (!engine_ || start.isEmpty() || end.isEmpty() || start.size() > 4096 || end.size() > 4096) {
        return {};
    }
    auto *doc = engine_->getDocument();
    ldomXPointerEx a(doc->createXPointer(cre(start))), b(doc->createXPointer(cre(end)));
    if (a.isNull() || b.isNull() || !a.isText() || !b.isText()) {
        return {};
    }
    if (a.getOffset() < 0 || a.getOffset() > a.getNode()->getText().length() || b.getOffset() < 0
        || b.getOffset() > b.getNode()->getText().length()) {
        return {};
    }
    if (a.compare(b) < 0) {
        return textRange(start, end);
    }
    // A reversed end points just after the earlier word. Move inside it before
    // applying the same Qt segmentation used by visible word hit testing.
    if (!b.prevVisibleChar(true)) {
        return {};
    }
    const auto first = sourceWord(engine_.get(), size_, b, true);
    const auto last = sourceWord(engine_.get(), size_, a, true);
    if (first.text.isEmpty() || last.text.isEmpty()) {
        return {};
    }
    return textRange(first.start, last.end);
}

TextRange EngineDocument::textRange(const QString &start, const QString &end)
{
    if (!engine_ || start.isEmpty() || end.isEmpty() || start.size() > 4096 || end.size() > 4096) {
        return {};
    }
    auto *doc = engine_->getDocument();
    ldomXPointerEx a(doc->createXPointer(cre(start))), b(doc->createXPointer(cre(end)));
    if (a.isNull() || b.isNull() || !a.isText() || !b.isText() || a == b) {
        return {};
    }
    if (a.getOffset() < 0 || a.getOffset() > a.getNode()->getText().length() || b.getOffset() < 0
        || b.getOffset() > b.getNode()->getText().length() || !a.isVisible() || !b.isVisible()) {
        return {};
    }
    if (a.compare(b) > 0) {
        std::swap(a, b);
    }
    ldomXRange range(a, b);
    const auto text = qstring(range.getRangeText()).simplified();
    if (text.isEmpty() || text.size() > 256 * 1024) {
        return {};
    }
    return { qstring(a.toString()), qstring(b.toString()), text };
}

QVector<int>
EngineDocument::intersectingRanges(const QString &start, const QString &end,
                                   const QVector<QPair<QString, QString>> &ranges) const
{
    QVector<int> result;
    if (!engine_) {
        return result;
    }
    auto *doc = engine_->getDocument();
    auto resolve = [doc](const QString &anchor) {
        ldomXPointerEx pointer;
        if (anchor.isEmpty() || anchor.size() > 4096) {
            return pointer;
        }
        pointer = ldomXPointerEx(doc->createXPointer(cre(anchor)));
        if (pointer.isNull() || !pointer.isText() || pointer.getOffset() < 0
            || pointer.getOffset() > pointer.getNode()->getText().length()
            || !pointer.isVisible()) {
            return ldomXPointerEx();
        }
        return pointer;
    };
    const auto first = resolve(start), last = resolve(end);
    if (first.isNull() || last.isNull() || first.compare(last) >= 0) {
        return result;
    }
    const auto index = sourceRanges(ranges);
    for (const int i : index->intersections(first, last)) {
        auto &a = index->positions[index->lookup.value(ranges[i].first)];
        auto &b = index->positions[index->lookup.value(ranges[i].second)];
        // Visibility accepts normalized element ranges. Saving a passage has
        // always required forward, visible text endpoints, so filter candidates
        // with that stricter contract before reporting an overlap.
        if (a.selectable() && b.selectable() && a.pointer.compare(b.pointer) < 0)
            result.append(i);
    }
    return result;
}

std::shared_ptr<EngineDocument::SourceIndex>
EngineDocument::sourceRanges(const QVector<QPair<QString, QString>> &ranges) const
{
    auto index = sourceIndex_;
    if (!index || index->layout != layoutSignature() || index->ranges != ranges) {
        QStringList anchors;
        anchors.reserve(ranges.size() * 2);
        bool oversized = ranges.size() > 2001;
        for (const auto &range : ranges) {
            if (range.first.size() <= 4096 && range.second.size() <= 4096)
                anchors << range.first << range.second;
            else
                oversized = true;
        }
        index = sourceIndex(anchors);
        if (oversized)
            sourceIndex_.reset(); // Do not retain unbounded range arrays or invalid key payloads.
        index->ranges.clear();
        index->ranges.reserve(ranges.size());
        for (const auto &range : ranges) {
            const auto start = index->lookup.constFind(range.first),
                       end = index->lookup.constFind(range.second);
            index->ranges.append({ start == index->lookup.cend() ? range.first : start.key(),
                                   end == index->lookup.cend() ? range.second : end.key() });
        }
        index->compiled.clear();
        index->compiled.reserve(ranges.size());
        index->tree.clear();
        index->tree.reserve(ranges.size());
        index->ordered = true;
        for (int i = 0; i < ranges.size(); ++i) {
            if (i % 64 == 0)
                engineActivity().checkpoint();
            const auto &range = ranges[i];
            if (range.first.isEmpty() || range.second.isEmpty() || range.first.size() > 4096
                || range.second.size() > 4096)
                continue;
            int start = index->lookup.value(range.first), end = index->lookup.value(range.second);
            const auto &a = index->positions[start].pointer, &b = index->positions[end].pointer;
            if (a.isNull() || b.isNull())
                continue;
            const int direction = a.compare(b);
            if (direction == 0)
                continue;
            if (direction > 0)
                std::swap(start, end);
            if (index->positions[start].pointer.compare(index->positions[end].pointer) >= 0)
                continue;
            // Element child offsets do not have an antisymmetric source order
            // in CREngine. Preserve the original scan for these uncommon inputs.
            if ((a.isElement() && a.getOffset() > 0) || (b.isElement() && b.getOffset() > 0))
                index->ordered = false;
            index->compiled.append({ start, end, i });
        }
        QVector<int> order;
        order.reserve(index->compiled.size());
        for (int i = 0; i < index->compiled.size(); ++i)
            order.append(i);
        if (index->ordered) {
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                return index->positions[index->compiled[a].start].pointer.compare(
                               index->positions[index->compiled[b].start].pointer)
                        < 0;
            });
            index->root = index->buildTree(order, 0, order.size());
        }
    }
    return index;
}

QVector<int> EngineDocument::visibleRanges(const QVector<QPair<QString, QString>> &ranges) const
{
    if (ranges.isEmpty() || ranges.size() > 2001 || !engine_ || pages() <= 0)
        return {};
    auto *doc = engine_->getDocument();
    const int current = page();
    // Page boundaries come from the current layout, never from stored page
    // numbers. A long passage remains visible on every intervening page.
    const ldomXPointerEx first(engine_->getPageBookmark(current));
    const ldomXPointerEx last = current + 1 < pages()
            ? ldomXPointerEx(engine_->getPageBookmark(current + 1))
            : ldomXRange(doc->getRootNode(), true).getEnd();
    if (first.isNull() || last.isNull())
        return {};
    return sourceRanges(ranges)->intersections(first, last);
}

bool EngineDocument::rangeVisible(const QString &start, const QString &end) const
{
    if (!engine_ || pages() <= 0 || start.isEmpty() || end.isEmpty() || start.size() > 4096
        || end.size() > 4096)
        return false;
    auto *doc = engine_->getDocument();
    ldomXPointerEx a(doc->createXPointer(cre(start))), b(doc->createXPointer(cre(end)));
    if (a.isNull() || b.isNull())
        return false;
    if (a.compare(b) > 0)
        std::swap(a, b);
    const int current = page();
    const ldomXPointerEx first(engine_->getPageBookmark(current));
    const ldomXPointerEx last = current + 1 < pages()
            ? ldomXPointerEx(engine_->getPageBookmark(current + 1))
            : ldomXRange(doc->getRootNode(), true).getEnd();
    return !first.isNull() && !last.isNull() && a.compare(b) < 0 && a.compare(last) < 0
            && first.compare(b) < 0;
}

AnnotationGeometry EngineDocument::annotationGeometry(const QString &start, const QString &end)
{
    AnnotationGeometry result;
    if (!engine_ || pages() <= 0 || start.isEmpty() || end.isEmpty() || start.size() > 4096
        || end.size() > 4096) {
        return result;
    }
    auto *doc = engine_->getDocument();
    ldomXPointerEx a(doc->createXPointer(cre(start))), b(doc->createXPointer(cre(end)));
    if (a.isNull() || b.isNull()) {
        return result;
    }
    if (a.compare(b) > 0) {
        std::swap(a, b);
    }
    if (a.compare(b) == 0) {
        return result;
    }
    result.rectangles = rangeRects(qstring(a.toString()), qstring(b.toString()));
    const int current = page();
    auto *pageList = engine_->getPageList();
    result.vertical = pageList->hasMixedWritingModes()
            ? css_wm_is_vertical((*pageList)[current * columns_]->writing_mode)
            : engine_->isVerticalText();
    ldomXPointerEx pageStart(engine_->getPageBookmark(current));
    ldomXPointerEx pageEnd = current + 1 < pages()
            ? ldomXPointerEx(engine_->getPageBookmark(current + 1))
            : ldomXRange(doc->getRootNode(), true).getEnd();
    const auto endpoint = [&](ldomXPointerEx pointer, bool ending) {
        SelectionEndpoint edge;
        if (!pointer.isText() || pointer.getOffset() < 0
            || pointer.getOffset() > pointer.getNode()->getText().length()) {
            return edge;
        }
        if (ending) {
            if (!pointer.prevVisibleChar(false) || pointer.compare(a) < 0) {
                return edge;
            }
        } else if (pointer.getOffset() == pointer.getNode()->getText().length()) {
            if (!pointer.nextVisibleChar(false) || pointer.compare(b) >= 0) {
                return edge;
            }
        }
        // Compare source anchors before resolving caret geometry: remote ends must
        // not format another chapter merely to discover they are not visible here.
        if (pageStart.isNull() || pageEnd.isNull() || pointer.compare(pageStart) < 0
            || pointer.compare(pageEnd) >= 0 || !pointer.isVisible()) {
            return edge;
        }
        lvRect box;
        int context = RECT_CTX_NONE;
        if (!pointer.getRectEx(box, false, &context) || box.height() <= 0) {
            return edge;
        }
        const bool rtl = (context & RECT_CTX_IS_RTL) != 0;
        const bool right = ending != rtl;
        const int x = right ? box.right : box.left;
        lvPoint first(x, box.top), last(x, box.bottom), middle(x, box.top + box.height() / 2);
        lvPoint inside(x + (right ? -1 : 1), middle.y);
        if (!engine_->docToWindowPoint(first) || !engine_->docToWindowPoint(last, true)
            || !engine_->docToWindowPoint(middle) || !engine_->docToWindowPoint(inside)) {
            return edge;
        }
        const auto onScreen = [&](const lvPoint &point) {
            return point.x >= 0 && point.y >= 0 && point.x <= size_.width()
                    && point.y <= size_.height();
        };
        if (!onScreen(first) || !onScreen(last)) {
            return edge;
        }
        const QPoint normal(middle.x - inside.x, middle.y - inside.y);
        if (normal.manhattanLength() != 1) {
            return edge;
        }
        edge.first = QPoint(first.x, first.y);
        edge.last = QPoint(last.x, last.y);
        edge.outward = normal;
        edge.visible = edge.first != edge.last;
        return edge;
    };
    result.start = endpoint(a, false);
    result.end = endpoint(b, true);
    return result;
}

QVector<QVector<QRect>>
EngineDocument::pageRangeRects(int target, const QVector<QPair<QString, QString>> &ranges)
{
    QVector<QVector<QRect>> result;
    if (!engine_ || target < 0 || target >= pages() || ranges.isEmpty() || ranges.size() > 2000)
        return result;
    result.resize(ranges.size());
    const int current = engine_->getCurPage();

    struct Restore
    {
        LVDocView &view;
        ldomXPointer bookmark;
        bool moved;

        ~Restore()
        {
            if (moved)
                view.goToBookmark(bookmark);
        }
    } restore{ *engine_, engine_->getBookmark(), target != current };

    if (restore.moved)
        engine_->goToPage(target);
    int remaining = 65536;
    for (const int index : visibleRanges(ranges)) {
        engineActivity().checkpoint();
        if (remaining <= 0)
            break;
        auto rectangles = rangeRects(ranges[index].first, ranges[index].second);
        if (rectangles.size() > remaining)
            rectangles.resize(remaining);
        remaining -= rectangles.size();
        result[index] = std::move(rectangles);
    }
    return result;
}

QVector<QRect> EngineDocument::rangeRects(const QString &start, const QString &end,
                                          bool includeImages)
{
    QVector<QRect> result;
    if (!engine_)
        return result;
    auto *doc = engine_->getDocument();
    ldomXPointerEx a(doc->createXPointer(cre(start))), b(doc->createXPointer(cre(end)));
    if (a.isNull() || b.isNull())
        return result;
    // Resolve geometry only for this page, even for a passage spanning chapters.
    // Image link ranges keep their existing engine behavior.
    if (!includeImages) {
        if (a.compare(b) > 0) {
            std::swap(a, b);
        }
        const int current = page();
        ldomXPointerEx first(engine_->getPageBookmark(current));
        ldomXPointerEx last = current + 1 < pages()
                ? ldomXPointerEx(engine_->getPageBookmark(current + 1))
                : ldomXRange(doc->getRootNode(), true).getEnd();
        if (!first.isNull() && a.compare(first) < 0) {
            a = first;
        }
        if (!last.isNull() && b.compare(last) > 0) {
            b = last;
        }
        if (a.compare(b) >= 0) {
            return result;
        }
    }
    ldomXRange range(a, b);
    LVArray<lvRect> rectangles;
    range.getSegmentRects(rectangles, includeImages);
    for (int i = 0; i < rectangles.length(); ++i) {
        const auto &r = rectangles[i];
        lvPoint top(r.left, r.top), bottom(r.right, r.bottom);
        if (!engine_->docToWindowPoint(top) || !engine_->docToWindowPoint(bottom, true))
            continue;
        const auto clipped = QRect(qMin(top.x, bottom.x), qMin(top.y, bottom.y),
                                   qAbs(bottom.x - top.x), qAbs(bottom.y - top.y))
                                     .intersected(QRect(QPoint(), size_));
        if (!clipped.isEmpty())
            result.append(clipped);
    }
    return result;
}

SearchBatch EngineDocument::searchPage(const QString &query, int number)
{
    SearchBatch result;
    result.nextPage = number + 1;
    result.done = !engine_ || number < 0 || number + 1 >= pages() || query.isEmpty()
            || query.size() > 256;
    if (!engine_ || number < 0 || number >= pages() || query.isEmpty() || query.size() > 256)
        return result;
    auto *doc = engine_->getDocument();
    ldomXPointerEx start(engine_->getPageBookmark(number));
    ldomXPointerEx end = number + 1 < pages() ? ldomXPointerEx(engine_->getPageBookmark(number + 1))
                                              : ldomXRange(doc->getRootNode(), true).getEnd();
    if (start.isNull() || end.isNull())
        return result;
    // Include the query's tail past the next page boundary. Search still owns
    // only matches beginning on this page, so wrapped phrases appear once.
    // Extra code points cover canonical decomposition and case folding.
    auto extended = end;
    for (int remaining = query.size() * 4 + 16; remaining > 0;) {
        auto next = extended;
        if (!next.nextVisibleChar(true))
            break;
        extended = next;
        const auto text = extended.getNode()->getText();
        if (extended.getOffset() >= text.length())
            continue;
        const char32_t c = text[extended.getOffset()];
        if (c != 0xad && !QChar::isSpace(c))
            --remaining;
    }
    ldomXRange range(start, extended);
    ldomXRangeList matches;
    // Compatibility normalization matches ligatures such as "ﬁ" to "fi". Folding
    // hyphens and skipping format controls matches "e‑mail" (a non-breaking
    // hyphen) and words split by zero-width characters as typed.
    const auto flags = LDOM_FIND_TEXT_MATCH_ACROSS_TEXT_NODES
            | LDOM_FIND_TEXT_COLLAPSE_CONSECUTIVE_SPACES | LDOM_FIND_TEXT_NORMALIZE_CANONICAL
            | LDOM_FIND_TEXT_NORMALIZE_COMPATIBILITY | LDOM_FIND_TEXT_IGNORE_FORMAT_CONTROL_CHARS
            | LDOM_FIND_TEXT_FOLD_SPACES | LDOM_FIND_TEXT_FOLD_APOSTROPHES
            | LDOM_FIND_TEXT_FOLD_HYPHENS;
    range.findText(cre(query), true, false, matches, 101, 0, -1, false, false, flags);
    for (int i = 0; i < matches.length(); ++i) {
        auto &match = *matches[i];
        const int page = engine_->getBookmarkPage(match.getStart());
        if (page != number)
            continue;
        if (result.hits.size() == 100) {
            result.limited = result.done = true;
            break;
        }
        auto before = match.getStart(), after = match.getEnd();
        for (int n = 0; n < 48; ++n)
            if (!before.prevVisibleChar(true))
                break;
        for (int n = 0; n < 96; ++n)
            if (!after.nextVisibleChar(true))
                break;
        ldomXRange prefix(before, match.getStart()), suffix(match.getEnd(), after);
        result.hits.append({ qstring(match.getStart().toString()),
                             qstring(match.getEnd().toString()),
                             contextText(qstring(prefix.getRangeText())),
                             qstring(match.getRangeText()).simplified(),
                             contextText(qstring(suffix.getRangeText())), page });
    }
    return result;
}
} // namespace kobalt
