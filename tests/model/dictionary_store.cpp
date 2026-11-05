#include "dictionaries/manager.h"
#include "dictionaries/stardict.h"
#include "support/qt_compat.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <cstring>
#include <QMap>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QtEndian>
#include <zlib.h>
using namespace kobalt;

namespace {
void require(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

void le16(QByteArray &b, quint16 v)
{
    v = qToLittleEndian(v);
    b.append(reinterpret_cast<char *>(&v), 2);
}

void le32(QByteArray &b, quint32 v)
{
    v = qToLittleEndian(v);
    b.append(reinterpret_cast<char *>(&v), 4);
}

void be32(QByteArray &b, quint32 v)
{
    v = qToBigEndian(v);
    b.append(reinterpret_cast<char *>(&v), 4);
}

void save(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    require(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "write fixture");
}

QByteArray dictzip(const QByteArray &body)
{
    constexpr int chunk = 24;
    QVector<QByteArray> blocks;
    z_stream stream{};
    require(deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK,
            "deflate init");
    for (int at = 0; at < body.size(); at += chunk) {
        QByteArray out(1024, '\0');
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(body.constData() + at));
        stream.avail_in = qMin(chunk, int(body.size()) - at);
        stream.next_out = reinterpret_cast<Bytef *>(out.data());
        stream.avail_out = out.size();
        require(deflate(&stream, Z_FULL_FLUSH) == Z_OK, "deflate chunk");
        out.resize(out.size() - stream.avail_out);
        blocks.append(out);
    }
    QByteArray tail(64, '\0');
    stream.next_in = nullptr;
    stream.avail_in = 0;
    stream.next_out = reinterpret_cast<Bytef *>(tail.data());
    stream.avail_out = tail.size();
    require(deflate(&stream, Z_FINISH) == Z_STREAM_END, "deflate finish");
    tail.resize(tail.size() - stream.avail_out);
    deflateEnd(&stream);
    QByteArray extra("RA");
    le16(extra, 6 + blocks.size() * 2);
    le16(extra, 1);
    le16(extra, chunk);
    le16(extra, blocks.size());
    for (const auto &block : blocks)
        le16(extra, block.size());
    QByteArray result = QByteArray::fromHex("1f8b0804000000000003");
    le16(result, extra.size());
    result += extra;
    for (const auto &block : blocks) {
        result += block;
    }
    result += tail;
    le32(result, crc32(0, reinterpret_cast<const Bytef *>(body.constData()), body.size()));
    le32(result, body.size());
    return result;
}

void replace16(QByteArray &bytes, int at, quint16 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + at));
}

void replace32(QByteArray &bytes, int at, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + at));
}

