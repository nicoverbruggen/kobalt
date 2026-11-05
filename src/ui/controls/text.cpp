#include "text.h"
#include "ui/style/rich_text.h"
#include "support/qt_font.h"
#include "ui/style/fonts.h"
#include "ui/style/font_data.h"
#include "ui/style/styles.h"
#include "ui/style/theme.h"
#include "reading/header_footer.h"
#include "support/strings.h"
#include <QAbstractTextDocumentLayout>
#include <QFontInfo>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QGlyphRun>
#include <QPainter>
#include <QStyle>
#include <QPainterPath>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QtMath>
#include <algorithm>

namespace kobalt::ui {
QString headerFooterText(const headerfooter::Content &content, const QFont &font, int width)
{
    if (interfaceTextWidth(content.text, font) <= width)
        return content.text;
    if (content.chapterPosition.isEmpty())
        return elideInterfaceText(content.text, font, width);
    const QString ellipsis(QChar(0x2026));
    const auto joined = [&](const QString &title) {
        return strings::get("reader.chapter_position").arg(title, content.chapterPosition);
    };
    QString best = joined(ellipsis);
    if (interfaceTextWidth(best, font) > width)
        return content.chapterPosition;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, content.chapterTitle);
    QVector<int> boundaries;
    finder.toStart();
    for (int end = finder.toNextBoundary(); end >= 0; end = finder.toNextBoundary())
        boundaries.append(end);
    // Measure the whole candidate so shaping and rounding at the join agree with painting.
    int first = 0, last = boundaries.size() - 1;
    while (first <= last) {
        const int middle = first + (last - first) / 2;
        const auto text = joined(content.chapterTitle.left(boundaries[middle]) + ellipsis);
        if (interfaceTextWidth(text, font) <= width) {
            best = text;
            first = middle + 1;
        } else {
            last = middle - 1;
        }
    }
    return best;
}

