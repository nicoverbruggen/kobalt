#pragma once
#include <QByteArray>
#include <QString>
#include <QIODevice>
#include <memory>

namespace kobalt {
// One archive directory, bounded to 4 MiB and 4096 entries. It retains no
// resource bytes or open files and is released with its publication owner.
class ZipResourceIndex
{
public:
    ZipResourceIndex();
    ~ZipResourceIndex();
    ZipResourceIndex(const ZipResourceIndex &) = delete;
    ZipResourceIndex &operator=(const ZipResourceIndex &) = delete;

private:
    friend class ZipResource;
    struct Data;
    std::unique_ptr<Data> data_;
    bool prepare(QIODevice &source);
};

// Sequential stored/deflated ZIP input. The caller must already hold Nickel's
// accepted publication. This reads bytes only; it cannot decode protected data.
class ZipResource
{
public:
    ZipResource();
    ~ZipResource();
    bool open(const QString &path, const QString &name, qint64 limit = 32 * 1024 * 1024);
    // The caller keeps source alive until the stream is destroyed.
    bool open(QIODevice &source, const QString &name, qint64 limit = 32 * 1024 * 1024);
    // Reuse directory metadata for one publication. File inputs are checked by
    // identity, size and nanosecond modification/change times on every open.
    bool open(const QString &path, const QString &name, ZipResourceIndex &index,
              qint64 limit = 32 * 1024 * 1024);
    // Non-file sources must keep the same bytes while the index is in use.
    // Each stream still validates its local header, bounds and payload CRC.
    bool open(QIODevice &source, const QString &name, ZipResourceIndex &index,
              qint64 limit = 32 * 1024 * 1024);
    QByteArray next();
    qint64 size() const;
    bool valid() const;
    bool finished() const;

private:
    struct Data;
    std::unique_ptr<Data> data_;
};
} // namespace kobalt
