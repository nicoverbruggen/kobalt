#pragma once

#include "check.h"
#include "assets/assets.h"
#include "interfaces/reader/ui/style/private_text.h"
#include "interfaces/reader/ui/controls/rows.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/style/styles.h"
#include "interfaces/reader/ui/style/theme.h"

#include <QPainter>
#include <QTextBoundaryFinder>
#include <QFileInfo>

inline void checkPrivateFallbackStyles(const kobalt::AssetBundle &assets) {
    using namespace kobalt::ui;
    struct Sample {
        const char *stem;
        const char *text;
        bool italics;
    };
    for (const auto &sample :
         {Sample{"NotoSerif", "ΕλληνικάРусские", true}, Sample{"NotoNaskhArabic", "العَرَبِيَّة", false},
          Sample{"NotoSerifHebrew", "עִבְרִית", false}, Sample{"NotoSerifArmenian", "Հայերեն", false},
          Sample{"NotoSerifGeorgian", "ქართული", false}, Sample{"NotoSerifDevanagari", "हिन्दी", false},
          Sample{"NotoSerifBengali", "বাংলা", false}, Sample{"NotoSerifTamil", "தமிழ்", true},
          Sample{"NotoSerifThai", "ภาษาไทย", false}}) {
        const auto text = QString::fromUtf8(sample.text);
        const auto alias = QString("Style reference %1").arg(sample.stem);
        for (const auto *style : {"Regular", "Bold", "Italic", "BoldItalic"}) {
            const QString name(style);
            if (name.contains("Italic") && !sample.italics) {
                continue;
            }
            require(registerPrivateInterfaceFont(assets.file(QString("fonts/%1-%2.ttf").arg(sample.stem, style)), alias,
                                                 true) == PrivateFontRegistration::Private,
                    "Cannot register fallback style reference");
        }
        for (const bool italic : {false, true}) {
            if (italic && !sample.italics) {
                continue;
            }
            for (const bool bold : {false, true}) {
                QFont fallback("Kobalt UI Serif"), reference(alias);
                for (auto *font : {&fallback, &reference}) {
                    font->setPixelSize(48);
                    font->setBold(bold);
                    font->setItalic(italic);
                }
                require(needsPrivateText(text, fallback), "Style fixture is covered by its primary face");
                const auto actual = renderPrivateText(text, fallback, 0);
                const auto expected = renderPrivateText(text, reference, 0);
                require(!actual.image.isNull() && actual.missingGlyphs == 0 && actual.image == expected.image &&
                            actual.advance == expected.advance && actual.origin == expected.origin,
                        qPrintable(QString("Fallback ignored the real %1 bold=%2 italic=%3 face")
                                       .arg(sample.stem)
                                       .arg(bold)
                                       .arg(italic)));
                const QVector<QPair<int, int>> match{{0, int(text.size())}};
                const auto highlighted = renderPrivateText(text, fallback, 0, match, {}, PrivateTextEmphasis::FontFace);
                const auto expectedMatch =
                    renderPrivateText(text, reference, 0, match, {}, PrivateTextEmphasis::FontFace);
                require(highlighted.image == expectedMatch.image && highlighted.advance == expectedMatch.advance &&
                            measurePrivateText(text, fallback, match, PrivateTextEmphasis::FontFace) ==
                                highlighted.advance,
                        "Fallback search match did not use the real bold face for measurement and drawing");
            }
        }
    }
}

