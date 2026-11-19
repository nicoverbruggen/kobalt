#include "support/diagnostics.h"
#include "assets.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>

namespace kobalt {
namespace {
std::atomic<quint64> nextGeneration{ 1 };

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

// Kobalt cannot draw its own screens, or most books, without these. Any other
// asset that fails its check is set aside on its own.
bool requiredAsset(const QString &name)
{
    static const QSet<QString> required{
        "fonts/IBMPlexSans-Regular.ttf",    "fonts/IBMPlexSans-Bold.ttf",
        "fonts/IBMPlexSans-Italic.ttf",     "fonts/IBMPlexMono-Regular.ttf",
        "fonts/IBMPlexMono-Bold.ttf",       "fonts/InstrumentSerif-Regular.ttf",
        "fonts/InstrumentSerif-Italic.ttf", "fonts/NotoSerif-Regular.ttf",
        "fonts/NotoSerif-Bold.ttf",         "fonts/NotoSerif-Italic.ttf",
        "fonts/NotoSerif-BoldItalic.ttf",
    };
    return required.contains(name);
}

// The files a full check found intact, with the size and modification time
// they had then. A later check trusts a file whose size and time still match
// and computes SHA-256 again only for the others.
QString verifiedRecordPath(const QString &root)
{
    return root + "/.verified.json";
}

QJsonObject readVerifiedRecord(const QString &root, const QString &version)
{
    QFile file(verifiedRecordPath(root));
    if (!regular(QFileInfo(file)) || file.size() > 65536 || !file.open(QIODevice::ReadOnly))
        return {};
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    return record.value("version").toString() == version ? record.value("files").toObject()
                                                         : QJsonObject();
}

void writeVerifiedRecord(const QString &root, const QString &version, const QJsonObject &files)
{
    QJsonObject record;
    record["version"] = version;
    record["files"] = files;
    QSaveFile file(verifiedRecordPath(root));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(record).toJson(QJsonDocument::Compact)) < 0 || !file.commit())
        diagnostic("asset-record-unsaved", {}, DiagnosticSeverity::Warning);
}

QJsonObject fileRecord(qint64 size, const QDateTime &modified, const QByteArray &hash)
{
    QJsonObject entry;
    entry["size"] = double(size);
    entry["modified_ms"] = double(modified.toMSecsSinceEpoch());
    entry["sha256"] = QString::fromLatin1(hash);
    return entry;
}

} // namespace

struct AssetBundle::Data
{
    QString root, version;
    QMutex mutex;
    QList<Entry> checked;
    // Optional assets that failed their check, by manifest name.
    QSet<QString> setAside;
    Verification verification = Verification::OnDemand;
    std::atomic_bool verified{ false };
    std::atomic<quint64> generation{ nextGeneration++ };
};

AssetBundle::AssetBundle(QString root, QString expectedVersion, Verification verification)
    : data(new Data)
{
    data->root = std::move(root);
    data->version = std::move(expectedVersion);
    data->verification = verification;
}

AssetBundle::~AssetBundle() = default;

const QString &AssetBundle::root() const
{
    return data->root;
}

AssetBundle::Verification AssetBundle::verification() const
{
    return data->verification;
}

quint64 AssetBundle::generation() const
{
    return data->generation.load();
}

void AssetBundle::invalidate()
{
    data->generation.store(nextGeneration++);
    data->verified.store(false);
    QMutexLocker lock(&data->mutex);
    data->checked.clear();
    data->setAside.clear();
    data->verified.store(false);
}

bool AssetBundle::setAside(const QString &name) const
{
    QMutexLocker lock(&data->mutex);
    return data->setAside.contains(name);
}

QString AssetBundle::file(const QString &name) const
{
    if (data->root.isEmpty() || name.isEmpty() || name.startsWith('/')
        || name.split('/').contains(".."))
        return {};
    return data->root + '/' + name;
}

QMap<QString, QByteArray> AssetBundle::verifiedFontDigests() const
{
    QMutexLocker lock(&data->mutex);
    QMap<QString, QByteArray> result;
    const auto directory = file("fonts") + '/';
    for (const auto &entry : data->checked)
        if (entry.path.startsWith(directory))
            result.insert(entry.path, entry.digest);
    return result;
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
    if (data->verification == Verification::Explicit)
        return data->verified.load();
    QMutexLocker lock(&data->mutex);
    return verifyFiles();
}

