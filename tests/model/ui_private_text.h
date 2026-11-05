#pragma once

#include "check.h"
#include "assets/assets.h"
#include "ui/style/private_text.h"
#include "ui/style/rich_text.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"

#include <QPainter>
#include <QTextBoundaryFinder>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QBuffer>

inline void checkPrivateRichText()
{
    using namespace kobalt::ui;
    const auto font = chromeFont(28);
    for (const bool dark : { false, true }) {
        for (const auto &container :
             { QStringLiteral("span"), QStringLiteral("paragraph"), QStringLiteral("cell") }) {
            for (int sample = 0; sample < 3; ++sample) {
                const QColor foreground = sample == 1 ? Qt::black : Qt::white;
                const QColor background = sample == 0 ? Qt::black
                        : sample == 1                 ? Qt::white
                                                      : Qt::blue;
                PrivateRichText document;
                document.setFont(font);
                const auto style = QString("color:%1;background-color:%2")
                                           .arg(foreground.name(), background.name());
                document.setHtml(
                        container == "span"
                                ? QString("<p style='margin:0'><span style='%1'>MMMM</span></p>")
                                          .arg(style)
                                : container == "paragraph"
                                ? QString("<p style='margin:0;%1'>MMMM</p>").arg(style)
                                : QString("<table cellspacing='0' cellpadding='0'><tr><td style='%1'>MMMM</td></tr></table>")
                                          .arg(style));
                document.layout(400);
                QPalette palette;
                palette.setColor(QPalette::Window, dark ? Qt::black : Qt::white);
                palette.setColor(QPalette::WindowText, dark ? Qt::white : Qt::black);
                QImage image(400, document.height(), QImage::Format_RGB32);
                image.fill(palette.color(QPalette::Window));
                {
                    QPainter painter(&image);
                    document.draw(painter, image.rect(), palette);
                }
                const auto capture = qEnvironmentVariable("KOBALT_RICH_TEXT_CAPTURE");
                if (!capture.isEmpty()) {
                    require(QDir().mkpath(capture), "Cannot create rich-text capture directory");
                    require(image.save(capture
                                       + QString("/contrast-%1-%2-%3.png")
                                                 .arg(dark ? "dark" : "light", container,
                                                      QString::number(sample))),
                            "Cannot save rich-text contrast capture");
                }
                QRect backdrop;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x)
                        if (image.pixelColor(x, y) == background)
                            backdrop |= QRect(x, y, 1, 1);
                require(backdrop.width() >= document.naturalWidth() && backdrop.height() > 20,
                        "Rich-text background was dropped");
                int ink = 0;
                for (int y = backdrop.top(); y <= backdrop.bottom(); ++y)
                    for (int x = backdrop.left(); x <= backdrop.right(); ++x)
                        ink += image.pixelColor(x, y) == foreground;
                require(ink > 100,
                        "Explicit rich-text foreground lost contrast against its background");
            }
        }
    }
    QVector<QImage> bullets;
    for (const auto &style :
         { QStringLiteral("disc"), QStringLiteral("circle"), QStringLiteral("square") }) {
        PrivateRichText document;
        document.setFont(font);
        document.setHtml("<ul type='" + style + "' style='margin:0'><li>MMMM</li></ul>");
        document.layout(400);
        const auto lines = document.lineRectangles();
        require(lines.size() == 2, "Unordered list lost its marker");
        QImage image(400, document.height(), QImage::Format_RGB32);
        image.fill(Qt::white);
        QPalette palette;
        palette.setColor(QPalette::WindowText, Qt::black);
        {
            QPainter painter(&image);
            document.draw(painter, image.rect(), palette);
        }
        QRect ink;
        const auto marker = lines.last().toAlignedRect().intersected(image.rect());
        for (int y = marker.top(); y <= marker.bottom(); ++y)
            for (int x = marker.left(); x <= marker.right(); ++x)
                if (qGray(image.pixel(x, y)) < 128)
                    ink |= QRect(x, y, 1, 1);
        require(ink.height() >= 5, "Unordered bullet rendered as a tiny period");
        bullets.append(image.copy(ink));
    }
    require(bullets[0] != bullets[1] && bullets[1] != bullets[2] && bullets[0] != bullets[2],
            "Unordered list styles did not retain distinct bullet shapes");
    for (const auto &tag : { QStringLiteral("ul"), QStringLiteral("ol") }) {
        PrivateRichText document;
        document.setFont(interfaceFont(32));
        document.setHtml("<" + tag + " dir='rtl' style='margin:0'><li>العربية</li></" + tag + ">");
        document.layout(400);
        const auto lines = document.lineRectangles();
        require(lines.size() == 2 && lines.last().left() >= lines.first().right()
                        && lines.last().right() <= 400,
                "RTL list marker is on the wrong margin or outside the viewport");
    }
    {
        PrivateRichText document;
        document.setFont(font);
        const int characters = 16384;
        document.setPlainText(QString(characters, QLatin1Char('M')));
        const int width = 4 * measurePrivateText(QStringLiteral("M"), font);
        QElapsedTimer timer;
        timer.start();
        document.layout(width);
        require(timer.elapsed() < 5000,
                "Long rich-text paragraph repeatedly shaped its remaining contents");
        const auto lines = document.lineRectangles();
        require(lines.size() == characters / 4,
                "Long rich-text paragraph lost or repeated wrapped characters");
        for (int i = 0; i < lines.size(); ++i)
            require(lines[i].left() >= 0 && lines[i].right() <= width && lines[i].height() > 0
                            && (i == 0 || lines[i].top() >= lines[i - 1].bottom()),
                    "Long rich-text paragraph has overlapping or out-of-bounds lines");
    }
    {
        PrivateRichText document;
        document.setFont(font);
        document.setHtml("<ol style='margin:0'><li>MMMM<ol style='margin:0'><li>MMMM</li></ol></li>"
                         "<li>MMMM</li></ol>");
        document.layout(400);
        QVector<QRectF> contents;
        for (const auto &line : document.lineRectangles())
            if (line.width() > 100)
                contents.append(line);
        require(contents.size() == 3 && contents[1].left() > contents[0].left()
                        && contents[2].left() == contents[0].left(),
                "Nested rich-text list lost its hierarchy or retained the child indent");
    }
    {
        PrivateRichText document;
        document.setFont(font);
        const QString paragraph = "<p style='margin:0 0 80px'>M</p>";
        document.setHtml(paragraph);
        document.layout(400);
        const int pageHeight = qCeil(document.lineRectangles().first().height());
        document.layout(400, pageHeight);
        require(document.pages() == 1, "Trailing rich-text paragraph margin created blank pages");
        document.setHtml(paragraph + "<p style='margin:0'>M</p>");
        document.layout(400, pageHeight);
        const auto lines = document.lineRectangles();
        require(lines.size() == 2 && lines[1].top() >= lines[0].bottom() + 80
                        && document.pages() > 1,
                "Trimming trailing rich-text margin removed spacing before subsequent content");
        document.setHtml("<table cellspacing='8' cellpadding='0'><tr><td>M</td></tr></table>");
        document.layout(400, pageHeight);
        require(document.pages() == 1, "Trailing rich-text table spacing created a blank page");
    }
    for (const bool mixed : { false, true }) {
        PrivateRichText document;
        document.setFont(font);
        document.setHtml(
                mixed ? "<table cellspacing='8' cellpadding='0'><tr><td width='80%'>M</td><td>M</td></tr></table>"
                      : "<table cellspacing='8' cellpadding='0'><tr><td width='50%'>M</td>"
                        "<td width='50%'>M</td></tr></table>");
        document.layout(400);
        const auto lines = document.lineRectangles();
        require(lines.size() == 2 && lines[0].left() >= 0 && lines[1].right() <= 400
                        && lines[1].left() > lines[0].right(),
                "Rich-text table widths exceed the viewport after cell spacing");
        require(mixed ? lines[0].width() > 3 * lines[1].width()
                      : qAbs(lines[0].width() - lines[1].width()) <= 1,
                "Rich-text table did not allocate the remaining width to its unconstrained column");
    }
    for (const bool plainText : { false, true }) {
        PrivateRichText document;
        document.setFont(font);
        const auto setText = [&](const QString &text) {
            if (plainText)
                document.setPlainText(text);
            else
                document.setHtml("<pre style='margin:0'>" + text + "</pre>");
            document.layout(400);
        };
        const auto inkCount = [&document] {
            QImage image(400, document.height(), QImage::Format_RGB32);
            image.fill(Qt::white);
            QPalette palette;
            palette.setColor(QPalette::WindowText, Qt::black);
            {
                QPainter painter(&image);
                document.draw(painter, image.rect(), palette);
            }
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    count += qGray(image.pixel(x, y)) < 128;
            return count;
        };
        setText("AB");
        const int letters = inkCount();
        const int plainWidth = document.naturalWidth();
        setText("A\tB");
        const int firstTabWidth = document.naturalWidth();
        require(inkCount() == letters && firstTabWidth > plainWidth,
                "Preformatted or license tab painted a missing glyph instead of blank spacing");
        setText("AA\tB");
        require(document.naturalWidth() == firstTabWidth,
                "Preformatted or license tab did not align the following text to a tab stop");
    }
    {
        const auto render = [&font](const QString &html) {
            PrivateRichText document;
            document.setFont(font);
            document.setHtml(html);
            document.layout(400);
            QImage image(400, document.height(), QImage::Format_RGB32);
            image.fill(Qt::white);
            QPalette palette;
            palette.setColor(QPalette::Window, Qt::white);
            palette.setColor(QPalette::WindowText, Qt::black);
            QPainter painter(&image);
            document.draw(painter, image.rect(), palette);
            return image;
        };
        const auto plain = render("<p style='margin:0'>oooo</p>");
        const auto struck = render("<p style='margin:0'><s>oooo</s></p>");
        require(plain.size() == struck.size(), "Strikeout changed rich-text line geometry");
        int mostAddedInk = 0;
        for (int y = 0; y < plain.height(); ++y) {
            int added = 0;
            for (int x = 0; x < plain.width(); ++x)
                added += qGray(struck.pixel(x, y)) + 40 < qGray(plain.pixel(x, y));
            mostAddedInk = qMax(mostAddedInk, added);
        }
        require(mostAddedInk > 20,
                "Footnote strikeout did not paint a horizontal stroke through its text");
    }
    for (const auto &heading : { QStringLiteral("العربية"), QStringLiteral("עברית") }) {
        PrivateRichText document;
        document.setFont(interfaceFont(32));
        document.setHtml("<p style='margin:0'>" + heading + "<br><b><u>123</u></b></p>");
        document.layout(400);
        const auto lines = document.lineRectangles();
        require(lines.size() == 2, "RTL rich-text fixture did not retain its two lines");
        QImage image(400, document.height(), QImage::Format_RGB32);
        image.fill(Qt::white);
        QPalette palette;
        palette.setColor(QPalette::WindowText, Qt::black);
        {
            QPainter painter(&image);
            document.draw(painter, image.rect(), palette);
        }
        for (const auto &line : lines) {
            int leftInk = 0, rightInk = 0;
            for (int y = qCeil(line.top()); y < qMin(image.height(), qFloor(line.bottom())); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qGray(image.pixel(x, y)) < 128)
                        (x < image.width() / 2 ? leftInk : rightInk)++;
            require(rightInk > 30 && leftInk == 0,
                    "Rich text lost automatic RTL paragraph direction on a formatted numeric continuation");
        }
    }
    {
        QImage icon(19, 61, QImage::Format_RGB32);
        icon.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        require(buffer.open(QIODevice::WriteOnly) && icon.save(&buffer, "PNG"),
                "Cannot encode the inline-image fixture");
        const auto markup = QString("<img src='data:image/png;base64,%1' width='19' height='61'>")
                                    .arg(QString::fromLatin1(png.toBase64()));
        PrivateRichText document;
        document.setFont(font);
        document.setHtml("A" + markup + "A");
        document.layout(400);
        const int expectedWidth = measurePrivateText("AA", font) + icon.width();
        require(qAbs(document.naturalWidth() - expectedWidth) <= 1
                        && document.lineRectangles().size() == 1
                        && document.height() >= icon.height(),
                "Inline rich-text image lost its advance or baseline height");
        const auto imageBounds = [&document] {
            QImage image(400, document.height(), QImage::Format_RGB32);
            image.fill(Qt::white);
            QPalette palette;
            palette.setColor(QPalette::WindowText, Qt::black);
            {
                QPainter painter(&image);
                document.draw(painter, image.rect(), palette);
            }
            QRect red;
            int pixels = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixel(x, y) == qRgb(255, 0, 0)) {
                        red |= QRect(x, y, 1, 1);
                        ++pixels;
                    }
            require(pixels == red.width() * red.height(),
                    "Inline rich-text image has missing interior pixels");
            return red;
        };
        require(imageBounds().size() == icon.size(),
                "Inline rich-text image was omitted, clipped or resized");
        document.layout(expectedWidth - 2);
        require(document.lineRectangles().size() == 2 && imageBounds().size() == icon.size(),
                "Rich-text wrapping ignored the inline image advance or clipped its height");
        document.setHtml(markup + markup);
        document.layout(400);
        require(document.naturalWidth() == 2 * icon.width()
                        && imageBounds().size() == QSize(2 * icon.width(), icon.height()),
                "Adjacent identical rich-text images collapsed into one object");
    }
}