namespace {
struct ExcerptLine
{
    QString text;
    QVector<QPair<int, int>> bold, underline;
    qreal top, height;
    int ascent, advance;
    Qt::LayoutDirection direction;
};

qreal excerptLineBottom(const QTextLayout &layout, const QTextLine &line)
{
    qreal bottom = layout.position().y() + line.y() + line.height();
    // A proportional text line can end above its glyph ink, including descenders.
    for (const auto &run : line.glyphRuns()) {
        bottom = qMax(bottom, layout.position().y() + run.boundingRect().bottom());
    }
    return qCeil(bottom);
}

bool privateExcerptNeeded(const QTextDocument &document, const QFont &font)
{
    auto text = document.toPlainText();
    text.remove(QChar::LineSeparator);
    text.remove('\n');
    return needsPrivateText(text, font);
}

QVector<QPair<int, int>> lineRanges(const QVector<QPair<int, int>> &ranges, int start, int length)
{
    QVector<QPair<int, int>> result;
    for (const auto &range : ranges) {
        const int first = qMax(start, range.first),
                  last = qMin(start + length, range.first + range.second);
        if (first < last) {
            result.append({ first - start, last - first });
        }
    }
    return result;
}

QVector<ExcerptLine> privateExcerptLines(const QTextDocument &document, const QFont &font,
                                         int width, int maximumLines = 0, bool *overflow = nullptr)
{
    if (overflow)
        *overflow = false;
    QVector<ExcerptLine> lines;
    qreal top = 0;
    qreal unitAdvance = 0;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const auto text = block.text();
        const auto direction = text.isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight;
        QVector<QPair<int, int>> bold, underline;
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const auto value = fragment.fragment();
            if (!value.isValid()) {
                continue;
            }
            const QPair<int, int> range(value.position() - block.position(), value.length());
            if (value.charFormat().fontWeight() >= QFont::Bold) {
                bold.append(range);
            }
            if (value.charFormat().fontUnderline()) {
                underline.append(range);
            }
        }
        QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, text);
        QVector<int> ends;
        for (int end = graphemes.toNextBoundary(); end >= 0; end = graphemes.toNextBoundary()) {
            ends.append(end);
        }
        QTextBoundaryFinder breaks(QTextBoundaryFinder::Line, text);
        int start = 0;
        do {
            const int separator = text.indexOf(QChar::LineSeparator, start);
            const int limit = separator < 0 ? text.size() : separator;
            int end = fittingTextEnd(
                    ends, start, limit, qMax(1, width),
                    [&](int candidate) {
                        return measurePrivateText(text.mid(start, candidate - start), font,
                                                  lineRanges(bold, start, candidate - start),
                                                  PrivateTextEmphasis::FontFace, direction);
                    },
                    unitAdvance);
            if (end == start && start < limit) {
                graphemes.setPosition(start);
                end = graphemes.toNextBoundary();
            }
            if (end < limit) {
                breaks.setPosition(end);
                const int wordEnd = breaks.isAtBoundary() ? end : breaks.toPreviousBoundary();
                if (wordEnd > start) {
                    end = wordEnd;
                }
            }
            const int length = end - start;
            const auto lineText = text.mid(start, length);
            const auto lineBold = lineRanges(bold, start, length);
            const auto measured = measurePrivateTextLayout(
                    lineText.isEmpty() ? QStringLiteral("M") : lineText, font, lineBold,
                    PrivateTextEmphasis::FontFace, direction);
            const auto format = block.blockFormat();
            qreal height = measured.lineHeight;
            if (format.lineHeightType() == QTextBlockFormat::ProportionalHeight) {
                height *= format.lineHeight() / 100.0;
            }
            lines.append({ lineText, lineBold, lineRanges(underline, start, length), top, height,
                           measured.lineAscent, lineText.isEmpty() ? 0 : measured.advance,
                           direction });
            top += height;
            start = end;
            if (start == separator) {
                ++start;
            } else {
                while (start < limit && text.at(start).isSpace()) {
                    ++start;
                }
            }
            if (maximumLines > 0 && lines.size() >= maximumLines) {
                if (overflow)
                    *overflow = start < text.size() || block.next().isValid();
                return lines;
            }
        } while (start < text.size());
    }
    return lines;
}
} // namespace

namespace {
QString interfaceHtml(const QString &text)
{
    return text.toHtmlEscaped().replace('\n', "<br/>");
}
} // namespace

int wrappedInterfaceTextHeight(const QString &text, const QFont &font, int width, int maximumLines)
{
    if (!needsPrivateText(text, font))
        return QFontMetrics(font)
                .boundingRect(QRect(0, 0, qMax(1, width), 0), Qt::TextWordWrap, text)
                .height();
    QTextDocument document;
    document.setDefaultFont(font);
    document.setHtml(interfaceHtml(text));
    const auto lines = privateExcerptLines(document, font, qMax(1, width), maximumLines);
    const int count = maximumLines > 0 ? qMin(int(lines.size()), maximumLines) : int(lines.size());
    return count ? qCeil(lines[count - 1].top + lines[count - 1].height) : 0;
}

