#include "storage/custom_pins.h"
#include "check.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <unistd.h>

using namespace kobalt::storage;

namespace {
const QByteArray cat = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
                       "<path fill='black' d='M3 18 L3 3 L9 7 L15 7 L21 3 L21 18 Z'/></svg>";

void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
            "Cannot write SVG fixture");
}
} // namespace

int main()
{
    QElapsedTimer timer;
    timer.start();
    QFile realistic(KOBALT_TEST_CUSTOM_PIN_SVG);
    require(realistic.open(QIODevice::ReadOnly), "Cannot read the Lucide cat SVG fixture");
    const auto catSvg = realistic.readAll();
    const auto catMask = customPinMask(catSvg);
    require(!catMask.isNull(), "Unmodified Lucide cat SVG rejected");
    auto decorated = catSvg;
    decorated.replace(
            "<svg ",
            "<svg class='lucide lucide-cat' role='img' aria-label='Cat' data-export='icon' ");
    require(customPinMask(decorated) == catMask, "Harmless SVG export metadata changed the icon");
    const auto mask = customPinMask(cat);
    require(mask.size() == QSize(96, 96) && qAlpha(mask.pixel(48, 48)) > 0,
            "Basic path did not render");
    require(qAlpha(mask.pixel(0, 0)) == 0, "Transparent margin became opaque");
    const auto wide =
            customPinMask("<svg viewBox='0 0 40 20'><rect width='40' height='20'/></svg>");
    require(!wide.isNull() && qAlpha(wide.pixel(48, 0)) == 0 && qAlpha(wide.pixel(48, 48)) > 0,
            "Non-square viewBox stretched");
    require(!customPinMask(
                     "<svg viewBox='0 0 24 24'><g transform='translate(1 1)' fill='none' stroke='red' stroke-width='2'><circle cx='10' cy='10' r='5'/><ellipse cx='8' cy='8' rx='3' ry='2'/><line x1='0' y1='0' x2='5' y2='5'/><polyline points='0,0 5,5 5,10'/><polygon points='1,1 2,1 2,2'/></g></svg>")
                     .isNull(),
            "Basic shapes rejected");
    for (const auto &bad :
         { QByteArray("<svg>"), QByteArray("<svg viewBox='0 0 0 24'><path d='M0 0L24 24'/></svg>"),
           QByteArray("<svg viewBox='nan 0 24 24'><circle r='3'/></svg>"),
           QByteArray("<svg viewBox='0 0 1e-300 24'><circle r='3'/></svg>"),
           QByteArray(
                   "<svg viewBox='0 0 24 24'><path transform='scale(10000) scale(10000)' d='M0 0C1 5 8 4 10 10Z'/></svg>"),
           QByteArray(
                   "<svg viewBox='0 0 24 24'><path transform='scale(1) scale(1) scale(1) scale(1) scale(1) scale(1) scale(1) scale(1) scale(1)' d='M0 0L24 24Z'/></svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><circle r='nan'/></svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><image href='file:///private/image.png'/></svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><use href='#cat'/></svg>"),
           QByteArray(
                   "<svg viewBox='0 0 24 24'><path style='fill:url(https://example.org)'/></svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><animate attributeName='x'/></svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><text>Cat</text></svg>"),
           QByteArray("<!DOCTYPE svg [<!ENTITY x 'boom'>]><svg viewBox='0 0 24 24'>&x;</svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><path d='M1e999 0L1 1'/></svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><path d='M0 0L24 24' stroke='url(#a)'/></svg>"),
           QByteArray("<svg viewBox='0 0 24 24'><rect width='10' height='10' opacity='0'/></svg>"),
           QByteArray(maxCustomPinBytes + 1, 'x') })
        require(customPinMask(bad).isNull(), "Unsafe, malformed or empty icon accepted");
    require(!customPinMask(
                     "<svg viewBox='0 0 24 24'><title>Cat</title><rect width='12' height='12' fill='#123456'/></svg>")
                     .isNull(),
            "Hex colors or title metadata rejected");
    QByteArray deep("<svg viewBox='0 0 24 24'>");
    for (int i = 0; i < 9; ++i)
        deep += "<g>";
    deep += "<circle r='3'/>";
    for (int i = 0; i < 9; ++i)
        deep += "</g>";
    require(customPinMask(deep + "</svg>").isNull(), "Nesting limit ignored");
    QByteArray many("<svg viewBox='0 0 24 24'>");
    for (int i = 0; i < 128; ++i)
        many += "<circle r='3'/>";
    require(customPinMask(many + "</svg>").isNull(), "Element limit ignored");

    QTemporaryDir root;
    require(root.isValid(), "No test directory");
    require(readCustomPins(root.path() + "/missing").icons.isEmpty(), "Missing folder not empty");
    const auto pins = root.path() + "/custom-pins";
    require(QDir().mkpath(pins), "No pin folder");
    write(pins + "/cat.svg", cat);
    write(pins + "/bad.svg", "not XML");
    write(pins + "/too-big.svg", QByteArray(maxCustomPinBytes + 1, 'x'));
    require(QFile::link(pins + "/cat.svg", pins + "/link.svg"), "Cannot make link");
    require(::mkfifo((pins + "/pipe.svg").toUtf8().constData(), 0600) == 0, "Cannot make FIFO");
    auto catalog = readCustomPins(pins);
    require(catalog.icons.size() == 1 && catalog.skipped == 4, "Unsafe files not skipped");
    require(catalog.failedFiles
                    == QStringList({ "bad.svg", "link.svg", "pipe.svg", "too-big.svg" }),
            "Failed filenames not retained");
    for (int i = 0; i < 7; ++i)
        write(pins + QString("/broken-%1.svg").arg(i), "not XML");
    const auto failures = readCustomPins(pins);
    require(failures.skipped == 11 && failures.failedFiles.size() == 5
                    && failures.failedFiles.first() == "bad.svg"
                    && failures.failedFiles.last() == "broken-3.svg",
            "Failed filename list not bounded or sorted");
    require(catalog.find("custom:cat.svg") && !catalog.find("custom:../cat.svg"),
            "Unstable or unsafe identity");
    const auto previous = catalog.icons.first().mask;
    write(pins + "/cat.svg", "<svg viewBox='0 0 24 24'><circle cx='12' cy='12' r='4'/></svg>");
    auto replaced = readCustomPins(pins);
    require(replaced.icons.first().id == catalog.icons.first().id
                    && replaced.icons.first().mask != previous,
            "Replacement lost identity or kept stale image");
    require(QFile::remove(pins + "/cat.svg") && readCustomPins(pins).icons.isEmpty(),
            "Removed icon cached across discovery");
    require(catalog.find("custom:cat.svg")->mask == previous,
            "Live catalog changed after file removal");
    write(pins + "/cat.svg", cat);
    require(readCustomPins(pins).find("custom:cat.svg") != nullptr,
            "Restoring the same filename did not restore its identity");
    require(QFile::remove(pins + "/cat.svg"), "Cannot remove restored fixture");
    require(QFile::link(pins, root.path() + "/linked-pins"), "Cannot link folder");
    require(readCustomPins(root.path() + "/linked-pins").icons.isEmpty(),
            "Symlink folder followed");
    for (int i = 0; i < 130; ++i)
        write(pins + QString("/icon%1.svg").arg(i, 3, 10, QLatin1Char('0')), cat);
    catalog = readCustomPins(pins);
    require(catalog.icons.size() == maxCustomPins && catalog.truncated
                    && catalog.icons.first().id == "custom:icon000.svg",
            "Catalog count or order wrong");
    for (int i = 0; i < 300; ++i)
        write(pins + QString("/extra%1.txt").arg(i), "ignored");
    require(readCustomPins(pins).truncated, "Directory scan limit ignored");
    std::printf("Custom SVG parsing, discovery and replacement checks: %lld ms\n", timer.elapsed());
}