inline void checkPrivateFallbackStyles(const kobalt::AssetBundle &assets)
{
    using namespace kobalt::ui;

    struct Sample
    {
        const char *stem;
        const char *text;
        bool italics;
    };

    for (const auto &sample :
         { Sample{ "NotoSerif", "ΕλληνικάРусские", true },
           Sample{ "NotoNaskhArabic", "العَرَبِيَّة", false },
           Sample{ "NotoSerifHebrew", "עִבְרִית", false },
           Sample{ "NotoSerifArmenian", "Հայերեն", false },
           Sample{ "NotoSerifGeorgian", "ქართული", false },
           Sample{ "NotoSerifDevanagari", "हिन्दी", false },
           Sample{ "NotoSerifBengali", "বাংলা", false }, Sample{ "NotoSerifTamil", "தமிழ்", true },
           Sample{ "NotoSerifThai", "ภาษาไทย", false } }) {
        const auto text = QString::fromUtf8(sample.text);
        const auto alias = QString("Style reference %1").arg(sample.stem);
        for (const auto *style : { "Regular", "Bold", "Italic", "BoldItalic" }) {
            const QString name(style);
            if (name.contains("Italic") && !sample.italics) {
                continue;
            }
            require(registerPrivateInterfaceFont(
                            assets.file(QString("fonts/%1-%2.ttf").arg(sample.stem, style)), alias),
                    "Cannot register fallback style reference");
        }
        for (const bool italic : { false, true }) {
            if (italic && !sample.italics) {
                continue;
            }
            for (const bool bold : { false, true }) {
                QFont fallback("Instrument Serif"), reference(alias);
                for (auto *font : { &fallback, &reference }) {
                    font->setPixelSize(48);
                    font->setBold(bold);
                    font->setItalic(italic);
                }
                require(needsPrivateText(text, fallback),
                        "Style fixture is covered by its primary face");
                const auto actual = renderPrivateText(text, fallback, 0);
                const auto expected = renderPrivateText(text, reference, 0);
                require(!actual.image.isNull() && actual.missingGlyphs == 0
                                && actual.image == expected.image
                                && actual.advance == expected.advance
                                && actual.origin == expected.origin,
                        qPrintable(QString("Fallback ignored the real %1 bold=%2 italic=%3 face")
                                           .arg(sample.stem)
                                           .arg(bold)
                                           .arg(italic)));
                const QVector<QPair<int, int>> match{ { 0, int(text.size()) } };
                const auto highlighted = renderPrivateText(text, fallback, 0, match, {},
                                                           PrivateTextEmphasis::FontFace);
                const auto expectedMatch = renderPrivateText(text, reference, 0, match, {},
                                                             PrivateTextEmphasis::FontFace);
                require(highlighted.image == expectedMatch.image
                                && highlighted.advance == expectedMatch.advance
                                && measurePrivateText(text, fallback, match,
                                                      PrivateTextEmphasis::FontFace)
                                        == highlighted.advance,
                        "Fallback search match did not use the real bold face for measurement and drawing");
            }
        }
    }
}

