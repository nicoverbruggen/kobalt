#pragma once
#include "engine/crengine/column_view.h"
#include "engine/crengine/engine_document.h"
#include <algorithm>

namespace kobalt {
// Independent copies of the pre-index algorithms for correctness and timing.
// This probe is linked only into the focused test and opt-in benchmark.
struct ReaderSourceIndexProbe {
    using Ranges = QVector<QPair<QString, QString>>;
    static lString32 cre(const QString &text) { return Utf8ToUnicode(text.toUtf8().constData()); }
    static void clear(EngineDocument &model) { model.sourceIndex_.reset(); }
    static QPair<int, qint64> usage(const EngineDocument &model) { return model.sourceIndexUsage(); }
    static bool cached(const EngineDocument &model) { return bool(model.sourceIndex_); }
    static bool positionLookupPreservesNames(const EngineDocument &model, bool normalized) {
        auto *doc = model.engine_->getDocument();
        const char *name = normalized ? "unknown_position_v2" : "unknown_position_v1";
        if (doc->findElementNameIndex(name))
            return false;
        const auto path = cre('/' + QString::fromLatin1(name) + "[1]");
        const auto version = doc->getDOMVersionRequested();
        doc->setDOMVersionRequested(normalized ? DOM_VERSION_WITH_NORMALIZED_XPOINTERS
                                              : DOM_VERSION_WITH_NORMALIZED_XPOINTERS - 1);
        const auto pointer = doc->createXPointer(doc->getRootNode(), path);
        doc->setDOMVersionRequested(version);
        return pointer.isNull() && !doc->findElementNameIndex(name);
    }
    static QVector<int> visible(const EngineDocument &model, const Ranges &ranges) {
        QVector<int> result;
        if (ranges.isEmpty() || ranges.size() > 2001 || !model.engine_ || model.pages() <= 0)
            return result;
        auto *doc = model.engine_->getDocument();
        const int current = model.page();
        const ldomXPointerEx first(model.engine_->getPageBookmark(current));
        const ldomXPointerEx last = current + 1 < model.pages() ? ldomXPointerEx(model.engine_->getPageBookmark(current + 1))
                                                              : ldomXRange(doc->getRootNode(), true).getEnd();
        if (first.isNull() || last.isNull())
            return result;
        for (int i = 0; i < ranges.size(); ++i) {
            const auto &range = ranges[i];
            if (range.first.isEmpty() || range.second.isEmpty() || range.first.size() > 4096 || range.second.size() > 4096)
                continue;
            ldomXPointerEx start(doc->createXPointer(cre(range.first))), end(doc->createXPointer(cre(range.second)));
            if (start.isNull() || end.isNull())
                continue;
            if (start.compare(end) > 0)
                std::swap(start, end);
            if (start.compare(end) < 0 && start.compare(last) < 0 && first.compare(end) < 0)
                result.append(i);
        }
        return result;
    }
    static QVector<int> overlaps(const EngineDocument &model, const QString &start, const QString &end, const Ranges &ranges) {
        QVector<int> result;
        if (!model.engine_)
            return result;
        auto resolve = [&model](const QString &anchor) {
            if (anchor.isEmpty() || anchor.size() > 4096)
                return ldomXPointerEx();
            ldomXPointerEx pointer(model.engine_->getDocument()->createXPointer(cre(anchor)));
            if (pointer.isNull() || !pointer.isText() || pointer.getOffset() < 0 ||
                pointer.getOffset() > pointer.getNode()->getText().length() || !pointer.isVisible())
                return ldomXPointerEx();
            return pointer;
        };
        const auto first = resolve(start), last = resolve(end);
        if (first.isNull() || last.isNull() || first.compare(last) >= 0)
            return result;
        for (int i = 0; i < ranges.size(); ++i) {
            const auto a = resolve(ranges[i].first), b = resolve(ranges[i].second);
            if (!a.isNull() && !b.isNull() && a.compare(b) < 0 && a.compare(last) < 0 && first.compare(b) < 0)
                result.append(i);
        }
        return result;
    }
    static QString shifted(const EngineDocument &model, const QString &anchor, int amount) {
        auto point = model.engine_->getDocument()->createXPointer(cre(anchor));
        if (point.isNull())
            return {};
        point.setOffset(point.getOffset() + amount);
        return QString::fromUtf8(UnicodeToUtf8(point.toString()).c_str());
    }
    static QString textPoint(const EngineDocument &model, const QString &needle, int offset) {
        std::function<ldomNode *(ldomNode *)> find = [&](ldomNode *node) -> ldomNode * {
            if (node->isText() && QString::fromUtf8(UnicodeToUtf8(node->getText()).c_str()).contains(needle))
                return node;
            for (int i = 0; i < node->getChildCount(); ++i)
                if (auto *found = find(node->getChildNode(i)))
                    return found;
            return nullptr;
        };
        auto *node = find(model.engine_->getDocument()->getRootNode());
        return node ? QString::fromUtf8(UnicodeToUtf8(ldomXPointer(node, offset).toString()).c_str()) : QString();
    }
    static QVector<int> pages(const EngineDocument &model, const QStringList &anchors, bool endpoints, QVector<int> *order = nullptr) {
        QVector<int> result;
        QVector<ldomXPointerEx> pointers;
        if (order) {
            pointers.resize(anchors.size());
            order->fill(-1, anchors.size());
        }
        if (!model.engine_) {
            result.fill(-1, anchors.size());
            return result;
        }
        for (int i = 0; i < anchors.size(); ++i) {
            ldomXPointerEx pointer(anchors[i].isEmpty() ? ldomXPointer() : model.engine_->getDocument()->createXPointer(cre(anchors[i])));
            if (order)
                pointers[i] = pointer;
            if (endpoints && i % 2 == 1 && !pointer.isNull())
                pointer.prevVisibleChar(false);
            result.append(pointer.isNull() ? -1 : model.engine_->getBookmarkPage(pointer));
        }
        if (order) {
            QVector<int> indices;
            for (int i = 0; i < pointers.size(); ++i)
                if (!pointers[i].isNull())
                    indices.append(i);
            std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) { return pointers[a].compare(pointers[b]) < 0; });
            for (int i = 0; i < indices.size(); ++i)
                (*order)[indices[i]] = i;
        }
        return result;
    }
    static QString childOffset(const EngineDocument &model, const QString &anchor) {
        auto point = model.engine_->getDocument()->createXPointer(cre(anchor));
        if (point.isNull())
            return {};
        auto *parent = point.getNode()->getParentNode();
        if (!parent || !parent->getChildCount())
            return {};
        ldomXPointer child(parent, qMin(1, parent->getChildCount()));
        return QString::fromUtf8(UnicodeToUtf8(child.toString()).c_str());
    }
};
}
