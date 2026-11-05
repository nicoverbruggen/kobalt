#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace kobalt {
struct WordCountChapter
{
    QString position;
    // -1 means the source destination was not reached in the counted text.
    qint64 words = -1;
};

struct DocumentWordCounts
{
    qint64 total = -1;
    QVector<WordCountChapter> chapters;
    bool cached = false;
};
} // namespace kobalt
