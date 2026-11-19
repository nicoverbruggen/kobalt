#include "check.h"
#include "engine/blaze/adapter/engine_document.h"
#include "reader_inputs.h"
#include <lvfntman.h>
#include <hyphman.h>
#include <textlang.h>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QXmlStreamReader>

namespace {
void compile(const QString &source, const QString &destination)
{
    auto xml = LVOpenFileStream(QFile::encodeName(source).constData(), LVOM_READ);
    auto output = LVCreateMemoryStream(nullptr, 0, false, LVOM_WRITE);
    require(HyphMan::compilePatterns(xml, output), qPrintable("Cannot compile " + source));
    QByteArray bytes(output->GetSize(), char(0));
    lvsize_t read;
    require(output->SetPos(0) == 0 && output->Read(bytes.data(), bytes.size(), &read) == LVERR_OK
                    && read == lvsize_t(bytes.size()),
            "Cannot read the compiled dictionary");
    QSaveFile file(destination);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
            "Cannot save the compiled dictionary");
}

QByteArray breaks(HyphMethod *method, const QString &text, int available)
{
    auto word = Utf8ToUnicode(text.toUtf8().constData());
    require(word.length() < WORD_LENGTH, "Oversize test word");
    lUInt16 widths[WORD_LENGTH] = {};
    lUInt8 flags[WORD_LENGTH] = {};
    for (int i = 0; i < word.length(); ++i)
        widths[i] = (i + 1) * 10;
    method->hyphenate(word.c_str(), word.length(), widths, flags, 5, available);
    return QByteArray(reinterpret_cast<const char *>(flags), word.length());
}

struct Dictionary
{
    const char *name;
    const char *language;
    int leftMin;
    int rightMin;
    const char *word;
};

const Dictionary packedDictionaries[] = {
    { "Dutch", "nl", 2, 2, "internationaal" },
    { "English_GB", "en-GB", 2, 3, "constitutional" },
    { "English_US", "en-US", 2, 3, "constitutional" },
    { "Finnish", "fi", 2, 2, "tietokonejärjestelmä" },
    { "German", "de", 2, 2, "Donaudampfschifffahrt" },
    { "Italian", "it", 2, 2, "internazionale" },
    { "Portuguese", "pt", 2, 3, "internacionalização" },
    { "Portuguese_BR", "pt-BR", 2, 3, "internacionalização" },
    { "Spanish", "es", 2, 2, "internacionalización" },
};

struct Methods
{
    HyphMethod *reader;
    HyphMethod *xml;

    void compare(const QString &word, int available = 1000) const
    {
        require(breaks(reader, word, available) == breaks(xml, word, available),
                qPrintable("Hyphenation differs for " + word));
    }
};

Methods methodsFor(const Dictionary &dictionary, const QString &source)
{
    const auto name = QString::fromLatin1(dictionary.name);
    const auto id = Utf8ToUnicode((name + ".pattern").toUtf8().constData());
    auto *reader = TextLangMan::getTextLangCfg(lString32(dictionary.language), true)
                           ->getDefaultHyphMethod();
    require(reader && reader->getId() == id && reader->getLeftHyphenMin() == dictionary.leftMin
                    && reader->getRightHyphenMin() == dictionary.rightMin,
            qPrintable("Language selected the wrong dictionary or limits: " + name));
    const auto *listed = HyphMan::getDictList()->find(id);
    require(listed && listed->getHash() == lString32(dictionary.name).getHash()
                    && !HyphMan::getDictList()->find(
                            Utf8ToUnicode((name + ".khg").toUtf8().constData())),
            qPrintable("Unexpected dictionary list or identity: " + name));

    const auto referenceId = id + U" XML reference";
    auto *referenceDictionary = new HyphDictionary(HDT_DICT_TEX, referenceId, referenceId,
                                                   Utf8ToUnicode(source.toUtf8().constData()));
    require(HyphMan::addDictionaryItem(referenceDictionary), "Cannot register the XML reference");
    auto *xml = HyphMan::getHyphMethodForDictionary(referenceId, dictionary.leftMin,
                                                    dictionary.rightMin);
    require(xml && xml->getCount() > 0 && xml->getCount() == reader->getCount(),
            qPrintable("Dictionary lost patterns: " + name));
    return { reader, xml };
}

void comparePatterns(const Methods &methods, const QString &source)
{
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), "Cannot open the source patterns");
    QXmlStreamReader xml(QString::fromUtf8(file.readAll()));
    const QRegularExpression markers("[0-9]");
    unsigned count = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("pattern"))
            continue;
        const auto word = xml.readElementText().remove(markers).trimmed();
        methods.compare(word);
        methods.compare("an" + word + "ung", 55);
        ++count;
    }
    require(!xml.hasError(), qPrintable(xml.errorString()));
    require(count == methods.xml->getCount(),
            qPrintable("Some patterns were not exercised: " + source));
}