inline void checkPrivateInterfaceText(const kobalt::AssetBundle &assets)
{
    using namespace kobalt::ui;
    require(privateInterfaceFontBytes() == 0, "Private interface fonts were loaded eagerly");
    QFont font(QStringLiteral("IBM Plex Sans"));
    font.setPixelSize(32);
    require(needsPrivateText(QStringLiteral("Chapter 4"), font),
            "Covered Latin text bypassed the private renderer");
    QFont readingFont("Custom Reading Face");
    require(needsPrivateText(QStringLiteral("第四章 한국어 목차"), readingFont),
            "Private interface renderer replaced a requested reading font");
    require(needsPrivateText(QStringLiteral("第四章 한국어 목차"), font),
            "CJK text stayed on the native font backend");
    const auto primaryBytes = QFileInfo(assets.file("fonts/IBMPlexSans-Regular.ttf")).size();
    require(privateInterfaceFontBytes() == 0, "Text routing loaded font bytes before rendering");
    const auto firstCjk = renderPrivateText(QStringLiteral("第一章 中文目录"), font);
    require(firstCjk.missingGlyphs == 0 && !firstCjk.image.isNull(),
            "Lazy CJK font did not render on first use");
    const auto cjkBytes = QFileInfo(assets.file("fonts/NotoSerifCJK-Regular.ttc")).size();
    require(privateInterfaceFontBytes() == primaryBytes + cjkBytes,
            "CJK rendering loaded unrelated fallback fonts");
    renderPrivateText(QStringLiteral("한국어 日本語 中文"), font);
    require(privateInterfaceFontBytes() == primaryBytes + cjkBytes,
            "CJK regional faces duplicated font bytes");
    for (const auto &sample :
         { QStringLiteral("第一章 中文目录 简体汉字"),
           QStringLiteral("第二章 中文目錄 繁體漢字 龍"), QStringLiteral("第三章 日本語の目次"),
           QStringLiteral("第四章 한국어 목차"), QStringLiteral("Chapter 4 العربية الكتاب 123"),
           QStringLiteral("中文 English 한국어 हिन्दी"), QStringLiteral("Ελληνικά Русские ∑∫√"),
           QStringLiteral("עברית Հայերեն ქართული"),
           QStringLiteral("हिन्दी अध्याय বাংলা অধ্যায় தமிழ் அத்தியாயம் ไทย สารบัญ") }) {
        const auto rendered = renderPrivateText(sample, font);
        require(rendered.missingGlyphs == 0, "Private interface renderer produced a missing glyph");
        require(rendered.advance > 0 && !rendered.image.isNull(),
                "Private interface renderer produced no text");
        bool ink = false;
        for (int y = 0; y < rendered.image.height() && !ink; ++y) {
            for (int x = 0; x < rendered.image.width(); ++x) {
                if (qAlpha(rendered.image.pixel(x, y))) {
                    ink = true;
                    break;
                }
            }
        }
        require(ink, "Private interface renderer produced an empty bitmap");
        require(elidePrivateText(sample, font, rendered.advance) == sample,
                "Private text elided a fitting title");
        const auto elided = elidePrivateText(sample, font, 100);
        require(elided.endsWith(QChar(0x2026)), "Private text did not mark the omitted title");
        require(renderPrivateText(elided, font).advance <= 100,
                "Private ellipsis overflowed the available width");
        QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, sample);
        boundary.setPosition(elided.size() - 1);
        require(boundary.isAtBoundary(), "Private ellipsis split a grapheme");
        require(elidePrivateText(sample, font, 0).isEmpty(),
                "Private text drew into zero available width");
        QVector<int> ends;
        boundary.toStart();
        for (int end = boundary.toNextBoundary(); end >= 0; end = boundary.toNextBoundary())
            ends.append(end);
        const auto advance = [&](int start, int end) {
            return measurePrivateText(sample.mid(start, end - start), font);
        };
        // A carried estimate from an earlier line must not change the break.
        qreal unitAdvance = 0;
        for (int start : { 0, ends.value(ends.size() / 3) })
            for (int width : { 1, 40, 100, 160, 300, rendered.advance }) {
                const int end = fittingTextEnd(
                        ends, start, sample.size(), width,
                        [&](int candidate) { return advance(start, candidate); }, unitAdvance);
                require(end == start || (end > start && advance(start, end) <= width),
                        "Line fitting returned text wider than the line");
                const auto next = std::upper_bound(ends.begin(), ends.end(), end);
                require(next == ends.end() || advance(start, *next) > width,
                        "Line fitting stopped before the last grapheme that fits");
            }
    }
    {
        // On text whose advance grows with its length, the search finds the
        // same break as checking every grapheme.
        const QString text =
                QStringLiteral("The quick brown fox jumps over the lazy dog, again and again. ")
                        .repeated(4);
        QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, text);
        QVector<int> ends;
        for (int end = graphemes.toNextBoundary(); end >= 0; end = graphemes.toNextBoundary())
            ends.append(end);
        qreal unitAdvance = 0;
        for (int limit : { int(text.size()), 40 })
            for (int width = 1; width < 1400; width += 37) {
                int expected = 0;
                for (int end : ends)
                    if (end <= limit && measurePrivateText(text.left(end), font) <= width)
                        expected = end;
                require(fittingTextEnd(
                                ends, 0, limit, width,
                                [&](int end) { return measurePrivateText(text.left(end), font); },
                                unitAdvance)
                                == expected,
                        "Line fitting found a different break from a full scan");
            }
    }
    // Registering a large catalog must not retain any of its file contents.
    const auto beforeCatalog = privateInterfaceFontBytes();
    QTemporaryDir catalogDirectory;
    require(catalogDirectory.isValid(), "Cannot create font catalog fixture");
    QStringList catalogFiles;
    for (int i = 0; i < 4; ++i) {
        const auto path = catalogDirectory.path() + QString("/font-%1.ttf").arg(i);
        require(QFile::copy(assets.file("fonts/IBMPlexSans-Regular.ttf"), path),
                "Cannot copy font fixture");
        QFile padded(path);
        // Three of these already exceed the budget, so the reload cycles below must evict.
        require(padded.open(QIODevice::ReadWrite)
                        && padded.resize(privateInterfaceFontBudget * 3 / 8),
                "Cannot pad font fixture");
        padded.close();
        require(registerPrivateInterfaceFont(path, QString("Budget Test %1").arg(i)),
                "Cannot register budget fixture");
        catalogFiles.append(path);
    }
    require(privateInterfaceFontBytes() == beforeCatalog,
            "Font metadata registration retained file bytes");
    const auto oversized = catalogDirectory.filePath("oversized.ttf");
    require(QFile::copy(catalogFiles.first(), oversized), "Cannot copy oversized font fixture");
    QFile oversizedFile(oversized);
    require(oversizedFile.open(QIODevice::ReadWrite)
                    && oversizedFile.resize(privateInterfaceFontBudget + 1),
            "Cannot grow oversized font fixture");
    oversizedFile.close();
    require(!registerPrivateInterfaceFont(oversized, "Oversized Test"),
            "Oversized font was accepted");
    const auto damaged = catalogDirectory.filePath("damaged.ttf");
    require(QFile::copy(catalogFiles.first(), damaged)
                    && registerPrivateInterfaceFont(damaged, "Damaged Test"),
            "Cannot register changing font fixture");
    QFile damagedFile(damaged);
    require(damagedFile.open(QIODevice::ReadWrite) && damagedFile.write("bad!", 4) == 4,
            "Cannot damage font after metadata discovery");
    damagedFile.close();
    QFont damagedFont("Damaged Test");
    damagedFont.setPixelSize(32);
    renderPrivateText(QStringLiteral("Unreadable face"), damagedFont);
    // The later reload cycles must reclaim this invalid source too.

    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < catalogFiles.size(); ++i) {
            QFont candidate(QString("Budget Test %1").arg(i));
            candidate.setPixelSize(32);
            const auto result = renderPrivateText(
                    QString("Candidate %1 round %2").arg(i).arg(cycle), candidate);
            require(!result.image.isNull() && result.missingGlyphs == 0,
                    "Evicted font did not reload");
            require(privateInterfaceFontBytes() <= privateInterfaceFontBudget,
                    "Font cache exceeded its byte budget");
        }
        const auto multilingual =
                renderPrivateText(QStringLiteral("中文 한국어 العربية हिन्दी"), font);
        require(multilingual.missingGlyphs == 0,
                "Fallback faces were lost after font cache eviction");
    }
    require(registerPrivateInterfaceFont(assets.file("fonts/NotoSerifCJK-Regular.ttc"),
                                         "Noto Serif CJK SC"),
            "Cannot retain the CJK collection");
    QFont cjkFont("Noto Serif CJK SC");
    cjkFont.setPixelSize(32);
    require(needsPrivateText(QStringLiteral("Hox"), cjkFont),
            "CFF preview stayed on the native font backend");
    const auto cjkPreview = renderPrivateText(QStringLiteral("第一章 한국어 Hox"), cjkFont);
    require(!cjkPreview.image.isNull() && cjkPreview.missingGlyphs == 0,
            "Private CJK preview lost the selected face");
    require(registerPrivateInterfaceFont(assets.file("fonts/IBMPlexSans-Regular.ttf"),
                                         "Reading Test Alias"),
            "Cannot retain a TrueType reading font");
    QFont readingAlias("Reading Test Alias");
    readingAlias.setPixelSize(32);
    require(needsPrivateText(QStringLiteral("Hox"), readingAlias),
            "Covered TrueType reading text changed backend");
    require(needsPrivateText(QStringLiteral("第一章"), readingAlias),
            "Reading font did not use private CJK fallback");
    QTemporaryDir readingDirectory;
    require(readingDirectory.isValid(), "Cannot create private reading-font fixture");
    const auto readingPath = readingDirectory.path() + "/reading.ttf";
    require(QFile::copy(assets.file("fonts/IBMPlexSans-Regular.ttf"), readingPath),
            "Cannot copy private reading-font fixture");
    const auto fontFamilies = [] {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return QFontDatabase::families();
#else
        return QFontDatabase().families();
#endif
    };
    const auto familiesBefore = fontFamilies();
    require(registerPrivateInterfaceFont(readingPath, "Retained Reading Test"),
            "Cannot retain a TrueType preview font");
    require(fontFamilies() == familiesBefore,
            "Private reading registration changed Qt's font database");

    QFont retainedReading("Retained Reading Test");
    retainedReading.setPixelSize(32);
    renderPrivateText(QStringLiteral("Hox"), retainedReading);
    require(QFile::remove(readingPath), "Cannot remove private reading-font fixture");
    const auto retainedSample = renderPrivateText(QStringLiteral("Hox"), retainedReading);
    require(needsPrivateText(QStringLiteral("Hox"), retainedReading)
                    && !retainedSample.image.isNull() && retainedSample.missingGlyphs == 0,
            "Private TrueType preview could not render after its source file was removed");
    require(!registerPrivateInterfaceFont(assets.root() + "/missing-font.ttf", "Missing"),
            "Missing reading font was accepted");
    require(registerPrivateInterfaceFont(assets.file("fonts/ebook-core/Libron-Regular.ttf"),
                                         "Private Excerpt Serif"),
            "Cannot register private excerpt serif");
    require(registerPrivateInterfaceFont(assets.file("fonts/ebook-core/Libron-Bold.ttf"),
                                         "Private Excerpt Serif"),
            "Cannot register private excerpt bold");
    {
        // A private label without descenders centers its letters, as the
        // font button's name does with Qt's tight bounds.
        QImage canvas(400, 72, QImage::Format_RGB32);
        canvas.fill(Qt::white);
        QPainter painter(&canvas);
        QFont face("Private Excerpt Serif");
        face.setPixelSize(32);
        painter.setFont(face);
        painter.setPen(Qt::black);
        drawOpticalText(painter, QRectF(canvas.rect()), QStringLiteral("Libron"));
        painter.end();
        int first = -1, last = -1;
        for (int y = 0; y < canvas.height(); ++y)
            for (int x = 0; x < canvas.width(); ++x)
                if (qGray(canvas.pixel(x, y)) < 128) {
                    if (first < 0)
                        first = y;
                    last = y;
                    break;
                }
        require(first >= 0 && qAbs((first + last + 1) - canvas.height()) <= 2,
                "Private optical text did not center its letters");
    }
    WholeLineLabel excerpt;
    excerpt.setAlignment(Qt::AlignLeft | Qt::AlignTop);
    excerpt.setStyleSheet("background:white;color:black;border:none;");
    excerpt.resize(800, 120);
    excerpt.setText("<div style=\"line-height:130%\"><b><u>More</u></b> fixture</div>");
    const auto excerptPixels = [&](const QString &family) {
        QFont face(family);
        face.setPixelSize(32);
        excerpt.setFont(face);
        QImage image(excerpt.size(), QImage::Format_RGB32);
        image.fill(Qt::white);
        excerpt.render(&image);
        return image;
    };
    const auto sansExcerpt = excerptPixels("Retained Reading Test");
    const auto serifExcerpt = excerptPixels("Private Excerpt Serif");
    require(sansExcerpt != serifExcerpt, "Search excerpts ignored their private reading face");
    excerpt.setText("<div style=\"line-height:130%\"><b>More</b> fixture</div>");
    const auto boldExcerpt = excerptPixels("Private Excerpt Serif");
    int underlinePixels = 0;
    for (int y = 0; y < serifExcerpt.height(); ++y) {
        for (int x = 0; x < serifExcerpt.width(); ++x) {
            underlinePixels += qGray(serifExcerpt.pixel(x, y)) < qGray(boldExcerpt.pixel(x, y));
        }
    }
    require(underlinePixels > 50, "Private search match lost its underline pixels");
    excerpt.setText("<div style=\"line-height:130%\">More fixture</div>");
    require(excerptPixels("Private Excerpt Serif") != boldExcerpt,
            "Private search match lost its bold face");
    const auto oneLine =
            excerptTextHeight("<div style=\"line-height:130%\">More</div>", excerpt.font(), 800);
    const auto twoLines = excerptTextHeight("<div style=\"line-height:130%\">More<br>More</div>",
                                            excerpt.font(), 800);
    require(twoLines == 2 * oneLine && twoLines > 80,
            "Private excerpt measurement lost reading line spacing");
    const auto excerptMetrics = measurePrivateTextLayout("M", excerpt.font());
    require(excerptMetrics.height == 33 && excerptMetrics.lineHeight == 43
                    && excerptMetrics.lineAscent == 34 && qAbs(twoLines - 111.8) < 0.01,
            "Private excerpt lost the legacy book-font baseline and line box");
    require(qMax(int(metrics::list::threeLines),
                 metrics::search::excerptTop + qCeil(twoLines) + metrics::search::rowBottom)
                    == 188,
            "Private excerpt changed the established search row height");
    // At 32px the pinned CJK header needs 37px above the baseline, while
    // Naskh Arabic needs 21px below it. Neither face alone needs all 58px.
    const auto mixedMetrics =
            measurePrivateTextLayout(QStringLiteral("中文 العربية"), excerpt.font());
    require(mixedMetrics.lineAscent == 37 && mixedMetrics.lineHeight == 58,
            "Mixed-script excerpt lost a fallback face's ascent or descent");
    {
        QFont face("Private Excerpt Serif");
        face.setPixelSize(32);
        const auto preview = [&](const QString &text, int maximumLines, int *count = nullptr) {
            QImage image(320, 180, QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            painter.setFont(face);
            painter.setPen(Qt::black);
            QVector<QRect> lines;
            drawWrappedInterfaceText(painter, image.rect(), text, Qt::AlignLeft | Qt::AlignTop,
                                     maximumLines, 0, &lines);
            if (count)
                *count = lines.size();
            return image;
        };
        const QString two = QStringLiteral("First line\nSecond line");
        const auto complete = preview(two, 0);
        require(preview(two, 2) == complete, "Two complete preview lines gained an ellipsis");
        int count = 0;
        const auto clipped = preview(two + QStringLiteral("\nThird line"), 2, &count);
        require(count == 2 && clipped != complete,
                "Overflowing preview lost its two-line ellipsis");
        require(preview(two + QStringLiteral("\nThird line ").repeated(1000), 2) == clipped,
                "Hidden passage changed the visible preview lines");
        require(preview(two + QStringLiteral("\n"), 2) == complete,
                "Terminal line break gained an ellipsis");
        require(preview(two + QStringLiteral("\n\n"), 2) == clipped,
                "Trailing blank line did not count as preview overflow");
        const QString words =
                QStringLiteral("A long passage with e\u0301 and 中文 words repeated. ");
        const auto shortPreview = preview(words.repeated(4), 2, &count);
        require(count == 2 && preview(words.repeated(200), 2) == shortPreview,
                "Long wrapped passage changed the visible Unicode preview");
        require(wrappedInterfaceTextHeight(words.repeated(200), face, 320, 2)
                        == wrappedInterfaceTextHeight(words.repeated(4), face, 320, 2),
                "Bounded preview height included hidden lines");
    }
    for (const int ppi : { 212, 300 }) {
        setDensity(ppi);
        for (const bool dark : { false, true }) {
            QFont face = excerpt.font();
            face.setPixelSize(metrics::search::excerptText);
            excerpt.setFont(face);
            const QString line = "<div style=\"line-height:130%\"><b><u>More</u></b> fixture</div>";
            const int fullHeight = qCeil(excerptTextHeight(line, face, 800));
            excerpt.resize(800, fullHeight);
            excerpt.setStyleSheet(dark ? "background:black;color:white;border:none;"
                                       : "background:white;color:black;border:none;");
            excerpt.setText(line);
            const auto firstLine = excerpt.grab().toImage();
            excerpt.resize(800, fullHeight + 1);
            excerpt.setText(
                    QString(line).replace("</div>", "<br><b><u>More</u></b> fixture</div>"));
            require(excerpt.grab().toImage().copy(0, 0, 800, fullHeight) == firstLine,
                    "Private excerpt painted a clipped second line");
        }
    }
    setDensity(designPpi);
    excerpt.resize(800, 180);
    excerpt.setStyleSheet("background:white;color:black;border:none;");
    excerpt.setText(
            QStringLiteral("<div style=\"line-height:130%\">العربية<br><b><u>123</u></b></div>"));
    const auto rtlExcerpt = excerpt.grab().toImage();
    int leftInk = 0, rightInk = 0;
    for (int y = 80; y < rtlExcerpt.height(); ++y) {
        for (int x = 0; x < rtlExcerpt.width(); ++x) {
            if (qGray(rtlExcerpt.pixel(x, y)) < 128) {
                (x < 400 ? leftInk : rightInk)++;
            }
        }
    }
    require(rightInk > 50 && leftInk == 0,
            "Wrapped RTL excerpt lost paragraph direction on a numeric line");
    excerpt.setFont(cjkFont);
    excerpt.setText(
            QStringLiteral("<div style=\"line-height:130%\"><b><u>第一章</u></b> 한국어</div>"));
    const auto beforeCjkExcerpt = fontFamilies();
    const auto richCjk = excerpt.grab().toImage();
    excerpt.clear();
    require(richCjk != excerpt.grab().toImage() && fontFamilies() == beforeCjkExcerpt,
            "CFF search excerpt lost text or registered its font with Qt");
    const QString clusterTitle = QStringLiteral("किं العربية 한국어");
    const auto clusterEllipsis = elidePrivateText(clusterTitle, font, 100);
    QTextBoundaryFinder clusterBoundary(QTextBoundaryFinder::Grapheme, clusterTitle);
    clusterBoundary.setPosition(clusterEllipsis.size() - 1);
    require(clusterBoundary.isAtBoundary(), "Ellipsis split combining marks in mixed RTL text");
    const QString longTitle(10000, QChar(0x4e2d));
    const auto shortened = elidePrivateText(longTitle, font, 100);
    require(shortened.size() < 10 && measurePrivateText(shortened, font) <= 100,
            "Long CJK title was not safely measured and elided");
    QFont tracked = font;
    tracked.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    const QString fourLetters = QStringLiteral("中文한국");
    require(measurePrivateText(fourLetters, tracked) == measurePrivateText(fourLetters, font) + 6,
            "Private interface renderer lost caption tracking");
    QFont capitals = font;
    capitals.setCapitalization(QFont::AllUppercase);
    require(renderPrivateText(QStringLiteral("Ελληνικά"), capitals).image
                    == renderPrivateText(QStringLiteral("ΕΛΛΗΝΙΚΆ"), font).image,
            "Private caption capitalization changed");
    const QString preview = QStringLiteral("Noto Serif CJK");
    const auto ordinaryPreview = renderPrivateText(preview, cjkFont, 0);
    const auto bionicPreview =
            renderPrivateText(preview, cjkFont, 0, { { 0, 2 }, { 5, 3 }, { 11, 2 } });
    require(ordinaryPreview.image != bionicPreview.image,
            "Private font preview lost bold prefixes");
    QWidget readerHost;
    readerHost.resize(1200, 200);
    readerStyle(&readerHost, styles::reader());
    ListRow row(&readerHost);
    row.setTitlePixels(32);
    row.resize(1200, 100);
    QImage emptyRow(row.size(), QImage::Format_ARGB32_Premultiplied);
    emptyRow.fill(Qt::white);
    row.render(&emptyRow);
    const QString chineseTitle = QStringLiteral("第一章 中文目录 简体汉字");
    row.setTitle(chineseTitle);
    QImage chineseRow(row.size(), QImage::Format_ARGB32_Premultiplied);
    chineseRow.fill(Qt::white);
    row.render(&chineseRow);
    require(dynamic_cast<PrivateTextLabel *>(row.titleLabel()),
            "Chapter row title did not use the private label");
    require(needsPrivateText(row.titleLabel()->text(), row.titleLabel()->font()),
            "Chinese chapter row stayed on the native text path");
    require(row.titleLabel()->text() == chineseTitle && emptyRow != chineseRow,
            "Chinese chapter row did not paint its title");
    PrivateTextLabel label(&readerHost);
    label.setFont(font);
    label.resize(800, 80);
    label.setText(QStringLiteral("第四章 한국어 목차"));
    QImage pixels(label.size(), QImage::Format_ARGB32_Premultiplied);
    pixels.fill(Qt::white);
    label.render(&pixels);
    require(label.text() == QStringLiteral("第四章 한국어 목차"),
            "Private label lost its accessible text");
    auto firstInkColumn = [](const QImage &image) {
        for (int x = 0; x < image.width(); ++x) {
            for (int y = 0; y < image.height(); ++y) {
                if (qGray(image.pixel(x, y)) < 128) {
                    return x;
                }
            }
        }
        return image.width();
    };
    label.setStyleSheet("background:white;color:black;border:none;");
    label.setLayoutDirection(Qt::LeftToRight);
    for (const auto &rtl :
         { QStringLiteral("العربية فصول الكتاب"), QStringLiteral("עברית פרקי הספר") }) {
        label.setText(rtl);
        label.setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        pixels.fill(Qt::white);
        label.render(&pixels);
        const int logicalLeft = firstInkColumn(pixels);
        label.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute | Qt::AlignVCenter);
        pixels.fill(Qt::white);
        label.render(&pixels);
        const int absoluteLeft = firstInkColumn(pixels);
        require(logicalLeft > label.width() / 2 && absoluteLeft < 100,
                "Private RTL label did not preserve QLabel visual alignment and absolute left alignment");
    }
    checkPrivateFallbackStyles(assets);
    checkPrivateRichText();
    require(initializePrivateInterfaceText(assets),
            "Cannot restore private font state after component checks");
}
