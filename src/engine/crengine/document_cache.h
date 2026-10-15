#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <lvtinydom.h>
#include <sys/statvfs.h>

namespace kobalt {
// CREngine's document cache is process-wide, like its font manager. Kobalt
// enables it only while one book loads or saves, so that previews and other
// documents never write to user storage. A document restored from the cache
// keeps its own file open until it is destroyed.
class DocumentCacheScope {
public:
    static constexpr lvsize_t maxBytes = 128 * 1024 * 1024;
    // Below this much free space on the storage, books open and close
    // without the cache.
    static constexpr unsigned long long minimumFreeBytes = 256ULL * 1024 * 1024;
    explicit DocumentCacheScope(const QString &directory) {
        if (directory.isEmpty())
            return;
        struct statvfs storage {};
        const auto parent = QFile::encodeName(QFileInfo(directory).absolutePath());
        if (statvfs(parent.constData(), &storage) != 0 || (storage.f_flag & ST_RDONLY) ||
            (unsigned long long)storage.f_bavail * storage.f_frsize < minimumFreeBytes)
            return;
        const auto path = directory.toUtf8();
        enabled_ = ldomDocCache::init(Utf8ToUnicode(lString8(path.constData())), maxBytes);
    }
    ~DocumentCacheScope() {
        if (enabled_)
            ldomDocCache::close();
    }
    DocumentCacheScope(const DocumentCacheScope &) = delete;
    DocumentCacheScope &operator=(const DocumentCacheScope &) = delete;
    bool enabled() const {
        return enabled_;
    }

private:
    bool enabled_ = false;
};

// CREngine names a cache file after the document name, which it shortens to
// 24 characters, and a 32-bit checksum. The name carries the publication
// fingerprint; the checksum carries everything that shapes the parsed book.
inline QString documentCacheName(const QByteArray &fingerprint) {
    return QString::fromLatin1(fingerprint);
}
inline lUInt32 documentCacheChecksum(const QByteArray &policy, int domVersion,
                                     const QByteArray &kobaltVersion = QByteArray(KOBALT_VERSION)) {
    // Bump when the cached book's contents change for a reason the policy
    // does not cover.
    constexpr int version = 4;
    const auto digest = QCryptographicHash::hash(policy + '|' + kobaltVersion + '|' + QByteArray::number(version) +
                                                     '|' + QByteArray::number(domVersion),
                                                 QCryptographicHash::Sha256);
    return lUInt32(uchar(digest[0])) << 24 | lUInt32(uchar(digest[1])) << 16 | lUInt32(uchar(digest[2])) << 8 |
           lUInt32(uchar(digest[3]));
}
}
