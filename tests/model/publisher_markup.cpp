#include "reader_inputs.h"
#include "support/qt_compat.h"
#include "engine/crengine/engine_document.h"
#include <lvfntman.h>
#include <textlang.h>
#include <hyphman.h>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <cstdio>
#include "check.h"

namespace {
QByteArray read(const char *path)
{
    QFile file(QString::fromLocal8Bit(path));
    require(file.open(QIODevice::ReadOnly), "Cannot read fixture");
    return file.readAll();
}

QString text(const lString32 &value)
{
    return QString::fromUtf8(UnicodeToUtf8(value).c_str());
}

QString hyphenated(HyphMethod *method, const lString32 &word)
{
    lUInt16 widths[WORD_LENGTH] = {};
    lUInt8 flags[WORD_LENGTH] = {};
    require(word.length() < WORD_LENGTH, "Hyphenation probe is too long");
    for (int i = 0; i < word.length(); ++i)
        widths[i] = (i + 1) * 10;
    method->hyphenate(word.c_str(), word.length(), widths, flags, 5, 1000);
    lString32 result;
    for (int i = 0; i < word.length(); ++i) {
        result << word[i];
        if (flags[i] & LCHAR_ALLOW_HYPH_WRAP_AFTER)
            result << U'-';
    }
    return text(result);
}

void checkEnglishExceptions()
{
    const auto bytes = read(KOBALT_TEST_HYPHENATION_CORPUS);
    require(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                    == "9392bd6e14b2f34ebc31d2ba4eddb7d1266bf563bfd89c6161f63cfd7f00ef5c",
            "The upstream US English exception corpus changed");
    auto source = QString::fromUtf8(bytes);
    source.remove(QRegularExpression("%[^\\n]*"));
    const auto body = QRegularExpression("\\\\hyphenation\\{([^}]+)\\}").match(source);
    require(body.hasMatch(), "The exception corpus has no hyphenation list");
    const auto entries = body.captured(1).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    require(entries.size() == 1753, "The US English exception corpus lost entries");
    auto *method = TextLangMan::getTextLangCfg(lString32("en-US"))->getHyphMethod();
    require(text(method->getId()) == "English_US.pattern", "Corpus selected the wrong dictionary");
    require(method->getLeftHyphenMin() == 2 && method->getRightHyphenMin() == 3,
            "US English hyphenation limits changed");
    int failures = 0;
    for (const auto &entry : entries) {
        require(QRegularExpression("^[A-Za-z]+(?:-[A-Za-z]+)*$").match(entry).hasMatch(),
                "The exception corpus contains an unsupported entry");
        auto word = entry;
        word.remove('-');
        QString expected;
        int letters = 0;
        // TeX exceptions replace the general patterns. Apply only the language's
        // two-letter prefix and three-letter suffix limits to their break list.
        for (const auto character : entry.toLower()) {
            if (character == '-') {
                if (letters >= method->getLeftHyphenMin()
                    && word.size() - letters >= method->getRightHyphenMin())
                    expected += '-';
            } else {
                expected += character;
                ++letters;
            }
        }
        const auto actual = hyphenated(method, Utf8ToUnicode(word.toUtf8().constData())).toLower();
        if (actual != expected) {
            if (failures < 5)
                std::fprintf(stderr, "%s: expected %s, got %s\n", qPrintable(word),
                             qPrintable(expected), qPrintable(actual));
            ++failures;
        }
    }
    // This general-pattern result is deliberately outside the exception repair.
    require(hyphenated(method, lString32(U"seagull")) == "seag-ull",
            "The exception repair changed seagull");
    require(failures == 0,
            qPrintable(QString("%1 of 1753 US English exceptions disagree with the corpus")
                               .arg(failures)));
    std::puts("PASS: all 1753 upstream US English exceptions and unchanged seagull");
}

// Every match of `query` in the book, in reading order.
QVector<kobalt::SearchHit> find(kobalt::EngineDocument &model, const QString &query)
{
    QVector<kobalt::SearchHit> hits;
    for (int page = 0; page < model.pages(); ++page)
        hits += model.searchPage(query, page).hits;
    return hits;
}

int width(kobalt::EngineDocument &model, const kobalt::SearchHit &hit)
{
    int result = 0;
    for (const auto &rect : model.rangeRects(hit.start, hit.end))
        result += rect.width();
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 4, "Pass the English, German and Russian publisher EPUBs");
    const auto english = read(argv[1]);
    const QSize size(1072, 1448);
    QString error;
    {
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(english, size,
                           kobalt::qt::object({ { "font_face", "" },
                                                { "default_font_face",
                                                  kobalt::test::alternateReadingFace() } }),
                           &error),
                qPrintable(error));
        require(model.embeddedFontCount() == 0, "Unembedded book reported publisher fonts");
        const auto lead = find(model, "Linear text continues");
        require(!lead.isEmpty() && model.goTo(lead.first().start)
                        && model.textFontFace() == "Libron",
                "The default book override changed Publisher Fonts");
        require(model.applyAppearance(kobalt::qt::object({ { "default_font_face", "Libron" } })),
                "Cannot change default book override");
        require(model.state()["font_face"].toString().isEmpty(),
                "Default change replaced Publisher Fonts selection");
        require(model.textFontFace() == "Libron",
                "Default book override changed the fixed publisher fallback");
        const auto path = QFileInfo(QString::fromLocal8Bit(argv[1]))
                                  .absoluteDir()
                                  .filePath("embedded-fonts.epub")
                                  .toLocal8Bit();
        require(model.open(read(path.constData()), size,
                           kobalt::qt::object({ { "font_face", "" } }), &error),
                qPrintable(error));
        require(model.embeddedFontCount() == 1,
                "Embedded count included styles, local aliases or missing fonts");
    }

    {
        const auto path = QFileInfo(QString::fromLocal8Bit(argv[1]))
                                  .absoluteDir()
                                  .filePath("font-families.epub")
                                  .toLocal8Bit();
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(read(path.constData()), size,
                           kobalt::qt::object({ { "font_face", "" } }), &error),
                qPrintable(error));
        const auto checkFace = [&](const QString &word, const QString &expected) {
            const auto hits = find(model, word);
            require(hits.size() == 1,
                    qPrintable(word + " has " + QString::number(hits.size()) + " search hits"));
            require(model.goTo(hits.first().start), qPrintable("Cannot locate " + word));
            require(model.textFontFace() == expected,
                    qPrintable(word + " used " + model.textFontFace() + " instead of " + expected));
        };
        checkFace("Serifmarker", "Libron");
        checkFace("Sansmarker",
                  model.fontFaces().contains(kobalt::publisherSansFontFace)
                          ? QString(kobalt::publisherSansFontFace)
                          : QString("IBM Plex Sans"));
        checkFace("Monomarker", "IBM Plex Mono");
        checkFace("Namedmarker", "IBM Plex Sans");
        require(model.applyAppearance(kobalt::qt::object(
                        { { "default_font_face", kobalt::test::alternateReadingFace() } })),
                "Cannot change the default book override");
        checkFace("Serifmarker", "Libron");
        checkFace("Sansmarker",
                  model.fontFaces().contains(kobalt::publisherSansFontFace)
                          ? QString(kobalt::publisherSansFontFace)
                          : QString("IBM Plex Sans"));
        checkFace("Monomarker", "IBM Plex Mono");
        checkFace("Namedmarker", "IBM Plex Sans");
        const auto saved = model.state();
        require(model.open(read(path.constData()), size, saved, &error), qPrintable(error));
        checkFace("Serifmarker", "Libron");
        checkFace("Sansmarker",
                  model.fontFaces().contains(kobalt::publisherSansFontFace)
                          ? QString(kobalt::publisherSansFontFace)
                          : QString("IBM Plex Sans"));
        require(model.applyAppearance(kobalt::qt::object(
                        { { "font_face", kobalt::test::alternateReadingFace() } })),
                "Cannot select an explicit reading font");
        QVector<QImage> explicitPages;
        for (int page = 0; page < model.pages(); ++page) {
            explicitPages.append(model.image(page));
        }
        checkFace("Serifmarker", kobalt::test::alternateReadingFace());
        checkFace("Sansmarker", kobalt::test::alternateReadingFace());
        checkFace("Namedmarker", kobalt::test::alternateReadingFace());
        require(model.applyAppearance(kobalt::qt::object({ { "default_font_face", "Libron" } })),
                "Cannot change the default alongside an explicit reading font");
        require(model.pages() == explicitPages.size(),
                "Default changes repaginated an explicit override");
        for (int page = 0; page < model.pages(); ++page) {
            require(model.image(page) == explicitPages[page],
                    "Default changes replaced an explicit override");
        }
        require(model.open(read(path.constData()), size,
                           kobalt::qt::object({ { "font_face", "Missing face" },
                                                { "default_font_face",
                                                  kobalt::test::alternateReadingFace() } }),
                           &error),
                qPrintable(error));
        require(model.fontFace() == kobalt::test::alternateReadingFace(),
                "Missing explicit reading font ignored the default book override");
        const auto embeddedPath = QFileInfo(QString::fromLocal8Bit(argv[1]))
                                          .absoluteDir()
                                          .filePath("font-families-embedded.epub")
                                          .toLocal8Bit();
        require(model.open(read(embeddedPath.constData()), size,
                           kobalt::qt::object({ { "font_face", "" } }), &error),
                qPrintable(error));
        require(model.embeddedFontCount() == 1, "Embedded fixture did not register its font");
        QVector<QImage> originalPages;
        for (int page = 0; page < model.pages(); ++page) {
            originalPages.append(model.image(page));
        }
        require(model.applyAppearance(kobalt::qt::object(
                        { { "default_font_face", kobalt::test::alternateReadingFace() } })),
                "Cannot save defaults while publisher fonts are present");
        require(model.pages() == originalPages.size(),
                "Defaults repaginated a book with embedded fonts");
        for (int page = 0; page < model.pages(); ++page) {
            require(model.image(page) == originalPages[page],
                    "Defaults changed a book with embedded fonts");
        }
        require(model.go(0), "Cannot locate embedded book preview");
        kobalt::EngineDocument preview(kobalt::test::readerFonts());
        require(preview.openPreview(model, size, &error), qPrintable(error));
        require(preview.embeddedFontCount() == 1 && preview.image(0) == model.image(0),
                "Preview lost the full publication's embedded-font gate");
    }

    {
        const auto directory = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteDir();
        const auto fallbacks =
                kobalt::qt::object({ { "font_face", "" },
                                     { "serif_font_face", kobalt::test::alternateReadingFace() },
                                     { "sans_font_face", "IBM Plex Mono" },
                                     { "monospace_font_face", "IBM Plex Sans" } });
        for (const auto *name : { "font-families.epub", "font-families-embedded.epub" }) {
            const auto path = directory.filePath(name).toLocal8Bit();
            const auto bytes = read(path.constData());
            kobalt::EngineDocument model(kobalt::test::readerFonts());
            require(model.open(bytes, size, fallbacks, &error), qPrintable(error));
            const auto checkFace = [&](const QString &word, const QString &face) {
                const auto hits = find(model, word);
                require(hits.size() == 1 && model.goTo(hits.first().start),
                        qPrintable("Cannot locate " + word));
                require(model.textFontFace() == face,
                        qPrintable(word + " used " + model.textFontFace() + " instead of " + face));
            };
            const auto checkFamilies = [&] {
                checkFace("Serifmarker", kobalt::test::alternateReadingFace());
                checkFace("Sansmarker", "IBM Plex Mono");
                checkFace("Monomarker", "IBM Plex Sans");
                checkFace("Namedmarker", "IBM Plex Sans");
                if (model.embeddedFontCount())
                    checkFace("Embeddedmarker", "Embedded");
            };
            checkFamilies();
            require(model.applyAppearance(kobalt::qt::object({ { "serif_font_face", "" },
                                                               { "sans_font_face", "" },
                                                               { "monospace_font_face", "" } })),
                    "Cannot restore automatic fallbacks");
            require(model.applyAppearance(fallbacks),
                    "Cannot change generic fallback fonts on an open book");
            checkFamilies();
            kobalt::EngineDocument preview(kobalt::test::readerFonts());
            require(preview.openPreview(model, size, &error), qPrintable(error));
            for (const auto *key : kobalt::appearance::fallbackFontKeys)
                require(preview.state().value(key) == model.state().value(key),
                        "Preview lost a custom fallback");
            require(preview.textFontFace() == model.textFontFace(),
                    "Preview changed the current fallback or embedded font");
            const auto saved = model.state();
            require(model.open(bytes, size, saved, &error), qPrintable(error));
            checkFamilies();
            const auto before = model.state();
            require(!model.applyAppearance(
                            kobalt::qt::object({ { "serif_font_face", "Missing font" } })),
                    "Missing fallback was accepted");
            require(model.state() == before, "Rejected fallback changed the model");
            require(model.applyAppearance(kobalt::qt::object({ { "font_face", "Libron" } })),
                    "Explicit override failed");
            checkFace("Serifmarker", "Libron");
            checkFace("Sansmarker", "Libron");
            checkFace("Namedmarker", "Libron");
            require(model.applyAppearance(kobalt::qt::object({ { "font_face", "" },
                                                               { "serif_font_face", "" },
                                                               { "sans_font_face", "" },
                                                               { "monospace_font_face", "" } })),
                    "Cannot reset fallback choices");
            checkFace("Monomarker", "IBM Plex Mono");
        }
        kobalt::EngineDocument missing(kobalt::test::readerFonts());
        auto unavailable = fallbacks;
        unavailable["serif_font_face"] = QStringLiteral("Removed Personal Font");
        const auto path = directory.filePath("font-families.epub").toLocal8Bit();
        require(missing.open(read(path.constData()), size, unavailable, &error), qPrintable(error));
        require(missing.state().value("serif_font_face").toString().isEmpty(),
                "Missing fallback did not reset safely");
    }

    {
        const auto path = QFileInfo(QString::fromLocal8Bit(argv[1]))
                                  .absoluteDir()
                                  .filePath("equations.epub")
                                  .toLocal8Bit();
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(read(path.constData()), size,
                           kobalt::qt::object({ { "font_pixels", 40 } }), &error),
                qPrintable(error));
        const auto image = model.image(0);
        QRect ink;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = QColor::fromRgba(image.pixel(x, y));
                if (pixel.red() > 150 && pixel.green() < 80 && pixel.blue() < 100)
                    ink |= QRect(x, y, 1, 1);
            }
        require(qAbs(ink.width() - 80) <= 2 && qAbs(ink.height() - 40) <= 2,
                "Font-relative equation dimensions were enlarged after CSS layout");
        QRect accent;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (QColor::fromRgba(image.pixel(x, y)) == QColor(0, 204, 102))
                    accent |= QRect(x, y, 1, 1);
        require(!accent.isEmpty(), "Equation colour marker is missing");
        require(model.applyAppearance(kobalt::qt::object({ { "dark_mode", true } })),
                "Cannot enable equation dark mode");
        const auto dark = model.image(0);
        const QPoint marker = accent.center();
        require(QColor::fromRgba(image.pixel(marker - QPoint(80, 0))) == Qt::black
                        && QColor::fromRgba(dark.pixel(marker - QPoint(80, 0))) == Qt::white,
                "Black equation ink is invisible in dark mode");
        require(QColor::fromRgba(dark.pixel(marker - QPoint(40, 0))) == QColor(204, 204, 204),
                "Dark grey equation ink was not inverted");
        require(QColor::fromRgba(dark.pixel(marker)) == QColor::fromRgba(image.pixel(marker)),
                "Equation accent changed colour");
        require(QColor::fromRgba(dark.pixel(marker + QPoint(40, 0))) == Qt::black,
                "Transparent equation background became opaque");
        require(QColor::fromRgba(dark.pixel(ink.center()))
                        == QColor::fromRgba(image.pixel(ink.center())),
                "Ordinary artwork changed colour");
        require(model.applyAppearance(kobalt::qt::object({ { "dark_mode", false } })),
                "Cannot restore equation light mode");
        require(model.image(0) == image, "Equation night rendering changed its source pixels");
    }

    {
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(english, size, {}, &error), qPrintable(error));

        // Code keeps a fixed width under the chosen reading font.
        const auto narrow = find(model, "iiiiiiiiii"), wide = find(model, "MMMMMMMMMM");
        require(narrow.size() == 1 && wide.size() == 1, "Code fixture text is missing");
        require(qAbs(width(model, narrow.first()) - width(model, wide.first())) <= 2,
                "Code lost its monospace face under the reading font");

        // Libron has none of these characters, so their glyphs come from the
        // Noto fallback faces.
        require(text(Utf8ToUnicode(fontMan->GetFallbackFontFaces())) == kobalt::fallbackFontFaces,
                "Fallback faces are not set");
        auto reading =
                fontMan->GetFont(44, 400, false, css_ff_serif, lString8(kobalt::defaultFontFace));
        require(!reading.isNull(), "Reading font is unavailable");
        for (const char32_t character :
             { U'α', U'Ж', U'ɛ', U'∑', U'→', U'骨', U'漢', U'あ', U'한', U'韓', U'龙', U'龍' }) {
            LVFont::glyph_info_t glyph;
            require(reading->getGlyphInfo(character, &glyph),
                    "No fallback face has a glyph for a supported script");
        }

        // Search matches what a reader types.
        require(find(model, "final").size() == 1,
                "Search missed a word written with the fi ligature");
        require(find(model, "e-mail").size() == 1,
                "Search missed a word with a non-breaking hyphen");
        require(find(model, "hyphenation").size() == 1,
                "Search missed a word split by a zero-width space");

        // lang attributes select their own rules; the package language is the default.
        require(TextLangMan::getEmbeddedLangsEnabled(), "lang attributes are ignored");
        require(text(TextLangMan::getMainLang()) == "en", "The package language was not applied");
        require(text(TextLangMan::getTextLangCfg()->getHyphMethod()->getId())
                        == "English_US.pattern",
                "English does not use its dictionary");
        checkEnglishExceptions();

        // A note without a container keeps its later paragraphs, up to the next note.
        model.go(0);
        QString first, second;
        for (const auto &link : model.pageLinks()) {
            const auto details = model.linkDetails(link.source);
            if (details.label == "1")
                first = details.html;
            if (details.label == "2")
                second = details.html;
        }
        require(first.contains("Note one opens here.") && first.contains("Note one continues"),
                "A multi-paragraph note lost its second paragraph");
        require(!first.contains("Note two"), "A note ran into the next note");
        require(second.contains("Note two stands alone.") && !second.contains("Closingword"),
                "A note ran past the next heading");

        require(!model.state().value("hide_nonlinear_fragments").toBool(),
                "Non-linear fragments are hidden by default");
        for (int page = 0; page < model.pages(); ++page) {
            require(model.turnTarget(page, 1) == qMin(page + 1, model.pages() - 1),
                    "Default forward paging skipped a physical page");
            require(model.turnTarget(page, -1) == qMax(page - 1, 0),
                    "Default backward paging skipped a physical page");
        }
        require(model.applyAppearance(kobalt::qt::object({ { "hide_nonlinear_fragments", true } })),
                "Cannot enable hiding non-linear fragments");
        require(model.state().value("hide_nonlinear_fragments").toBool(),
                "Hiding setting was not retained");
        // Hiding skips the footnote aside and the non-linear spine item.
        int page = 0;
        QStringList seen;
        bool checkedSkippedCache = false;
        for (int turns = 0; turns < model.pages(); ++turns) {
            model.go(page);
            for (const auto *word : { "Asidetext", "Nonlinearspine", "Closingword" })
                if (!model.searchPage(word, page).hits.isEmpty())
                    seen.append(word);
            const int next = model.turnTarget(page, 1);
            if (next == page)
                break;
            if (next > page + 1) {
                model.image(page);
                const auto prepared = model.image(next);
                const auto preparedKey = prepared.cacheKey();
                require(!prepared.isNull() && model.cachedImageKey(next) == preparedKey,
                        "Prepared turn target across non-linear text was discarded");
                model.image(page + 1);
                require(model.cachedImageKey(page + 1) == 0 && model.cachedPages() <= 3,
                        "Page cache retained a skipped physical neighbour");
                model.prepareDamage(page, next);
                QRect damage;
                require(model.preparedDamage(model.cachedImageKey(page), preparedKey, &damage),
                        "Cannot prepare damage across non-linear text");
                require(model.go(next) && model.image(next).cacheKey() == preparedKey,
                        "Turn across non-linear text redrew its prepared target");
                checkedSkippedCache = true;
            }
            page = next;
        }
        require(checkedSkippedCache, "Non-linear cache fixture did not skip a page");
        require(!seen.contains("Asidetext") && !seen.contains("Nonlinearspine"),
                "Page turns showed non-linear content");
        require(seen.contains("Closingword") && page == model.turnTarget(page, 1),
                "Page turns did not reach the end of the text");
        const auto aside = find(model, "Asidetext");
        require(aside.size() == 1 && model.goTo(aside.first().start),
                "The footnote aside cannot be opened directly");
        require(model.turnTarget(model.page(), 1) != model.page(),
                "A turn inside a non-linear section is stuck");
        require(model.applyAppearance(
                        kobalt::qt::object({ { "hide_nonlinear_fragments", false } })),
                "Cannot disable hiding non-linear fragments");
        require(model.turnTarget(1, -1) == 0, "Cannot turn back to the first physical page");
        require(model.state().value("dom_version").toInt() > 20260000,
                "A new book does not record its DOM version");
    }
    {
        // A position written by an older DOM version still finds its text.
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        const auto saved =
                kobalt::qt::object({ { "position", "/body/DocFragment[3]/body/p[2]/text().0" },
                                     { "dom_version", 20210904 } });
        require(model.open(english, size, saved, &error),
                qPrintable("Old position failed: " + error));
        require(!model.searchPage("Domtarget", model.page()).hits.isEmpty(),
                "An old position opened on the wrong page");
        require(model.state().value("dom_version").toInt() == 20210904,
                "The book lost the DOM version of its position");
    }
    {
        // A German book uses the German dictionary.
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(read(argv[2]), size, kobalt::qt::object({ { "hyphenation", "on" } }),
                           &error),
                qPrintable(error));
        require(text(TextLangMan::getMainLang()) == "de",
                "The German package language was not applied");
        require(text(TextLangMan::getTextLangCfg()->getHyphMethod()->getId()) == "German.pattern",
                "German does not use its dictionary");
        // Long compounds must break inside words at a narrow width.
        require(model.applyAppearance(kobalt::qt::object({ { "text_alignment", "justify" } }),
                                      QSize(480, 1448)),
                "Cannot narrow the page");
        const auto hyphenated = model.image(model.page());
        require(model.setHyphenation("off"), "Cannot disable hyphenation");
        require(model.image(model.page()) != hyphenated,
                "German hyphenation did not change wrapping");
    }
    {
        // Russian uses its dictionary, including regional language tags.
        kobalt::EngineDocument model(kobalt::test::readerFonts());
        require(model.open(read(argv[3]), size, kobalt::qt::object({ { "hyphenation", "on" } }),
                           &error),
                qPrintable(error));
        require(text(TextLangMan::getMainLang()) == "ru",
                "The Russian package language was not applied");
        for (const auto *language : { "ru", "ru-RU" }) {
            auto *method = TextLangMan::getTextLangCfg(lString32(language))->getHyphMethod();
            require(text(method->getId()) == "Russian.pattern" && method->getCount() > 0,
                    "Russian does not use its bundled patterns");
            require(hyphenated(method, lString32(U"программирование")) == "про-грам-ми-ро-ва-ние",
                    "Russian patterns lost their dictionary-specific breaks");
        }
        require(text(TextLangMan::getTextLangCfg(lString32("bg"))->getHyphMethod()->getId())
                        == "@algorithm",
                "An unbundled language lost algorithmic hyphenation");
    }
    {
        const auto directory = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteDir();
        const QMap<QString, QString> dictionaries = {
            { "et-EE", "Estonian" },  { "fi-FI", "Finnish" },    { "ka-GE", "Georgian" },
            { "hu-HU", "Hungarian" }, { "lt-LT", "Lithuanian" }, { "lv-LV", "Latvian" },
            { "pl-PL", "Polish" },    { "ru-RU", "Russian" },    { "sk-SK", "Slovak" },
            { "sl-SI", "Slovenian" }, { "uk-UA", "Ukrainian" }
        };
        for (auto entry = dictionaries.cbegin(); entry != dictionaries.cend(); ++entry) {
            kobalt::EngineDocument model(kobalt::test::readerFonts());
            const auto path =
                    directory.filePath("hyphenation-" + entry.key() + ".epub").toLocal8Bit();
            require(model.open(read(path.constData()), QSize(320, 720),
                               kobalt::qt::object({ { "hyphenation", "on" },
                                                    { "text_alignment", "justify" } }),
                               &error),
                    qPrintable(error));
            require(text(TextLangMan::getMainLang()) == entry.key(),
                    "Package language was not applied");
            auto *method = TextLangMan::getTextLangCfg()->getHyphMethod();
            require(text(method->getId()) == entry.value() + ".pattern",
                    qPrintable(entry.key() + " did not select its dictionary"));
            require(method->getCount() > 0, "Selected dictionary has no loaded patterns");
            require(text(TextLangMan::getTextLangCfg(lString32("hu"))->getHyphMethod()->getId())
                                    == "Hungarian.pattern"
                            && text(TextLangMan::getTextLangCfg(lString32("pl"))
                                            ->getHyphMethod()
                                            ->getId())
                                    == "Polish.pattern",
                    "Inline language selection lost Hungarian or Polish patterns");
            const auto hyphenated = model.image(model.page());
            require(model.setHyphenation("off"), "Cannot disable hyphenation");
            require(model.image(model.page()) != hyphenated,
                    qPrintable(entry.key() + " hyphenation did not change wrapping"));
        }
    }
    {
        QMap<QString, QImage> pages;
        const auto directory = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteDir();
        for (const auto *language : { "ja", "ko", "zh-Hans", "zh-Hant", "zh-HK", "inline" }) {
            kobalt::EngineDocument model(kobalt::test::readerFonts());
            const auto path =
                    directory.filePath(QString("cjk-%1.epub").arg(language)).toLocal8Bit();
            require(model.open(read(path.constData()), size, {}, &error), qPrintable(error));
            require(find(model, QString::fromUtf8("骨直令曜")).size() == 1,
                    "CJK text is not searchable");
            pages[language] = model.image(0);
            for (const auto &font : model.fontCatalog())
                require(!font.family.startsWith("Noto Serif CJK")
                                || font.origin == kobalt::FontOrigin::Interface,
                        "Supplemental CJK font lost its language-filtered classification");
            if (QString(language) == "ja") {
                // All regional faces must survive collection registration.
                for (const auto *region : { "JP", "KR", "SC", "TC", "HK" })
                    require(model.fontFaces().contains(QString("Noto Serif CJK %1").arg(region)),
                            "CJK collection face is missing");
                kobalt::EngineDocument preview(kobalt::test::readerFonts());
                require(preview.openPreview(model, size, &error), qPrintable(error));
                require(preview.image(0) == pages[language], "CJK preview changed glyphs");
                model.setDarkMode(true);
                require(model.image(0) != pages[language], "CJK dark mode did not change pixels");
                model.setDarkMode(false);
                require(model.image(0) == pages[language],
                        "CJK dark mode changed the original page");
            }
        }
        require(pages["ja"] != pages["zh-Hans"], "CJK fallback ignored regional glyph forms");
        require(pages["zh-Hans"] != pages["zh-Hant"],
                "Chinese simplified and traditional glyph forms are identical");
        require(pages["inline"] == pages["zh-Hans"],
                "Inline language did not override the book's CJK glyph forms");
    }
    std::puts(
            "PASS: code font, fallback glyphs and CJK regional forms, search folding, book language, dictionaries, multi-paragraph notes, non-linear sections and old DOM positions");
}
