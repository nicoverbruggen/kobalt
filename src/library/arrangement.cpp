#include "arrangement.h"
#include <algorithm>

namespace kobalt::library {
namespace {
// Compares without case or accents, and reads runs of digits as numbers so
// "Book 9" comes before "Book 10". QCollator would depend on whether Qt was
// built with ICU, which differs between hosts.
int naturalCompare(const QString &first, const QString &second)
{
    const auto a = searchKey(first), b = searchKey(second);
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].isDigit() && b[j].isDigit()) {
            int endA = i, endB = j;
            while (endA < a.size() && a[endA].isDigit())
                ++endA;
            while (endB < b.size() && b[endB].isDigit())
                ++endB;
            auto digitsA = a.mid(i, endA - i), digitsB = b.mid(j, endB - j);
            while (digitsA.size() > 1 && digitsA.startsWith('0'))
                digitsA.remove(0, 1);
            while (digitsB.size() > 1 && digitsB.startsWith('0'))
                digitsB.remove(0, 1);
            if (digitsA.size() != digitsB.size())
                return digitsA.size() < digitsB.size() ? -1 : 1;
            if (const int compared = digitsA.compare(digitsB))
                return compared;
            i = endA;
            j = endB;
            continue;
        }
        if (a[i] != b[j])
            return a[i] < b[j] ? -1 : 1;
        ++i;
        ++j;
    }
    return int(a.size() - i) - int(b.size() - j);
}

// "Mary Shelley" files under Shelley. A name already written "Shelley, Mary"
// files under its first part.
QString surnameKey(const QString &author)
{
    if (author.contains(','))
        return author;
    const auto parts = author.split(' ', Qt::SkipEmptyParts);
    return parts.isEmpty() ? QString()
                           : parts.last() + ' ' + parts.mid(0, parts.size() - 1).join(' ');
}
} // namespace

QString searchKey(const QString &text)
{
    const auto decomposed = text.normalized(QString::NormalizationForm_KD);
    QString result;
    result.reserve(decomposed.size());
    for (const auto character : decomposed)
        if (character.category() != QChar::Mark_NonSpacing)
            result.append(character.toCaseFolded());
    return result;
}

QString firstAuthor(const Book &book)
{
    // Several creators are stored joined by ", ". A single "Surname, Given"
    // creator cannot be told apart, so only the first comma part is used when
    // it contains a space.
    const auto first = book.author.section(QStringLiteral(", "), 0, 0).trimmed();
    return first.contains(' ') ? first : book.author.trimmed();
}

QVector<const Book *> arrange(const Index &index, const Query &query)
{
    QVector<const Book *> result;
    const auto needle = searchKey(query.text.trimmed());
    for (const auto &book : index.books) {
        if (book.hidden || (query.hideFinished && book.status() == Status::Finished))
            continue;
        if (!needle.isEmpty() && !searchKey(book.displayTitle()).contains(needle)
            && !searchKey(book.author).contains(needle) && !searchKey(book.series).contains(needle))
            continue;
        result.append(&book);
    }
    const auto byTitle = [](const Book *a, const Book *b) {
        const int compared = naturalCompare(a->displayTitle(), b->displayTitle());
        return compared ? compared < 0 : a->key < b->key;
    };
    switch (query.sort) {
    case Sort::Opened:
        std::sort(result.begin(), result.end(), [&](const Book *a, const Book *b) {
            if (a->opened != b->opened)
                return a->opened > b->opened;
            if (a->added != b->added)
                return a->added > b->added;
            return byTitle(a, b);
        });
        break;
    case Sort::Added:
        std::sort(result.begin(), result.end(), [&](const Book *a, const Book *b) {
            return a->added != b->added ? a->added > b->added : byTitle(a, b);
        });
        break;
    case Sort::Title:
        std::sort(result.begin(), result.end(), byTitle);
        break;
    case Sort::Author:
        std::sort(result.begin(), result.end(), [&](const Book *a, const Book *b) {
            const auto first = firstAuthor(*a), second = firstAuthor(*b);
            // Books without an author come last.
            if (first.isEmpty() != second.isEmpty())
                return second.isEmpty();
            const int compared = naturalCompare(surnameKey(first), surnameKey(second));
            return compared ? compared < 0 : byTitle(a, b);
        });
        break;
    case Sort::Series:
        // Series in name order, each in reading order; books outside a series last.
        std::sort(result.begin(), result.end(), [&](const Book *a, const Book *b) {
            if (a->series.isEmpty() != b->series.isEmpty())
                return b->series.isEmpty();
            if (const int compared = naturalCompare(a->series, b->series))
                return compared < 0;
            if (a->seriesIndex != b->seriesIndex)
                return a->seriesIndex < b->seriesIndex;
            return byTitle(a, b);
        });
        break;
    }
    return result;
}
} // namespace kobalt::library
