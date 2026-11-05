#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QMap>
#include <memory>
#include <functional>

namespace kobalt {
class AssetBundle
{
public:
    AssetBundle(QString root, QString expectedVersion);
    ~AssetBundle();
    AssetBundle(const AssetBundle &) = delete;
    AssetBundle &operator=(const AssetBundle &) = delete;
    bool available() const;
    bool hasFilesystemRecoveryFiles() const;
    void invalidate();
    QString file(const QString &name) const;
    QByteArray fontData(const QString &name) const;
    // Capture verified metadata without retaining font bytes or this bundle.
    // The reader checks the file again when it is first needed.
    std::function<QByteArray()> deferredFontData(const QString &name) const;
    // Verify the whole bundle once for a batch. Each reader still verifies its
    // own file when called. A missing member rejects the complete batch.
    QMap<QString, std::function<QByteArray()>> deferredFontData(const QStringList &names) const;
    // Checks one font against the manifest without verifying the whole bundle
    // first. After a full verification, this is the same as fontData().
    QByteArray manifestFontData(const QString &name) const;
    // Retain an independent manifest reader for text shown before full verification.
    std::function<QByteArray()> deferredManifestFontData(const QString &name) const;
    const QString &root() const;

private:
    struct Data;
    std::unique_ptr<Data> data;
};

// Keep this owner alive as long as Qt can retain registered font faces. Asset
// invalidation releases verification state, not these independent copies.
// Creation and writes belong on the GUI thread.
class FontCopies
{
public:
    FontCopies();
    ~FontCopies();
    FontCopies(const FontCopies &) = delete;
    FontCopies &operator=(const FontCopies &) = delete;
    QString file(const QString &name, const QByteArray &bytes);

private:
    struct Data;
    std::unique_ptr<Data> data;
};
} // namespace kobalt