void checkDictzip(const QString &directory, const QByteArray &body)
{
    require(QDir().mkpath(directory), "make dictzip fixture directory");
    const auto prefix = directory + "/fixture";
    QByteArray index("word\0", 5);
    be32(index, 0);
    be32(index, body.size());
    save(prefix + ".ifo",
         "StarDict's dict ifo file\nversion=2.4.2\nwordcount=1\nidxfilesize="
                 + QByteArray::number(index.size()) + "\nsametypesequence=h\n");
    save(prefix + ".idx", index);
    const auto valid = dictzip(body);
    const int extraSize =
            qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(valid.constData() + 10));
    const int payload = 12 + extraSize;
    const auto rejected = [&](const QByteArray &bytes, const char *message) {
        save(prefix + ".dict.dz", bytes);
        QString error;
        require(!StarDict::prepare(directory, "fixture", &error), message);
        require(error == "Invalid dictzip data.",
                "invalid compression metadata reports its own failure");
        require(!QFile::exists(prefix + ".idx.kobalt"),
                "invalid compression metadata creates no lookup index");
    };
    auto bytes = valid;
    bytes[0] = 0;
    rejected(bytes, "bad gzip signature rejected");
    bytes = valid;
    bytes[3] = 0;
    rejected(bytes, "dictzip requires the random-access extra field");
    bytes = valid;
    bytes[3] = char(4 | 32);
    rejected(bytes, "unsupported gzip flags rejected");
    rejected(valid.left(11), "truncated fixed header rejected");
    rejected(valid.left(payload - 1), "truncated extra field rejected");
    bytes = valid;
    bytes.replace(12, 2, "ZZ");
    rejected(bytes, "missing random-access record rejected");
    bytes = valid;
    bytes.insert(payload, valid.mid(12, extraSize));
    replace16(bytes, 10, extraSize * 2);
    rejected(bytes, "duplicate random-access records rejected");
    bytes = valid;
    replace16(bytes, 14, extraSize);
    rejected(bytes, "extra record cannot extend beyond the declared extra field");
    bytes = valid;
    replace16(bytes, 16, 2);
    rejected(bytes, "unsupported random-access version rejected");
    bytes = valid;
    replace16(bytes, 18, 0);
    rejected(bytes, "zero uncompressed chunk size rejected");
    bytes = valid;
    replace16(bytes, 20, 0);
    rejected(bytes, "zero chunk count rejected");
    bytes = valid;
    replace16(bytes, 20,
              qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(valid.constData() + 20))
                      + 1);
    rejected(bytes, "chunk count must agree with the size table");
    bytes = valid;
    replace16(bytes, 22, 0);
    rejected(bytes, "zero compressed chunk size rejected");
    bytes = valid;
    replace16(bytes, 22, 65535);
    rejected(bytes, "chunk table cannot overlap the gzip trailer");
    const int chunks =
            qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(valid.constData() + 20));
    bytes = valid;
    replace32(bytes, bytes.size() - 4, (chunks - 1) * 24);
    rejected(bytes, "trailer length must include the last chunk");
    bytes = valid;
    replace32(bytes, bytes.size() - 4, chunks * 24 + 1);
    rejected(bytes, "trailer length cannot exceed the chunk capacity");
    bytes = valid.left(payload) + "unterminated";
    bytes[3] = char(4 | 16);
    rejected(bytes, "truncated optional gzip comment rejected");
    bytes = valid;
    bytes[3] = char(4 | 8);
    bytes.insert(payload, QByteArray(4096, 'x'));
    rejected(bytes, "optional gzip name has a bounded scan");

    // An unrelated extra record and both optional strings must preserve the payload offset.
    bytes = valid;
    bytes[3] = char(4 | 8 | 16);
    bytes.insert(payload, QByteArray("fixture.dict\0fixture comment\0", 29));
    const QByteArray unrelated = QByteArray::fromHex("585902000102");
    bytes.insert(12, unrelated);
    replace16(bytes, 10, extraSize + unrelated.size());
    save(prefix + ".dict.dz", bytes);
    QString error;
    require(StarDict::prepare(directory, "fixture", &error), qPrintable(error));
    {
        StarDict dictionary;
        require(dictionary.open(directory, "fixture", &error), qPrintable(error));
        require(dictionary.lookup("word").html.toUtf8() == body,
                "optional gzip fields preserve dictionary lookup");
    }

    // The header is internally consistent, but each inflated chunk has the wrong length.
    bytes = valid;
    replace16(bytes, 18, 25);
    save(prefix + ".dict.dz", bytes);
    require(StarDict::prepare(directory, "fixture", &error), qPrintable(error));
    {
        StarDict dictionary;
        require(dictionary.open(directory, "fixture", &error), qPrintable(error));
        const auto result = dictionary.lookup("word");
        require(result.received && result.html.isEmpty()
                        && result.error == "Dictionary definition is damaged.",
                "decompression length mismatch cannot return a partial definition");
    }

    save(prefix + ".dict.dz", valid);
    require(StarDict::prepare(directory, "fixture", &error), qPrintable(error));
    auto damagedIndex = index;
    qToBigEndian<quint32>(body.size(), reinterpret_cast<uchar *>(damagedIndex.data() + 5));
    save(prefix + ".idx", damagedIndex);
    {
        StarDict dictionary;
        require(dictionary.open(directory, "fixture", &error), qPrintable(error));
        const auto result = dictionary.lookup("word");
        require(result.html.isEmpty() && result.error == "Dictionary definition is damaged.",
                "an installed index changed to an invalid body offset cannot return data");
    }
    save(prefix + ".idx", index);
    save(prefix + ".ifo",
         "StarDict's dict ifo file\nversion=2.4.2\nwordcount=1\nsynwordcount=1\nidxfilesize="
                 + QByteArray::number(index.size()) + "\nsametypesequence=h\n");
    QByteArray synonym("alias\0", 6);
    be32(synonym, 0);
    save(prefix + ".syn", synonym);
    require(StarDict::prepare(directory, "fixture", &error), qPrintable(error));
    {
        StarDict dictionary;
        require(dictionary.open(directory, "fixture", &error), qPrintable(error));
        require(dictionary.lookup("alias").html.toUtf8() == body,
                "synonym resolves a prepared word number");
    }
    qToBigEndian<quint32>(1, reinterpret_cast<uchar *>(synonym.data() + 6));
    save(prefix + ".syn", synonym);
    {
        StarDict dictionary;
        require(dictionary.open(directory, "fixture", &error), qPrintable(error));
        const auto result = dictionary.lookup("alias");
        require(result.html.isEmpty() && result.error == "Dictionary index is damaged.",
                "an installed synonym changed to an invalid word number cannot return data");
    }
}