bool AssetBundle::verify(const std::function<bool()> &current)
{
    QMutexLocker lock(&data->mutex);
    data->generation.store(nextGeneration++);
    data->verified.store(false);
    data->checked.clear();
    const bool valid = verifyFiles() && (!current || current());
    data->verified.store(valid);
    return valid;
}

bool AssetBundle::verifyFiles() const
{
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
    if (!checked.isEmpty())
        data->generation.store(nextGeneration++);
    checked.clear();
    data->setAside.clear();
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
    const auto previous = readVerifiedRecord(data->root, data->version);
    QJsonObject record;
    QList<Entry> verified;
    QStringList setAside;
    int hashed = 0;
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        const auto name = entry.value("path").toString();
        const auto path = this->file(name);
        const qint64 size = qint64(entry.value("size").toDouble(-1));
        const auto hash = entry.value("sha256").toString().toLatin1();
        if ((!name.startsWith("fonts/") && !name.startsWith("hyph/")) || path.isEmpty() || size < 0
            || size > 32 * 1024 * 1024 || hash.size() != 64)
            return false;
        // A required asset fails the whole bundle. Another one is set aside.
        const auto failed = [&] {
            if (requiredAsset(name))
                return false;
            setAside.append(name);
            return true;
        };
        restoreRecoveredAsset(data->root, path, size, hash);
        if (!regularMember(data->root, path)) {
            if (!failed())
                return false;
            continue;
        }
        const QFileInfo info(path);
        const auto known = previous.value(name).toObject();
        if (info.size() == size && known.value("sha256").toString().toLatin1() == hash
            && qint64(known.value("size").toDouble(-1)) == size
            && qint64(known.value("modified_ms").toDouble(-1))
                    == info.lastModified().toMSecsSinceEpoch()) {
            verified.append({ path, size, info.lastModified(), hash });
            record[name] = known;
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            reportDescriptorExhaustion("asset-open");
            return false;
        }
        ++hashed;
        if (file.size() != size || digest(file) != hash) {
            if (!failed())
                return false;
            continue;
        }
        verified.append({ path, size, info.lastModified(), hash });
        record[name] = fileRecord(size, info.lastModified(), hash);
    }
    if (record != previous)
        writeVerifiedRecord(data->root, data->version, record);
    if (!setAside.isEmpty())
        diagnostic("assets-set-aside", { { "files", QJsonArray::fromStringList(setAside) } },
                   DiagnosticSeverity::Warning);
    diagnostic("assets-hashed", { { "hashed", hashed }, { "files", entries.size() } });
    for (const auto &name : setAside)
        data->setAside.insert(name);
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

QByteArray AssetBundle::manifestFontData(const QString &name, QByteArray *digest) const
{
    if (digest)
        digest->clear();
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
                const auto expected = entry.value("sha256").toString().toLatin1();
                if (bytes.size() != size
                    || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                            != expected)
                    return {};
                if (digest)
                    *digest = expected;
                return bytes;
            }
            return {};
        }
    }
    QMap<QString, QByteArray> digests;
    const auto read =
            deferredFontData(QStringList{ name }, digest ? &digests : nullptr).value(name);
    const auto bytes = read ? read() : QByteArray();
    if (digest && !bytes.isEmpty())
        *digest = digests.value(name);
    return bytes;
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
AssetBundle::deferredFontData(const QStringList &names, QMap<QString, QByteArray> *digests) const
{
    if (digests)
        digests->clear();
    if (!available())
        return {};
    QMutexLocker lock(&data->mutex);
    QMap<QString, std::function<QByteArray()>> readers;
    QMap<QString, QByteArray> expected;
    for (const auto &name : names) {
        // An optional font that failed its check is simply left out.
        if (data->setAside.contains("fonts/" + name))
            continue;
        const auto path = file("fonts/" + name);
        const auto found =
                std::find_if(data->checked.cbegin(), data->checked.cend(), [&](const Entry &entry) {
                    return entry.path == path && !entry.digest.isEmpty();
                });
        if (found == data->checked.cend())
            return {};
        if (digests)
            expected.insert(name, found->digest);
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
    if (digests)
        *digests = std::move(expected);
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
