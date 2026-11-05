#include "column_view.h"
#include "engine_document.h"

#include "engine_activity.h"
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

lString32 cre(const QString &value)
{
    return Utf8ToUnicode(value.toUtf8().constData());
}

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
    QJsonArray entries;
    int remaining = maximumCacheBytes - 64;
    for (const auto &chapter : counts.chapters) {
        // JSON escaping needs at most six bytes per UTF-16 code unit.
        // Reserve each entry before constructing the temporary JSON arrays.
        if (remaining < 32 || chapter.position.size() > (remaining - 32) / 6) {
            return {};
        }
        remaining -= chapter.position.size() * 6 + 32;
        entries.append(qt::array({ chapter.position, QString::number(chapter.words) }));
    }
    return QJsonDocument(qt::object({ { "total", QString::number(counts.total) },
                                      { "chapters", entries } }))
            .toJson(QJsonDocument::Compact);
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
} // namespace

DocumentWordCounts EngineDocument::wordCounts(std::function<bool()> current)
{
    if (!engine_ || (current && !current())) {
        return {};
    }
    auto *document = engine_->getDocument();
    const auto chapters = tocChapters();
    const auto publish = [&](DocumentWordCounts counts) {
        totalWords_ = counts.total;
        graph_ = chapters;
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

    QHash<ldomNode *, QVector<Event>> events;
    QVector<Prefix> prefixes(chapters.size());
    DocumentWordCounts result;
    for (int i = 0; i < chapters.size(); ++i) {
        result.chapters.append({ chapters[i].position, -1 });
        auto pointer = document->createXPointer(cre(chapters[i].position));
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
    while (!stack.empty()) {
        if (++nodes % 128 == 0 && !checkpoint()) {
            return {};
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
                            return {};
                        }
                        capture(event);
                    }
                }
                if (!scan(text.length())) {
                    return {};
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
                return {};
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
        return {};
    }
    result.total = counter.words();
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
    if (!checkpoint()) {
        return {};
    }
    if (!bytes.isEmpty() && bytes.size() <= maximumCacheBytes) {
        properties->setString(property, lString8(bytes.constData()));
    }
    return publish(std::move(result));
}
} // namespace kobalt