void drawWrappedInterfaceText(QPainter &painter, const QRect &bounds, const QString &text,
                              Qt::Alignment alignment, int maximumLines, int lineSpacing,
                              QVector<QRect> *lineRectangles)
{
    if (lineRectangles)
        lineRectangles->clear();
    if (!needsPrivateText(text, painter.font())) {
        if (maximumLines > 0) {
            QTextLayout layout(text, painter.font());
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            option.setTextDirection(text.isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight);
            layout.setTextOption(option);
            QVector<QTextLine> lines;
            qreal height = 0;
            layout.beginLayout();
            for (int i = 0; i < maximumLines; ++i) {
                auto line = layout.createLine();
                if (!line.isValid())
                    break;
                line.setLineWidth(bounds.width());
                line.setPosition(QPointF(0, height));
                height += qMax(qreal(lineSpacing), line.height());
                lines.append(line);
            }
            layout.endLayout();
            const qreal top = bounds.y()
                    + (alignment & Qt::AlignVCenter          ? (bounds.height() - height) / 2
                               : alignment & Qt::AlignBottom ? bounds.height() - height
                                                             : 0);
            painter.save();
            painter.setClipRect(bounds, Qt::IntersectClip);
            for (int i = 0; i < lines.size(); ++i) {
                const auto &line = lines[i];
                const int end = line.textStart() + line.textLength();
                qreal advance = line.naturalTextWidth();
                if (i + 1 == maximumLines && end < text.size()) {
                    const auto last =
                            QFontMetrics(painter.font())
                                    .elidedText(
                                            text.mid(line.textStart(), line.textLength()).trimmed()
                                                    + QStringLiteral(" …"),
                                            Qt::ElideRight, bounds.width());
                    painter.drawText(QPointF(bounds.x(), top + line.y() + line.ascent()), last);
                    advance = kobalt::qt::textWidth(QFontMetricsF(painter.font()), last);
                } else {
                    line.draw(&painter, QPointF(bounds.x(), top));
                }
                if (lineRectangles)
                    lineRectangles->append(
                            QRectF(bounds.x(), top + line.y(), advance, line.height())
                                    .toAlignedRect());
            }
            painter.restore();
            return;
        }
        painter.drawText(bounds, alignment | Qt::TextWordWrap, text);
        return;
    }
    QTextDocument document;
    document.setDefaultFont(painter.font());
    document.setDocumentMargin(0);
    document.setHtml(interfaceHtml(text));
    bool overflow = false;
    auto lines =
            privateExcerptLines(document, painter.font(), bounds.width(), maximumLines, &overflow);
    if (overflow) {
        auto &last = lines.last();
        last.text =
                elidePrivateText(last.text + QStringLiteral(" …"), painter.font(), bounds.width());
        last.advance = measurePrivateText(last.text, painter.font());
    }
    if (lineSpacing > 0) {
        qreal top = 0;
        for (auto &line : lines) {
            line.top = top;
            top += qMax(qreal(lineSpacing), line.height);
        }
    }
    const qreal height = lines.isEmpty() ? 0 : lines.last().top + lines.last().height;
    const qreal top = bounds.y()
            + (alignment & Qt::AlignVCenter          ? (bounds.height() - height) / 2
                       : alignment & Qt::AlignBottom ? bounds.height() - height
                                                     : 0);
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    for (const auto &line : lines) {
        auto rendered = renderPrivateText(line.text, painter.font());
        if (rendered.image.isNull())
            continue;
        QPainter color(&rendered.image);
        color.setCompositionMode(QPainter::CompositionMode_SourceIn);
        color.fillRect(rendered.image.rect(), painter.pen().color());
        color.end();
        const auto visual = QStyle::visualAlignment(line.direction, alignment);
        const qreal x = bounds.x()
                + (visual & Qt::AlignHCenter         ? (bounds.width() - line.advance) / 2
                           : visual & Qt::AlignRight ? bounds.width() - line.advance
                                                     : 0);
        painter.drawImage(QPointF(x + rendered.origin.x(),
                                  top + line.top + line.ascent + rendered.origin.y()),
                          rendered.image);
        if (lineRectangles)
            lineRectangles->append(
                    QRectF(x, top + line.top, line.advance, line.height).toAlignedRect());
    }
    painter.restore();
}

int InterfaceLabel::heightForWidth(int width) const
{
    if (!wordWrap() || !needsPrivateText(text(), font()))
        return QLabel::heightForWidth(width);
    const auto margins = contentsMargins();
    const int available = qMax(1, width - margins.left() - margins.right() - 2 * margin());
    int height;
    if (textFormat() == Qt::RichText
        || (textFormat() == Qt::AutoText && Qt::mightBeRichText(text()))) {
        PrivateRichText document;
        document.setFont(font());
        document.setHtml(text());
        document.layout(available);
        height = document.height();
    } else
        height = wrappedInterfaceTextHeight(text(), font(), available);
    return height + margins.top() + margins.bottom() + 2 * margin();
}