void compareGermanOverrides(const Methods &methods, const QTemporaryDir &directory)
{
    for (const auto &word : { "überraschend", "Gewässerschutz", "Straße", "a\xcc\x88hnlichkeit",
                              "Silben\xc2\xadtrennung" })
        methods.compare(QString::fromUtf8(word));
    require(HyphMan::setTrustSoftHyphens(1), "Cannot enable soft hyphens");
    const auto softWord = QString::fromUtf8("Silben\xc2\xadtrennung");
    const auto softFlags = breaks(methods.reader, softWord, 1000);
    require(softFlags[6] != 0, "Trusted soft hyphen must allow a break");
    methods.compare(softWord);

    const auto userPath = directory.filePath("user.dict");
    QFile user(userPath);
    require(user.open(QIODevice::WriteOnly) && user.write("dampfschiff;dam-pfschiff\n") > 0,
            "Cannot write the user dictionary fixture");
    user.close();
    require(UserHyphDict::init(QFile::encodeName(userPath).constData()) == USER_HYPH_DICT_RELOAD
                    && UserHyphDict::hasWords(),
            "Cannot load the user dictionary fixture");
    const auto flags = breaks(methods.reader, "dampfschiff", 1000);
    require(flags[2] != 0 && flags.count(char(0)) == flags.size() - 1,
            "User hyphenation did not take precedence");
    methods.compare("dampfschiff");
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 4 && arguments[1] == "--compile") {
        compile(arguments[2], arguments[3]);
        return 0;
    }
    auto fonts = kobalt::test::readerFonts();
    auto &assets = kobalt::test::readerAssets();
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create temporary dictionary directory");
    const QString mode = arguments.value(1);
    const QStringList fallbackModes{ "--missing", "--damaged", "--stale", "--wrong-language" };
    require(mode.isEmpty() || fallbackModes.contains(mode), "Unknown fallback check");
    if (mode.isEmpty()) {
        // The engine retains its dictionary methods across documents. Each child
        // exercises the first use of its replacement files.
        for (const auto &fallback : fallbackModes)
            require(QProcess::execute(app.applicationFilePath(), { fallback }) == 0,
                    qPrintable(fallback));
    }

    for (const auto &dictionary : packedDictionaries) {
        const QString name = QString::fromLatin1(dictionary.name);
        const auto id = name + ".pattern";
        const auto packed = name + ".khg";
        const auto staged = assets.file("hyph/" + packed);
        require(fonts.compiledHyphenation.value(id) == staged,
                qPrintable("The verified bundle must select " + packed));
        const auto testPath = directory.filePath(packed);
        if (mode.isEmpty()) {
            compile(assets.file("hyph/" + id), testPath);
            QFile generated(testPath), shipped(staged);
            require(generated.open(QIODevice::ReadOnly) && shipped.open(QIODevice::ReadOnly)
                            && generated.readAll() == shipped.readAll(),
                    qPrintable("Cannot reproduce " + packed + " from its XML"));
        } else if (mode == "--wrong-language") {
            fonts.compiledHyphenation[id] =
                    assets.file(name == "German" ? "hyph/Dutch.khg" : "hyph/German.khg");
        } else {
            fonts.compiledHyphenation[id] = testPath;
            if (mode == "--missing")
                continue;
            require(QFile::copy(staged, testPath), "Cannot copy the staged packed dictionary");
            QFile file(testPath);
            require(file.open(QIODevice::ReadWrite), "Cannot open the packed fixture");
            // Damage the payload, or its checksum of the corresponding XML.
            const auto offset = mode == "--damaged" ? file.size() - 1 : 4;
            char original;
            require(file.seek(offset) && file.getChar(&original) && file.seek(offset)
                            && file.putChar(char(original ^ 1)),
                    "Cannot damage the packed fixture");
        }
    }

    kobalt::EngineDocument model(fonts);
    require(model.initializeFonts(), "Cannot initialize engine fonts");
    Methods german{};
    for (const auto &dictionary : packedDictionaries) {
        const auto name = QString::fromLatin1(dictionary.name);
        const auto source = assets.file("hyph/" + name + ".pattern");
        const auto methods = methodsFor(dictionary, source);
        const auto packedSize = QFileInfo(assets.file("hyph/" + name + ".khg")).size();
        require(methods.xml->getSize() != packedSize, "Reference must use the XML loader");
        require(methods.reader->getSize() == (mode.isEmpty() ? packedSize : methods.xml->getSize()),
                qPrintable("Unexpected dictionary representation: " + name));
        methods.compare(QString::fromUtf8(dictionary.word));
        if (mode.isEmpty())
            comparePatterns(methods, source);
        if (name == "German")
            german = methods;
    }

    const Dictionary russian{ "Russian", "ru", 2, 2, "представление" };
    require(!fonts.compiledHyphenation.contains("Russian.pattern"), "Russian must use its XML");
    const auto xmlOnly = methodsFor(russian, assets.file("hyph/Russian.pattern"));
    require(xmlOnly.reader->getSize() == xmlOnly.xml->getSize(), "XML-only dictionary changed");
    xmlOnly.compare(QString::fromUtf8(russian.word));
    if (mode.isEmpty())
        compareGermanOverrides(german, directory);
}
