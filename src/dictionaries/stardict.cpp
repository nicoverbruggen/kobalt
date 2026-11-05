#include "stardict.h"
#include "support/main_thread.h"
#include <QFile>
#include <QDir>
#include <QtEndian>
#include <QMap>
#include <QStringList>
#include <QSaveFile>
#include <zlib.h>
#include <cstring>

namespace kobalt {
namespace {
QString sourceSummary(const QString &attribution)
{
    QStringList sources;
    bool inSources = false;
    for (auto line : attribution.split('\n')) {
        line = line.trimmed();
        if (line == "It is built from the following sources:") {
            inSources = true;
            continue;
        }
        if (inSources && line.startsWith("-- "))
            break;
        if (!inSources || !line.startsWith("* "))
            continue;
        const int colon = line.indexOf(':', 2);
        if (colon < 0)
            continue;
        const auto id = line.mid(2, colon - 2).trimmed();
        auto name = line.mid(colon + 1).simplified();
        // Keep the originating project, rather than the archive's import path.
        name = name.section(", via ", 0, 0).section(" via ", 0, 0).trimmed();
        if (id == "pwn30")
            name = "WordNet 3.0";
        if (!name.isEmpty() && !sources.contains(name))
            sources.append(name);
    }
    return sources.join(QString::fromUtf8(" · "));
}

quint32 be32(const char *p)
{
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(p));
}

quint16 le16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

quint32 le32(const char *p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p));
}

QByteArray fold(QByteArray text)
{
    for (auto &c : text)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return text;
}

