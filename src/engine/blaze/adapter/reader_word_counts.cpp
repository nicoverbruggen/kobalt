#include "column_view.h"
#include "engine_document.h"
#include "reader_source.h"

#include "engine/engine_activity.h"
#include "reading/logical_word_counter.h"
#include "support/qt_compat.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <lvdocview.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace kobalt {
namespace {
constexpr auto property = "kobalt.word-count-v1";
constexpr int maximumCacheBytes = 512 * 1024;
constexpr int maximumDepth = 512;
constexpr int chunkCharacters = 4096;

bool excluded(ldomNode *node)
{
    return node->isNodeName("head") || node->isNodeName("script") || node->isNodeName("style")
            || node->isNodeName("svg") || node->isNodeName("rt") || node->isNodeName("rp")
            || node->getRendMethod() == erm_invisible
            || (!node->getStyle().isNull() && node->getStyle()->display == css_d_none);
}

bool block(ldomNode *node)
{
    return node->isNodeName("br")
            || (!node->getStyle().isNull() && node->getStyle()->display >= css_d_block
                && node->getStyle()->display != css_d_inline_block
                && node->getStyle()->display != css_d_inline_table);
}

QByteArray encode(const DocumentWordCounts &counts)
{
    QByteArray result = "{\"total\":\"" + QByteArray::number(counts.total) + "\",\"chapters\":[";
    for (const auto &chapter : counts.chapters) {
        // Bound the temporary entry, then charge its actual escaped size.
        // A worst-case estimate for the whole list rejects ordinary large TOCs.
        if (chapter.position.size() > maximumCacheBytes / 6)
            return {};
        const auto entry =
                QJsonDocument(qt::array({ chapter.position, QString::number(chapter.words) }))
                        .toJson(QJsonDocument::Compact);
        if (result.size() + entry.size() + 3 > maximumCacheBytes)
            return {};
        if (!result.endsWith('['))
            result += ',';
        result += entry;
    }
    return result + "]}";
}

DocumentWordCounts decode(const QByteArray &bytes, const QVector<ChapterLink> &chapters)
{
    if (bytes.isEmpty() || bytes.size() > maximumCacheBytes) {
        return {};
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return {};
    }
    const auto object = json.object();
    bool valid = false;
    const auto total = object.value("total").toString().toLongLong(&valid);
    if (!valid || total < 0 || !object.value("chapters").isArray()) {
        return {};
    }
    const auto entries = object.value("chapters").toArray();
    if (entries.size() != chapters.size()) {
        return {};
    }
    DocumentWordCounts result;
    result.total = total;
    result.cached = true;
    qint64 covered = 0;
    for (int i = 0; i < entries.size(); ++i) {
        const auto entry = entries[i].toArray();
        if (entry.size() != 2 || entry[0].toString() != chapters[i].position) {
            return {};
        }
        const auto words = entry[1].toString().toLongLong(&valid);
        if (!valid || words < -1 || words > total) {
            return {};
        }
        if (words >= 0) {
            if (words > total - covered) {
                return {};
            }
            covered += words;
        }
        result.chapters.append({ chapters[i].position, words });
    }
    return result;
}

struct Event
{
    int offset;
    int chapter;
};

struct Prefix
{
    qint64 words = -1;
    int order = -1;
};

qint64 countPrefixes(ldomDocument *document, const QVector<ldomXPointerEx> &positions,
                     QVector<Prefix> &prefixes, const std::function<bool()> &current,
                     bool countToEnd = true)
{
    int requested = 0;
    QHash<ldomNode *, QVector<Event>> events;
    prefixes.resize(positions.size());
    for (int i = 0; i < positions.size(); ++i) {
        const auto &pointer = positions[i];
        if (pointer.isNull()) {
            continue;
        }
        auto *node = pointer.getNode();
        int offset = pointer.getOffset();
        if (node->isElement() && offset > 0) {
            if (offset > node->getChildCount()) {
                continue;
            }
            if (offset == node->getChildCount()) {
                offset = -1;
            } else {
                node = node->getChildNode(offset);
                offset = 0;
            }
        }
        events[node].append({ offset, i });
        ++requested;
    }
    for (auto it = events.begin(); it != events.end(); ++it) {
        std::stable_sort(it->begin(), it->end(),
                         [](const Event &a, const Event &b) { return a.offset < b.offset; });
    }

    LogicalWordCounter counter;
    int order = 0;
    const auto capture = [&](const Event &event) {
        prefixes[event.chapter] = { counter.words(), order++ };
    };
    const auto checkpoint = [&] {
        engineActivity().checkpoint();
        return !current || current();
    };

    struct Frame
    {
        ldomNode *node;
        int child = -1;
    };

    std::vector<Frame> stack{ { document->getRootNode() } };
    unsigned nodes = 0;
    unsigned characters = 0;
    while (!stack.empty() && (countToEnd || order < requested)) {
        if (++nodes % 128 == 0 && !checkpoint()) {
            return -1;
        }
        auto &frame = stack.back();
        auto *node = frame.node;
        if (frame.child < 0) {
            if (node->isElement() && excluded(node)) {
                stack.pop_back();
                continue;
            }
            const auto found = events.constFind(node);
            if (node->isText()) {
                const auto text = node->getText();
                int offset = 0;
                const auto scan = [&](int end) {
                    for (; offset < end; ++offset) {
                        if (++characters % chunkCharacters == 0 && !checkpoint()) {
                            return false;
                        }
                        counter.append(text[offset]);
                    }
                    return true;
                };
                if (found != events.cend()) {
                    for (const auto &event : found.value()) {
                        if (event.offset < 0 || event.offset > text.length()) {
                            continue;
                        }
                        if (!scan(event.offset)) {
                            return -1;
                        }
                        capture(event);
                    }
                }
                if (!scan(text.length())) {
                    return -1;
                }
                stack.pop_back();
                continue;
            }
            if (found != events.cend()) {
                for (const auto &event : found.value()) {
                    if (event.offset == 0) {
                        capture(event);
                    }
                }
            }
            if (block(node)) {
                counter.boundary();
            }
            frame.child = 0;
        }
        if (frame.child < node->getChildCount()) {
            if (stack.size() >= maximumDepth) {
                return -1;
            }
            auto *child = node->getChildNode(frame.child++);
            stack.push_back({ child });
        } else {
            const auto found = events.constFind(node);
            if (found != events.cend()) {
                for (const auto &event : found.value()) {
                    if (event.offset == -1) {
                        capture(event);
                    }
                }
            }
            if (block(node)) {
                counter.boundary();
            }
            stack.pop_back();
        }
    }
    if (!checkpoint()) {
        return -1;
    }
    return counter.words();
}

} // namespace