QSize InterfaceLabel::sizeHint() const
{
    if (!needsPrivateText(text(), font()))
        return QLabel::sizeHint();
    const auto margins = contentsMargins();
    QSize size;
    if (textFormat() == Qt::RichText
        || (textFormat() == Qt::AutoText && Qt::mightBeRichText(text()))) {
        PrivateRichText document;
        document.setFont(font());
        document.setHtml(text());
        document.layout(32768);
        size = QSize(document.naturalWidth(), document.height());
    } else
        size = measurePrivateTextSize(text(), font());
    size += QSize(margins.left() + margins.right() + 2 * margin(),
                  margins.top() + margins.bottom() + 2 * margin());
    return size;
}

bool InterfaceLabel::event(QEvent *event)
{
    // A new parent or style makes QFrame set its contents margins again from
    // its contents rectangle. Before the first layout a label keeps Qt's
    // default size, and margins at a large interface scale can exceed it.
    // QFrame then finds no valid rectangle and drops them.
    const auto margins = contentsMargins();
    const bool exceeded = margins.left() + margins.right() >= width()
            || margins.top() + margins.bottom() >= height();
    const bool handled = QLabel::event(event);
    if (exceeded && contentsMargins() != margins)
        setContentsMargins(margins);
    return handled;
}

void InterfaceLabel::paintEvent(QPaintEvent *event)
{
    if (!needsPrivateText(text(), font()) || text().isEmpty()) {
        QLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(isEnabled() ? QPalette::WindowText : QPalette::Mid));
    const QRect bounds = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    if (textFormat() == Qt::RichText
        || (textFormat() == Qt::AutoText && Qt::mightBeRichText(text()))) {
        PrivateRichText document;
        document.setFont(font());
        document.setHtml(text());
        document.layout(bounds.width());
        const int offset =
                alignment() & Qt::AlignVCenter ? (bounds.height() - document.height()) / 2 : 0;
        painter.translate(bounds.x(), bounds.y() + offset);
        document.draw(painter, QRectF(0, -offset, bounds.width(), bounds.height()), palette());
        return;
    }
    if (wordWrap())
        drawWrappedInterfaceText(painter, bounds, text(), alignment());
    else
        drawInterfaceText(painter, bounds, text(), alignment());
}

qreal excerptTextHeight(const QString &html, const QFont &font, int width)
{
    QTextDocument document;
    document.setDefaultFont(font);
    document.setDocumentMargin(0);
    document.setHtml(html);
    if (privateExcerptNeeded(document, font)) {
        const auto lines = privateExcerptLines(document, font, width);
        return lines.isEmpty() ? 0 : lines.last().top + lines.last().height;
    }
    document.setTextWidth(width);
    qreal bottom = document.size().height();
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const auto *layout = block.layout();
        for (int i = 0; i < layout->lineCount(); ++i) {
            bottom = qMax(bottom, excerptLineBottom(*layout, layout->lineAt(i)));
        }
    }
    return bottom;
}