QByteArray zip(const QMap<QByteArray, QByteArray> &files)
{
    QByteArray bytes, directory;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto name = QByteArray("en-en/") + it.key();
        const auto body = it.value();
        const auto at = bytes.size();
        const auto crc = crc32(0, reinterpret_cast<const Bytef *>(body.constData()), body.size());
        le32(bytes, 0x04034b50);
        le16(bytes, 20);
        le16(bytes, 0);
        le16(bytes, 0);
        le32(bytes, 0);
        le32(bytes, crc);
        le32(bytes, body.size());
        le32(bytes, body.size());
        le16(bytes, name.size());
        le16(bytes, 0);
        bytes += name;
        bytes += body;
        le32(directory, 0x02014b50);
        le16(directory, 20);
        le16(directory, 20);
        le16(directory, 0);
        le16(directory, 0);
        le32(directory, 0);
        le32(directory, crc);
        le32(directory, body.size());
        le32(directory, body.size());
        le16(directory, name.size());
        le16(directory, 0);
        le16(directory, 0);
        le16(directory, 0);
        le16(directory, 0);
        le32(directory, 0);
        le32(directory, at);
        directory += name;
    }
    const auto offset = bytes.size();
    bytes += directory;
    le32(bytes, 0x06054b50);
    le16(bytes, 0);
    le16(bytes, 0);
    le16(bytes, files.size());
    le16(bytes, files.size());
    le32(bytes, directory.size());
    le32(bytes, offset);
    le16(bytes, 0);
    return bytes;
}

class Empty : public DictionaryService
{
public:
    QVector<DictionaryChoice> choices() const override { return {}; }

    bool select(const QString &, const QString &, QString *) override { return false; }

    DictionaryResult lookup(const QString &, const QString &word) override
    {
        DictionaryResult result;
        result.word = word;
        result.error = "Empty";
        return result;
    }
};
} // namespace

class NativeChoice final : public Empty
{
public:
    QVector<DictionaryChoice> choices() const override { return { { "native", "Native" } }; }

    bool select(const QString &, const QString &id, QString *) override { return id == "native"; }

    DictionaryResult lookup(const QString &, const QString &word) override
    {
        DictionaryResult result;
        result.word = word;
        result.dictionaryId = "native";
        result.html = "Native definition";
        return result;
    }
};

class BufferedDownload final : public QNetworkReply
{
public:
    qint64 remaining = 0;
    int readsAfterAbort = 0, aborts = 0;

    explicit BufferedDownload(const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly);
    }

    qint64 bytesAvailable() const override { return remaining + QNetworkReply::bytesAvailable(); }

    void abort() override
    {
        if (isFinished())
            return;
        ++aborts;
        setError(OperationCanceledError, "Operation canceled");
        setFinished(true);
        emit finished();
    }

    void deliver(qint64 size)
    {
        remaining = size;
        emit readyRead();
    }

    void complete(int status = 200, const QUrl &redirect = {}, NetworkError error = NoError)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (!redirect.isEmpty())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, redirect);
        if (error != NoError)
            setError(error, "Fixture network failure");
        setFinished(true);
        emit finished();
    }