int compare(const QByteArray &a, const QByteArray &b)
{
    const int primary = std::strcmp(fold(a).constData(), fold(b).constData());
    return primary ? primary : std::strcmp(a.constData(), b.constData());
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

struct Mapped
{
    QFile file;
    uchar *bytes = nullptr;

    explicit Mapped(const QString &path) : file(path)
    {
        if (file.open(QIODevice::ReadOnly) && file.size() > 0)
            bytes = file.map(0, file.size());
    }

    ~Mapped()
    {
        if (bytes)
            file.unmap(bytes);
    }
};

struct Body
{
    QFile file;
    QVector<quint32> chunks;
    int chunkBytes = 0;
    quint32 length = 0;

    bool open(const QString &path)
    {
        file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        if (!path.endsWith(".dz")) {
            if (file.size() <= 0 || file.size() > 0xffffffffLL)
                return false;
            length = file.size();
            return true;
        }
        const auto header = file.read(12);
        if (header.size() != 12 || uchar(header[0]) != 31 || uchar(header[1]) != 139
            || header[2] != 8 || (uchar(header[3]) & ~quint8(4 | 8 | 16)) || !(header[3] & 4))
            return false;
        const auto extra = file.read(le16(header.constData() + 10));
        if (extra.size() != le16(header.constData() + 10))
            return false;
        QVector<quint16> sizes;
        for (int at = 0; at + 4 <= extra.size();) {
            const int size = le16(extra.constData() + at + 2);
            if (at + 4 + size > extra.size())
                return false;
            if (extra.mid(at, 2) == "RA") {
                if (!sizes.isEmpty() || size < 6 || le16(extra.constData() + at + 4) != 1)
                    return false;
                chunkBytes = le16(extra.constData() + at + 6);
                const int count = le16(extra.constData() + at + 8);
                if (!chunkBytes || !count || size != 6 + count * 2)
                    return false;
                for (int i = 0; i < count; ++i)
                    sizes.append(le16(extra.constData() + at + 10 + i * 2));
            }
            at += 4 + size;
        }
        for (int flag : { 8, 16 })
            if (header[3] & flag) {
                bool ended = false;
                for (int i = 0; i < 4096; ++i) {
                    char c;
                    if (!file.getChar(&c))
                        return false;
                    if (!c) {
                        ended = true;
                        break;
                    }
                }
                if (!ended)
                    return false;
            }
        quint64 offset = file.pos();
        for (int size : sizes) {
            if (!size || offset > 0xffffffffULL)
                return false;
            chunks.append(offset);
            offset += size;
        }
        if (chunks.isEmpty() || offset >= quint64(file.size() - 8))
            return false;
        chunks.append(offset);
        if (!file.seek(file.size() - 4))
            return false;
        const auto footer = file.read(4);
        if (footer.size() != 4)
            return false;
        length = le32(footer.constData());
        return length > quint64(sizes.size() - 1) * chunkBytes
                && length <= quint64(sizes.size()) * chunkBytes;
    }

    QByteArray read(quint32 offset, quint32 size)
    {
        if (!size || size > 4 * 1024 * 1024 || quint64(offset) + size > length)
            return {};
        if (chunks.isEmpty()) {
            if (!file.seek(offset))
                return {};
            return file.read(size);
        }
        QByteArray result;
        const quint32 end = offset + size;
        for (quint32 index = offset / chunkBytes; index <= (end - 1) / chunkBytes; ++index) {
            if (!file.seek(chunks[index]))
                return {};
            auto input = file.read(chunks[index + 1] - chunks[index]);
            QByteArray output(chunkBytes + 1, '\0');
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                return {};
            stream.next_in = reinterpret_cast<Bytef *>(input.data());
            stream.avail_in = input.size();
            stream.next_out = reinterpret_cast<Bytef *>(output.data());
            stream.avail_out = output.size();
            const int status = inflate(&stream, Z_SYNC_FLUSH);
            const auto count = stream.total_out;
            const auto left = stream.avail_in;
            inflateEnd(&stream);
            const quint32 expected = qMin<quint32>(chunkBytes, length - index * chunkBytes);
            if ((status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) || left
                || count != expected)
                return {};
            const quint32 start = qMax(offset, index * chunkBytes) - index * chunkBytes;
            const quint32 take =
                    qMin(end, index * chunkBytes + expected) - index * chunkBytes - start;
            result.append(output.constData() + start, take);
        }
        return result;
    }
};

bool index(const QString &path, int suffix, quint32 bodyLength, quint32 wordCount, QString *error,
           const std::function<bool()> &cancelled, quint32 expectedCount)
{
    QFile file(path);
    mainthread::BlockingOperation blockingOperation("dictionary-index-write");
    QSaveFile positions(path + ".kobalt");
    if (!file.open(QIODevice::ReadOnly) || !positions.open(QIODevice::WriteOnly))
        return fail(error, "Cannot read dictionary index.");
    QByteArray previous;
    quint32 count = 0;
    while (!file.atEnd()) {
        if (cancelled && cancelled())
            return fail(error, "Installation cancelled.");
        const qint64 position = file.pos();
        if (position > 0xffffffffLL)
            return fail(error, "Dictionary index is too large.");
        QByteArray word;
        bool ended = false;
        for (int i = 0; i < 256; ++i) {
            char c;
            if (!file.getChar(&c))
                break;
            if (!c) {
                ended = true;
                break;
            }
            word.append(c);
        }
        const auto value = file.read(suffix);
        if (!ended || word.isEmpty() || QString::fromUtf8(word).toUtf8() != word
            || value.size() != suffix || (!previous.isEmpty() && compare(previous, word) > 0))
            return fail(error, "Invalid StarDict index.");
        if (suffix == 8
                    ? quint64(be32(value.constData())) + be32(value.constData() + 4) > bodyLength
                    : be32(value.constData()) >= wordCount)
            return fail(error, "Dictionary index points outside its data.");
        char bytes[4];
        qToBigEndian<quint32>(position, reinterpret_cast<uchar *>(bytes));
        if (positions.write(bytes, 4) != 4)
            return fail(error, "Cannot save dictionary index.");
        previous = word;
        ++count;
    }
    if (count != expectedCount)
        return fail(error, "StarDict word count differs from its index.");
    if (!positions.commit())
        return fail(error, "Cannot save dictionary index.");
    return true;
}

qint64 find(Mapped &offsets, QFile &indexFile, const QByteArray &word)
{
    const auto wanted = fold(word);
    // Prefer the spelling in the book before folding case, so “set” does not resolve to “SET”.
    for (bool exact : { true, false }) {
        qint64 low = 0, high = offsets.file.size() / 4;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (!indexFile.seek(be32(reinterpret_cast<char *>(offsets.bytes) + middle * 4)))
                return -1;
            const auto line = indexFile.read(264);
            const int zero = line.indexOf('\0');
            if (zero <= 0)
                return -1;
            const auto candidate = line.left(zero);
            const int order = exact ? compare(candidate, word)
                                    : std::strcmp(fold(candidate).constData(), wanted.constData());
            if (order < 0)
                low = middle + 1;
            else
                high = middle;
        }
        if (low >= offsets.file.size() / 4)
            continue;
        if (!indexFile.seek(be32(reinterpret_cast<char *>(offsets.bytes) + low * 4)))
            return -1;
        const auto line = indexFile.read(264);
        const int zero = line.indexOf('\0');
        if (zero > 0 && (exact ? line.left(zero) == word : fold(line.left(zero)) == wanted))
            return low;
    }
    return -1;
}
} // namespace

struct StarDict::Data
{
    QString id, name, source;
    Body body;
    QFile idx, syn;
    std::unique_ptr<Mapped> words, synonyms;
};

StarDict::StarDict() = default;
StarDict::~StarDict() = default;

