#include "support/diagnostics.h"
#include "assets.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <algorithm>

namespace kobalt {
namespace {
struct Entry
{
    QString path;
    qint64 size;
    QDateTime modified;
    QByteArray digest;
};

QByteArray digest(QFile &file)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto bytes = file.read(64 * 1024);
        if (bytes.isEmpty() && file.error() != QFile::NoError)
            return {};
        hash.addData(bytes);
    }
    return hash.result().toHex();
}

bool regular(const QFileInfo &file)
{
    return file.isFile() && !file.isSymLink();
}

bool regularMember(const QString &root, const QString &path)
{
    const auto relative = path.mid(root.size() + 1).split('/');
    QString current = root;
    for (int i = 0; i < relative.size(); ++i) {
        current += '/' + relative[i];
        const QFileInfo info(current);
        if (info.isSymLink() || (i + 1 < relative.size() && !info.isDir()))
            return false;
    }
    return regular(QFileInfo(path));
}

bool recoveryName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^FSCK[0-9]{4}\\.[0-9]{3}$"),
                                            QRegularExpression::CaseInsensitiveOption);
    return pattern.match(name).hasMatch();
}

bool restoreRecoveredAsset(const QString &root, const QString &path, qint64 size,
                           const QByteArray &hash)
{
    const QFileInfo target(path);
    if (target.exists() || target.isSymLink())
        return false;
    const QDir parent(target.absolutePath());
    for (const auto &candidate : parent.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
        if (!recoveryName(candidate.fileName()) || candidate.size() != size
            || !regularMember(root, candidate.absoluteFilePath()))
            continue;
        QFile recovered(candidate.absoluteFilePath());
        if (!recovered.open(QIODevice::ReadOnly) || digest(recovered) != hash)
            continue;
        recovered.close();
        // QFile::rename refuses an existing destination. Never replace another
        // asset, and leave unmatched recovery files for the user to inspect.
        if (recovered.rename(path))
            return true;
    }
    return false;
}

} // namespace

struct AssetBundle::Data
{
    QString root, version;
    QMutex mutex;
    QList<Entry> checked;
};

AssetBundle::AssetBundle(QString root, QString expectedVersion) : data(new Data)
{
    data->root = std::move(root);
    data->version = std::move(expectedVersion);
}

AssetBundle::~AssetBundle() = default;

const QString &AssetBundle::root() const
{
    return data->root;
}

void AssetBundle::invalidate()
{
    QMutexLocker lock(&data->mutex);
    data->checked.clear();
}

QString AssetBundle::file(const QString &name) const
{
    if (data->root.isEmpty() || name.isEmpty() || name.startsWith('/')
        || name.split('/').contains(".."))
        return {};
    return data->root + '/' + name;
}

bool AssetBundle::hasFilesystemRecoveryFiles() const
{
    const QFileInfo root(data->root);
    if (data->root.isEmpty() || !root.isDir() || root.isSymLink())
        return false;
    QDirIterator files(data->root, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        if (recoveryName(files.fileName()))
            return true;
    }
    return false;
}

bool AssetBundle::available() const
{
    QMutexLocker lock(&data->mutex);
    if (data->root.isEmpty())
        return false;
    auto &checked = data->checked;
    bool unchanged = !checked.isEmpty();
    for (const auto &entry : checked) {
        const QFileInfo file(entry.path);
        if (!regularMember(data->root, entry.path) || file.size() != entry.size
            || file.lastModified() != entry.modified) {
            unchanged = false;
            break;
        }
    }
    if (unchanged)
        return true;
    checked.clear();
    QFile manifest(data->root + "/manifest.json");
    if (!regular(QFileInfo(manifest)) || !manifest.open(QIODevice::ReadOnly)
        || manifest.size() > 65536)
        return false;
    const auto manifestBytes = manifest.readAll();
    const auto document = QJsonDocument::fromJson(manifestBytes).object();
    if (document.value("schema").toInt() != 1
        || document.value("version").toString() != data->version)
        return false;
    const auto entries = document.value("files").toArray();
    if (entries.isEmpty() || entries.size() > 256)
        return false;
    QList<Entry> verified;
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        const auto name = entry.value("path").toString();
        const auto path = this->file(name);
        const qint64 size = qint64(entry.value("size").toDouble(-1));
        const auto hash = entry.value("sha256").toString().toLatin1();
        if ((!name.startsWith("fonts/") && !name.startsWith("hyph/")) || path.isEmpty() || size < 0
            || size > 32 * 1024 * 1024 || hash.size() != 64)
            return false;
        restoreRecoveredAsset(data->root, path, size, hash);
        QFile file(path);
        const QFileInfo info(file);
        if (!regularMember(data->root, path))
            return false;
        if (!file.open(QIODevice::ReadOnly)) {
            reportDescriptorExhaustion("asset-open");
            return false;
        }
        if (file.size() != size || digest(file) != hash)
            return false;
        verified.append({ path, size, info.lastModified(), hash });
    }
    verified.append(
            { manifest.fileName(), manifestBytes.size(), QFileInfo(manifest).lastModified(), {} });
    checked = verified;
    return true;
}

