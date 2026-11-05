#include "assets/assets.h"
#include "assets/font_catalog.h"
#include "assets/system_fonts.h"
#include "engine/crengine/engine_document.h"
#include "reader_inputs.h"
#include "font_fixtures.h"
#include "support/qt_compat.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace kobalt;

static void expect(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

// Checks font-source precedence, KF/NV collection pairs and hidden KF rows that still render under their saved name.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    expect(argc == 3, "supply a bundled font fixture and a book");
    const QVector<ReadingFont> unordered{ { "Zeta", {}, FontOrigin::Recommended, {}, {} },
                                          { "Personal", {}, FontOrigin::Sideloaded, {}, {} },
                                          { "Alpha", {}, FontOrigin::Recommended, {}, {} },
                                          { "Libron", {}, FontOrigin::Recommended, {}, {} },
                                          { "System", {}, FontOrigin::BuiltIn, {}, {} },
                                          { "Mono", {}, FontOrigin::Interface, {}, {} } };
    const auto ordered = orderedReadingFonts(unordered);
    const QStringList expectedOrder{ "Libron", "Alpha", "Zeta", "System", "Personal", "Mono" };
    for (int index = 0; index < ordered.size(); ++index)
        expect(ordered[index].family == expectedOrder[index],
               "font pickers share source groups and Libron-first ordering");
    const QVector<ReadingFont> variants{ { "NV Bitter", {}, FontOrigin::Recommended, {}, {} },
                                         { "KF Bitter", {}, FontOrigin::Sideloaded, {}, {} },
                                         { "Sourcerer", {}, FontOrigin::Recommended, {}, {} },
                                         { "KF Sourcerer", {}, FontOrigin::Sideloaded, {}, {} },
                                         { "KF Unique", {}, FontOrigin::Sideloaded, {}, {} },
                                         { "Other KF Bitter", {}, FontOrigin::Sideloaded, {}, {} },
                                         { "KF Unrelated", {}, FontOrigin::Sideloaded, {}, {} },
                                         { "Unrelated", {}, FontOrigin::Sideloaded, {}, {} } };
    const auto families = [](const QVector<ReadingFont> &fonts) {
        QStringList result;
        for (const auto &font : fonts)
            result.append(font.family);
        return result;
    };
    const auto filtered = families(orderedReadingFonts(variants));
    expect(!filtered.contains("KF Bitter") && !filtered.contains("KF Sourcerer"),
           "equivalent NV and unprefixed collection counterparts hide KF rows");
    expect(filtered.contains("KF Unique") && filtered.contains("Other KF Bitter")
                   && filtered.contains("KF Unrelated") && filtered.contains("Unrelated"),
           "KF-only and unrelated names remain available");
    expect(families(orderedReadingFonts(variants, { "KF Bitter" })).contains("KF Bitter"),
           "selected KF family remains an exact selectable row");
    expect(families(orderedReadingFonts(variants, { "KF Bitter", "KF Sourcerer" }))
                   .contains("KF Sourcerer"),
           "reading and default selections can both remain visible");
    expect(variants.size() == 8 && fontFamilyKey("KF Bitter") != fontFamilyKey("NV Bitter"),
           "picker filtering neither mutates the catalog nor aliases saved font identity");
    QTemporaryDir temporary;
    expect(temporary.isValid(), "temporary directory");
    const auto root = temporary.path();
    for (const auto &directory : { "ebook-core", "system", "sideload" }) {
        expect(QDir().mkpath(root + '/' + directory), "font source directory");
        expect(QFile::copy(QString::fromLocal8Bit(argv[1]),
                           root + '/' + directory + "/Regular.ttf"),
               "copy bundled font fixture");
    }
    const auto previous = QDir::currentPath();
    expect(QDir::setCurrent(root + "/system"), "select populated working directory");
    expect(discoverReadingFonts(FontSources{}).isEmpty(),
           "empty font sources must not scan working directory");
    expect(QDir::setCurrent(previous), "restore working directory");
    FontSources sources{ { root + "/ebook-core/Regular.ttf" },
                         root + "/system",
                         root + "/sideload" };
    const auto catalog = discoverReadingFonts(sources);
    expect(catalog.size() == 1, "duplicate families respect source precedence");
    expect(catalog.first().origin == FontOrigin::Recommended,
           "bundled reading font takes precedence");
    expect(catalog.first().files == sources.bundled,
           "lower-priority files do not mix with bundled family");
    sources.bundled.clear();
    auto system = discoverReadingFonts(sources);
    expect(system.size() == 1 && system.first().origin == FontOrigin::BuiltIn,
           "explicit system source takes precedence over sideload");
    QFile original(root + "/system/Regular.ttf");
    expect(original.open(QIODevice::ReadOnly), "read system font before wrapping");
    const auto wrapped = "QTD" + qCompress(original.readAll());
    original.close();
    expect(original.open(QIODevice::WriteOnly | QIODevice::Truncate)
                   && original.write(wrapped) == wrapped.size(),
           "wrap system font as Kobo firmware does");
    original.close();
    kobalt::FontCopies copies;
    sources.systemFiles = systemFontFiles(sources.systemDirectory, copies);
    sources.systemDirectory.clear();
    system = discoverReadingFonts(sources);
    expect(system.size() == 1 && system.first().origin == FontOrigin::BuiltIn
                   && system.first().files == sources.systemFiles,
           "decoded firmware font remains built-in and takes precedence over sideload");
    const auto systemFamily = system.first().family;
    auto inputs = test::readerFonts();
    inputs.sources.systemFiles = sources.systemFiles;
    const auto kfDirectory = root + "/kf-fonts";
    expect(QDir().mkpath(kfDirectory), "create KF font fixture directory");
    expect(test::renamedFontFixture(
                   test::readerAssets().file("fonts/ebook-core/NV_Bitter-Regular.ttf"),
                   kfDirectory + "/KF_Bitter.ttf", "NV Bitter", "KF Bitter"),
           "create name-only KF catalog fixture");
    inputs.sources.sideloadDirectory = kfDirectory;
    EngineDocument model(inputs);
    bool registered = false;
    for (const auto &font : model.fontCatalog()) {
        if (font.family == systemFamily) {
            registered = font.origin == FontOrigin::BuiltIn && font.files == sources.systemFiles;
        }
    }
    for (const auto *family : { monospaceFontFace, mathFontFace }) {
        bool previewable = false;
        for (const auto &font : model.fontCatalog())
            previewable |= font.family == family && !font.files.isEmpty();
        expect(previewable, "bundled fallback family has files for its font-name preview");
    }
    expect(registered, "engine registers the prepared font as built-in");
    QFile book(QString::fromLocal8Bit(argv[2]));
    expect(book.open(QIODevice::ReadOnly), "open book fixture");
    QString error;
    expect(model.open(book.readAll(), QSize(800, 1000),
                      qt::object({ { "font_face", systemFamily } }), &error),
           qPrintable(error));
    expect(model.fontFace() == systemFamily && !model.image(model.page()).isNull(),
           "prepared firmware font can be selected and render a book page");
    expect(families(model.fontCatalog()).contains("KF Bitter")
                   && !families(orderedReadingFonts(model.fontCatalog())).contains("KF Bitter"),
           "hidden KF family remains registered in the rendering catalog");
    book.seek(0);
    expect(model.open(book.readAll(), QSize(800, 1000),
                      qt::object({ { "font_face", "KF Bitter" } }), &error),
           qPrintable(error));
    expect(model.fontFace() == "KF Bitter"
                   && model.state().value("font_face").toString() == "KF Bitter"
                   && !model.image(model.page()).isNull(),
           "saved KF choice retains its identity and renders");
    auto changed = sources;
    changed.systemFiles.clear();
    expect(!(changed == sources), "prepared font paths participate in source identity");
    sources.systemFiles.clear();
    auto sideload = discoverReadingFonts(sources);
    expect(sideload.size() == 1 && sideload.first().origin == FontOrigin::Sideloaded,
           "disabled system source leaves sideload");

    const auto nested = root + "/nested";
    QFile fixture(QString::fromLocal8Bit(argv[1]));
    expect(fixture.open(QIODevice::ReadOnly), "read font fixture for nested folders");
    const auto fontBytes = fixture.readAll();
    const auto place = [&](const QString &relative, const QByteArray &bytes) {
        const auto path = nested + '/' + relative;
        expect(QDir().mkpath(QFileInfo(path).absolutePath()), "create nested font folder");
        QFile file(path);
        expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
               "write nested font");
        return path;
    };
    const QStringList found{ place("Root.ttf", fontBytes), place("Family/Regular.otf", fontBytes),
                             place("Family/static/Bold.ttf", fontBytes) };
    place("Family/static/deeper/Italic.ttf", fontBytes);
    place(".hidden/Regular.ttf", fontBytes);
    place("Family/._Regular.otf", fontBytes);
    place("kobo/Encrypted.ttf", "QTE" + QByteArray(32, 'x'));
    place("kobo/Compressed.ttf", "QTD" + qCompress(fontBytes));
    expect(sideloadedFontFiles(nested) == found,
           "sideload scan reaches two folder levels and skips hidden and wrapped fonts");
    expect(QDir::setCurrent(nested), "select populated working directory");
    expect(sideloadedFontFiles({}).isEmpty(),
           "empty sideload source never scans working directory");
    expect(QDir::setCurrent(previous), "restore working directory");
    const auto nestedCatalog = discoverReadingFonts(FontSources{ {}, {}, nested });
    expect(nestedCatalog.size() == 1 && nestedCatalog.first().origin == FontOrigin::Sideloaded
                   && nestedCatalog.first().files == found,
           "nested sideloaded files join one family");
}
