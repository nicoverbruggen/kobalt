#pragma once
#include "library_index.h"

namespace kobalt::library {
struct Query
{
    Sort sort = Sort::Opened;
    bool hideFinished = false;
    QString text;
};

// Search compares letters without case or accents.
QString searchKey(const QString &text);
// The visible books that match, in the order of the sort.
QVector<const Book *> arrange(const Index &index, const Query &query);

// The author a book is filed under: the first one listed.
QString firstAuthor(const Book &book);
} // namespace kobalt::library
