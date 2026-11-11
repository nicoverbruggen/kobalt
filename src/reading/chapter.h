#pragma once
#include <QString>
#include <QVector>

namespace kobalt {
// Total entries in a table of contents, including headings and nested rows.
constexpr int maximumChapterEntries = 10000;

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
    // The nearest later entry with a greater page, even for unsorted lists.
    // A stack keeps shared-page TOCs linear instead of rescanning every suffix.
    QVector<int> later;
    for (int i = int(contents.size()) - 1; i >= 0; --i) {
        auto &chapter = contents[i];
        while (!later.isEmpty() && contents[later.last()].page <= chapter.page)
            later.removeLast();
        const int end = later.isEmpty() ? pages : contents[later.last()].page;
        later.append(i);
        chapter.words = 0;
        if (chapter.page < 0 || chapter.page >= pages)
            continue;
        chapter.words = qMax(1, end - chapter.page);
        any = true;
    }
    return any ? contents : QVector<ChapterLink>();
}
} // namespace kobalt
