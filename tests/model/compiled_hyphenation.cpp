#include "check.h"
#include "engine/blaze/adapter/engine_document.h"
#include "reader_inputs.h"
#include <lvfntman.h>
#include <hyphman.h>
#include <textlang.h>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

namespace {
void compile(const QString &source, const QString &destination)
{
    auto xml = LVOpenFileStream(QFile::encodeName(source).constData(), LVOM_READ);
    auto output = LVCreateMemoryStream(nullptr, 0, false, LVOM_WRITE);
    require(HyphMan::compileGermanPatterns(xml, output),
            "Cannot compile the staged German dictionary");
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
    const auto source = kobalt::test::readerAssets().file("hyph/German.pattern");
    const auto staged = kobalt::test::readerAssets().file("hyph/German.khg");
    require(fonts.compiledGermanPatterns == staged, "The verified bundle must select German.khg");
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create temporary dictionary directory");
    const auto packed = directory.filePath("German.khg");
    const bool fallback = arguments.size() > 1;
    if (fallback) {
        fonts.compiledGermanPatterns = packed;
        if (arguments[1] == "--damaged" || arguments[1] == "--stale") {
            require(QFile::copy(staged, packed), "Cannot copy the staged packed dictionary");
            QFile file(packed);
            require(file.open(QIODevice::ReadWrite), "Cannot open the packed fixture");
            // Damage the payload, or just its checksum of the corresponding XML.
            const auto offset = arguments[1] == "--damaged" ? file.size() - 1 : 4;
            char original;
            require(file.seek(offset) && file.getChar(&original) && file.seek(offset)
                            && file.putChar(char(original ^ 1)),
                    "Cannot damage the packed fixture");
        } else {
            require(arguments[1] == "--missing", "Unknown fallback case");
        }
    } else {
        compile(source, packed);
        QFile generated(packed), shipped(staged);
        require(generated.open(QIODevice::ReadOnly) && shipped.open(QIODevice::ReadOnly)
                        && generated.readAll() == shipped.readAll(),
                "Shipped dictionary must reproduce from the canonical XML");
        // The engine keeps loaded dictionaries across documents in this process.
        for (const auto *mode : { "--missing", "--damaged", "--stale" })
            require(QProcess::execute(app.applicationFilePath(), { QString::fromLatin1(mode) })
                            == 0,
                    mode);
    }
    kobalt::EngineDocument model(fonts);
    require(model.initializeFonts(), "Cannot initialize engine fonts");
    auto *candidate = TextLangMan::getTextLangCfg(lString32("de"), true)->getDefaultHyphMethod();
    require(candidate->getLeftHyphenMin() == 2 && candidate->getRightHyphenMin() == 2,
            "German language hyphenation limits changed");
    const auto *listed = HyphMan::getDictList()->find(U"German.pattern");
    require(listed && listed->getHash() == lString32(U"German").getHash()
                    && !HyphMan::getDictList()->find(U"German.khg"),
            "The packed file must preserve the original dictionary identity");
    const lString32 referenceId(U"German XML reference");
    auto *referenceDictionary = new HyphDictionary(HDT_DICT_TEX, referenceId, referenceId,
                                                   Utf8ToUnicode(source.toUtf8().constData()));
    require(HyphMan::addDictionaryItem(referenceDictionary), "Cannot register the XML reference");
    auto *reference = HyphMan::getHyphMethodForDictionary(
            referenceId, candidate->getLeftHyphenMin(), candidate->getRightHyphenMin());
    require(candidate->getId() == U"German.pattern" && reference->getCount() > 0
                    && candidate->getCount() == reference->getCount(),
            "Dictionary lost patterns");
    require(fallback ? candidate->getSize() == reference->getSize()
                     : candidate->getSize() < reference->getSize(),
            "Unexpected dictionary representation");

    auto check = [&](const QString &word, int available = 1000) {
        require(breaks(candidate, word, available) == breaks(reference, word, available),
                qPrintable("Hyphenation differs for " + word));
    };
    check(QStringLiteral("Donaudampfschifffahrt"));
    if (fallback)
        return 0;

    QFile file(source);
    require(file.open(QIODevice::ReadOnly), "Cannot open the source patterns");
    const auto contents = file.readAll();
    const auto xml = QString::fromUtf8(contents);
    auto patterns = QRegularExpression("<pattern>([^<]+)</pattern>").globalMatch(xml);
    const QRegularExpression markers("[0-9]");
    int count = 0;
    while (patterns.hasNext()) {
        auto word = patterns.next().captured(1).remove(markers).trimmed();
        check(word);
        check("an" + word + "ung", 55);
        ++count;
    }
    require(unsigned(count) == reference->getCount(), "Some German patterns were not exercised");
    for (const auto &word : { "Donaudampfschifffahrt", "überraschend", "Gewässerschutz", "Straße",
                              "a\xcc\x88hnlichkeit", "Silben\xc2\xadtrennung" })
        check(QString::fromUtf8(word));
    require(HyphMan::setTrustSoftHyphens(1), "Cannot enable soft hyphens");
    check(QString::fromUtf8("Silben\xc2\xadtrennung"));

    const auto userPath = directory.filePath("user.dict");
    QFile user(userPath);
    require(user.open(QIODevice::WriteOnly) && user.write("dampfschiff;dam-pfschiff\n") > 0,
            "Cannot write the user dictionary fixture");
    user.close();
    require(UserHyphDict::init(QFile::encodeName(userPath).constData()) == USER_HYPH_DICT_RELOAD
                    && UserHyphDict::hasWords(),
            "Cannot load the user dictionary fixture");
    const auto flags = breaks(candidate, "dampfschiff", 1000);
    require(flags[2] != 0 && flags.count(char(0)) == flags.size() - 1,
            "User hyphenation did not take precedence");
    check("dampfschiff");
}
