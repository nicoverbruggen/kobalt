#include "rich_text.h"
#include "fonts.h"
#include <QPainter>
#include <QBuffer>
#include <QHash>
#include <QImageReader>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QtMath>
#include <algorithm>

namespace kobalt::ui {
namespace {
class HtmlDocument final : public QTextDocument {
    QVariant loadResource(int, const QUrl &) override { return {}; }
};
struct Line {
    QString text;
    QVector<PrivateTextStyle> styles;
    QFont font;
    QPointF baseline;
    QRectF bounds;
    Qt::LayoutDirection direction;
};
QVector<PrivateTextStyle> slice(const QVector<PrivateTextStyle> &styles, int start, int length) {
    QVector<PrivateTextStyle> result;
    auto first = std::lower_bound(styles.begin(), styles.end(), start,
                                  [](const PrivateTextStyle &style, int offset) {
                                      return style.start + style.length <= offset;
                                  });
    for (auto it = first; it != styles.end() && it->start < start + length; ++it) {
        auto style = *it;
        const int begin = qMax(start, style.start), end = qMin(start + length, style.start + style.length);
        if (begin >= end)
            continue;
        style.start = begin - start;
        style.length = end - begin;
        result.append(style);
    }
    return result;
}
PrivateText measure(const QString &text, const QFont &font, const QVector<PrivateTextStyle> &styles,
                    Qt::LayoutDirection direction) {
    return measurePrivateTextLayout(text.isEmpty() ? QStringLiteral("M") : text, font, {},
                                    PrivateTextEmphasis::FontFace, direction, styles);
}
QImage inlineImage(const QTextCharFormat &format) {
    if (!format.isImageFormat())
        return {};
    const auto imageFormat = format.toImageFormat();
    const auto name = imageFormat.name();
    const QString prefix = QStringLiteral("data:image/png;base64,");
    // Only bounded, embedded PNGs are decoded. Rich text cannot read files or
    // make network requests, including while USB owns user storage.
    if (!name.startsWith(prefix) || name.size() > 1024 * 1024)
        return {};
    QByteArray bytes = QByteArray::fromBase64(name.mid(prefix.size()).toLatin1());
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 1024 || size.height() > 1024)
        return {};
    const int width = qBound(1, qRound(imageFormat.width() > 0 ? imageFormat.width() : size.width()), 1024);
    const int height = qBound(1, qRound(imageFormat.height() > 0 ? imageFormat.height() : size.height()), 1024);
    return reader.read().scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

}
struct PrivateRichText::Data {
    HtmlDocument document;
    QVector<Line> lines;
    QVector<QRectF> rules;
    int width = 1, pageHeight = 0, leading = 0;
    qreal bottom = 0;
    // Line ends by block position and available width. Page height and
    // leading move lines without changing where they break.
    QHash<QPair<int, int>, QVector<int>> breaks;
    qreal unitAdvance = 0;