qint64 EngineDocument::wordsInMainText(const QString &first, const QString &after,
                                       const std::function<bool()> &current)
{
    if (totalWords_ < 0 || (first.isEmpty() && after.isEmpty()))
        return totalWords_;
    const QPair<QString, QString> range{ first, after };
    if (range == countedRange_)
        return rangeWords_;

    qint64 begin = 0, end = totalWords_;
    if (windowFirst_ >= 0) {
        auto &map = pageMap();
        if (!map.allMeasured())
            return -1;
        const auto prefix = [&](const QString &position) -> qint64 {
            const int item = spineItemIndex(position);
            if (item < 0 || item >= map.items())
                return -1;
            textFraction(item, position, false, current);
            qint64 count = source_->textWordPrefixes.value(position, -1);
            if (count < 0)
                return -1;
            for (int i = 0; i < item; ++i) {
                const auto words = map.itemWords(i);
                if (words < 0)
                    return -1;
                count += words;
            }
            return count;
        };
        if (!first.isEmpty())
            begin = prefix(first);
        if (!after.isEmpty())
            end = prefix(after);
    } else {
        auto *document = engine_->getDocument();
        const auto pointer = [document](const QString &position) {
            if (position.isEmpty())
                return ldomXPointerEx();
            ldomXPointerEx anchor(
                    document->createXPointer(Utf8ToUnicode(position.toUtf8().constData())));
            // A bare element path denotes the start of its contents.
            if (!anchor.isNull() && anchor.getNode()->isElement() && anchor.getOffset() < 0)
                anchor.setOffset(0);
            return anchor;
        };
        QVector<Prefix> prefixes;
        if (countPrefixes(document, { pointer(first), pointer(after) }, prefixes, current, false) < 0)
            return -1;
        if (!first.isEmpty())
            begin = prefixes[0].words;
        if (!after.isEmpty())
            end = prefixes[1].words;
    }
    if (current && !current())
        return -1;
    countedRange_ = range;
    rangeWords_ = begin >= 0 && end >= begin && end <= totalWords_ ? end - begin : -1;
    return rangeWords_;
}