QByteArray AssetBundle::fontData(const QString &name) const
{
    const auto read = deferredFontData(name);
    return read ? read() : QByteArray();
}

QByteArray AssetBundle::manifestFontData(const QString &name) const
{
    {
        QMutexLocker lock(&data->mutex);
        if (data->checked.isEmpty()) {
            const auto path = file("fonts/" + name);
            QFile manifest(data->root + "/manifest.json");
            if (path.isEmpty() || !regular(QFileInfo(manifest))
                || !manifest.open(QIODevice::ReadOnly) || manifest.size() > 65536)
                return {};
            const auto document = QJsonDocument::fromJson(manifest.readAll()).object();
            if (document.value("schema").toInt() != 1
                || document.value("version").toString() != data->version)
                return {};
            for (const auto &value : document.value("files").toArray()) {
                const auto entry = value.toObject();
                if (entry.value("path").toString() != "fonts/" + name)
                    continue;
                const qint64 size = qint64(entry.value("size").toDouble(-1));
                QFile font(path);
                if (size < 0 || size > 32 * 1024 * 1024 || !regularMember(data->root, path)
                    || !font.open(QIODevice::ReadOnly) || font.size() != size)
                    return {};
                const auto bytes = font.readAll();
                return bytes.size() == size
                                && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                                                .toHex()
                                        == entry.value("sha256").toString().toLatin1()
                        ? bytes
                        : QByteArray();
            }
            return {};
        }
    }
    return fontData(name);
}

std::function<QByteArray()> AssetBundle::deferredManifestFontData(const QString &name) const
{
    // Retain an independent owner: the original bundle can be invalidated or
    // destroyed before the opening view first needs a fallback face.
    auto opening = std::make_shared<AssetBundle>(data->root, data->version);
    return [opening, name] { return opening->manifestFontData(name); };
}

std::function<QByteArray()> AssetBundle::deferredFontData(const QString &name) const
{
    return deferredFontData(QStringList{ name }).value(name);
}

QMap<QString, std::function<QByteArray()>>
AssetBundle::deferredFontData(const QStringList &names) const
{
    if (!available())
        return {};
    QMutexLocker lock(&data->mutex);
    QMap<QString, std::function<QByteArray()>> readers;
    for (const auto &name : names) {
        const auto path = file("fonts/" + name);
        const auto found =
                std::find_if(data->checked.cbegin(), data->checked.cend(), [&](const Entry &entry) {
                    return entry.path == path && !entry.digest.isEmpty();
                });
        if (found == data->checked.cend())
            return {};
        readers.insert(name, [root = data->root, entry = *found] {
            if (!regularMember(root, entry.path))
                return QByteArray();
            QFile original(entry.path);
            if (!original.open(QIODevice::ReadOnly) || original.size() != entry.size)
                return QByteArray();
            const auto bytes = original.readAll();
            return bytes.size() == entry.size
                            && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                                    == entry.digest
                    ? bytes
                    : QByteArray();
        });
    }
    return readers;
}

struct FontCopies::Data
{
    QTemporaryDir directory{ QDir::tempPath() + "/kobalt-assets-XXXXXX" };
};

FontCopies::FontCopies() : data(new Data) { }

FontCopies::~FontCopies() = default;

QString FontCopies::file(const QString &name, const QByteArray &bytes)
{
    if (name.isEmpty() || name.startsWith('/') || name.split('/').contains(".."))
        return {};
    auto &temporary = data->directory;
    if (!temporary.isValid())
        return {};
    // Qt may still map an earlier copy. Changed bytes need another file even
    // when an asset refresh reuses the same logical filename.
    const auto digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const auto target = temporary.path() + '/' + digest + '/' + name;
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return {};
    QFile copy(target);
    if (copy.open(QIODevice::ReadOnly)) {
        const bool same = copy.readAll() == bytes;
        copy.close();
        if (same)
            return target;
    }
    if (!copy.open(QIODevice::WriteOnly | QIODevice::Truncate) || copy.write(bytes) != bytes.size())
        return {};
    copy.close();
    return target;
}
} // namespace kobalt
