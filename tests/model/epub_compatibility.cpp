#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QtEndian>
#include "publication/zip_resource.h"
#include <QDir>
#include "publication/publication.h"

using namespace kobalt;

namespace {
// Move the central directory and all resources after mimetype past the ZIP32
// offset limit. A sparse file gives the reader real 64-bit seeks without a large fixture.
void checkLargeArchiveOffsets(QByteArray bytes)
{
    const auto read32 = [&](int p) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + p));
    };
    const auto read64 = [&](int p) {
        return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(bytes.constData() + p));
    };
    const auto set64 = [&](int p, quint64 value) {
        qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + p));
    };
    const auto set32 = [&](int p, quint32 value) {
        qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + p));
    };
    const int eocd = bytes.size() - 22, locator = eocd - 20, record = int(read64(locator + 8));
    const int directory = int(read64(record + 48));
    const quint64 gap = (quint64(1) << 32) + 7;
    int first = bytes.indexOf("PK\x03\x04", 4);
    require(first > 0, "Missing second ZIP fixture member");
    for (int p = directory; p < record;) {
        const auto read16 = [&](int offset) {
            return qFromLittleEndian<quint16>(
                    reinterpret_cast<const uchar *>(bytes.constData() + offset));
        };
        const int extra = p + 46 + read16(p + 28);
        if (read32(p + 42) == 0xffffffff) {
            require(read16(extra) == 1, "Fixture must place the ZIP64 field first");
            const int at = extra + 4 + 8 * (read32(p + 24) == 0xffffffff)
                    + 8 * (read32(p + 20) == 0xffffffff);
            set64(at, read64(at) + gap);
        } else {
            require(read32(p + 42) == 0, "Only mimetype may use a small offset in this fixture");
        }
        p = extra + read16(p + 30) + read16(p + 32);
    }
    set64(record + 48, gap + directory);
    set64(locator + 8, gap + record);
    set32(eocd + 16, 0xffffffff);
    QTemporaryFile large;
    require(large.open() && large.write(bytes.constData(), first) == first
                    && large.seek(gap + first)
                    && large.write(bytes.constData() + first, bytes.size() - first)
                            == bytes.size() - first
                    && large.flush(),
            "Could not create sparse ZIP64 fixture");
    QString error;
    Publication publication;
    require(publication.inspect(large, &error), qPrintable(error));
    ZipResource chapter;
    require(chapter.open(large, "chapter.xml") && chapter.next().contains("kingfisher")
                    && chapter.valid() && chapter.finished(),
            "ZIP64 local offset was truncated");
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // This independent ZIP writer fixture has ZIP64 member sizes, offsets, end record and locator.
    QFile fixture(
            QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("../fixtures/zip64-dtbook.epub"));
    require(fixture.open(QIODevice::ReadOnly), "Missing ZIP64 fixture");
    const auto bytes = fixture.readAll();
    QString error;
    Publication publication;
    require(publication.inspect(bytes, &error) && publication.zip64(), qPrintable(error));
    checkLargeArchiveOffsets(bytes);
    auto broken = bytes;
    broken[broken.size() - 22 - 20 + 8] = char(0xff);
    require(!publication.inspect(broken, &error), "Invalid ZIP64 locator admitted");
    return 0;
}
