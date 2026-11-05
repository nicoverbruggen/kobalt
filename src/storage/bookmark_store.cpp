#include "bookmark_store.h"
#include "support/main_thread.h"
#include "reader_storage.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <sys/stat.h>

namespace kobalt {
namespace {
constexpr int formatVersion = 1;
constexpr int maxFolders = 10000;

QJsonObject readObject(const QString &path, QString *error)
{
    const auto read = storage::readJsonFile(path, BookmarkStore::maxFileBytes);
    const char *problem = nullptr;
    switch (read.status) {
    case storage::ReadStatus::Ok:
    case storage::ReadStatus::Missing:
        break;
    case storage::ReadStatus::Unreadable:
        problem = "unreadable";
        break;
    case storage::ReadStatus::TooLarge:
        problem = "too-large";
        break;
    case storage::ReadStatus::Invalid:
    case storage::ReadStatus::Unsupported:
    case storage::ReadStatus::DatabaseError:
        problem = "invalid";
        break;
    }
    if (problem && error)
        *error = QString::fromLatin1(problem);
    return read.object;
}

bool writeObject(const QString &path, const QByteArray &bytes)
{
    mainthread::BlockingOperation blockingOperation("bookmark-write");
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

// This derived index is disposable. The book folders remain authoritative.
// Its inventory checks metadata file identities and timestamps without reading
// their contents. Restoring an existing folder must also invalidate the index.
constexpr qint64 maxIndexBytes = 4 * 1024 * 1024;

QString indexPath(const QString &root)
{
    return root + "/.index/paths.json";
}

QStringList annotationFolders(const QString &root)
{
    return QDir(root).entryList(QStringList() << "file-*", QDir::Dirs | QDir::NoDotAndDotDot,
                                QDir::Name);
}

QString inventoryDigest(const QString &root, const QStringList &folders)
{
    QCryptographicHash digest(QCryptographicHash::Sha256);
    for (int i = 0; i < folders.size() && i < maxFolders; ++i) {
        digest.addData(folders[i].toUtf8());
        digest.addData(QByteArray(1, '\0'));
        struct stat info{};
        if (stat(QFile::encodeName(root + '/' + folders[i] + "/book.json").constData(), &info) != 0
            || !S_ISREG(info.st_mode)) {
            digest.addData(QByteArray("missing"));
            continue;
        }
#if defined(__APPLE__)
        const auto modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
        const auto modified = info.st_mtim, changed = info.st_ctim;
#endif
        for (const qint64 value : { qint64(info.st_dev), qint64(info.st_ino), qint64(info.st_size),
                                    qint64(modified.tv_sec), qint64(modified.tv_nsec),
                                    qint64(changed.tv_sec), qint64(changed.tv_nsec) }) {
            digest.addData(QByteArray::number(value));
            digest.addData(QByteArray(1, ':'));
        }
    }
    return QString::fromLatin1(digest.result().toHex());
}

QString identityHash(const QString &identity)
{
    return QString::fromLatin1(
            QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

struct PathIndex
{
    QJsonObject paths;
    bool valid = false;
};

PathIndex readIndex(const QString &root, const QStringList &folders)
{
    const auto read = storage::readJsonFile(indexPath(root), maxIndexBytes);
    const auto object = read.object;
    if (read.status != storage::ReadStatus::Ok || !QFileInfo(root + "/.index").isWritable()
        || object.value("version").toInt() != 1
        || object.value("inventory").toString() != inventoryDigest(root, folders)
        || !object.value("paths").isObject()) {
        return {};
    }
    const auto paths = object.value("paths").toObject();
    int count = 0;
    for (auto it = paths.begin(); it != paths.end(); ++it) {
        if (it.key().size() != 64 || !it.value().isArray()) {
            return {};
        }
        for (const auto &entry : it.value().toArray()) {
            const auto folder = entry.toString();
            if (!entry.isString() || !folder.startsWith("file-") || folder.size() > 255
                || folder.contains('/') || folder.contains('\\') || folder.contains(QChar(0))
                || ++count > maxFolders) {
                return {};
            }
        }
    }
    return { paths, true };
}

bool writeIndex(const QString &root, const QJsonObject &paths, const QStringList &folders)
{
    mainthread::BlockingOperation blockingOperation("bookmark-index-write");
    if (!QDir().mkpath(root + "/.index")) {
        return false;
    }
    QJsonObject object;
    object["version"] = 1;
    object["inventory"] = inventoryDigest(root, folders);
    object["paths"] = paths;
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return bytes.size() <= maxIndexBytes && writeObject(indexPath(root), bytes);
}

PathIndex rebuildIndex(const QString &root, const QStringList &folders, int &metadataReads)
{
    QMap<QString, QJsonArray> paths;
    for (int i = 0; i < folders.size() && i < maxFolders; ++i) {
        ++metadataReads;
        const auto identity = readObject(root + '/' + folders[i] + "/book.json", nullptr)
                                      .value("identity")
                                      .toString();
        if (identity.startsWith("file://")) {
            paths[identityHash(identity)].append(folders[i]);
        }
    }
    QJsonObject object;
    for (auto it = paths.begin(); it != paths.end(); ++it) {
        object[it.key()] = it.value();
    }
    // A read-only annotation directory can still use the rebuilt index for
    // this load. A failed cache write must never make bookmarks unavailable.
    if (QFileInfo(root).isDir()) {
        writeIndex(root, object, folders);
    }
    return { object, true };
}

void updateIndex(const QString &root, PathIndex index, const QString &identity,
                 const QString &folder)
{
    if (!index.valid) {
        return;
    }
    // The same fingerprint may now be used at a different path. Remove its
    // old association before publishing the last path recorded by book.json.
    for (auto it = index.paths.begin(); it != index.paths.end();) {
        QJsonArray retained;
        for (const auto &value : it.value().toArray()) {
            if (value.toString() != folder) {
                retained.append(value);
            }
        }
        if (retained.isEmpty()) {
            it = index.paths.erase(it);
        } else {
            it.value() = retained;
            ++it;
        }
    }
    QStringList folders;
    for (const auto &value : index.paths.value(identityHash(identity)).toArray()) {
        folders.append(value.toString());
    }
    folders.append(folder);
    folders.sort();
    index.paths[identityHash(identity)] = QJsonArray::fromStringList(folders);
    if (!writeIndex(root, index.paths, annotationFolders(root))) {
        QFile::remove(indexPath(root));
    }
}

} // namespace

QJsonObject bookmarkJson(const Bookmark &bookmark)
{
    QJsonObject object;
    object["position"] = bookmark.position;
    object["chapter"] = bookmark.chapter;
    object["excerpt"] = bookmark.excerpt;
    object["created"] = bookmark.created.toUTC().toString(Qt::ISODate);
    if (!bookmark.name.isEmpty())
        object["name"] = bookmark.name;
    if (!bookmark.icon.isEmpty())
        object["icon"] = bookmark.icon;
    return object;
}

bool bookmarkFromJson(const QJsonObject &object, Bookmark *bookmark)
{
    const auto position = object.value("position").toString();
    if (position.isEmpty() || position.size() > 4096)
        return false;
    bookmark->position = position;
    bookmark->chapter = object.value("chapter").toString().left(512);
    bookmark->excerpt = object.value("excerpt").toString().left(512);
    bookmark->created =
            QDateTime::fromString(object.value("created").toString(), Qt::ISODate).toUTC();
    for (const auto &key : { "name", "icon" })
        if (object.contains(key) && !object.value(key).isString())
            return false;
    bookmark->name = object.value("name").toString();
    bookmark->icon = object.value("icon").toString();
    if (bookmark->name.size() > 128 || bookmark->icon.size() > 64)
        return false;
    return true;
}

QString BookmarkStore::bookKey(const QString &identity, const QByteArray &fingerprint)
{
    static const QRegularExpression legacyUuid(
            "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    static const QRegularExpression digest("^[0-9a-f]{64}$");
    // Keep existing UUID folders. Other server IDs get a case-sensitive hash
    // so their length and filename characters cannot affect annotation paths.
    if (legacyUuid.match(identity).hasMatch())
        return "kobo-" + identity.toLower();
    if (identity.startsWith("file://") && digest.match(QString::fromLatin1(fingerprint)).hasMatch())
        return "file-" + QString::fromLatin1(fingerprint);
    if (!identity.isEmpty() && identity != "." && identity != ".." && !identity.contains('/')
        && !identity.contains('\\') && !identity.contains(QChar(0))) {
        return "kobo-id-"
                + QString::fromLatin1(
                        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
                                .toHex());
    }
    return {};
}

BookmarkStore::BookmarkStore(QString root, QString identity, QByteArray fingerprint, QString title,
                             QString database)
    : root_(std::move(root)),
      identity_(std::move(identity)),
      title_(std::move(title)),
      database_(std::move(database)),
      key_(bookKey(identity_, fingerprint)),
      fingerprint_(std::move(fingerprint))
{
}

QString BookmarkStore::folder() const
{
    return key_.isEmpty() ? QString() : root_ + '/' + key_;
}

bool BookmarkStore::readFolder(const QString &folder, QString *error)
{
    QString problem;
    const auto object = readObject(folder + "/bookmarks.json", &problem);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return false;
    }
    // An explicit list with the wrong type is damaged data, not an empty list.
    // Preserve its bytes by refusing edits after this failed load.
    const auto list = object.value("bookmarks");
    if (!list.isUndefined() && !list.isArray()) {
        if (error)
            *error = "invalid";
        return false;
    }
    const auto rows = list.toArray();
    if (rows.size() > maxBookmarks) {
        if (error)
            *error = "invalid";
        return false;
    }
    QVector<Bookmark> loaded;
    int pins = 0;
    for (const auto &value : rows) {
        Bookmark bookmark;
        if (!bookmarkFromJson(value.toObject(), &bookmark)
            || (bookmark.pinned() && ++pins > maxPins)) {
            if (error)
                *error = "invalid";
            return false;
        }
        loaded.append(bookmark);
    }
    bookmarks_ = loaded;
    return true;
}

bool BookmarkStore::load(QString *error)
{
    bookmarks_.clear();
    adoptedFrom_.clear();
    adoptionMetadataReads_ = 0;
    loadFailed_ = true;
    if (key_.isEmpty()) {
        if (error)
            *error = "no-stable-key";
        return false;
    }
    if (sqlite_) {
        loadFailed_ = !AnnotationDatabase::load(
                { root_, identity_, title_, key_, fingerprint_, database_ }, &bookmarks_,
                &databaseState_, error);
        if (!loadFailed_) {
            persistedBookmarks_ = bookmarks_;
            adoptedFrom_ = databaseState_.adoptedFrom;
        }
        return !loadFailed_;
    }
    if (QFile::exists(folder() + "/bookmarks.json")) {
        loadFailed_ = !readFolder(folder(), error);
        return !loadFailed_;
    }
    // A sideloaded file that was edited has a new fingerprint. Keep the
    // bookmarks of the folder that last saw this path.
    if (!identity_.startsWith("file://")) {
        loadFailed_ = false;
        return true;
    }
    const auto folders = annotationFolders(root_);
    auto index = readIndex(root_, folders);
    if (!index.valid) {
        index = rebuildIndex(root_, folders, adoptionMetadataReads_);
    }
    QString newest;
    QDateTime modified;
    // Validate the small candidate set against the authoritative metadata.
    // A removed or externally reassigned candidate invalidates the index.
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool stale = false;
        newest.clear();
        modified = {};
        for (const auto &entry : index.paths.value(identityHash(identity_)).toArray()) {
            const auto name = entry.toString();
            const auto candidate = root_ + '/' + name;
            ++adoptionMetadataReads_;
            if (readObject(candidate + "/book.json", nullptr).value("identity").toString()
                != identity_) {
                stale = true;
                continue;
            }
            if (name == key_) {
                continue;
            }
            const auto saved = QFileInfo(candidate + "/bookmarks.json").lastModified();
            if (newest.isEmpty() || saved > modified) {
                newest = name;
                modified = saved;
            }
        }
        if (!stale || attempt != 0) {
            break;
        }
        index = rebuildIndex(root_, folders, adoptionMetadataReads_);
    }
    if (!newest.isEmpty()) {
        if (!readFolder(root_ + '/' + newest, error))
            return false;
        adoptedFrom_ = newest;
    }
    loadFailed_ = false;
    return true;
}

bool BookmarkStore::save(QString *error)
{
    int pins = 0;
    for (const auto &bookmark : bookmarks_) {
        if (bookmark.name.size() > 128 || bookmark.icon.size() > 64
            || (bookmark.pinned() && ++pins > maxPins)) {
            if (error)
                *error = "invalid-bookmark-pins";
            return false;
        }
    }
    // A failed read is not an empty list. Keep the old file for recovery.
    if (loadFailed_) {
        if (error)
            *error = "load-failed";
        return false;
    }
    if (key_.isEmpty()) {
        if (error)
            *error = "no-stable-key";
        return false;
    }
    if (sqlite_) {
        const AnnotationBook book{ root_, identity_, title_, key_, fingerprint_, database_ };
        if (databaseState_.revision < 0
            && !AnnotationDatabase::load(book, &persistedBookmarks_, &databaseState_, error))
            return false;
        if (!AnnotationDatabase::save(book, bookmarks_, persistedBookmarks_, &databaseState_,
                                      error))
            return false;
        persistedBookmarks_ = bookmarks_;
        adoptedFrom_.clear();
        return true;
    }
    QJsonObject book;
    book["version"] = formatVersion;
    book["identity"] = identity_;
    book["title"] = title_;
    book["fingerprint"] = QString::fromLatin1(fingerprint_);
    QJsonArray list;
    for (const auto &bookmark : bookmarks_)
        list.append(bookmarkJson(bookmark));
    QJsonObject marks;
    marks["version"] = formatVersion;
    marks["bookmarks"] = list;
    const auto marksBytes = QJsonDocument(marks).toJson();
    const auto bookBytes = QJsonDocument(book).toJson();
    // Check both before replacing either file. A successful save must remain
    // readable by load(), and an oversized edit must keep the previous list.
    if (marksBytes.size() > maxFileBytes || bookBytes.size() > maxFileBytes) {
        if (error)
            *error = "storage-limit";
        return false;
    }
    QFile metadata(folder() + "/book.json");
    const bool sameMetadata = metadata.size() == bookBytes.size()
            && metadata.open(QIODevice::ReadOnly) && metadata.readAll() == bookBytes;
    metadata.close();
    const auto index = identity_.startsWith("file://") && !sameMetadata
            ? readIndex(root_, annotationFolders(root_))
            : PathIndex{};
    if (!QDir().mkpath(folder())) {
        if (error) {
            *error = "create-directory";
        }
        return false;
    }
    // Ordinary bookmark edits keep identical metadata and its file stamp.
    // Metadata changes invalidate the cache before either authoritative file
    // commits, so interrupted saves cannot leave a trusted stale inventory.
    if (index.valid) {
        QFile::remove(indexPath(root_));
    }
    // The list is authoritative. A metadata failure must leave its previous
    // bytes intact; metadata can safely be refreshed before the list commits.
    if (!sameMetadata && !writeObject(folder() + "/book.json", bookBytes)) {
        if (error)
            *error = "write-metadata";
        return false;
    }
    if (!writeObject(folder() + "/bookmarks.json", marksBytes)) {
        if (error)
            *error = "write-bookmarks";
        return false;
    }
    if (index.valid) {
        updateIndex(root_, index, identity_, key_);
    }
    adoptedFrom_.clear();
    return true;
}

void BookmarkStore::setBookmarks(QVector<Bookmark> bookmarks)
{
    if (bookmarks.size() > maxBookmarks)
        bookmarks.resize(maxBookmarks);
    bookmarks_ = std::move(bookmarks);
}

QString excerptAfterChapter(const QString &excerpt, const QString &chapter)
{
    const auto text = excerpt.simplified(), title = chapter.simplified();
    // The title must end at a space, so "Chapter 1" does not match "Chapter 10".
    if (title.isEmpty() || text.size() <= title.size() + 1
        || !text.startsWith(title, Qt::CaseInsensitive) || !text.at(title.size()).isSpace())
        return excerpt;
    return text.mid(title.size() + 1);
}

bool BookmarkStore::merge(const std::function<int(const QString &)> &page,
                          const std::function<Bookmark(int, const QDateTime &)> &atPage)
{
    QMap<int, QVector<Bookmark>> pages;
    QVector<Bookmark> unresolved;
    for (const auto &bookmark : bookmarks_) {
        const int number = page(bookmark.position);
        // Named shortcuts retain their exact anchors across reflow, even when
        // several shortcuts now resolve to the same page.
        if (number < 0 || bookmark.pinned())
            unresolved.append(bookmark);
        else
            pages[number].append(bookmark);
    }
    QVector<Bookmark> result;
    bool changed = false;
    for (auto it = pages.begin(); it != pages.end(); ++it) {
        if (it.value().size() == 1) {
            result.append(it.value().first());
            continue;
        }
        QDateTime oldest = it.value().first().created;
        for (const auto &bookmark : it.value())
            if (bookmark.created.isValid() && (!oldest.isValid() || bookmark.created < oldest))
                oldest = bookmark.created;
        const auto replacement = atPage(it.key(), oldest);
        const bool pinnedAnchor =
                std::any_of(unresolved.begin(), unresolved.end(), [&](const Bookmark &bookmark) {
                    return bookmark.pinned() && bookmark.position == replacement.position;
                });
        // Position identifies rows in the session and editor. A retained pin
        // already represents this anchor; do not create a second identity.
        if (!pinnedAnchor)
            result.append(replacement);
        changed = true;
    }
    // Keep the stored order when nothing merged.
    if (!changed)
        return false;
    bookmarks_ = result + unresolved;
    return true;
}
} // namespace kobalt