bool StarDict::prepare(const QString &directory, const QString &id, QString *error,
                       const std::function<bool()> &cancelled)
{
    const auto prefix = QDir(directory).filePath(id);
    QFile ifo(prefix + ".ifo");
    if (!ifo.open(QIODevice::ReadOnly) || ifo.size() > 65536)
        return fail(error, "Missing StarDict metadata.");
    const auto text = ifo.readAll();
    if (!text.startsWith("StarDict's dict ifo file\n"))
        return fail(error, "Invalid StarDict metadata.");
    QMap<QByteArray, QByteArray> values;
    for (const auto &line : text.split('\n')) {
        int at = line.indexOf('=');
        if (at > 0)
            values[line.left(at)] = line.mid(at + 1).trimmed();
    }
    bool valid = false;
    const quint32 words = values["wordcount"].toUInt(&valid);
    if (!valid || !words || values["version"] != "2.4.2" || values["sametypesequence"] != "h")
        return fail(error, "Unsupported StarDict format.");
    const auto indexSize = values["idxfilesize"].toULongLong(&valid);
    if (!valid || indexSize != quint64(QFile(prefix + ".idx").size()))
        return fail(error, "StarDict index size differs.");
    Body body;
    if (!body.open(prefix + ".dict.dz"))
        return fail(error, "Invalid dictzip data.");
    if (!index(prefix + ".idx", 8, body.length, words, error, cancelled, words))
        return false;
    const quint32 synonyms = values.value("synwordcount", "0").toUInt(&valid);
    if (!valid || (synonyms && !QFile::exists(prefix + ".syn")))
        return fail(error, "Missing StarDict inflection index.");
    if (QFile::exists(prefix + ".syn")
        && !index(prefix + ".syn", 4, body.length, words, error, cancelled, synonyms))
        return false;
    return true;
}

bool StarDict::open(const QString &directory, const QString &id, QString *error)
{
    data.reset();
    auto next = std::make_unique<Data>();
    next->id = id;
    next->name = id;
    const auto prefix = QDir(directory).filePath(id);
    next->idx.setFileName(prefix + ".idx");
    next->syn.setFileName(prefix + ".syn");
    next->words = std::make_unique<Mapped>(prefix + ".idx.kobalt");
    if (!next->words->bytes || next->words->file.size() % 4 || !next->idx.open(QIODevice::ReadOnly)
        || !next->body.open(prefix + ".dict.dz"))
        return fail(error, "Installed dictionary is damaged.");
    if (next->syn.exists() && next->syn.size() > 0) {
        next->synonyms = std::make_unique<Mapped>(prefix + ".syn.kobalt");
        if (!next->synonyms->bytes || next->synonyms->file.size() % 4
            || !next->syn.open(QIODevice::ReadOnly))
            return fail(error, "Installed inflection index is damaged.");
    }
    QFile credits(QDir(directory).filePath("ATTRIBUTION"));
    if (credits.open(QIODevice::ReadOnly))
        next->source = sourceSummary(QString::fromUtf8(credits.read(65536))).toHtmlEscaped();
    data = std::move(next);
    return true;
}

DictionaryResult StarDict::lookup(const QString &word)
{
    DictionaryResult result;
    result.word = word;
    if (!data) {
        result.error = "No dictionary installed.";
        return result;
    }
    result.received = true;
    result.dictionary = data->name;
    result.dictionaryId = "downloaded:" + data->id;
    result.source = data->source;
    auto number = find(*data->words, data->idx, word.toUtf8());
    if (number < 0 && data->synonyms) {
        const auto synonym = find(*data->synonyms, data->syn, word.toUtf8());
        if (synonym >= 0
            && data->syn.seek(
                    be32(reinterpret_cast<char *>(data->synonyms->bytes) + synonym * 4))) {
            const auto entry = data->syn.read(260);
            const int zero = entry.indexOf('\0');
            if (zero > 0 && zero + 5 <= entry.size())
                number = be32(entry.constData() + zero + 1);
        }
    }
    if (number < 0)
        return result;
    if (number >= data->words->file.size() / 4
        || !data->idx.seek(be32(reinterpret_cast<char *>(data->words->bytes) + number * 4))) {
        result.error = "Dictionary index is damaged.";
        return result;
    }
    const auto entry = data->idx.read(264);
    const int zero = entry.indexOf('\0');
    if (zero <= 0 || zero + 9 > entry.size()) {
        result.error = "Dictionary index is damaged.";
        return result;
    }
    const auto html =
            data->body.read(be32(entry.constData() + zero + 1), be32(entry.constData() + zero + 5));
    if (html.isEmpty())
        result.error = "Dictionary definition is damaged.";
    else
        result.html = QString::fromUtf8(html);
    return result;
}
} // namespace kobalt
