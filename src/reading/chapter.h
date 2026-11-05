#pragma once
#include <QString>
#include <QVector>

namespace kobalt {
// The platform supplies source locations; the engine resolves them without
// changing the reading position. Duplicate destinations remain separate rows.
struct ChapterTarget
{
    QString title, location;
    int depth = 0;
    qint64 words = 0;
};

struct ChapterLink
{
    QString title, position;
    int page = -1, depth = 0;
    qint64 words = 0;
};

struct ChapterSelection
{
    int current = -1, next = -1, page = 0, pages = 0;
};

// Chapter lengths in pages when the shared count is unavailable. A chapter
// runs to the next one that starts later. Shared-page chapters count one page. A chapter without a
// page keeps its place with no length, so selections still index the list.
inline QVector<ChapterLink> chapterPageLengths(QVector<ChapterLink> contents, int pages)
{
    bool any = false;
    for (int i = 0; i < contents.size(); ++i) {
        auto &chapter = contents[i];
        chapter.words = 0;
        if (chapter.page < 0 || chapter.page >= pages)
            continue;
        int end = pages;
        for (int j = i + 1; j < contents.size(); ++j)
            if (contents[j].page > chapter.page) {
                end = contents[j].page;
                break;
            }
        chapter.words = qMax(1, end - chapter.page);
        any = true;
    }
    return any ? contents : QVector<ChapterLink>();
}
} // namespace kobalt
