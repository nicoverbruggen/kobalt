#include "assets/assets.h"
#include "assets/system_fonts.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

namespace {
void expect(bool value, const char *message)
{
    if (!value) {
        qFatal("%s", message);
    }
}

QByteArray read(const QString &path)
{
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "open font fixture or prepared copy");
    return file.readAll();
}

void write(const QString &path, const QByteArray &bytes)
{
    expect(QDir().mkpath(QFileInfo(path).absolutePath()), "create font directory");
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
           "write font fixture");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    expect(argc == 2, "supply an ordinary font fixture");
    const auto fontBytes = read(QString::fromLocal8Bit(argv[1]));
    QTemporaryDir temporary;
    expect(temporary.isValid(), "temporary font sources");
    const auto root = temporary.path();
    const auto compressed = "QTD" + qCompress(fontBytes, 9);
    const QByteArray encrypted = "QTE" + QByteArray(32, 'x');
    const auto plain = root + "/plain/Regular.ttf";
    write(plain, fontBytes);
    kobalt::FontCopies copies;
    expect(kobalt::systemFontFiles(root + "/plain", copies) == QStringList{ plain },
           "ordinary font keeps its original file");
    const auto previous = QDir::currentPath();
    expect(QDir::setCurrent(root + "/plain"), "select populated working directory");
    expect(kobalt::systemFontFiles({}, copies).isEmpty(),
           "empty source never scans working directory");
    expect(QDir::setCurrent(previous), "restore working directory");

    const auto original = root + "/compressed/Regular.ttf";
    write(original, compressed);
    auto prepared = kobalt::systemFontFiles(root + "/compressed", copies);
    expect(prepared.size() == 1 && prepared.first() != original,
           "compressed font gets a private copy");
    expect(read(prepared.first()) == fontBytes && read(original) == compressed,
           "decompression preserves the font and leaves the original unchanged");
    expect(kobalt::systemFontFiles(root + "/compressed", copies) == prepared,
           "repeated preparation retains stable paths");

    const auto qtePath = root + "/encrypted/Regular.ttf";
    write(qtePath, encrypted);
    expect(kobalt::systemFontFiles(root + "/encrypted", copies).isEmpty(),
           "encrypted fonts remain unavailable");
    expect(read(qtePath) == encrypted, "encrypted font is unchanged");

    auto oversized = compressed;
    qToBigEndian<quint32>(32 * 1024 * 1024 + 1, reinterpret_cast<uchar *>(oversized.data() + 3));
    auto mismatch = compressed;
    qToBigEndian<quint32>(fontBytes.size() - 1, reinterpret_cast<uchar *>(mismatch.data() + 3));
    auto corrupt = compressed;
    corrupt[corrupt.size() - 1] = char(corrupt.at(corrupt.size() - 1) ^ 1);
    const QList<QByteArray> invalid{
        QByteArray(), "QTD",     compressed.left(7), compressed.left(compressed.size() - 1),
        corrupt,      oversized, mismatch,           compressed + "trailing",
        "QTE",        "QTEbad"
    };
    for (int i = 0; i < invalid.size(); ++i) {
        const auto directory = root + "/malformed/" + QString::number(i);
        write(directory + "/Regular.ttf", invalid[i]);
        expect(kobalt::systemFontFiles(directory, copies).isEmpty(),
               "malformed wrapper is rejected");
    }

    QString removedCopy;
    {
        kobalt::FontCopies scopedCopies;
        removedCopy = kobalt::systemFontFiles(root + "/compressed", scopedCopies).first();
        expect(QFile::exists(removedCopy), "prepared font exists while its owner lives");
    }
    expect(!QFile::exists(removedCopy), "temporary font is removed with its owner");
}
