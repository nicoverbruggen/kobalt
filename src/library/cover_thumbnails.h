#pragma once
#include <QImage>
#include <QObject>
#include <QPair>
#include <QSize>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

namespace kobalt::library {
// Small covers read from book files, for lists of books that are not open.
// The interface asks only for what it shows. Books are read one at a time on
// a worker thread, and the thumbnails stay in a disposable cache, so the same
// page shows them at once the next time. The interface thread never reads a
// book file or the cache.
class CoverThumbnails final : public QObject
{
public:
    // `key` is the caller's name for the book. A null cover means the file has
    // no cover that Kobalt can read, such as a protected store book, or that
    // the file is gone, which `missing` tells apart.
    using Ready = std::function<void(const QString &key, const QImage &cover, bool missing)>;
    CoverThumbnails(QString cacheDirectory, QObject *parent = nullptr);
    // Stops reading books for earlier requests. It never waits: a book being
    // read finishes on the worker, and its cover is dropped.
    ~CoverThumbnails() override;
    // Replaces earlier requests. `books` pairs each key with its file.
    // `ready` runs on this object's thread once for each key after checking the current file.
    void request(const QVector<QPair<QString, QString>> &books, QSize size, Ready ready);

private:
    QString directory;
    std::shared_ptr<std::atomic<quint64>> generation;
};
} // namespace kobalt::library
