#include "support/diagnostics.h"
#include "zip_resource.h"
#include "resource_cache.h"
#include <QFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <vector>

#include <sys/stat.h>
#include <zlib.h>

namespace kobalt {
namespace {
quint16 u16(const QByteArray &b, int p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + p));
}

quint32 u32(const QByteArray &b, int p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + p));
}

QByteArray memberName(const QByteArray &directory, int offset)
{
    return QByteArray::fromRawData(directory.constData() + offset + 46,
                                   u16(directory, offset + 28));
}

struct FileIdentity
{
    dev_t device;
    ino_t inode;
    off_t size;
    timespec modified, changed;

    bool operator==(const FileIdentity &other) const
    {
        return device == other.device && inode == other.inode && size == other.size
                && modified.tv_sec == other.modified.tv_sec
                && modified.tv_nsec == other.modified.tv_nsec
                && changed.tv_sec == other.changed.tv_sec
                && changed.tv_nsec == other.changed.tv_nsec;
    }
};

bool identity(QIODevice &source, FileIdentity &result)
{
    auto *file = qobject_cast<QFile *>(&source);
    struct stat info{};
    if (!file || file->handle() < 0 || fstat(file->handle(), &info) != 0
        || !S_ISREG(info.st_mode)) {
        return false;
    }
#if defined(__APPLE__)
    result = { info.st_dev, info.st_ino, info.st_size, info.st_mtimespec, info.st_ctimespec };
#else
    result = { info.st_dev, info.st_ino, info.st_size, info.st_mtim, info.st_ctim };
#endif
    return true;
}
} // namespace

struct ZipResourceIndex::Data
{
    QByteArray directory;
    std::vector<int> offsets;
    qint64 length = 0;
    quint32 directoryOffset = 0;
    const QIODevice *source = nullptr;
    FileIdentity file{};
    bool hasIdentity = false;
};

ZipResourceIndex::ZipResourceIndex() = default;
ZipResourceIndex::~ZipResourceIndex() = default;

bool ZipResourceIndex::prepare(QIODevice &source)
{
    FileIdentity current{};
    const bool hasIdentity = identity(source, current);
    if (data_
        && ((hasIdentity && data_->hasIdentity && data_->file == current)
            || (!hasIdentity && !data_->hasIdentity && !qobject_cast<QFile *>(&source)
                && data_->source == &source && data_->length == source.size()))) {
        return true;
    }
    data_.reset();
    if (source.isSequential()) {
        return false;
    }
    auto &file = source;
    const auto length = file.size(), tailStart = qMax<qint64>(0, length - 65557);
    if (!file.seek(tailStart)) {
        return false;
    }
    const auto tail = file.read(length - tailStart);
    int end = -1;
    for (int p = tail.size() - 22; p >= 0; --p) {
        if (u32(tail, p) == 0x06054b50U && p + 22 + u16(tail, p + 20) == tail.size()) {
            end = p;
            break;
        }
    }
    if (end < 0 || u16(tail, end + 4) || u16(tail, end + 6)
        || u16(tail, end + 8) != u16(tail, end + 10)) {
        return false;
    }
    const auto directorySize = u32(tail, end + 12), directoryOffset = u32(tail, end + 16);
    if (directorySize > 4 * 1024 * 1024
        || quint64(directoryOffset) + directorySize != quint64(tailStart + end)
        || !file.seek(directoryOffset)) {
        return false;
    }
    const auto directory = file.read(directorySize);
    if (directory.size() != int(directorySize)) {
        return false;
    }
    const int count = u16(tail, end + 10);
    if (!count || count > 4096) {
        return false;
    }
    auto parsed = std::make_unique<Data>();
    parsed->offsets.reserve(count);
    int at = 0;
    for (int i = 0; i < count; ++i) {
        if (at + 46 > directory.size() || u32(directory, at) != 0x02014b50U) {
            return false;
        }
        const int nameLength = u16(directory, at + 28);
        const int next = at + 46 + nameLength + u16(directory, at + 30) + u16(directory, at + 32);
        if (next > directory.size()) {
            return false;
        }
        parsed->offsets.push_back(at);
        at = next;
    }
    FileIdentity after{};
    if (at != directory.size()
        || (hasIdentity && (!identity(source, after) || !(current == after)))) {
        return false;
    }
    // Only offsets are indexed. Names remain in the bounded directory buffer.
    // Stable ordering preserves the first match for standalone duplicate names.
    std::stable_sort(parsed->offsets.begin(), parsed->offsets.end(), [&](int a, int b) {
        return memberName(directory, a) < memberName(directory, b);
    });
    parsed->directory = directory;
    parsed->directoryOffset = directoryOffset;
    parsed->length = length;
    parsed->source = &source;
    parsed->file = current;
    parsed->hasIdentity = hasIdentity;
    data_ = std::move(parsed);
    return true;
}

struct ZipResource::Data
{
    std::unique_ptr<QFile> ownedFile;
    QIODevice *source = nullptr;
    qint64 packed = 0, size = 0, emitted = 0;
    quint32 expectedCrc = 0, crc = 0;
    bool deflated = false, initialized = false, valid = true, finished = false;
    z_stream stream{};
    std::array<char, ResourceCache::blockBytes> input;

    ~Data()
    {
        if (initialized)
            inflateEnd(&stream);
    }
};

ZipResource::ZipResource() = default;
ZipResource::~ZipResource() = default;

bool ZipResource::open(const QString &path, const QString &name, qint64 limit)
{
    ZipResourceIndex index;
    return open(path, name, index, limit);
}

bool ZipResource::open(QIODevice &source, const QString &name, qint64 limit)
{
    ZipResourceIndex index;
    return open(source, name, index, limit);
}