void WholeLineLabel::paintEvent(QPaintEvent *)
{
    QTextDocument document;
    document.setDefaultFont(font());
    document.setDocumentMargin(0);
    document.setHtml(text());
    if (privateExcerptNeeded(document, font())) {
        const auto lines = privateExcerptLines(document, font(), width());
        qreal bottom = 0;
        for (const auto &line : lines) {
            if (line.top + line.height <= height()) {
                bottom = line.top + line.height;
            }
        }
        QPainter painter(this);
        painter.setClipRect(rect());
        if (alignment() & Qt::AlignVCenter) {
            painter.translate(0, (height() - bottom) / 2);
        }
        for (const auto &line : lines) {
            if (line.top + line.height > bottom) {
                break;
            }
            auto rendered = renderPrivateText(line.text, font(), 0, line.bold, line.underline,
                                              PrivateTextEmphasis::FontFace, line.direction);
            if (rendered.image.isNull()) {
                continue;
            }
            QPainter color(&rendered.image);
            color.setCompositionMode(QPainter::CompositionMode_SourceIn);
            color.fillRect(rendered.image.rect(), palette().color(QPalette::WindowText));
            color.end();
            const int x = line.direction == Qt::RightToLeft ? width() - line.advance : 0;
            painter.drawImage(
                    QPointF(x + rendered.origin.x(), line.top + line.ascent + rendered.origin.y()),
                    rendered.image);
        }
        return;
    }
    document.setTextWidth(width());
    document.documentLayout()->documentSize();
    qreal bottom = 0;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const auto *layout = block.layout();
        for (int i = 0; i < layout->lineCount(); ++i) {
            const auto end = excerptLineBottom(*layout, layout->lineAt(i));
            if (end <= height()) {
                bottom = qMax(bottom, end);
            }
        }
    }
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::WindowText));
    painter.setClipRect(rect());
    if (alignment() & Qt::AlignVCenter) {
        painter.translate(0, (height() - bottom) / 2);
    }
    // Paint admitted lines only; an ink overhang must not expose the next partial line.
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const auto *layout = block.layout();
        for (int i = 0; i < layout->lineCount(); ++i) {
            const auto line = layout->lineAt(i);
            if (excerptLineBottom(*layout, line) > bottom) {
                return;
            }
            line.draw(&painter, layout->position());
        }
    }
}

QVector<QPair<int, int>> samplePrefixes(const QString &text)
{
    const auto letters = text.toUcs4();
    auto letter = [](uint ch) {
        const auto script = QChar::script(ch);
        const bool supported = script == QChar::Script_Latin || script == QChar::Script_Greek
                || script == QChar::Script_Cyrillic || script == QChar::Script_Hebrew;
        const auto category = QChar::category(ch);
        return supported && category >= QChar::Letter_Uppercase && category <= QChar::Letter_Other;
    };
    auto mark = [](uint ch) {
        const auto category = QChar::category(ch);
        return category >= QChar::Mark_NonSpacing && category <= QChar::Mark_Enclosing;
    };
    auto word = [&](uint ch) {
        return letter(ch) || mark(ch) || QChar::category(ch) == QChar::Number_DecimalDigit
                || ch == '\'' || ch == 0x2019 || ch == 0x00ad;
    };
    QVector<int> offsets;
    int offset = 0;
    for (const auto ch : letters) {
        offsets.append(offset);
        offset += ch > 0xffff ? 2 : 1;
    }
    offsets.append(offset);
    QVector<QPair<int, int>> result;
    for (int i = 0; i < letters.size();) {
        if (!letter(letters[i])) {
            ++i;
            continue;
        }
        const int start = i;
        int end = i + 1, count = 1;
        while (end < letters.size() && word(letters[end])) {
            if (letter(letters[end]))
                ++count;
            ++end;
        }
        int prefix = start, remaining = (count + 1) / 2;
        while (prefix < end && remaining > 0) {
            if (letter(letters[prefix]))
                --remaining;
            ++prefix;
        }
        while (prefix < end && mark(letters[prefix]))
            ++prefix;
        result.append({ offsets[start], offsets[prefix] - offsets[start] });
        i = end;
    }
    return result;
}

void drawOpticalText(QPainter &painter, const QRectF &area, const QString &text, bool centered)
{
    drawOpticalText(painter, area, text, centered, { -1, QStringLiteral("off") });
}