inline void checkPrivateInterfaceText(const kobalt::AssetBundle &assets) {
    using namespace kobalt::ui;
    require(privateInterfaceFontBytes() == 0, "Private interface fonts were loaded eagerly");
    QFont font(QStringLiteral("Kobalt UI Sans"));
    font.setPixelSize(32);
    require(!needsPrivateText(QStringLiteral("Chapter 4"), font), "Private renderer replaced covered Latin text");
    QFont readingFont("Custom Reading Face");
    require(!needsPrivateText(QStringLiteral("第四章 한국어 목차"), readingFont),
            "Private interface renderer replaced a requested reading font");
    require(needsPrivateText(QStringLiteral("第四章 한국어 목차"), font), "CJK text stayed on the native font backend");
    const auto primaryBytes = QFileInfo(assets.file("fonts/IBMPlexSans-Regular.ttf")).size();
    require(privateInterfaceFontBytes() == primaryBytes, "Latin text or fallback detection loaded fallback fonts");
    const auto firstCjk = renderPrivateText(QStringLiteral("第一章 中文目录"), font);
    require(firstCjk.missingGlyphs == 0 && !firstCjk.image.isNull(), "Lazy CJK font did not render on first use");
    const auto cjkBytes = QFileInfo(assets.file("fonts/NotoSerifCJK-Regular.ttc")).size();
    require(privateInterfaceFontBytes() == primaryBytes + cjkBytes, "CJK rendering loaded unrelated fallback fonts");
    renderPrivateText(QStringLiteral("한국어 日本語 中文"), font);
    require(privateInterfaceFontBytes() == primaryBytes + cjkBytes, "CJK regional faces duplicated font bytes");
    for (const auto &sample :
         {QStringLiteral("第一章 中文目录 简体汉字"), QStringLiteral("第二章 中文目錄 繁體漢字 龍"),
          QStringLiteral("第三章 日本語の目次"), QStringLiteral("第四章 한국어 목차"),
          QStringLiteral("Chapter 4 العربية الكتاب 123"), QStringLiteral("中文 English 한국어 हिन्दी"),
          QStringLiteral("Ελληνικά Русские ∑∫√"), QStringLiteral("עברית Հայերեն ქართული"),
          QStringLiteral("हिन्दी अध्याय বাংলা অধ্যায় தமிழ் அத்தியாயம் ไทย สารบัญ")}) {
        const auto rendered = renderPrivateText(sample, font);
        require(rendered.missingGlyphs == 0, "Private interface renderer produced a missing glyph");
        require(rendered.advance > 0 && !rendered.image.isNull(), "Private interface renderer produced no text");
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
        require(elidePrivateText(sample, font, rendered.advance) == sample, "Private text elided a fitting title");
        const auto elided = elidePrivateText(sample, font, 100);
        require(elided.endsWith(QChar(0x2026)), "Private text did not mark the omitted title");
        require(renderPrivateText(elided, font).advance <= 100, "Private ellipsis overflowed the available width");
        QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, sample);
        boundary.setPosition(elided.size() - 1);
        require(boundary.isAtBoundary(), "Private ellipsis split a grapheme");
        require(elidePrivateText(sample, font, 0).isEmpty(), "Private text drew into zero available width");
    }
    require(registerPrivateInterfaceFont(assets.file("fonts/NotoSerifCJK-Regular.ttc"), "Noto Serif CJK SC") ==
                PrivateFontRegistration::Private,
            "CJK collection was allowed onto the native font backend");
    QFont cjkFont("Noto Serif CJK SC");
    cjkFont.setPixelSize(32);
    require(needsPrivateText(QStringLiteral("Hox"), cjkFont), "CFF preview stayed on the native font backend");
    const auto cjkPreview = renderPrivateText(QStringLiteral("第一章 한국어 Hox"), cjkFont);
    require(!cjkPreview.image.isNull() && cjkPreview.missingGlyphs == 0, "Private CJK preview lost the selected face");
    require(registerPrivateInterfaceFont(assets.file("fonts/IBMPlexSans-Regular.ttf"), "Reading Test Alias") ==
                PrivateFontRegistration::Native,
            "TrueType reading font could not retain native painting");
    QFont readingAlias("Reading Test Alias");
    readingAlias.setPixelSize(32);
    require(!needsPrivateText(QStringLiteral("Hox"), readingAlias), "Covered TrueType reading text changed backend");
    require(needsPrivateText(QStringLiteral("第一章"), readingAlias), "Reading font did not use private CJK fallback");
    require(registerPrivateInterfaceFont(assets.file("fonts/IBMPlexSans-Regular.ttf"), "Reading Test Alias", true) ==
                    PrivateFontRegistration::Private &&
                needsPrivateText(QStringLiteral("Hox"), readingAlias),
            "Forced private painting reused a cached native registration");
    require(registerPrivateInterfaceFont(assets.file("fonts/IBMPlexSans-Regular.ttf"), "Reading Test Alias") ==
                PrivateFontRegistration::Private,
            "A later registration put a private preview back on the native backend");
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
    require(registerPrivateInterfaceFont(readingPath, "Retained Reading Test", true) ==
                PrivateFontRegistration::Private,
            "TrueType preview requested native Qt registration");
    require(fontFamilies() == familiesBefore, "Private reading registration changed Qt's font database");
    require(QFile::remove(readingPath), "Cannot remove private reading-font fixture");
    QFont retainedReading("Retained Reading Test");
    retainedReading.setPixelSize(32);
    const auto retainedSample = renderPrivateText(QStringLiteral("Hox"), retainedReading);
    require(needsPrivateText(QStringLiteral("Hox"), retainedReading) && !retainedSample.image.isNull() &&
                retainedSample.missingGlyphs == 0,
            "Private TrueType preview could not render after its source file was removed");
    require(registerPrivateInterfaceFont(assets.root() + "/missing-font.ttf", "Missing") ==
                PrivateFontRegistration::Failed,
            "Missing reading font was accepted");
    require(registerPrivateInterfaceFont(assets.file("fonts/ebook-core/Libron-Regular.ttf"), "Private Excerpt Serif",
                                         true) == PrivateFontRegistration::Private,
            "Cannot register private excerpt serif");
    require(registerPrivateInterfaceFont(assets.file("fonts/ebook-core/Libron-Bold.ttf"), "Private Excerpt Serif",
                                         true) == PrivateFontRegistration::Private,
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
    require(excerptPixels("Private Excerpt Serif") != boldExcerpt, "Private search match lost its bold face");
    const auto oneLine = excerptTextHeight("<div style=\"line-height:130%\">More</div>", excerpt.font(), 800);
    const auto twoLines = excerptTextHeight("<div style=\"line-height:130%\">More<br>More</div>", excerpt.font(), 800);
    require(twoLines == 2 * oneLine && twoLines > 80, "Private excerpt measurement lost reading line spacing");
    const auto excerptMetrics = measurePrivateTextLayout("M", excerpt.font());
    require(excerptMetrics.height == 33 && excerptMetrics.lineHeight == 43 && excerptMetrics.lineAscent == 34 &&
                qAbs(twoLines - 111.8) < 0.01,
            "Private excerpt lost the legacy book-font baseline and line box");
    require(qMax(int(metrics::list::threeLines),
                 metrics::search::excerptTop + qCeil(twoLines) + metrics::search::rowBottom) == 188,
            "Private excerpt changed the established search row height");
    // At 32px the pinned CJK header needs 37px above the baseline, while
    // Naskh Arabic needs 21px below it. Neither face alone needs all 58px.
    const auto mixedMetrics = measurePrivateTextLayout(QStringLiteral("中文 العربية"), excerpt.font());
    require(mixedMetrics.lineAscent == 37 && mixedMetrics.lineHeight == 58,
            "Mixed-script excerpt lost a fallback face's ascent or descent");
    for (const int ppi : {212, 300}) {
        setDensity(ppi);
        for (const bool dark : {false, true}) {
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
            excerpt.setText(QString(line).replace("</div>", "<br><b><u>More</u></b> fixture</div>"));
            require(excerpt.grab().toImage().copy(0, 0, 800, fullHeight) == firstLine,
                    "Private excerpt painted a clipped second line");
        }
    }
    setDensity(designPpi);
    excerpt.resize(800, 180);
    excerpt.setStyleSheet("background:white;color:black;border:none;");
    excerpt.setText(QStringLiteral("<div style=\"line-height:130%\">العربية<br><b><u>123</u></b></div>"));
    const auto rtlExcerpt = excerpt.grab().toImage();
    int leftInk = 0, rightInk = 0;
    for (int y = 80; y < rtlExcerpt.height(); ++y) {
        for (int x = 0; x < rtlExcerpt.width(); ++x) {
            if (qGray(rtlExcerpt.pixel(x, y)) < 128) {
                (x < 400 ? leftInk : rightInk)++;
            }
        }
    }
    require(rightInk > 50 && leftInk == 0, "Wrapped RTL excerpt lost paragraph direction on a numeric line");
    excerpt.setFont(cjkFont);
    excerpt.setText(QStringLiteral("<div style=\"line-height:130%\"><b><u>第一章</u></b> 한국어</div>"));
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
    require(renderPrivateText(QStringLiteral("Ελληνικά"), capitals).image ==
                renderPrivateText(QStringLiteral("ΕΛΛΗΝΙΚΆ"), font).image,
            "Private caption capitalization changed");
    const QString preview = QStringLiteral("Noto Serif CJK");
    const auto ordinaryPreview = renderPrivateText(preview, cjkFont, 0);
    const auto bionicPreview = renderPrivateText(preview, cjkFont, 0, {{0, 2}, {5, 3}, {11, 2}});
    require(ordinaryPreview.image != bionicPreview.image, "Private font preview lost bold prefixes");
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
    require(dynamic_cast<PrivateTextLabel *>(row.titleLabel()), "Chapter row title did not use the private label");
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
    require(label.text() == QStringLiteral("第四章 한국어 목차"), "Private label lost its accessible text");
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
    for (const auto &rtl : {QStringLiteral("العربية فصول الكتاب"), QStringLiteral("עברית פרקי הספר")}) {
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
    require(initializePrivateInterfaceText(assets), "Cannot restore private font state after component checks");
}