bool ZipResource::open(const QString &path, const QString &name, ZipResourceIndex &index,
                       qint64 limit)
{
    data_.reset();
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) {
        reportDescriptorExhaustion("book-open");
        return false;
    }
    if (!open(*file, name, index, limit))
        return false;
    data_->ownedFile = std::move(file);
    return true;
}

bool ZipResource::open(QIODevice &source, const QString &name, ZipResourceIndex &index,
                       qint64 limit)
{
    data_.reset();
    if (!index.prepare(source)) {
        return false;
    }
    const auto wanted = name.toUtf8();
    const auto &directory = index.data_->directory;
    const auto &offsets = index.data_->offsets;
    const auto found = std::lower_bound(
            offsets.begin(), offsets.end(), wanted,
            [&](int offset, const QByteArray &key) { return memberName(directory, offset) < key; });
    if (found == offsets.end() || memberName(directory, *found) != wanted) {
        return false;
    }
    const auto directoryOffset = index.data_->directoryOffset;
    const int at = *found;
    auto data = std::make_unique<Data>();
    data->source = &source;
    auto &file = source;
    const auto flags = u16(directory, at + 8), method = u16(directory, at + 10);
    const auto local = u32(directory, at + 42);
    data->size = u32(directory, at + 24);
    data->packed = u32(directory, at + 20);
    data->expectedCrc = u32(directory, at + 16);
    if ((flags & 0x41) || u16(directory, at + 34) || (method != 0 && method != 8) || limit < 0
        || data->size > limit || data->packed > limit || quint64(local) + 30 > directoryOffset
        || !file.seek(local)) {
        return false;
    }
    const auto header = file.read(30);
    if (header.size() != 30 || u32(header, 0) != 0x04034b50U || u16(header, 6) != flags
        || u16(header, 8) != method || u16(header, 26) != wanted.size()) {
        return false;
    }
    const qint64 start = qint64(local) + 30 + u16(header, 26) + u16(header, 28);
    if (start + data->packed > directoryOffset || file.read(wanted.size()) != wanted
        || !file.seek(start)) {
        return false;
    }
    data->deflated = method == 8;
    if (!data->deflated && data->packed != data->size) {
        return false;
    }
    if (data->deflated) {
        if (inflateInit2(&data->stream, -MAX_WBITS) != Z_OK) {
            return false;
        }
        data->initialized = true;
    }
    data_ = std::move(data);
    return true;
}

QByteArray ZipResource::next()
{
    if (!valid() || finished())
        return {};
    auto &d = *data_;
    QByteArray output;
    if (!d.deflated) {
        const auto count = qMin<qint64>(ResourceCache::blockBytes, d.packed);
        output = d.source->read(count);
        if (output.size() != count) {
            d.valid = false;
            return {};
        }
        d.packed -= count;
        d.finished = !d.packed;
    } else {
        output.resize(ResourceCache::blockBytes);
        d.stream.next_out = reinterpret_cast<Bytef *>(output.data());
        d.stream.avail_out = output.size();
        // Consume at most one new input block per call, even for damaged input
        // which produces no output. The caller can yield before the next call.
        qint64 inputRead = 0;
        while (d.stream.avail_out) {
            if (!d.stream.avail_in && d.packed) {
                if (inputRead)
                    break;
                const auto count = qMin<qint64>(d.input.size(), d.packed);
                if (d.source->read(d.input.data(), count) != count) {
                    d.valid = false;
                    return {};
                }
                d.packed -= count;
                inputRead += count;
                d.stream.next_in = reinterpret_cast<Bytef *>(d.input.data());
                d.stream.avail_in = count;
            }
            const auto beforeIn = d.stream.avail_in, beforeOut = d.stream.avail_out;
            const int result = inflate(&d.stream, Z_NO_FLUSH);
            if (result == Z_STREAM_END) {
                d.finished = true;
                break;
            }
            if (result != Z_OK
                || (beforeIn == d.stream.avail_in && beforeOut == d.stream.avail_out)) {
                d.valid = false;
                return {};
            }
        }
        output.resize(output.size() - d.stream.avail_out);
    }
    d.emitted += output.size();
    d.crc = crc32(d.crc, reinterpret_cast<const Bytef *>(output.constData()), output.size());
    if (d.emitted > d.size
        || (d.finished
            && (d.emitted != d.size || d.packed || d.stream.avail_in || d.crc != d.expectedCrc))) {
        d.valid = false;
        return {};
    }
    // Check the stream trailer before delivering the declared final byte. A
    // corrupt resource must not look complete to the receiving cache.
    if (d.emitted == d.size && !d.finished) {
        char extra;
        d.stream.next_out = reinterpret_cast<Bytef *>(&extra);
        d.stream.avail_out = 1;
        if (!d.stream.avail_in && d.packed) {
            const auto count = qMin<qint64>(d.input.size(), d.packed);
            if (d.source->read(d.input.data(), count) != count) {
                d.valid = false;
                return {};
            }
            d.packed -= count;
            d.stream.next_in = reinterpret_cast<Bytef *>(d.input.data());
            d.stream.avail_in = count;
        }
        if (inflate(&d.stream, Z_NO_FLUSH) != Z_STREAM_END || !d.stream.avail_out || d.packed
            || d.stream.avail_in || d.crc != d.expectedCrc) {
            d.valid = false;
            return {};
        }
        d.finished = true;
    }
    return output;
}

qint64 ZipResource::size() const
{
    return data_ ? data_->size : -1;
}

bool ZipResource::valid() const
{
    return data_ && data_->valid;
}

bool ZipResource::finished() const
{
    return data_ && data_->finished;
}
} // namespace kobalt