void drawOpticalText(QPainter &painter, const QRectF &area, const QString &text, bool centered,
                     SampleAppearance appearance)
{
    if (needsPrivateText(text, painter.font())) {
        const bool bold = appearance.bionic == "bold";
        const auto ranges = bold && !privateFontFixedPitch(painter.font())
                ? samplePrefixes(text)
                : QVector<QPair<int, int>>();
        drawOpticalPrivateText(
                painter, area.toRect(),
                elidePrivateText(text, painter.font(), qMax(0, int(area.width())), ranges),
                centered ? Qt::AlignHCenter : Qt::AlignLeft, bold ? 0 : appearance.weight, ranges);
        return;
    }
    const bool bold = appearance.bionic == "bold";
    const int weight = bold ? 0 : qBound(0, appearance.weight, 12);
    if (!bold && !weight) {
        const auto label =
                painter.fontMetrics().elidedText(text, Qt::ElideRight, qMax(0, int(area.width())));
        const auto bounds = QFontMetricsF(painter.font()).tightBoundingRect(label);
        painter.drawText(QPointF((centered ? area.center().x() - bounds.width() / 2 : area.left())
                                         - bounds.left(),
                                 area.center().y() - bounds.center().y()),
                         label);
        return;
    }
    // Shape the full label so RTL order and combining marks survive emphasis.
    // Intrinsic width includes real bold advances before the label is elided.
    const bool prefixesEnabled = bold && !QFontInfo(painter.font()).fixedPitch();
    QString label = text;
    for (;;) {
        QTextLayout layout(label, painter.font());
        const auto prefixes = prefixesEnabled ? samplePrefixes(label) : QVector<QPair<int, int>>();
        QVector<QTextLayout::FormatRange> formats;
        if (bold)
            for (const auto &prefix : prefixes) {
                QTextLayout::FormatRange range;
                range.start = prefix.first;
                range.length = prefix.second;
                range.format.setFontWeight(QFont::Bold);
                formats.append(range);
            }
#if QT_VERSION < QT_VERSION_CHECK(5, 6, 0)
        layout.setAdditionalFormats(formats.toList());
#else
        layout.setFormats(formats);
#endif
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid())
            line.setLineWidth(qMax(1.0, area.width()));
        layout.endLayout();
        if (!line.isValid())
            return;
        if (line.naturalTextWidth() > area.width() && label.size() > 1) {
            if (label.endsWith(QChar(0x2026)))
                label.chop(1);
            if (label.isEmpty())
                return;
            label.chop(label.at(label.size() - 1).isLowSurrogate() ? 2 : 1);
            label.append(QChar(0x2026));
            continue;
        }

        struct Glyph
        {
            QPainterPath path;
            QGlyphRun run;
            QRectF clip;
            bool split = false;
        };

        QVector<Glyph> glyphs;
        QRectF bounds;
        auto append = [&](int start, int length, int steps) {
            for (const auto &run : line.glyphRuns(start, length)) {
                QPainterPath path;
                path.setFillRule(Qt::WindingFill);
                const auto indexes = run.glyphIndexes();
                const auto positions = run.positions();
                for (int i = 0; i < indexes.size(); ++i) {
                    auto outline = run.rawFont().pathForGlyph(indexes[i]);
                    if (steps)
                        outline =
                                weightedOutline(outline, run.rawFont().pixelSize() * steps / 256.0);
                    outline.translate(positions[i]);
                    path.addPath(outline);
                }
                const bool split = run.flags().testFlag(QGlyphRun::SplitLigature);
                const auto ink = path.isEmpty() ? run.boundingRect() : path.boundingRect();
                const auto clip = run.boundingRect();
                bounds = bounds.united(split ? ink.intersected(clip) : ink);
                glyphs.append({ path, run, clip, split });
            }
        };
        append(0, label.size(), weight);
        const QPointF origin((centered ? area.center().x() - bounds.width() / 2 : area.left())
                                     - bounds.left(),
                             area.center().y() - bounds.center().y());
        painter.save();
        painter.translate(origin);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto ink = painter.pen().color();
        for (const auto &glyph : glyphs) {
            painter.save();
            if (glyph.split)
                painter.setClipRect(glyph.clip, Qt::IntersectClip);
            if (glyph.path.isEmpty())
                painter.drawGlyphRun(QPointF(), glyph.run);
            else
                painter.fillPath(glyph.path, ink);
            painter.restore();
        }
        painter.restore();
        return;
    }
}

