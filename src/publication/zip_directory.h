#pragma once

#include <QIODevice>
#include <QtEndian>
#include <limits>

namespace kobalt::zip {
template <typename T>
T field(const QByteArray &bytes, int offset)
{
    return qFromLittleEndian<T>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

struct Directory
{
    QByteArray entries, endRecord;
    quint64 offset = 0;
    int count = 0;
    bool zip64 = false;
};

// The record ends at the locator, immediately before the classic end record.
inline quint64 zip64BodyLength(quint64 classicEnd, quint64 record)
{
    constexpr quint64 locatorSize = 20, recordPrefixSize = 12, minBodySize = 44;
    constexpr auto overhead = locatorSize + recordPrefixSize;
    if (classicEnd < overhead + minBodySize || record > classicEnd - overhead - minBodySize)
        return 0;
    return classicEnd - overhead - record;
}

template <typename T>
bool matchesClassicEndField(T classic, quint64 extended)
{
    return classic == std::numeric_limits<T>::max() || classic == extended;
}

// EPUB archives use one disk. ZIP64 may be present even when all values fit in ZIP32.
inline bool readDirectory(QIODevice &source, Directory &result)
{
    const qint64 length = source.size();
    const auto start = qMax<qint64>(0, length - 65557 - 20);
    if (source.isSequential() || length < 22 || !source.seek(start))
        return false;
    const auto tail = source.read(length - start);
    int end = tail.size() - 22;
    for (; end >= 0; --end)
        if (field<quint32>(tail, end) == 0x06054b50
            && end + 22 + field<quint16>(tail, end + 20) == tail.size())
            break;
    if (end < 0 || tail.size() != length - start)
        return false;
    quint64 count = field<quint16>(tail, end + 10);
    quint64 size = field<quint32>(tail, end + 12);
    quint64 offset = field<quint32>(tail, end + 16);
    quint64 boundary = start + end;
    const auto disk = field<quint16>(tail, end + 4), centralDisk = field<quint16>(tail, end + 6);
    const auto diskCount = field<quint16>(tail, end + 8);
    if (end >= 20 && field<quint32>(tail, end - 20) == 0x07064b50 && offset + size != boundary) {
        const auto record = field<quint64>(tail, end - 12);
        const auto bodyLength = zip64BodyLength(boundary, record);
        if (field<quint32>(tail, end - 16) || field<quint32>(tail, end - 4) != 1 || !bodyLength
            || !source.seek(record))
            return false;
        const auto header = source.read(56);
        if (header.size() != 56 || field<quint32>(header, 0) != 0x06064b50
            || field<quint64>(header, 4) != bodyLength || field<quint32>(header, 16)
            || field<quint32>(header, 20))
            return false;
        const auto entries = field<quint64>(header, 32);
        const auto bytes = field<quint64>(header, 40), location = field<quint64>(header, 48);
        if (!matchesClassicEndField(disk, 0) || !matchesClassicEndField(centralDisk, 0)
            || field<quint64>(header, 24) != entries
            || !matchesClassicEndField(diskCount, entries)
            || !matchesClassicEndField(quint16(count), entries)
            || !matchesClassicEndField(quint32(offset), location)
            || !matchesClassicEndField(quint32(size), bytes))
            return false;
        result.zip64 = true;
        boundary = record;
        count = entries;
        size = bytes;
        offset = location;
    } else if (disk || centralDisk || diskCount != count) {
        return false;
    }
    if (!count || count > 4096 || size > 4 * 1024 * 1024 || offset > boundary
        || size != boundary - offset || !source.seek(offset))
        return false;
    result.count = int(count);
    result.offset = offset;
    result.endRecord = tail.mid(end);
    result.entries = source.read(size);
    return quint64(result.entries.size()) == size;
}

struct Member
{
    quint64 size, packed, local;
};

// In extra field 1 only the values replaced by sentinel fields are stored, in this order.
inline bool member(const QByteArray &bytes, int at, Member &m)
{
    m = { field<quint32>(bytes, at + 24), field<quint32>(bytes, at + 20),
          field<quint32>(bytes, at + 42) };
    const auto disk = field<quint16>(bytes, at + 34);
    if (m.size != 0xffffffff && m.packed != 0xffffffff && m.local != 0xffffffff && disk != 0xffff)
        return disk == 0;
    int pos = at + 46 + field<quint16>(bytes, at + 28);
    const int end = pos + field<quint16>(bytes, at + 30);
    if (end > bytes.size())
        return false;
    while (pos + 4 <= end) {
        const auto tag = field<quint16>(bytes, pos);
        const int next = pos + 4 + field<quint16>(bytes, pos + 2);
        if (next > end)
            return false;
        pos += 4;
        if (tag == 1) {
            for (auto value : { &m.size, &m.packed, &m.local }) {
                if (*value != 0xffffffff)
                    continue;
                if (pos + 8 > next)
                    return false;
                *value = field<quint64>(bytes, pos);
                pos += 8;
            }
            return disk == 0 || (disk == 0xffff && pos + 4 <= next && !field<quint32>(bytes, pos));
        }
        pos = next;
    }
    return false;
}
} // namespace kobalt::zip
