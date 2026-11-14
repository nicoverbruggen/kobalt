#pragma once
#include "reading/chapter.h"
#include <QString>

namespace kobalt {
class Engine;

// Optional host position mapping. Calls run on the engine worker. No GUI
// object, native handle or document pointer crosses this value boundary.
struct PositionCodec
{
    QString (*decode)(const Engine &, const QString &, QString *) = nullptr;
    QString (*encode)(const Engine &, QString *) = nullptr;
    QString (*chapterStart)(const QString &) = nullptr;

    explicit operator bool() const { return decode && encode && chapterStart; }
};

struct ResolvedChapters
{
    QVector<ChapterLink> contents, graph;
};

ResolvedChapters resolveChapterTargets(const Engine &engine, const PositionCodec &codec,
                                       const QVector<ChapterTarget> &contents,
                                       const QVector<ChapterTarget> &graph);
} // namespace kobalt
