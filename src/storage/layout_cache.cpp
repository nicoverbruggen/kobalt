#include "support/qt_compat.h"
#include "support/main_thread.h"
#include "layout_cache.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace kobalt {
namespace {
bool validDigest(const QByteArray &digest)
{
    if (digest.size() != 64)
        return false;
    for (const char c : digest)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool boundedInteger(const QJsonValue &value, int maximum)
{
    const double n = value.toDouble(-1);
    return value.isDouble() && n >= 0 && n <= maximum && std::floor(n) == n;
}

bool validEntry(const QJsonObject &entry)
{
    return entry.size() == 4 && validDigest(entry.value("digest").toString().toLatin1())
            && boundedInteger(entry.value("verdict"), LayoutCompatibility::RightToLeftProgression)
            && boundedInteger(entry.value("resources"), 512)
            && boundedInteger(entry.value("bytes"), 8 * 1024 * 1024);
}
} // namespace

LayoutCache::LayoutCache(QString path, QByteArray policy)
    : path_(std::move(path)), policy_(std::move(policy))
{
    QFile file(path_);
    if (path_.isEmpty() || policy_.isEmpty() || !QFileInfo(path_).isFile()
        || !file.open(QIODevice::ReadOnly) || file.size() > maxBytes)
        return;
    const auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes)
        return;
    const auto document = QJsonDocument::fromJson(bytes);
    const auto root = document.object();
    if (root.size() != 3 || root.value("version") != QJsonValue(1)
        || root.value("policy").toString().toLatin1() != policy_
        || !root.value("entries").isArray())
        return;
    const auto entries = root.value("entries").toArray();
    if (entries.size() > maxEntries)
        return;
    QSet<QString> seen;
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        const auto digest = entry.value("digest").toString();
        if (!validEntry(entry) || seen.contains(digest))
            return;
        seen.insert(digest);
    }
    entries_ = entries;
}

bool LayoutCache::lookup(const QByteArray &digest, LayoutCompatibility *result)
{
    if (!result || !validDigest(digest))
        return false;
    for (int i = 0; i < entries_.size(); ++i) {
        const auto entry = entries_[i].toObject();
        if (entry.value("digest").toString().toLatin1() != digest)
            continue;
        result->verdict = static_cast<LayoutCompatibility::Verdict>(entry.value("verdict").toInt());
        result->resources = entry.value("resources").toInt();
        result->bytes = entry.value("bytes").toInt();
        if (i) {
            entries_.removeAt(i);
            entries_.prepend(entry);
            save();
        }
        return true;
    }
    return false;
}

bool LayoutCache::store(const QByteArray &digest, const LayoutCompatibility &result)
{
    QJsonObject entry = kobalt::qt::object({ { "digest", QString::fromLatin1(digest) },
                                             { "verdict", int(result.verdict) },
                                             { "resources", result.resources },
                                             { "bytes", double(result.bytes) } });
    if (!validEntry(entry))
        return false;
    for (int i = entries_.size() - 1; i >= 0; --i)
        if (entries_[i].toObject().value("digest") == entry.value("digest"))
            entries_.removeAt(i);
    entries_.prepend(entry);
    while (entries_.size() > maxEntries)
        entries_.removeLast();
    return save();
}

bool LayoutCache::save()
{
    mainthread::BlockingOperation blockingOperation("layout-cache-write");
    if (path_.isEmpty() || policy_.isEmpty() || !QDir().mkpath(QFileInfo(path_).absolutePath()))
        return false;
    const auto bytes =
            QJsonDocument(kobalt::qt::object({ { "version", 1 },
                                               { "policy", QString::fromLatin1(policy_) },
                                               { "entries", entries_ } }))
                    .toJson(QJsonDocument::Compact);
    if (bytes.size() > maxBytes)
        return false;
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(bytes) == bytes.size() && file.commit();
}
} // namespace kobalt