    QFont fragmentFont(const QTextCharFormat &format) const {
        auto font = document.defaultFont();
        const auto family = format.font().family().toLower();
        if (format.fontFixedPitch() || family.contains("mono") || family.contains("courier"))
            font.setFamily(chromeFont().family());
        if (format.hasProperty(QTextFormat::FontPixelSize))
            font.setPixelSize(qMax(1, qRound(format.property(QTextFormat::FontPixelSize).toDouble())));
        else if (format.fontPointSize() > 0)
            font.setPixelSize(qMax(1, qRound(format.fontPointSize() * 96 / 72)));
        else if (format.hasProperty(QTextFormat::FontSizeAdjustment))
            font.setPixelSize(qMax(1, qRound(font.pixelSize() * qPow(1.2, format.property(QTextFormat::FontSizeAdjustment).toInt()))));
        if (format.hasProperty(QTextFormat::FontWeight))
            font.setWeight(QFont::Weight(format.fontWeight()));
        if (format.hasProperty(QTextFormat::FontItalic))
            font.setItalic(format.fontItalic());
        font.setUnderline(format.fontUnderline());
        font.setStrikeOut(format.fontStrikeOut());
        return font;
    }
    qreal fitLine(qreal y, qreal height) const {
        if (pageHeight > 0 && height <= pageHeight &&
            qFloor(y / pageHeight) != qFloor((y + height - .01) / pageHeight))
            return qCeil(y / pageHeight) * pageHeight;
        return y;
    }
    qreal block(const QTextBlock &block, qreal x, int available, qreal y) {
        const auto format = block.blockFormat();
        if (pageHeight && format.pageBreakPolicy().testFlag(QTextFormat::PageBreak_AlwaysBefore))
            y = qCeil(y / pageHeight) * pageHeight;
        y += qMax(qreal(0), format.topMargin());
        x += format.leftMargin();
        available = qMax(1, available - qCeil(format.leftMargin() + format.rightMargin()));
        const auto base = document.defaultFont();
        const auto text = block.text();
        const auto requestedDirection = format.hasProperty(QTextFormat::LayoutDirection)
                                            ? format.layoutDirection() : Qt::LayoutDirectionAuto;
        const auto direction = requestedDirection == Qt::LayoutDirectionAuto
                                   ? (text.isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight)
                                   : requestedDirection;
        QVector<PrivateTextStyle> styles;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const auto character = fragment.charFormat();
            auto font = fragmentFont(character);
            int shift = 0;
            if (character.verticalAlignment() == QTextCharFormat::AlignSuperScript ||
                character.verticalAlignment() == QTextCharFormat::AlignSubScript) {
                shift = character.verticalAlignment() == QTextCharFormat::AlignSuperScript ? -font.pixelSize() / 3 : font.pixelSize() / 5;
                font.setPixelSize(qMax(1, font.pixelSize() * 7 / 10));
            }
            const auto color = character.foreground().style() == Qt::NoBrush ? QColor() : character.foreground().color();
            const auto background = character.background().style() == Qt::NoBrush ? QColor() : character.background().color();
            styles.append({fragment.position() - block.position(), fragment.length(), font, color, shift,
                           inlineImage(character), background});
        }
        QString marker;
        if (auto *list = block.textList()) {
            switch (list->format().style()) {
            case QTextListFormat::ListDisc: marker = QChar(0x2022); break;
            case QTextListFormat::ListCircle: marker = QChar(0x25e6); break;
            case QTextListFormat::ListSquare: marker = QChar(0x25aa); break;
            default: marker = list->itemText(block); break;
            }
            const auto markerFont = styles.isEmpty() ? base : styles.first().font;
            const int level = qMax(1, list->format().indent());
            const int inset = qMax(markerFont.pixelSize(),
                                   qCeil(measurePrivateText(marker, markerFont) + markerFont.pixelSize() / 3.0)) * level;
            if (direction != Qt::RightToLeft)
                x += inset;
            available = qMax(1, available - inset);
        }
        if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            rules.append({x, y + base.pixelSize() / 2.0, qreal(available), 1});
            return y + base.pixelSize();
        }
        auto &lineEnds = breaks[qMakePair(block.position(), available)];
        const bool known = !lineEnds.isEmpty();
        QVector<int> ends;
        QTextBoundaryFinder clusters, lineBreaks;
        if (!known) {
            clusters = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, text);
            lineBreaks = QTextBoundaryFinder(QTextBoundaryFinder::Line, text);
            ends.reserve(text.size());
            for (int end = clusters.toNextBoundary(); end >= 0; end = clusters.toNextBoundary())
                ends.append(end);
        }
        int start = 0;
        bool first = true;
        int lineIndex = 0;
        int separator = text.indexOf(QChar::LineSeparator);
        do {
            const int limit = separator < 0 ? text.size() : separator;
            const qreal indent = first ? format.textIndent() : 0;
            const int lineWidth = qMax(1, available - qCeil(indent));
            int end;
            if (known) {
                end = lineEnds.value(lineIndex, text.size());
            } else {
                end = fittingTextEnd(ends, start, limit, lineWidth, [&](int candidate) {
                    return measure(text.mid(start, candidate - start), base, slice(styles, start, candidate - start),
                                   direction).advance;
                }, unitAdvance);
                if (end == start && start < limit) {
                    clusters.setPosition(start);
                    end = clusters.toNextBoundary();
                }
                if (end < limit) {
                    lineBreaks.setPosition(end);
                    const int boundary = lineBreaks.isAtBoundary() ? end : lineBreaks.toPreviousBoundary();
                    if (boundary > start)
                        end = boundary;
                }
                lineEnds.append(end);
            }
            ++lineIndex;
            const auto shown = text.mid(start, end - start);
            const auto formats = slice(styles, start, end - start);
            const auto metrics = measure(shown, base, formats, direction);
            qreal height = qMax(1, metrics.lineHeight);
            switch (format.lineHeightType()) {
            case QTextBlockFormat::ProportionalHeight: height *= format.lineHeight() / 100.0; break;
            case QTextBlockFormat::FixedHeight: height = format.lineHeight(); break;
            case QTextBlockFormat::MinimumHeight: height = qMax(height, format.lineHeight()); break;
            case QTextBlockFormat::LineDistanceHeight: height += format.lineHeight(); break;
            default: break;
            }
            height = qMax(height, metrics.lineHeight * qMax(100, leading) / 100.0);
            y = fitLine(y, height);
            const auto alignment = format.alignment();
            const qreal offset = alignment & Qt::AlignHCenter ? (lineWidth - metrics.advance) / 2.0
                                 : alignment & Qt::AlignRight ? lineWidth - metrics.advance
                                 : direction == Qt::RightToLeft && !(alignment & Qt::AlignAbsolute) ? lineWidth - metrics.advance : 0;
            const qreal baseline = y + (height - metrics.lineHeight) / 2 + metrics.lineAscent;
            lines.append({shown, formats, base, {x + indent + offset, baseline}, {x + indent, y, qreal(lineWidth), height}, direction});
            if (first && !marker.isEmpty()) {
                const auto markerFont = styles.isEmpty() ? base : styles.first().font;
                const auto measured = measure(marker, markerFont, {}, direction);
                const qreal gap = markerFont.pixelSize() / 3.0;
                const qreal markerX = direction == Qt::RightToLeft ? x + available + gap : x - measured.advance - gap;
                lines.append({marker, {}, markerFont, {markerX, baseline},
                              {markerX, y, qreal(measured.advance), height}, direction});
            }
            y += height;
            start = end;
            first = false;
            if (start == separator) {
                ++start;
                separator = text.indexOf(QChar::LineSeparator, start);
            } else if (!format.nonBreakableLines())
                while (start < limit && text.at(start).isSpace())
                    ++start;
        } while (start < text.size());
        return y + qMax(qreal(0), format.bottomMargin());
    }
    qreal flow(QTextFrame::iterator begin, QTextFrame::iterator end, qreal x, int available, qreal y) {
        bool afterFrame = false;
        for (auto it = begin; it != end; ++it) {
            if (auto *frame = it.currentFrame()) {
                if (auto *table = dynamic_cast<QTextTable *>(frame))
                    y = tableLayout(table, x, available, y);
                else
                    y = flow(frame->begin(), frame->end(), x, available, y);
                afterFrame = true;
            } else if (it.currentBlock().isValid()) {
                auto next = it;
                ++next;
                if (it.currentBlock().text().isEmpty() && (afterFrame || (next != end && next.currentFrame())))
                    continue;
                y = block(it.currentBlock(), x, available, y);
                afterFrame = false;
            }
        }
        return y;
    }
    qreal tableLayout(QTextTable *table, qreal x, int available, qreal y) {
        const auto format = table->format();
        const int columns = qMax(1, table->columns());
        const int gap = qCeil(format.cellSpacing()), padding = qCeil(format.cellPadding());
        const auto constraints = format.columnWidthConstraints();
        const int contentWidth = qMax(columns, available - (columns + 1) * gap);
        QVector<int> widths(columns, 0);
        int assigned = 0, flexible = 0;
        for (int c = 0; c < columns; ++c) {
            if (c < constraints.size() && constraints[c].type() != QTextLength::VariableLength) {
                widths[c] = qMax(1, qRound(constraints[c].value(contentWidth)));
                assigned += widths[c];
            } else
                ++flexible;
        }
        for (auto &width : widths)
            if (!width)
                width = qMax(1, (contentWidth - assigned) / qMax(1, flexible));
        int total = 0;
        for (const int width : widths)
            total += width;
        if (total > contentWidth) {
            int remaining = contentWidth;
            for (int c = 0; c < columns; ++c) {
                widths[c] = qBound(1, qRound(qreal(widths[c]) * contentWidth / total),
                                   remaining - (columns - c - 1));
                remaining -= widths[c];
            }
        }
        QVector<qreal> reservedBottom(table->rows(), y);
        for (int row = 0; row < table->rows(); ++row) {
            qreal bottom = y;
            qreal left = x + gap;
            for (int column = 0; column < columns; ++column) {
                const auto cell = table->cellAt(row, column);
                if (cell.row() == row && cell.column() == column) {
                    int spanWidth = 0;
                    for (int c = column; c < qMin(columns, column + cell.columnSpan()); ++c)
                        spanWidth += widths[c] + (c == column ? 0 : gap);
                    const qreal cellBottom = flow(cell.begin(), cell.end(), left + padding,
                                                  qMax(1, spanWidth - 2 * padding), y + padding) + padding;
                    const int lastRow = qMin(table->rows() - 1, row + cell.rowSpan() - 1);
                    reservedBottom[lastRow] = qMax(reservedBottom[lastRow], cellBottom);
                    if (cell.rowSpan() == 1)
                        bottom = qMax(bottom, cellBottom);
                }
                left += widths[column] + gap;
            }
            y = qMax(bottom, reservedBottom[row]) + gap;
        }
        return y;
    }
};
PrivateRichText::PrivateRichText() : data(std::make_unique<Data>()) {
    data->document.setDocumentMargin(0);
    data->document.setDefaultFont(interfaceFont());
    data->document.setUndoRedoEnabled(false);
}
PrivateRichText::~PrivateRichText() = default;
void PrivateRichText::setHtml(const QString &html) {
    data->breaks.clear();
    data->document.setHtml(html);
}
void PrivateRichText::setPlainText(const QString &text) {
    data->breaks.clear();
    data->document.setPlainText(text);
}
void PrivateRichText::setFont(const QFont &font) {
    data->breaks.clear();
    data->document.setDefaultFont(font);
}
QFont PrivateRichText::font() const { return data->document.defaultFont(); }
void PrivateRichText::layout(int width, int pageHeight, int minimumLineHeight) {
    // A fresh estimate keeps each layout independent of earlier ones.
    data->unitAdvance = 0;
    data->width = qMax(1, width);
    data->pageHeight = qMax(0, pageHeight);
    data->leading = minimumLineHeight;
    data->lines.clear();
    data->rules.clear();
    data->flow(data->document.rootFrame()->begin(), data->document.rootFrame()->end(), 0, data->width, 0);
    // Margins place subsequent content. A final margin or table gap must not
    // manufacture an empty page after the last line.
    data->bottom = 0;
    for (const auto &line : data->lines)
        data->bottom = qMax(data->bottom, line.bounds.bottom());
    for (const auto &rule : data->rules)
        data->bottom = qMax(data->bottom, rule.bottom());
}
int PrivateRichText::pages() const {
    return data->pageHeight ? qMax(1, qCeil(data->bottom / data->pageHeight)) : 1;
}
int PrivateRichText::height() const { return qCeil(data->bottom); }
int PrivateRichText::naturalWidth() const {
    qreal width = 0;
    for (const auto &line : data->lines)
        width = qMax(width, line.bounds.left() + measure(line.text, line.font, line.styles, line.direction).advance);
    return qCeil(width);
}
QVector<QRectF> PrivateRichText::lineRectangles() const {
    QVector<QRectF> result;
    for (const auto &line : data->lines)
        result.append(line.bounds);
    return result;
}
void PrivateRichText::draw(QPainter &painter, const QRectF &clip, const QPalette &palette) const {
    painter.save();
    painter.setClipRect(clip, Qt::IntersectClip);
    for (const auto &line : data->lines) {
        if (!line.bounds.intersects(clip))
            continue;
        auto styles = line.styles;
        if (styles.isEmpty())
            styles.append({0, int(line.text.size()), line.font, palette.color(QPalette::WindowText)});
        for (auto &style : styles)
            if (!style.color.isValid() || (style.color == Qt::black &&
                    (!style.background.isValid() || style.background.alpha() == 0)))
                style.color = palette.color(QPalette::WindowText);
        const auto rendered = renderPrivateText(line.text, line.font, -1, {}, {}, PrivateTextEmphasis::FontFace,
                                                line.direction, styles);
        if (!rendered.image.isNull())
            painter.drawImage(line.baseline + rendered.origin, rendered.image);
    }
    for (const auto &rule : data->rules)
        painter.fillRect(rule, palette.color(QPalette::Mid));
    painter.restore();
}
}
