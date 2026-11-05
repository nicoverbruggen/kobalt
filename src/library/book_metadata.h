#pragma once
#include <QImage>
#include <QIODevice>
#include <QString>

namespace kobalt::library {
// What the library shows before a book is opened. Reading the package
// document and one cover image is far cheaper than opening the book in the
// engine, so a scan can read every new book.
struct BookMetadata
{
    enum class Result { Ok, Protected, Unreadable };
    Result result = Result::Unreadable;
    QString title, author;
    // The series from EPUB 3 collection metadata or Calibre's tags, and the
    // book's place in it; 0 when the book does not say.
    QString series;
    double seriesIndex = 0;
    // Scaled to fit coverSize; null when the book has no usable cover.
    QImage cover;
};

// The largest stored cover. The shelf view on a 300 ppi screen draws covers
// about this tall, so larger thumbnails would only cost memory.
inline constexpr QSize coverSize{ 240, 360 };
// Raise this when readBookMetadata reads something new. Books read by an
// older version are read again by the next scan.
inline constexpr int metadataVersion = 1;
BookMetadata readBookMetadata(QIODevice &source);
} // namespace kobalt::library
