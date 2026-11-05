#include "platforms/android/assets.h"
#include "assets/assets.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>

static void require(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void write(const QString &path, const QByteArray &bytes)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create fixture folder");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "Cannot write fixture");
}

// Checks the Android asset installer: cleanup after install and cached startup, retention after failure and unrelated entries.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir profile, outside;
    require(profile.isValid() && outside.isValid(), "Cannot create asset fixtures");
    const auto assets = profile.path() + "/assets/";
    const auto old = assets + QString(64, 'a');
    write(old + "/hyph/old.pattern", "old bundle");
    write(assets + "unrelated/keep", "unrelated directory");
    write(assets + QString(64, 'c'), "unrelated file");
    write(outside.path() + "/keep", "external data");
    require(QFile::link(outside.path(), assets + QString(64, 'b')),
            "Cannot create sibling symlink");
    require(QFile::link(outside.path(), old + "/external"), "Cannot create nested symlink");
    QFile manifest(":/android-assets/manifest.json");
    require(manifest.open(QIODevice::ReadOnly), "Missing embedded fixture manifest");
    const auto digest =
            QCryptographicHash::hash(manifest.readAll(), QCryptographicHash::Sha256).toHex();
    const auto current = assets + QString::fromLatin1(digest);
    write(current, "blocks installation");
    require(!kobalt::android::installAssets(profile.path()), "Blocked installation succeeded");
    require(QFileInfo::exists(old + "/hyph/old.pattern"),
            "Failed installation removed the old bundle");
    require(QFile::remove(current), "Cannot remove installation blocker");
    auto bundle = kobalt::android::installAssets(profile.path());
    require(bundle && bundle->available(), "Current assets were not installed and verified");
    require(!QFileInfo::exists(old), "Successful installation retained the old bundle");
    require(QFileInfo::exists(current + "/hyph/fixture.pattern"), "Cleanup removed current assets");
    require(QFileInfo::exists(assets + "unrelated/keep")
                    && QFileInfo::exists(assets + QString(64, 'c')),
            "Cleanup removed an unrelated entry");
    require(QFileInfo(assets + QString(64, 'b')).isSymLink()
                    && QFileInfo::exists(outside.path() + "/keep"),
            "Cleanup followed a symlink");
    write(old + "/hyph/old.pattern", "old bundle restored by backup");
    bundle = kobalt::android::installAssets(profile.path());
    require(bundle && bundle->available() && !QFileInfo::exists(old),
            "Verified cached assets skipped cleanup");
    std::puts(
            "PASS: Android assets clean old bundles only after verification and preserve unrelated entries");
}
