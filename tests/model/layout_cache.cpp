#include "storage/layout_cache.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
using namespace kobalt;

static void expect(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static QByteArray digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    expect(directory.isValid(), "temporary directory");
    const auto path = (directory.path() + "/" + "metadata.json");
    const auto key = digest("synthetic publication");
    const QByteArray policy = "synthetic-policy-1";
    const LayoutCompatibility supported{ LayoutCompatibility::Supported, 3, 100 };
    LayoutCompatibility result;
    expect(!LayoutCache(path, policy).lookup(key, &result), "missing cache must miss");
    expect(LayoutCache(path, policy).store(key, supported), "store complete verdict");
    expect(LayoutCache(path, policy).lookup(key, &result) && result.verdict == supported.verdict
                   && result.resources == 3 && result.bytes == 100,
           "reopen exact result");
    expect(!LayoutCache(path, policy).lookup(digest("modified publication"), &result),
           "changed bytes must miss");
    expect(!LayoutCache(path, "synthetic-policy-2").lookup(key, &result),
           "changed policy must miss");
    expect(!LayoutCache(path, policy).store(key, {}), "incomplete inspection is not cached");
    expect(!LayoutCache(path, policy).store("../bad", supported), "malformed digest rejected");
    expect(LayoutCache(path, policy).lookup(key, &result),
           "invalid write must preserve complete verdict");
    for (const auto verdict :
         { LayoutCompatibility::VerticalWriting, LayoutCompatibility::RightToLeftProgression }) {
        const LayoutCompatibility unsupported{ verdict, 4, 200 };
        expect(LayoutCache(path, policy).store(key, unsupported),
               "store stable unsupported verdict");
        expect(LayoutCache(path, policy).lookup(key, &result) && result.verdict == verdict,
               "preserve native fallback verdict");
    }
    // Each fresh authorized opening may skip inspection only after hashing its source.
    int inspections = 0;
    auto inspect = [&](const QByteArray &source) {
        LayoutCache cache(path, policy);
        LayoutCompatibility value;
        if (!cache.lookup(digest(source), &value)) {
            ++inspections;
            value = supported;
            cache.store(digest(source), value);
        }
    };
    inspect("new source");
    inspect("new source");
    inspect("changed source");
    expect(inspections == 2, "only identical source skips duplicate inspection");
    QFile::remove(path);
    for (int i = 0; i < LayoutCache::maxEntries; ++i)
        expect(LayoutCache(path, policy).store(digest(QByteArray::number(i)), supported),
               "fill every entry");
    expect(LayoutCache(path, policy).lookup(digest("0"), &result), "touch oldest entry");
    expect(LayoutCache(path, policy)
                   .store(digest(QByteArray::number(LayoutCache::maxEntries)), supported),
           "insert one more entry");
    expect(!LayoutCache(path, policy).lookup(digest("1"), &result),
           "least recently used entry evicted");
    expect(LayoutCache(path, policy).lookup(digest("0"), &result), "recent hit retained");
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "read metadata");
    const auto bytes = file.readAll();
    file.close();
    const auto original = QJsonDocument::fromJson(bytes).object();
    expect(bytes.size() <= LayoutCache::maxBytes
                   && original.value("entries").toArray().size() == LayoutCache::maxEntries,
           "bounded metadata");
    expect(!bytes.contains("new source") && !bytes.contains("changed source"),
           "no publication text serialized");
    auto replace = [&](const QByteArray &data) {
        expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "corrupt fixture");
        expect(file.write(data) == data.size(), "write fixture");
        file.close();
    };
    auto reject = [&](const QByteArray &data) {
        replace(data);
        expect(!LayoutCache(path, policy).lookup(digest("0"), &result), "corrupt cache must miss");
        expect(LayoutCache(path, policy).store(key, supported), "replace corrupt cache atomically");
        expect(LayoutCache(path, policy).lookup(key, &result), "recovered cache works");
    };
    reject("{truncated");
    reject(QByteArray(LayoutCache::maxBytes + 1, ' '));
    for (const double version : { 0., 2., 1.5 }) {
        auto root = original;
        root["version"] = version;
        reject(QJsonDocument(root).toJson());
    }
    for (const int verdict : { -1, int(LayoutCompatibility::Incomplete), 99 }) {
        auto root = original;
        auto entries = root["entries"].toArray();
        auto entry = entries[0].toObject();
        entry["verdict"] = verdict;
        entries[0] = entry;
        root["entries"] = entries;
        reject(QJsonDocument(root).toJson());
    }
    {
        auto root = original;
        auto entries = root["entries"].toArray();
        entries[1] = entries[0];
        root["entries"] = entries;
        reject(QJsonDocument(root).toJson());
    }
    {
        auto root = original;
        root["content"] = QStringLiteral("not metadata");
        reject(QJsonDocument(root).toJson());
    }
    expect(!LayoutCache(directory.path(), policy).store(key, supported),
           "write failure remains nonfatal");
    expect(!LayoutCache({}, policy).store(key, supported), "disabled cache performs no write");
    std::puts(
            "PASS: exact source and policy keys, complete verdicts, five-entry LRU, bounded metadata, corrupt-file recovery and nonfatal write failures");
}
