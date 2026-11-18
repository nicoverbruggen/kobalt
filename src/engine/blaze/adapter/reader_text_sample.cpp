#include "engine_document.h"
#include "font_weight.h"
#include <lvdrawbuf.h>
#include <lvfntman.h>
#include <hyphman.h>
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QElapsedTimer>

// Text samples: a few lines drawn with the reading fonts, without opening a
// document. A document of a sample costs a parse, styles and a layout; a
// preview needs only the font, its size, weight and spacing.
namespace kobalt {
namespace {
lString32 text32(const QString &text)
{
    return Utf8ToUnicode(text.toUtf8().constData());
}

LVFontRef sampleFont(const TextSampleRequest &request, int weight)
{
    LVFontVariations variations;
    const float opticalSize = request.opticalSize > 0
            ? float(request.opticalSize)
            : float(request.pixels) * 72.0f / float(qMax(1, request.ppi));
    variations.set(LVFONT_TAG_OPSZ, opticalSize);
    return fontMan->GetFont(request.pixels, weight, false, css_ff_serif,
                            lString8(request.face.toUtf8().constData()), 0, -1, false, &variations);
}

struct Line
{
    QStringList words;
    int width = 0;
    bool last = false;
};

// The longest start of `word` that fits `room` with a hyphen after it, by
// the hyphenation patterns alone, or an empty string. No document is needed.
QString hyphenatedStart(const QString &word, LVFont &font, int room)
{
    auto *method = HyphMan::getHyphMethodForDictionary(U"English_US.pattern");
    const auto letters = text32(word);
    const int length = letters.length();
    if (!method || length < 4 || length > 64 || room <= 0)
        return {};
    lUInt16 widths[64];
    lUInt8 flags[64] = {};
    font.measureText(letters.c_str(), length, widths, flags, 0xFFFF, U'?', nullptr, 0, false);
    std::fill(flags, flags + length, 0);
    const int hyphen = int(font.getTextWidth(U"-", 1));
    if (!method->hyphenate(letters.c_str(), length, widths, flags, lUInt16(hyphen),
                           lUInt16(qMin(room, 0xFFFF))))
        return {};
    for (int at = length - 2; at > 0; --at)
        if ((flags[at] & LCHAR_ALLOW_HYPH_WRAP_AFTER) && widths[at] + hyphen <= room)
            return word.left(at + 1);
    return {};
}

// Words greedily, as many as fit `width` after `indent` on the first line.
// With `hyphenate`, a word that does not fit is broken where the patterns
// allow, as the reader does with hyphenation on.
QVector<Line> wrap(const QString &text, LVFont &font, int width, int indent, bool hyphenate)
{
    const int space = int(font.getTextWidth(U" ", 1));
    const auto measure = [&font](const QString &word) {
        const auto letters = text32(word);
        return int(font.getTextWidth(letters.c_str(), letters.length()));
    };
    QVector<Line> lines;
    Line line;
    QStringList words = text.split(QLatin1Char(' '), qt::skipEmptyParts);
    for (int index = 0; index < words.size(); ++index) {
        const auto word = words[index];
        const int advance = measure(word);
        const int available = width - (lines.isEmpty() ? indent : 0);
        const int needed = line.words.isEmpty() ? advance : line.width + space + advance;
        if (!line.words.isEmpty() && needed > available) {
            const auto start = hyphenate
                    ? hyphenatedStart(word, font, available - line.width - space)
                    : QString();
            if (!start.isEmpty()) {
                const auto first = start + QLatin1Char('-');
                line.width += space + measure(first);
                line.words.append(first);
                words.insert(index + 1, word.mid(start.size()));
                lines.append(line);
                line = {};
                continue;
            }
            lines.append(line);
            line = {};
        }
        line.width = line.words.isEmpty() ? advance : line.width + space + advance;
        line.words.append(word);
    }
    if (!line.words.isEmpty()) {
        line.last = true;
        lines.append(line);
    }
    return lines;
}
} // namespace

QImage EngineDocument::drawTextSample(const TextSampleRequest &request)
{
    if (request.canvas.isEmpty() || request.pixels <= 0 || !initializeFonts() || !fontMan)
        return {};
    LVGrayDrawBuf buffer(request.canvas.width(), request.canvas.height(), 8);
    buffer.FillRect(0, 0, buffer.GetWidth(), buffer.GetHeight(), 0xFFFFFF);
    buffer.SetTextColor(0x000000);
    buffer.SetBackgroundColor(0xFFFFFF);
    // Changing the thickness clears the glyph cache, so the reader's own
    // thickness comes back afterwards, not zero.
    const int readerWeight = extraFontWeight();
    setExtraFontWeight(request.extraWeight);
    // A face that no page has used yet is loaded here. Diagnostics report
    // that apart from the drawing.
    QElapsedTimer timer;
    timer.start();
    auto body = sampleFont(request, request.weight);
    auto heading = request.heading.isEmpty() ? LVFontRef()
                                             : sampleFont(request, qMax(700, request.weight));
    const double fontMilliseconds = timer.nsecsElapsed() / 1e6;
    timer.restart();
    if (body.isNull()) {
        setExtraFontWeight(readerWeight);
        return {};
    }
    const int width = request.canvas.width() - 2 * request.margin;
    // The reader sets line-height in percent of the text size, and CSS puts
    // the font's own height in the middle of that line.
    const int lineHeight = qMax(1, request.pixels * request.lineSpacing / 100);
    int y = request.margin;
    const auto drawLine = [&](LVFont &font, const Line &line, int x, int available,
                              const QString &alignment) {
        const int space = int(font.getTextWidth(U" ", 1));
        int gap = space, extra = 0;
        if (alignment == QLatin1String("justify") && !line.last && line.words.size() > 1) {
            const int spare = available - line.width;
            gap += spare / (line.words.size() - 1);
            extra = spare % (line.words.size() - 1);
        } else if (alignment == QLatin1String("center")) {
            x += (available - line.width) / 2;
        } else if (alignment == QLatin1String("right")) {
            x += available - line.width;
        }
        const int top = y + (lineHeight - font.getHeight()) / 2;
        for (int i = 0; i < line.words.size(); ++i) {
            const auto letters = text32(line.words[i]);
            x += font.DrawTextString(&buffer, x, top, letters.c_str(), letters.length(), U'?');
            x += gap + (i < extra ? 1 : 0);
        }
    };
    if (!request.heading.isEmpty()) {
        // Books set their chapter heading in bold.
        auto &font = heading.isNull() ? *body : *heading;
        for (const auto &line : wrap(request.heading, font, width, 0, false)) {
            drawLine(font, line, request.margin, width, QStringLiteral("center"));
            y += lineHeight;
        }
        // The sample's heading has a margin of 1em.
        y += request.pixels;
    }
    const int indent = qRound(request.pixels * 1.2);
    for (int i = 0; i < request.paragraphs.size() && y < request.canvas.height(); ++i) {
        // The first paragraph starts at the margin, as after a heading.
        const int first = i ? indent : 0;
        const auto lines = wrap(request.paragraphs[i], *body, width, first, request.hyphenate);
        for (int l = 0; l < lines.size() && y < request.canvas.height(); ++l) {
            const int x = request.margin + (l ? 0 : first);
            drawLine(*body, lines[l], x, width - (l ? 0 : first), request.alignment);
            y += lineHeight;
        }
    }
    setExtraFontWeight(readerWeight);
    if (diagnosticsActive())
        diagnostic("text-sample",
                   { { "face", request.face },
                     { "font_ms", fontMilliseconds },
                     { "draw_ms", timer.nsecsElapsed() / 1e6 } },
                   false);
    // Gray pixels in RGB: Qt 5.2 on Kobo has no grayscale image format.
    QImage image(request.canvas, QImage::Format_RGB32);
    if (image.isNull())
        return {};
    for (int row = 0; row < buffer.GetHeight(); ++row) {
        const auto *gray = buffer.GetScanLine(row);
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(row));
        for (int column = 0; column < buffer.GetWidth(); ++column)
            pixels[column] = qRgb(gray[column], gray[column], gray[column]);
    }
    return image;
}
} // namespace kobalt