void SampleLabel::paintEvent(QPaintEvent *event)
{
    if (!optical) {
        PrivateTextLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::WindowText));
    if (drawFontPreview(painter, rect(), text(), previewSource, metrics::choices::sampleInkHeight,
                        appearance.weight, appearance.bionic == "bold",
                        alignment().testFlag(Qt::AlignHCenter), wordWrap()))
        return;
    if (needsPrivateText(text(), font())) {
        if (wordWrap() && interfaceTextWidth(text(), font()) > width()) {
            int split = -1, best = -1;
            for (int i = 1; i < text().size() - 1; ++i) {
                if (!text().at(i).isSpace()) {
                    continue;
                }
                const auto first = text().left(i).trimmed(), second = text().mid(i + 1).trimmed();
                const int widest =
                        qMax(interfaceTextWidth(first, font()), interfaceTextWidth(second, font()));
                if (best < 0 || widest < best) {
                    best = widest;
                    split = i;
                }
            }
            const auto sample = renderPrivateText(text() + QStringLiteral(" Hox"), font(), 0).image;
            int firstInk = sample.height(), lastInk = -1;
            for (int y = 0; y < sample.height(); ++y)
                for (int x = 0; x < sample.width(); ++x)
                    if (qAlpha(sample.pixel(x, y)) > 128) {
                        firstInk = qMin(firstInk, y);
                        lastInk = y;
                        break;
                    }
            const int lineHeight = qMax(1, lastInk - firstInk + 1) + px(5);
            if (split >= 0 && lineHeight * 2 <= height()) {
                const int top = (height() - lineHeight * 2) / 2;
                drawOpticalText(painter, QRectF(0, top, width(), lineHeight),
                                text().left(split).trimmed(),
                                alignment().testFlag(Qt::AlignHCenter), appearance);
                drawOpticalText(painter, QRectF(0, top + lineHeight, width(), lineHeight),
                                text().mid(split + 1).trimmed(),
                                alignment().testFlag(Qt::AlignHCenter), appearance);
                return;
            }
        }
        drawOpticalText(painter, rect(), text(), alignment().testFlag(Qt::AlignHCenter),
                        appearance);
        return;
    }
    if (wordWrap() && painter.fontMetrics().boundingRect(text()).width() > width()) {
        // Keep whole words together. Balance two lines before eliding a word
        // that cannot fit, using the same optical painter for weight previews.
        const QFontMetricsF metrics(font());
        int split = -1;
        qreal best = -1;
        for (int i = 1; i < text().size() - 1; ++i) {
            if (!text().at(i).isSpace())
                continue;
            const auto first = text().left(i).trimmed(), second = text().mid(i + 1).trimmed();
            if (first.isEmpty() || second.isEmpty())
                continue;
            const qreal widest =
                    qMax(metrics.boundingRect(first).width(), metrics.boundingRect(second).width());
            if (best < 0 || widest < best) {
                best = widest;
                split = i;
            }
        }
        const qreal lineHeight = qMax(metrics.tightBoundingRect(text()).height(),
                                      metrics.tightBoundingRect(QStringLiteral("Hox")).height())
                + px(5);
        if (split >= 0 && lineHeight * 2 <= height()) {
            const qreal top = (height() - lineHeight * 2) / 2;
            drawOpticalText(painter, QRectF(0, top, width(), lineHeight),
                            text().left(split).trimmed(), alignment().testFlag(Qt::AlignHCenter),
                            appearance);
            drawOpticalText(painter, QRectF(0, top + lineHeight, width(), lineHeight),
                            text().mid(split + 1).trimmed(), alignment().testFlag(Qt::AlignHCenter),
                            appearance);
            return;
        }
    }
    drawOpticalText(painter, rect(), text(), alignment().testFlag(Qt::AlignHCenter), appearance);
}