protected:
    qint64 readData(char *data, qint64 maximum) override
    {
        if (aborts)
            ++readsAfterAbort;
        const auto size = qMin(maximum, remaining);
        std::memset(data, 0, size_t(size));
        remaining -= size;
        return size;
    }
};

class DownloadNetwork final : public QNetworkAccessManager
{
public:
    QPointer<BufferedDownload> reply;
    int requests = 0;

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        ++requests;
        reply = new BufferedDownload(request, this);
        return reply;
    }
};

// Checks dictionary downloads, archive limits, source names, installation, lookup and removal, with controlled replies.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    require(temp.isValid(), "temporary directory");
    {
        const auto entries = DictionaryManager::catalog();
        require(!entries.isEmpty(), "download catalog has a test entry");
        const auto entry =
                *std::min_element(entries.begin(), entries.end(),
                                  [](const auto &a, const auto &b) { return a.size < b.size; });
        DownloadNetwork network;
        DictionaryManager manager(temp.path() + "/downloads", std::make_unique<Empty>(), nullptr,
                                  &network);
        manager.download(entry.id);
        require(manager.busy() && network.reply, "controlled download starts");
        network.reply->deliver(entry.size + 3 * 65536);
        require(!manager.busy(), "oversized buffered reply finishes without a hang");
        require(manager.status() == "Download size or storage error.",
                "abort preserves the local download failure");
        require(network.reply->aborts == 1 && network.reply->readsAfterAbort == 0,
                "abort does not drain more buffered data");
        require(QDir(temp.path() + "/downloads").entryList(QDir::Files | QDir::Hidden).isEmpty(),
                "failed download removes temporary data");
        manager.download(entry.id);
        require(manager.busy(), "download can restart after failure");
        manager.cancel();
        require(!manager.busy() && manager.status() == "Download cancelled.",
                "user cancellation remains distinct");
        for (const auto status : { 404, 200 }) {
            manager.download(entry.id);
            network.reply->complete(status);
            require(!manager.busy(), "HTTP failure or truncated response releases the transfer");
            require(manager.status() == "Dictionary checksum or size differs.",
                    "failed HTTP or truncated download is rejected");
            require(!manager.installed(entry.id), "failed download cannot become installed");
        }
        manager.download(entry.id);
        network.reply->deliver(entry.size);
        network.reply->complete();
        require(!manager.busy() && !manager.installed(entry.id),
                "correct size with wrong checksum is rejected");
        require(manager.status() == "Dictionary checksum or size differs.",
                "digest is checked after exact-size download");
        manager.download(entry.id);
        network.reply->complete(200, {}, QNetworkReply::ConnectionRefusedError);
        require(!manager.busy() && manager.status() == "Fixture network failure",
                "network error survives cleanup");
        manager.download(entry.id);
        const int beforeUnsafeRedirect = network.requests;
        network.reply->complete(302, QUrl("http://unsafe.example/dictionary.zip"));
        require(network.requests == beforeUnsafeRedirect, "HTTPS downgrade never starts a request");
        require(!manager.busy() && manager.status() == "Unsafe download redirect.",
                "HTTPS downgrade releases the transfer");
        manager.download(entry.id);
        const int beforeRedirectLoop = network.requests;
        for (int redirect = 0; redirect < 6; ++redirect) {
            require(manager.busy(), "allowed redirect remains active");
            network.reply->complete(302, QUrl("/dictionary.zip"));
        }
        require(network.requests == beforeRedirectLoop + 5,
                "redirect limit prevents an unbounded request loop");
        require(!manager.busy() && manager.status() == "Unsafe download redirect.",
                "redirect limit ends the transfer");
        require(QDir(temp.path() + "/downloads").entryList(QDir::Files | QDir::Hidden).isEmpty(),
                "all failed transfers remove temporary files");
        manager.download(entry.id);
        const int beforeBusy = network.requests;
        manager.download(entry.id);
        manager.download("../invalid");
        require(network.requests == beforeBusy, "busy downloader ignores new requests");
        manager.cancel();
        manager.download("../invalid");
        require(!manager.busy() && network.requests == beforeBusy,
                "unknown dictionary starts no request");
    }
    {
        DictionaryManager native(temp.path() + "/new-root", std::make_unique<NativeChoice>());
        QString error;
        require(native.select("book", "native", &error),
                "native session choice works before any download");
    }
    const auto root = temp.path() + "/dictionaries";
    const auto archive = temp.path() + "/dictionary.zip";
    const QByteArray first(
            "<p>A domesticated animal with a definition crossing compressed chunks.</p>"),
            second("<p>\xd9\x83\xd8\xaa\xd8\xa7\xd8\xa8</p>");
    checkDictzip(temp.path() + "/dictzip-validation", first + second);
    {
        const auto folder = temp.path() + "/phrase";
        require(QDir().mkpath(folder), "make phrase dictionary directory");
        const QByteArray definition("<p>A frozen dessert.</p>");
        QByteArray index("ice cream\0", 10);
        be32(index, 0);
        be32(index, definition.size());
        save(folder + "/phrase.idx", index);
        save(folder + "/phrase.dict.dz", dictzip(definition));
        save(folder + "/phrase.ifo",
             "StarDict's dict ifo file\nversion=2.4.2\nwordcount=1\nidxfilesize="
                     + QByteArray::number(index.size()) + "\nsametypesequence=h\n");
        QString error;
        require(StarDict::prepare(folder, "phrase", &error), qPrintable(error));
        StarDict dictionary;
        require(dictionary.open(folder, "phrase", &error), qPrintable(error));
        require(dictionary.lookup("ice cream").html.toUtf8() == definition,
                "Dictionary lookup did not resolve a complete phrase entry");
        require(dictionary.lookup("ice").html.isEmpty(),
                "Phrase lookup silently used only its first word");
    }
    QByteArray idx("CAT\0", 4);
    be32(idx, first.size());
    be32(idx, second.size());
    idx += QByteArray("cat\0", 4);
    be32(idx, 0);
    be32(idx, first.size());
    idx += QByteArray("word\0", 5);
    be32(idx, first.size());
    be32(idx, second.size());
    QByteArray syn("cats\0", 5);
    be32(syn, 1);
    QMap<QByteArray, QByteArray> files;
    files["en-en.ifo"] =
            "StarDict's dict ifo file\nversion=2.4.2\nwordcount=3\nsynwordcount=1\nidxfilesize="
            + QByteArray::number(idx.size()) + "\nsametypesequence=h\n";
    files["en-en.idx"] = idx;
    files["en-en.syn"] = syn;
    files["en-en.dict.dz"] = dictzip(first + second);
    files["LICENSE"] = "Fixture license";
    files["ATTRIBUTION"] =
            "This dictionary is distributed under CC-BY-SA-4.0.\nhttps://license.example\n\n"
            "It is built from the following sources:\n\n"
            "  * pwn30: Princeton University WordNet 3.0\n"
            "  * wikt-en: English Wiktionary via kaikki.org (wiktextract)\n"
            "  * wikt-en-extra: English Wiktionary via kaikki.org (wiktextract)\n\n"
            "-- WordNet notice --\nFull license notice stays in this file.\n";
    DownloadDictionary entry;
    entry.id = "en-en";
    entry.name = "Fixture";
    const auto write = [&] {
        const auto bytes = zip(files);
        save(archive, bytes);
        entry.size = bytes.size();
        entry.sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    };
    write();
    QString error;
    auto wrong = entry;
    wrong.sha256[0] = char(wrong.sha256.at(0) ^ 1);
    require(!DictionaryManager::installArchive(root, wrong, archive, &error), "checksum rejected");
    require(!DictionaryManager::installArchive(root, entry, archive, &error, [] { return true; }),
            "cancel rejected");
    require(!QFile::exists(root + "/en-en"), "failed install invisible");
    require(DictionaryManager::installArchive(root, entry, archive, &error), qPrintable(error));
    StarDict dictionary;
    require(dictionary.open(root + "/en-en", "en-en", &error), qPrintable(error));
    require(dictionary.lookup("cat").html.toUtf8() == first, "cross-chunk definition");
    require(dictionary.lookup("cat").source
                    == QString::fromUtf8("WordNet 3.0 · English Wiktionary"),
            "Lookup source summary must name projects once and omit license and import details");
    require(dictionary.lookup("absent").source == dictionary.lookup("cat").source,
            "Missing definitions retain their dictionary source summary");
    QFile attribution(root + "/en-en/ATTRIBUTION");
    require(attribution.open(QIODevice::ReadOnly) && attribution.readAll() == files["ATTRIBUTION"],
            "Compact source display must preserve the full attribution file");
    attribution.close();
    save(root + "/en-en/ATTRIBUTION",
         "It is built from the following sources:\n  * custom: A <credit> & partner\n");
    StarDict escaped;
    require(escaped.open(root + "/en-en", "en-en", &error)
                    && escaped.lookup("cat").source == "A &lt;credit&gt; &amp; partner",
            "Source summaries must preserve unknown project names and escape markup");
    save(root + "/en-en/ATTRIBUTION", files["ATTRIBUTION"]);
    require(dictionary.lookup("CAT").html.toUtf8() == second,
            "exact spelling beats folded acronym");
    require(dictionary.lookup("Cat").html.toUtf8() == second, "ASCII fold fallback");
    require(dictionary.lookup("cats").html.toUtf8() == first, "inflection index");
    require(dictionary.lookup("word").html.toUtf8() == second, "Unicode definition");
    require(dictionary.lookup("absent").html.isEmpty(), "missing word");
    {
        DictionaryManager manager(root, std::make_unique<Empty>());
        require(manager.installed("en-en") && manager.choices().size() == 1, "installed catalog");
        qint64 installedBytes = 0;
        for (const auto &file :
             QDir(root + "/en-en").entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
            installedBytes += file.size();
        require(manager.installedSizes().value("en-en") == installedBytes && installedBytes > 0
                        && installedBytes != entry.size,
                "installed storage includes extracted files and generated indexes, not ZIP bytes");
        require(manager.availableBytes() >= 0,
                "free storage is available on the dictionary filesystem");
        require(manager.selectedDictionary("book").isEmpty(), "no session choice before selection");
        manager.setLanguage("en-US");
        require(manager.lookup("book", "cat").dictionaryId == "downloaded:en-en",
                "automatic book-language dictionary");
        require(manager.selectedDictionary("book").isEmpty(),
                "automatic choice is not a session override");
        int updates = 0;
        QObject::connect(&manager, &DictionaryManager::changed, [&] { ++updates; });
        require(manager.select("book", "downloaded:en-en", &error), "selection");
        require(manager.selectedDictionary("book") == "en-en" && updates == 1,
                "explicit session choice emits a change");
        require(manager.transfer().id.isEmpty(), "idle manager reports no active transfer");
        require(manager.lookup("book", "cats").html.toUtf8() == first, "manager lookup");
    }
    {
        DictionaryManager manager(root, std::make_unique<Empty>());
        require(manager.lookup("book", "cat").html.toUtf8() == first,
                "new manager chooses automatically without a saved selection");
        require(!manager.remove("../elsewhere", &error), "unsafe remove");
        require(manager.remove("en-en", &error), "remove");
        require(manager.choices().isEmpty() && !manager.installed("en-en"), "removed catalog");
        require(manager.installedSizes().isEmpty() && manager.selectedDictionary("book").isEmpty(),
                "removed dictionary has no storage or default badge");
    }
    {
        const auto orderedRoot = temp.path() + "/ordered";
        const QStringList ids{ "en-en", "en-fr", "en-ar", "ar-en", "ar-ar" };
        for (const auto &id : ids) {
            require(QDir().mkpath(orderedRoot + "/" + id), "make installed marker directory");
            save(orderedRoot + "/" + id + "/installed.json", "{\"id\":\"" + id.toUtf8() + "\"}");
        }
        DictionaryManager manager(orderedRoot, std::make_unique<NativeChoice>());
        manager.setLanguage(" EN_us ");
        const auto choices = manager.choices();
        const QStringList expected{ "downloaded:en-en", "downloaded:en-ar", "downloaded:en-fr",
                                    "native",           "downloaded:ar-ar", "downloaded:ar-en" };
        require(choices.size() == expected.size(), "all installed choices included once");
        for (int i = 0; i < choices.size(); ++i)
            require(choices[i].id == expected[i],
                    "book mono, translations, fallback, other languages");
        require(choices[0].language == "en" && !choices[0].fallback && choices[3].fallback,
                "lookup grouping metadata");
        manager.setLanguage("zz");
        require(manager.choices().first().id == "native",
                "unknown language prefers platform dictionary");
        manager.setLanguage(QString());
        require(manager.choices().first().id == "native",
                "empty language prefers platform dictionary");
        require(DictionaryManager::languageName("cs-CZ") == "Czech", "English language names");
        require(DictionaryManager::languageName("zz") == "zz",
                "unknown language names keep their code");
    }
    require(DictionaryManager::installArchive(root, entry, archive, &error),
            "reinstall session fixture");
    {
        const QByteArray oldSelection("{\"book\":\"native\"}");
        save(root + "/selection.json", oldSelection);
        DictionaryManager manager(root, std::make_unique<NativeChoice>());
        manager.setLanguage("en");
        require(manager.lookup("book", "cat").dictionaryId == "downloaded:en-en",
                "ignore legacy selection file");
        require(manager.select("book", "native", &error), "select native session dictionary");
        require(manager.lookup("book", "cat").dictionaryId == "native",
                "session selection overrides automatic rule");
        require(manager.lookup("book", "cat").dictionaryId == "native",
                "session selection holds on later lookup");
        require(manager.lookup("another-book", "cat").dictionaryId == "downloaded:en-en",
                "other book has no session override");
        QFile oldFile(root + "/selection.json");
        require(oldFile.open(QIODevice::ReadOnly) && oldFile.readAll() == oldSelection,
                "legacy selection file never written");
        require(manager.select("book", "downloaded:en-en", &error),
                "select downloaded session dictionary");
        require(manager.remove("en-en", &error), "remove session dictionary");
        require(manager.lookup("book", "cat").dictionaryId == "native",
                "removing session choice restores preference rule");
    }
    require(DictionaryManager::installArchive(root, entry, archive, &error),
            "reinstall fresh-manager fixture");
    {
        DictionaryManager manager(root, std::make_unique<NativeChoice>());
        manager.setLanguage("en");
        require(manager.lookup("book", "cat").dictionaryId == "downloaded:en-en",
                "fresh manager discards session choice");
        require(manager.remove("en-en", &error), "remove fresh-manager fixture");
    }
    require(!QFile::exists(temp.path() + "/new-root/selection.json"),
            "no selection file created for native choice");
    files.remove("en-en.syn");
    write();
    require(!DictionaryManager::installArchive(root, entry, archive, &error),
            "missing declared inflections rejected");
    files["en-en.syn"] = syn;
    files["en-en.idx"] = idx;
    files["en-en.idx"][7] = char(0xff);
    write();
    require(!DictionaryManager::installArchive(root, entry, archive, &error),
            "out of bounds index rejected");
    require(!QFile::exists(root + "/en-en"), "invalid dictionary invisible");
    if (argc == 2) {
        for (const auto &real : DictionaryManager::catalog())
            if (real.id == "ar-ar") {
                require(DictionaryManager::installArchive(root, real,
                                                          QString::fromLocal8Bit(argv[1]), &error),
                        qPrintable(error));
                StarDict arabic;
                require(arabic.open(root + "/ar-ar", "ar-ar", &error), qPrintable(error));
                QFile index(root + "/ar-ar/ar-ar.idx");
                require(index.open(QIODevice::ReadOnly), "Arabic index");
                const auto head = index.read(264).split('\0').first();
                require(!arabic.lookup(QString::fromUtf8(head)).html.isEmpty(),
                        "real Arabic lookup");
            }
    }
    qInfo("PASS: checksums, cancellation, atomic install, compressed lookup, inflections, Unicode, selection, removal, malformed indexes");
}
