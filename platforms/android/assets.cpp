#include "assets.h"
#include "assets/assets.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>

namespace kobalt::android {
namespace {
bool write(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

void removeOldBundles(const QString &directory, const QString &current)
{
    if (QFileInfo(directory).isSymLink())
        return;
    const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    for (const auto &entry :
         QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        if (entry.fileName() != current && digest.match(entry.fileName()).hasMatch())
            QDir(entry.absoluteFilePath()).removeRecursively();
}
} // namespace

std::unique_ptr<AssetBundle> installAssets(const QString &applicationData)
{
    if (applicationData.isEmpty()) {
        return {};
    }
    QFile manifest(":/android-assets/manifest.json");
    if (!manifest.open(QIODevice::ReadOnly))
        return {};
    const auto bytes = manifest.readAll();
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    const auto root = applicationData + "/assets/" + digest;
    auto bundle = std::make_unique<AssetBundle>(root, QStringLiteral(KOBALT_VERSION));
    if (bundle->available()) {
        removeOldBundles(applicationData + "/assets", QString::fromLatin1(digest));
        return bundle;
    }
    const auto entries = QJsonDocument::fromJson(bytes).object().value("files").toArray();
    if (entries.isEmpty())
        return {};
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        const auto name = entry.value("path").toString();
        if ((!name.startsWith("fonts/") && !name.startsWith("hyph/")) || name.contains('\\')
            || name.split('/').contains("..") || name.split('/').contains("."))
            return {};
        QFile source(":/android-assets/" + name);
        if (!source.open(QIODevice::ReadOnly))
            return {};
        const auto data = source.readAll();
        if (QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()
                    != entry.value("sha256").toString().toLatin1()
            || !write(root + '/' + name, data))
            return {};
    }
    if (!write(root + "/manifest.json", bytes) || !bundle->available())
        return {};
    removeOldBundles(applicationData + "/assets", QString::fromLatin1(digest));
    return bundle;
}
} // namespace kobalt::android