// Some fonts report an x-height that does not match their glyphs. Measure
// capitals as well as lowercase so small x-heights do not inflate font names.
qreal halfSpacedWidth(const QFont &font, const QString &text)
{
    return interfaceTextWidth(text, font)
            - text.count(' ') * interfaceTextWidth(QStringLiteral(" "), font) / 2.0;
}

void drawHalfSpaced(QPainter &painter, qreal x, const QRect &bounds, const QString &text,
                    int weight)
{
    if (weight < 0 && !needsPrivateText(text, painter.font())) {
        const auto metrics = painter.fontMetrics();
        const qreal baseline =
                bounds.y() + (bounds.height() - metrics.height()) / 2.0 + metrics.ascent();
        const qreal space = qt::textWidth(metrics, QStringLiteral(" ")) / 2.0;
        for (const auto &word : text.split(' ')) {
            painter.drawText(QPointF(x, baseline), word);
            x += qt::textWidth(metrics, word) + space;
        }
        return;
    }
    const qreal space = interfaceTextWidth(QStringLiteral(" "), painter.font()) / 2.0;
    for (const auto &word : text.split(' ')) {
        const int width = interfaceTextWidth(word, painter.font());
        drawInterfaceText(painter, QRect(qRound(x), bounds.y(), width, bounds.height()), word,
                          Qt::AlignLeft | Qt::AlignVCenter, weight);
        x += width + space;
    }
}

QFont fontSample(const QString &face)
{
    QFont font(face.isEmpty() ? kobalt::ui::interfaceFontFamily() : face);
    font.setWeight(QFont::Normal);
    font.setItalic(false);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setPixelSize(128);
    if (needsPrivateText(QStringLiteral("Hox"), font)) {
        const auto rendered = renderPrivateText(QStringLiteral("Hox"), font);
        int first = rendered.image.height(), last = -1;
        for (int y = 0; y < rendered.image.height(); ++y) {
            for (int x = 0; x < rendered.image.width(); ++x) {
                if (qAlpha(rendered.image.pixel(x, y)) >= 128) {
                    first = qMin(first, y);
                    last = y;
                    break;
                }
            }
        }
        const int size = last >= first
                ? qRound(128.0 * metrics::choices::sampleInkHeight / (last - first + 1))
                : metrics::choices::sampleBase;
        font.setPixelSize(qBound<int>(metrics::choices::sampleMinimum, size,
                                      metrics::choices::sampleMaximum));
        return font;
    }
    const auto resolved = QFontInfo(font).family();
    static QHash<QString, int> sizes;
    auto found = sizes.constFind(resolved);
    if (found == sizes.cend()) {
        QImage image(256, 256, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.setFont(font);
            painter.setPen(Qt::black);
            painter.drawText(QPointF(16, 180), QStringLiteral("Hox"));
        }
        int first = image.height(), last = -1;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qGray(image.pixel(x, y)) < 128) {
                    first = qMin(first, y);
                    last = y;
                    break;
                }
        const int size = last >= first
                ? qRound(128.0 * metrics::choices::sampleInkHeight / (last - first + 1))
                : metrics::choices::sampleBase;
        found = sizes.insert(resolved,
                             qBound<int>(metrics::choices::sampleMinimum, size,
                                         metrics::choices::sampleMaximum));
    }
    font.setPixelSize(*found);
    return font;
}

void captionLabel(QLabel *label, int pixels)
{
    label->setFont(captionFont(pixels));
    label->setTextFormat(Qt::PlainText);
    readerStyle(label, styles::caption(pixels));
}

} // namespace kobalt::ui