DocumentWordCounts EngineDocument::wordCounts(std::function<bool()> current)
{
    if (!engine_ || (current && !current())) {
        return {};
    }
    if (windowFirst_ >= 0) {
        // The styled DOM contains only one Fast window. Source counts cover
        // the whole book without rendering it; CSS visibility can differ.
        if (totalWords_ >= 0 && !graph_.isEmpty()
            && std::all_of(graph_.cbegin(), graph_.cend(),
                           [](const ChapterLink &chapter) { return chapter.words >= 0; })) {
            DocumentWordCounts result;
            result.total = totalWords_;
            result.cached = true;
            for (const auto &chapter : graph_)
                result.chapters.append({ chapter.position, chapter.words });
            return result;
        }
        auto &map = pageMap();
        if (!map.allMeasured())
            return {};
        QVector<qint64> starts(map.items() + 1, 0);
        for (int item = 0; item < map.items(); ++item) {
            const auto words = map.itemWords(item);
            if (words < 0 || (current && !current()))
                return {};
            starts[item + 1] = starts[item] + words;
        }
        const auto chapters = this->chapters();
        QVector<qint64> prefixes;
        for (const auto &chapter : chapters) {
            engineActivity().checkpoint();
            if (current && !current())
                return {};
            const int item = spineItemIndex(chapter.position);
            if (item < 0 || item >= map.items())
                return {};
            textFraction(item, chapter.position, false, current);
            const auto prefix = source_->textWordPrefixes.value(chapter.position, -1);
            if (prefix < 0)
                return {};
            prefixes.append(starts[item] + qMin(prefix, map.itemWords(item)));
        }
        DocumentWordCounts result;
        result.total = starts.last();
        QVector<int> ordered;
        for (int i = 0; i < chapters.size(); ++i) {
            result.chapters.append({ chapters[i].position, -1 });
            ordered.append(i);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [&prefixes](int a, int b) { return prefixes[a] < prefixes[b]; });
        for (int rank = 0; rank < ordered.size(); ++rank) {
            const int index = ordered[rank];
            const auto end = rank + 1 < ordered.size() ? prefixes[ordered[rank + 1]] : result.total;
            result.chapters[index].words = qMax<qint64>(0, end - prefixes[index]);
        }
        if (current && !current())
            return {};
        totalWords_ = result.total;
        graph_ = chapters;
        externalChapters_ = false;
        for (int i = 0; i < graph_.size(); ++i)
            graph_[i].words = result.chapters[i].words;
        graphIndex_ = {};
        return result;
    }
    auto *document = engine_->getDocument();
    ChapterPositions positions;
    QVector<ChapterLink> chapters;
    if (contents_.isEmpty()) {
        chapters = this->chapters();
        positions = *contentsIndex_.positions;
    } else {
        chapters = tocChapters(&positions);
    }
    const auto publish = [&](DocumentWordCounts counts) {
        totalWords_ = counts.total;
        graph_ = chapters;
        externalChapters_ = false;
        for (int i = 0; i < graph_.size(); ++i) {
            graph_[i].words = counts.chapters[i].words;
        }
        return counts;
    };
    auto properties = document->getProps();
    const auto saved = UnicodeToUtf8(properties->getStringDef(property, ""));
    auto cached = decode(QByteArray(saved.c_str()), chapters);
    if (cached.total >= 0) {
        engineActivity().checkpoint();
        if (current && !current()) {
            return {};
        }
        return publish(std::move(cached));
    }

    QVector<Prefix> prefixes;
    DocumentWordCounts result;
    result.total = countPrefixes(document, positions, prefixes, current);
    if (result.total < 0)
        return {};
    for (const auto &chapter : chapters)
        result.chapters.append({ chapter.position, -1 });
    QVector<int> ordered;
    for (int i = 0; i < prefixes.size(); ++i) {
        if (prefixes[i].order >= 0) {
            ordered.append(i);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [&](int a, int b) { return prefixes[a].order < prefixes[b].order; });
    for (int i = 0; i < ordered.size(); ++i) {
        const auto start = prefixes[ordered[i]].words;
        const auto end = i + 1 < ordered.size() ? prefixes[ordered[i + 1]].words : result.total;
        result.chapters[ordered[i]].words = end - start;
    }
    const auto bytes = encode(result);
    engineActivity().checkpoint();
    if (current && !current()) {
        return {};
    }
    if (!bytes.isEmpty() && bytes.size() <= maximumCacheBytes) {
        properties->setString(property, lString8(bytes.constData()));
    }
    return publish(std::move(result));
}
} // namespace kobalt
