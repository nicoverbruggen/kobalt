#pragma once
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

class QObject;

namespace kobalt::storage {
// Writes a finished preview as a PNG named after `key` in `directory`, then
// removes the files written longest ago until the directory holds at most
// `limit` bytes. Writes storage: run it on a worker thread.
bool writePreview(const QString &directory, const QByteArray &key, const QImage &image,
                  qint64 limit);
// The preview stored for `key`, or a null image when there is none or the
// file is too large or does not decode.
QImage readPreview(const QString &directory, const QByteArray &key);

// Finished preview tiles, such as the All presets grid's, kept in memory and
// in `directory`, so a later gallery, in this run or a later one, shows them
// without the engine. A disposable cache: a key names everything that decides
// a tile's pixels, and file names hash it with the store's format version.
// Grey tiles are held with one byte per pixel, in memory and on storage.
// The API is for the GUI thread. Storage work runs in order on one worker
// thread, so the GUI thread never reads or writes storage.
class PreviewStore
{
public:
    static constexpr qint64 memoryLimit = 4 * 1024 * 1024;
    static constexpr qint64 storageLimit = 4 * 1024 * 1024;

    explicit PreviewStore(QString directory);

    // True with the tile in `image` when memory holds `key`.
    bool find(const QByteArray &key, QImage *image);
    // Reads the keys that memory lacks from storage and keeps the tiles it
    // finds in memory. Then calls `done` on the GUI thread, unless `context`
    // was destroyed first.
    void load(const QVector<QByteArray> &keys, QObject *context, std::function<void()> done);
    // Keeps the tile in memory and writes it to storage in the background.
    void insert(const QByteArray &key, const QImage &image);

    qint64 memoryBytes() const;

private:
    struct Entry
    {
        QByteArray key;
        QImage image;
        qint64 bytes;
    };

    struct Memory
    {
        // Most recently used first.
        QList<Entry> entries;
        void keep(const QByteArray &key, const QImage &image);
    };

    QString directory;
    std::shared_ptr<Memory> memory;
};
} // namespace kobalt::storage
