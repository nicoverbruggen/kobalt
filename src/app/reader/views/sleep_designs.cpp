#include "sleep_screen.h"

#include "ui/controls/panels.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/private_text.h"
#include "ui/style/rich_text.h"
#include "ui/style/theme.h"
#include "support/qt_compat.h"
#include "support/strings.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QTextBoundaryFinder>
#include <QtMath>

#include <cmath>
#include <functional>

namespace kobalt::reader {
namespace {
using sleep::Design;

// The designs were drawn on the six-inch 1072 × 1448 screen. Their positions
// and sizes are kept in hundredths of that screen, inside the largest frame
// of the same shape that fits the screen, so they scale with it as the cover
// does. x and sizes are hundredths of the frame width, y of its height.
struct Frame
{
    QRectF box;
    qreal u = 1, v = 1;

    explicit Frame(QSize size)
    {
        const QSizeF shape = QSizeF(1072, 1448).scaled(QSizeF(size), Qt::KeepAspectRatio);
        box = QRectF(
                QPointF((size.width() - shape.width()) / 2, (size.height() - shape.height()) / 2),
                shape);
        u = box.width() / 100;
        v = box.height() / 100;
    }

    qreal x(qreal hundredths) const { return box.left() + hundredths * u; }

    qreal y(qreal hundredths) const { return box.top() + hundredths * v; }

    qreal right(qreal hundredths) const { return box.right() - hundredths * u; }

    qreal bottom(qreal hundredths) const { return box.bottom() - hundredths * v; }

    int px(qreal hundredths) const { return qMax(1, qRound(hundredths * u)); }
};

struct Palette
{
    QColor paper, ink, gray, soft;
};

Palette palette(bool dark)
{
    const auto colors = ui::themeColors(dark);
    return { colors.sleepPaper, colors.ink, dark ? QColor(0x7a7a7a) : QColor(0x9a9a9a),
             dark ? QColor(0x3a3a3a) : QColor(0xd8d8d8) };
}

struct Content
{
    const sleep::Snapshot &shot;
    SleepStatus status;
    int battery;

    bool charging() const
    {
        return status == SleepStatus::Charging || status == SleepStatus::Charged;
    }

    QString word() const { return sleepStatusText(status); }

    QString percent() const
    {
        return strings::get("common.percent").arg(qBound(0, shot.percent, 100));
    }
};

QFont mono(qreal pixels, qreal spacingEm)
{
    QFont font = ui::chromeFont(qMax(1, qRound(pixels)));
    font.setLetterSpacing(QFont::AbsoluteSpacing, spacingEm * pixels);
    return font;
}

QFont sans(qreal pixels)
{
    return ui::interfaceFont(qMax(1, qRound(pixels)));
}

QFont serif(qreal pixels, bool italic = false)
{
    return ui::brandFont(qMax(1, qRound(pixels)), italic);
}

// Jost is one of the bundled book faces; sleepFonts() registers it.
QFont jost(qreal pixels, bool bold = false)
{
    QFont font(QStringLiteral("NV Jost"));
    font.setPixelSize(qMax(1, qRound(pixels)));
    font.setBold(bold);
    return font;
}

QFont body(const QString &family, qreal pixels)
{
    QFont font(family);
    font.setPixelSize(qMax(1, qRound(pixels)));
    return font;
}

void text(QPainter &painter, const QFont &font, const QColor &color, const QRectF &area,
          const QString &value, Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter)
{
    painter.setFont(font);
    painter.setPen(color);
    ui::drawInterfaceText(painter, area.toAlignedRect(), value, alignment);
}

// Draws one line so its baseline sits at `baseline`, like SVG text.
void baselineText(QPainter &painter, const QFont &font, const QColor &color, QPointF baseline,
                  const QString &value, Qt::Alignment horizontal = Qt::AlignHCenter)
{
    const int ascent = ui::interfaceLineAscent(font);
    const int height = ui::interfaceLineHeight(font);
    const int width = ui::interfaceTextWidth(value, font) + 2;
    qreal left = baseline.x();
    if (horizontal & Qt::AlignHCenter)
        left -= width / 2.0;
    else if (horizontal & Qt::AlignRight)
        left -= width;
    text(painter, font, color, QRectF(left, baseline.y() - ascent, width, height), value,
         Qt::AlignLeft | Qt::AlignTop);
}

int lineHeight(const QFont &font)
{
    return ui::interfaceLineHeight(font);
}

// The battery percentage and its icon, as one line. Returns its width.
qreal batteryWidth(const QFont &font, int percent)
{
    const auto label = strings::get("common.percent").arg(percent);
    const qreal body = font.pixelSize() * 0.75;
    return ui::interfaceTextWidth(label, font) + font.pixelSize() * 0.35 + ui::batteryWidth(body);
}

// The middle of the letters of `value`, for a line whose box starts at `top`.
// Line boxes leave room for descenders, so their middle sits above the ink.
qreal inkMiddle(const QFont &font, qreal top, const QString &value)
{
    return top + ui::interfaceLineAscent(font) + ui::interfaceInkBounds(value, font).center().y();
}

// The percentage stays on the line's baseline; the icon centres on its digits.
// `iconFirst` puts the icon on the left.
void battery(QPainter &painter, const QFont &font, const QColor &ink, const QColor &paper,
             QPointF leftCentre, int percent, bool charging, bool iconFirst = false)
{
    const auto label = strings::get("common.percent").arg(percent);
    const qreal body = font.pixelSize() * 0.75;
    const qreal iconWidth = body * int(ui::metrics::panel::batteryBodyWidth)
            / int(ui::metrics::panel::batteryBodyHeight);
    const qreal gap = font.pixelSize() * 0.35;
    const int labelWidth = ui::interfaceTextWidth(label, font);
    const int height = lineHeight(font);
    const qreal top = leftCentre.y() - height / 2.0;
    const qreal labelLeft =
            iconFirst ? leftCentre.x() + ui::batteryWidth(body) + gap : leftCentre.x();
    const qreal iconLeft = iconFirst ? leftCentre.x() : leftCentre.x() + labelWidth + gap;
    text(painter, font, ink, QRectF(labelLeft, top, labelWidth + 2, height), label,
         Qt::AlignLeft | Qt::AlignTop);
    const qreal middle = inkMiddle(font, top, label);
    ui::paintBattery(painter, QRectF(iconLeft, middle - body / 2, iconWidth, body),
                     percent >= 0 && percent <= 100 ? percent : -1, charging, ink, paper);
}

// Never split a surrogate pair or a combining sequence when shortening metadata.
QString shortened(const QString &value, int length)
{
    if (length >= value.size())
        return value;
    QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, value);
    boundary.setPosition(qMax(0, length));
    if (!boundary.isAtBoundary())
        boundary.toPreviousBoundary();
    return value.left(qMax(0, boundary.position())).trimmed() + QChar(0x2026);
}

QString metadata(const QString &value)
{
    // Metadata can contain an entire paragraph, or malformed megabytes of text.
    // Only a small prefix can be visible; bound work before shaping it.
    return shortened(value.simplified(), 1024);
}

QString fittedMetadata(const QString &value, const std::function<bool(const QString &)> &fits)
{
    const auto clean = metadata(value);
    if (fits(clean))
        return clean;
    int low = 0, high = clean.size();
    while (low < high) {
        const int middle = (low + high + 1) / 2;
        if (fits(shortened(clean, middle)))
            low = middle;
        else
            high = middle - 1;
    }
    return shortened(clean, low);
}

// Breaks `value` at spaces into at most `count` lines of `width`. The last line
// is cut short with an ellipsis when the text does not fit.
QStringList wrapLines(const QString &value, const QFont &font, int width, int count)
{
    QStringList lines;
    QString line;
    const auto words = metadata(value).split(QLatin1Char(' '), qt::skipEmptyParts);
    for (int index = 0; index < words.size(); ++index) {
        const auto next = line.isEmpty() ? words[index] : line + QLatin1Char(' ') + words[index];
        if (line.isEmpty() || ui::interfaceTextWidth(next, font) <= width) {
            line = next;
            continue;
        }
        if (lines.size() + 1 == count) {
            line = next;
            for (++index; index < words.size(); ++index)
                line += QLatin1Char(' ') + words[index];
            break;
        }
        lines.append(ui::elideInterfaceText(line, font, width));
        line = words[index];
    }
    if (!line.isEmpty())
        lines.append(ui::elideInterfaceText(line, font, width));
    return lines;
}

// Lays out wrapped text, optionally with HTML for an italic title.
struct Block
{
    ui::PrivateRichText layout;
    int height = 0, width = 0;
};

std::unique_ptr<Block> block(const QString &html, const QFont &font, qreal width,
                             qreal lineSpacing = 0)
{
    auto result = std::make_unique<Block>();
    result->layout.setFont(font);
    result->layout.setHtml(html);
    result->layout.layout(qMax(1, qRound(width)), 0, qRound(lineSpacing));
    result->height = result->layout.height();
    result->width = qMax(1, qRound(width));
    return result;
}

void drawBlock(QPainter &painter, const Block &value, QPointF topLeft, const QColor &color)
{
    QPalette colors;
    colors.setColor(QPalette::Text, color);
    colors.setColor(QPalette::WindowText, color);
    painter.save();
    painter.translate(topLeft);
    const QRectF bounds(0, 0, value.width, value.height);
    painter.setClipRect(bounds, Qt::IntersectClip);
    value.layout.draw(painter, bounds, colors);
    painter.restore();
}

QString escaped(const QString &value)
{
    return value.toHtmlEscaped();
}

std::unique_ptr<Block> titleBlock(const QString &value, const QFont &font, qreal width,
                                  qreal height, qreal spacing)
{
    const auto fitted = fittedMetadata(value, [&](const QString &candidate) {
        const auto measured = block(escaped(candidate), font, width, spacing);
        return measured->height <= height;
    });
    return block(escaped(fitted), font, width, spacing);
}

// Text spread evenly around a circle, starting at nine o'clock and running
// clockwise over the top, like SVG text on a circular path.
void ringText(QPainter &painter, const QFont &font, const QColor &color, QPointF centre,
              qreal radius, qreal length, const QString &value)
{
    if (value.isEmpty())
        return;
    QVector<int> advances;
    qreal natural = 0;
    for (const auto character : value) {
        advances.append(ui::interfaceTextWidth(QString(character), font));
        natural += advances.last();
    }
    const qreal gap = (length - natural) / value.size();
    const int ascent = ui::interfaceLineAscent(font);
    const int height = lineHeight(font);
    qreal along = gap / 2;
    painter.setFont(font);
    painter.setPen(color);
    for (int index = 0; index < value.size(); ++index) {
        const qreal middle = along + advances[index] / 2.0;
        const qreal angle = M_PI + middle / radius;
        painter.save();
        painter.translate(centre + QPointF(std::cos(angle), std::sin(angle)) * radius);
        painter.rotate(qRadiansToDegrees(angle) + 90);
        ui::drawInterfaceText(painter,
                              QRect(-advances[index], -ascent, 2 * advances[index] + 2, height),
                              QString(value[index]), Qt::AlignHCenter | Qt::AlignTop);
        painter.restore();
        along += advances[index] + gap;
    }
}

void paper(QPainter &painter, QSize size, const Palette &colors)
{
    painter.fillRect(QRect(QPoint(), size), colors.paper);
}

// ---------------------------------------------------------------- A03, A03b

// The line under a seal's figure: READ, or the battery level and icon while
// charging, centred on `baseline`.
void sealStatus(QPainter &painter, const QFont &font, const QColor &ink, const QColor &paper,
                QPointF baseline, const Content &content)
{
    if (!content.charging()) {
        baselineText(painter, font, ink, baseline, strings::get("sleep.read").toUpper());
        return;
    }
    QFont plain = font;
    plain.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    const qreal width = batteryWidth(plain, content.battery);
    const qreal top = baseline.y() - ui::interfaceLineAscent(plain);
    battery(painter, plain, ink, paper,
            QPointF(baseline.x() - width / 2, top + lineHeight(plain) / 2.0), content.battery,
            true);
}

QRectF sealBox(const Frame &frame, bool medallion)
{
    const qreal side = 38 * frame.u;
    return { frame.right(medallion ? 6 : 5) - side, frame.bottom(medallion ? 5 : 4) - side, side,
             side };
}

void paintSeal(QPainter &painter, const Frame &frame, const Content &content, const Palette &colors)
{
    const auto box = sealBox(frame, false);
    const qreal s = box.width() / 200;
    const QPointF centre = box.center();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colors.ink, 3 * s));
    painter.setBrush(colors.paper);
    painter.drawEllipse(centre, 97 * s - 1.5 * s, 97 * s - 1.5 * s);
    painter.setPen(QPen(colors.ink, 1.2 * s));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(centre, 60 * s, 60 * s);
    painter.restore();
    const auto rim = (content.word().toUpper() + QStringLiteral(" · KOBALT · ")).repeated(2);
    ringText(painter, mono(13.2 * s, 3.1 / 13.2), colors.ink, centre, 74 * s, 458 * s, rim);
    baselineText(painter, serif(50 * s), colors.ink, centre + QPointF(0, 16 * s),
                 content.percent());
    sealStatus(painter, mono(10 * s, 0.2), colors.ink, colors.paper, centre + QPointF(0, 38 * s),
               content);
}

void paintMedallion(QPainter &painter, const Frame &frame, const Content &content,
                    const Palette &colors)
{
    const auto box = sealBox(frame, true);
    const qreal s = box.width() / 200;
    const QPointF centre = box.center();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.ink);
    for (int index = 0; index < 36; ++index) {
        const qreal angle = index / 36.0 * 2 * M_PI;
        painter.drawEllipse(centre + QPointF(std::cos(angle), std::sin(angle)) * 92 * s, 8.6 * s,
                            8.6 * s);
    }
    painter.drawEllipse(centre, 92 * s, 92 * s);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colors.paper, 1.4 * s));
    painter.drawEllipse(centre, 82 * s, 82 * s);
    painter.drawEllipse(centre, 52 * s, 52 * s);
    painter.restore();
    const auto rim = (content.word().toUpper() + QStringLiteral(" · KOBALT · ")).repeated(2);
    ringText(painter, mono(12 * s, 0.25), colors.paper, centre, 68 * s, 420 * s, rim);
    baselineText(painter, serif(46 * s), colors.paper, centre + QPointF(0, 14 * s),
                 content.percent());
    sealStatus(painter, mono(9.4 * s, 0.2), colors.paper, colors.ink, centre + QPointF(0, 34 * s),
               content);
}

// ---------------------------------------------------------------- A14

struct PillLayout
{
    QRectF pill;
    qreal height = 0;
};

// Each gap between the pill's parts holds a thin vertical rule.
constexpr qreal pillPadding = 4.6;
constexpr qreal pillGap = 2 * pillPadding;

PillLayout pillLayout(const Frame &frame, const Content &content)
{
    const QFont font = sans(3 * frame.u);
    const qreal gap = pillGap * frame.u;
    qreal width = 1.8 * frame.u + 2.2 * frame.u + ui::interfaceTextWidth(content.word(), font) + gap
            + ui::interfaceTextWidth(strings::get("sleep.percent_read").arg(content.shot.percent),
                                     font);
    if (content.charging())
        width += gap + batteryWidth(font, content.battery);
    width += 2 * pillPadding * frame.u;
    const qreal height = lineHeight(font) + 2 * 1.4 * frame.v;
    const QRectF pill(frame.box.center().x() - width / 2, frame.bottom(6) - height, width, height);
    return { pill, height };
}

void paintPill(QPainter &painter, const Frame &frame, const Content &content, const Palette &colors)
{
    const auto layout = pillLayout(frame, content);
    const auto pill = layout.pill;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colors.ink, 0.3 * frame.u));
    painter.setBrush(colors.paper);
    painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    const qreal dot = 1.8 * frame.u;
    qreal x = pill.left() + pillPadding * frame.u;
    const qreal middle = pill.center().y();
    painter.restore();
    const QFont font = sans(3 * frame.u);
    const int height = lineHeight(font);
    const qreal gap = pillGap * frame.u;
    const qreal top = middle - height / 2.0;
    const qreal dotMiddle = middle;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.ink);
    painter.drawEllipse(QPointF(x + dot / 2, dotMiddle), dot / 2, dot / 2);
    painter.restore();
    const auto rule = [&](qreal left) {
        painter.fillRect(QRectF(left + gap / 2 - 0.125 * frame.u, pill.top() + 0.3 * frame.u,
                                0.25 * frame.u, pill.height() - 0.6 * frame.u),
                         colors.gray);
    };
    const auto label = [&](const QString &value, const QColor &color) {
        painter.setFont(font);
        painter.setPen(color);
        ui::drawSegmentPrivateText(
                painter,
                QRectF(x, top, ui::interfaceTextWidth(value, font) + 2, height).toAlignedRect(),
                value, Qt::AlignLeft | Qt::AlignVCenter);
    };
    x += dot + 2.2 * frame.u;
    const auto word = content.word();
    label(word, colors.ink);
    x += ui::interfaceTextWidth(word, font);
    rule(x);
    x += gap;
    const auto read = strings::get("sleep.percent_read").arg(content.shot.percent);
    label(read, colors.gray);
    x += ui::interfaceTextWidth(read, font);
    if (content.charging()) {
        rule(x);
        x += gap;
        battery(painter, font, colors.ink, colors.paper, QPointF(x, top + height / 2.0),
                content.battery, true);
    }
}

QVector<QRect> pillAreas(const Frame &frame, const sleep::Snapshot &shot, QSize size)
{
    // The pill's width follows its text, so repaint the whole band across the screen.
    const Content sample{ shot, SleepStatus::Charging, 100 };
    const auto layout = pillLayout(frame, sample);
    const qreal border = 0.3 * frame.u;
    return { QRectF(0, layout.pill.top() - border - 1, size.width(), layout.height + 2 * border + 2)
                     .toAlignedRect() };
}

// ---------------------------------------------------------------- A19

struct CaptionLayout
{
    qreal top = 0, titleTop = 0, detailTop = 0, bottom = 0;
    std::unique_ptr<Block> title;
};

CaptionLayout captionLayout(const Frame &frame, const sleep::Snapshot &shot)
{
    CaptionLayout layout;
    const qreal width = 84 * frame.u;
    layout.title =
            titleBlock(shot.details.title, serif(9 * frame.u), width, 28 * frame.v, 9 * frame.u);
    const QFont detail = sans(3 * frame.u);
    const QFont state = mono(2.2 * frame.u, 0.3);
    layout.bottom = frame.bottom(6);
    layout.detailTop = layout.bottom - lineHeight(detail);
    layout.titleTop = layout.detailTop - 1.6 * frame.v - layout.title->height;
    layout.top = layout.titleTop - 1 * frame.v - lineHeight(state);
    return layout;
}

// The caption's fade belongs to its backdrop: it is drawn before the status
// underlay is saved, so a battery change redraws only the text over it.
void paintCaptionFade(QPainter &painter, const Frame &frame, QSize size)
{
    const qreal top = frame.bottom(46);
    QLinearGradient gradient(0, top, 0, size.height());
    gradient.setColorAt(0, QColor(0, 0, 0, 0));
    gradient.setColorAt(0.45, QColor(0, 0, 0, 217));
    gradient.setColorAt(1, Qt::black);
    painter.fillRect(QRectF(0, top, size.width(), size.height() - top), gradient);
}

void paintCaption(QPainter &painter, const Frame &frame, const Content &content)
{
    const auto layout = captionLayout(frame, content.shot);
    const QColor white(Qt::white);
    const QFont state = mono(2.2 * frame.u, 0.3);
    // While charging the state line carries the battery level: CHARGING (67%).
    const auto heading = content.charging()
            ? strings::get("sleep.state_with_battery").arg(content.word()).arg(content.battery)
            : content.word();
    text(painter, state, white, QRectF(frame.x(8), layout.top, 84 * frame.u, lineHeight(state)),
         heading.toUpper());
    drawBlock(painter, *layout.title, QPointF(frame.x(8), layout.titleTop), white);
    const QFont detail = sans(3 * frame.u);
    QString line = content.shot.progress;
    if (!content.shot.remaining.isEmpty())
        line += QStringLiteral(" · ") + content.shot.remaining;
    text(painter, detail, white,
         QRectF(frame.x(8), layout.detailTop, 84 * frame.u, lineHeight(detail)),
         ui::elideInterfaceText(line, detail, qRound(84 * frame.u)));
}

QVector<QRect> captionAreas(const Frame &frame, const sleep::Snapshot &shot, QSize size)
{
    const auto layout = captionLayout(frame, shot);
    const QFont state = mono(2.2 * frame.u, 0.3);
    return { QRectF(0, layout.top - 2, size.width(), lineHeight(state) + 4).toAlignedRect() };
}

// ---------------------------------------------------------------- B16b

struct WidgetsLayout
{
    QRectF word, book, cover, read, second, left;
    std::unique_ptr<Block> title;
    qreal titleTop = 0, authorTop = 0, labelTop = 0;
};

WidgetsLayout widgetsLayout(const Frame &frame, const sleep::Snapshot &shot)
{
    WidgetsLayout layout;
    const qreal u = frame.u, pad = 3.4 * u, gap = 3 * u;
    const QFont state = serif(12 * u);
    layout.word = QRectF(frame.x(7), frame.y(8), 86 * u, lineHeight(state));
    const qreal left = frame.x(7), width = 86 * u;
    const QSizeF coverShape =
            shot.coverArea.isEmpty() ? QSizeF(600, 851) : QSizeF(shot.coverArea.size());
    const qreal coverWidth = 24 * u;
    const qreal coverHeight = coverWidth * coverShape.height() / coverShape.width();
    const qreal textLeft = left + pad + coverWidth + 4 * u;
    const qreal textWidth = left + width - pad - textLeft;
    const QFont label = mono(1.9 * u, 0.2), author = sans(2.6 * u);
    layout.title =
            titleBlock(shot.details.title, serif(6.4 * u), textWidth, 24 * frame.v, 6.4 * u * 1.05);
    const qreal textHeight = lineHeight(label) + 0.6 * frame.v + layout.title->height
            + (shot.details.author.isEmpty() ? 0 : 0.8 * frame.v + lineHeight(author));
    const qreal inner = qMax(coverHeight, textHeight);
    layout.book = QRectF(left, frame.y(24), width, inner + 2 * pad);
    layout.cover =
            QRectF(left + pad, layout.book.center().y() - coverHeight / 2, coverWidth, coverHeight);
    layout.labelTop = layout.book.center().y() - textHeight / 2;
    layout.titleTop = layout.labelTop + lineHeight(label) + 0.6 * frame.v;
    layout.authorTop = layout.titleTop + layout.title->height + 0.8 * frame.v;
    const QFont figure = serif(13 * u), small = sans(2.4 * u);
    const qreal half = (width - gap) / 2;
    const qreal smallHeight = lineHeight(label) + lineHeight(figure) + 1 * frame.v
            + qMax<qreal>(0.9 * frame.v, lineHeight(small));
    const qreal top = layout.book.bottom() + gap;
    layout.read = QRectF(left, top, half, smallHeight + 2 * pad);
    layout.second = QRectF(left + half + gap, top, half, smallHeight + 2 * pad);
    const QFont remaining = serif(8 * u);
    layout.left = QRectF(left, layout.read.bottom() + gap, width, lineHeight(remaining) + 2 * pad);
    return layout;
}

QString todayText(qint64 milliseconds)
{
    const qint64 minutes = qMax<qint64>(0, milliseconds) / 60000;
    return minutes < 60 ? strings::get("sleep.today_minutes").arg(minutes)
                        : strings::get("sleep.today_hours").arg(minutes / 60).arg(minutes % 60);
}

void paintWidgets(QPainter &painter, const Frame &frame, const Content &content, QSize size,
                  const QImage &cover, const Palette &colors)
{
    const auto layout = widgetsLayout(frame, content.shot);
    const qreal u = frame.u, pad = 3.4 * u;
    const QColor background = colors.paper;
    painter.fillRect(QRect(QPoint(), size), background);
    const QFont state = serif(12 * u);
    text(painter, state, colors.ink, layout.word, content.word(), Qt::AlignLeft | Qt::AlignTop);
    if (content.charging()) {
        // The battery aligns with the tiles' right edge, on the state word's middle.
        const QFont level = sans(4 * u);
        const qreal middle = inkMiddle(state, layout.word.top(), content.word());
        const qreal left = layout.word.right() - batteryWidth(level, content.battery);
        battery(painter, level, colors.ink, background, QPointF(left, middle), content.battery,
                true);
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colors.gray, qMax<qreal>(1, 0.2 * u)));
    painter.setBrush(colors.paper);
    for (const auto &tile : { layout.book, layout.read, layout.second, layout.left })
        painter.drawRoundedRect(tile, 4 * u, 4 * u);
    painter.restore();
    if (!cover.isNull()) {
        QPainterPath clip;
        clip.addRoundedRect(layout.cover, 1 * u, 1 * u);
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.drawImage(layout.cover, cover);
        painter.restore();
    } else {
        painter.fillRect(layout.cover, colors.soft);
    }
    const QFont label = mono(1.9 * u, 0.2);
    const qreal textLeft = layout.cover.right() + 4 * u;
    const qreal textWidth = layout.book.right() - pad - textLeft;
    text(painter, label, colors.gray,
         QRectF(textLeft, layout.labelTop, textWidth, lineHeight(label)),
         strings::get("sleep.now_reading").toUpper());
    drawBlock(painter, *layout.title, QPointF(textLeft, layout.titleTop), colors.ink);
    if (!content.shot.details.author.isEmpty()) {
        const QFont author = sans(2.6 * u);
        text(painter, author, colors.ink,
             QRectF(textLeft, layout.authorTop, textWidth, lineHeight(author)),
             ui::elideInterfaceText(metadata(content.shot.details.author), author,
                                    qRound(textWidth)));
    }
    const QFont figure = serif(13 * u), small = sans(2.4 * u);
    const auto tileText = [&](const QRectF &tile, const QString &caption, const QString &value) {
        const qreal x = tile.left() + pad;
        qreal y = tile.top() + pad;
        text(painter, label, colors.gray, QRectF(x, y, tile.width() - 2 * pad, lineHeight(label)),
             caption.toUpper());
        y += lineHeight(label);
        text(painter, figure, colors.ink, QRectF(x, y, tile.width() - 2 * pad, lineHeight(figure)),
             value);
        return y + lineHeight(figure) + 1 * frame.v;
    };
    const qreal barTop = tileText(layout.read, strings::get("sleep.read_label"), content.percent());
    const QRectF bar(layout.read.left() + pad, barTop, layout.read.width() - 2 * pad,
                     0.9 * frame.v);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.soft);
    painter.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
    painter.setBrush(colors.ink);
    painter.setClipRect(
            QRectF(bar.topLeft(),
                   QSizeF(bar.width() * qBound(0, content.shot.percent, 100) / 100, bar.height())),
            Qt::IntersectClip);
    painter.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
    painter.restore();
    {
        const qreal y = tileText(layout.second, strings::get("sleep.today"),
                                 todayText(content.shot.details.todayMilliseconds));
        text(painter, small, colors.ink,
             QRectF(layout.second.left() + pad, y, layout.second.width() - 2 * pad,
                    lineHeight(small)),
             strings::get("sleep.of_reading"));
    }
    // Keep the two-line caption distinct from the remaining time.
    const QFont remaining = serif(8 * u);
    const qreal baseline = layout.left.top() + pad + ui::interfaceLineAscent(remaining);
    const auto caption = block(escaped(strings::get("sleep.time_remaining_in_book").toUpper())
                                       .replace(QLatin1Char('\n'), QStringLiteral("<br>")),
                               label, 40 * u);
    drawBlock(painter, *caption,
              QPointF(layout.left.left() + pad, layout.left.center().y() - caption->height / 2.0),
              colors.ink);
    const auto &left = content.shot.details.left;
    baselineText(painter, remaining, colors.ink, QPointF(layout.left.right() - pad, baseline),
                 left.isEmpty() ? content.shot.remaining : left, Qt::AlignRight);
}

QVector<QRect> widgetsAreas(const Frame &frame, const sleep::Snapshot &shot)
{
    const auto layout = widgetsLayout(frame, shot);
    return { layout.word.adjusted(-2, -2, 2, 2).toAlignedRect() };
}

// ---------------------------------------------------------------- C09

void star(QPainter &painter, QPointF centre, qreal radius, bool filled, const QColor &ink)
{
    QPainterPath path;
    for (int index = 0; index < 8; ++index) {
        const qreal angle = -M_PI / 2 + index * M_PI / 4;
        const qreal reach = index % 2 ? radius * 0.28 : radius;
        const QPointF point = centre + QPointF(std::cos(angle), std::sin(angle)) * reach;
        index ? path.lineTo(point) : path.moveTo(point);
    }
    path.closeSubpath();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (filled) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
    } else {
        painter.setPen(QPen(ink, qMax<qreal>(1, radius * 0.16)));
        painter.setBrush(Qt::NoBrush);
    }
    painter.drawPath(path);
    painter.restore();
}

struct NightLayout
{
    qreal wordTop = 0, lineTop = 0;
};

// The battery level, while charging, is a line of capitals in the top-left
// corner, so the words at the foot never move.
QFont nightLevel(const Frame &frame)
{
    return mono(2.4 * frame.u, 0.24);
}

NightLayout nightLayout(const Frame &frame)
{
    const QFont word = serif(15 * frame.u, true), detail = sans(3 * frame.u);
    NightLayout layout;
    layout.lineTop = frame.bottom(9) - lineHeight(detail);
    layout.wordTop = layout.lineTop - 2 * frame.v - lineHeight(word) * 0.95;
    return layout;
}

void paintNight(QPainter &painter, const Frame &frame, const Content &content, QSize size,
                const Palette &colors)
{
    paper(painter, size, colors);

    struct Star
    {
        qreal x, y, size;
        int kind;
    };

    // kind 0 is a filled star, 1 an outline, 2 a dot.
    const Star stars[] = { { 12, 14, 4, 0 },   { 30, 30, 2.4, 1 }, { 18, 44, 3, 2 },
                           { 70, 52, 3.4, 0 }, { 86, 40, 2.2, 1 }, { 52, 8, 2.6, 1 },
                           { 8, 62, 2.4, 0 },  { 44, 58, 2, 2 },   { 90, 8, 3, 2 },
                           { 62, 36, 2, 0 } };
    for (const auto &item : stars) {
        const qreal side = item.size * frame.u;
        const QPointF centre(frame.x(item.x) + side * 0.4, frame.y(item.y) + side * 0.66);
        if (item.kind == 2) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.ink);
            painter.drawEllipse(centre, side * 0.09, side * 0.09);
            painter.restore();
        } else {
            star(painter, centre, side * 0.36, item.kind == 0, colors.ink);
        }
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const qreal outer = 44 * frame.u, inner = 40 * frame.u;
    painter.setBrush(colors.ink);
    painter.drawEllipse(QRectF(frame.right(12) - outer, frame.y(10), outer, outer));
    painter.setBrush(colors.paper);
    painter.drawEllipse(QRectF(frame.right(3) - inner, frame.y(5), inner, inner));
    painter.restore();
    const auto layout = nightLayout(frame);
    QFont word = serif(15 * frame.u, true);
    const QFont detail = sans(3 * frame.u);
    // While asleep, the reader's own greeting replaces "Goodnight". A long one
    // shrinks to fit, down to half size, and is then cut short.
    const auto &greeting = content.shot.details.greeting;
    auto heading = content.status != SleepStatus::Sleeping ? content.word()
            : greeting.isEmpty()                           ? strings::get("sleep.goodnight")
                                                           : greeting;
    const int room = qRound(82 * frame.u);
    const int natural = ui::interfaceTextWidth(heading, word);
    if (natural > room)
        word = serif(qMax(7.5, 15 * qreal(room) / natural) * frame.u, true);
    heading = ui::elideInterfaceText(heading, word, room);
    text(painter, word, colors.ink,
         QRectF(frame.x(9), layout.wordTop, 82 * frame.u, lineHeight(word)), heading,
         Qt::AlignLeft | Qt::AlignTop);
    QString line = strings::get("sleep.percent_through")
                           .arg(qBound(0, content.shot.percent, 100))
                           .arg(content.shot.details.title);
    if (!content.shot.remaining.isEmpty())
        line += QStringLiteral(" · ") + content.shot.remaining;
    text(painter, detail, colors.ink,
         QRectF(frame.x(9), layout.lineTop, 82 * frame.u, lineHeight(detail)),
         ui::elideInterfaceText(line, detail, qRound(82 * frame.u)));
    if (content.charging()) {
        const QFont level = nightLevel(frame);
        text(painter, level, colors.ink,
             QRectF(frame.x(9), frame.y(8), 82 * frame.u, lineHeight(level)),
             strings::get("sleep.battery_charged").arg(content.battery).toUpper(),
             Qt::AlignLeft | Qt::AlignTop);
    }
}

QVector<QRect> nightAreas(const Frame &frame, QSize size)
{
    const auto layout = nightLayout(frame);
    const QFont word = serif(15 * frame.u, true);
    return { QRectF(frame.x(9) - 2, frame.y(8) - 2, 82 * frame.u + 4,
                    lineHeight(nightLevel(frame)) + 4)
                     .toAlignedRect(),
             QRectF(0, layout.wordTop - 2, size.width(), lineHeight(word) + 4).toAlignedRect() };
}

// ---------------------------------------------------------------- C12

void paintRing(QPainter &painter, const Frame &frame, const Content &content, QSize size,
               const Palette &colors)
{
    paper(painter, size, colors);
    const QFont state = mono(2.6 * frame.u, 0.34);
    text(painter, state, colors.ink,
         QRectF(frame.box.left(), frame.y(12), frame.box.width(), lineHeight(state)),
         content.word().toUpper(), Qt::AlignHCenter | Qt::AlignTop);
    const qreal side = 72 * frame.u, s = side / 100;
    const QRectF box(frame.x(14), frame.y(19), side, side);
    const QPointF centre = box.center();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colors.soft, 1.2 * s));
    painter.drawEllipse(centre, 42 * s, 42 * s);
    const int percent = qBound(0, content.shot.percent, 100);
    if (percent > 0) {
        QPen arc(colors.ink, 5 * s);
        arc.setCapStyle(Qt::RoundCap);
        painter.setPen(arc);
        painter.drawArc(QRectF(centre - QPointF(42 * s, 42 * s), QSizeF(84 * s, 84 * s)), 90 * 16,
                        -qRound(percent * 360 * 16 / 100.0));
    }
    painter.restore();
    const QFont figure = jost(25 * s), sign = jost(10 * s);
    const auto number = QString::number(percent);
    const int numberWidth = ui::interfaceTextWidth(number, figure);
    const int signWidth = ui::interfaceTextWidth(QStringLiteral("%"), sign);
    const qreal start = centre.x() - (numberWidth + signWidth) / 2.0;
    const qreal percentBaseline = box.top() + 54 * s;
    baselineText(painter, figure, colors.ink, QPointF(start, percentBaseline), number,
                 Qt::AlignLeft);
    baselineText(painter, sign, colors.ink, QPointF(start + numberWidth, percentBaseline),
                 QStringLiteral("%"), Qt::AlignLeft);
    // The title may take two lines under the figure; a longer one is cut short.
    const QFont title = mono(3.6 * s, 0.8 / 3.6);
    const auto lines = wrapLines(content.shot.details.title.toUpper(), title, qRound(60 * s), 2);
    for (int index = 0; index < lines.size(); ++index)
        baselineText(painter, title, colors.ink,
                     QPointF(centre.x(), box.top() + 68 * s + index * 5.2 * s), lines[index]);
    const QFont remaining = jost(4 * frame.u), detail = sans(2.8 * frame.u);
    text(painter, remaining, colors.ink,
         QRectF(frame.box.left(), frame.bottom(12) - lineHeight(remaining), frame.box.width(),
                lineHeight(remaining)),
         content.shot.remaining, Qt::AlignHCenter | Qt::AlignVCenter);
    if (content.charging()) {
        const qreal width = batteryWidth(detail, content.battery);
        battery(painter, detail, colors.ink, colors.paper,
                QPointF(frame.box.center().x() - width / 2,
                        frame.bottom(7) - lineHeight(detail) / 2.0),
                content.battery, true);
    }
}

QVector<QRect> ringAreas(const Frame &frame, QSize size)
{
    const QFont state = mono(2.6 * frame.u, 0.34), detail = sans(2.8 * frame.u);
    return { QRectF(0, frame.y(12) - 2, size.width(), lineHeight(state) + 4).toAlignedRect(),
             QRectF(0, frame.bottom(7) - lineHeight(detail) - 2, size.width(),
                    lineHeight(detail) + 4)
                     .toAlignedRect() };
}

// ---------------------------------------------------------------- C14

struct LinesLayout
{
    QVector<QVector<qreal>> segments;
    QVector<bool> indented;
    int read = 0;
    qreal top = 0, row = 0, gap = 0, labelTop = 0, labelHeight = 0;
};

LinesLayout linesLayout(const Frame &frame, int percent)
{
    LinesLayout layout;
    // 64 bits: the product overflows an int, which the JavaScript mockup never did.
    qint64 seed = 7;
    const auto random = [&seed] {
        seed = (seed * 9301 + 49297) % 233280;
        return seed / 233280.0;
    };
    const int total = 26;
    layout.read = qRound(total * qBound(0, percent, 100) / 100.0);
    for (int index = 0; index < total; ++index) {
        const bool paragraph = index == 0 || index == 9 || index == 17;
        const bool last = index == 8 || index == 16 || index == 25;
        QVector<qreal> parts;
        qreal left = 100 - (paragraph ? 8 : 0);
        const qreal end = last ? 40 + random() * 30 : 0;
        while (left > end + 6) {
            const qreal width = qMin(left - end, 6 + random() * 16);
            parts.append(width);
            left -= width + 1.6;
        }
        layout.segments.append(parts);
        layout.indented.append(paragraph);
    }
    layout.top = frame.y(8);
    layout.row = 1.5 * frame.v;
    layout.gap = 1.55 * frame.v;
    layout.labelTop = layout.top + layout.read * (layout.row + layout.gap);
    layout.labelHeight = layout.row;
    return layout;
}

void paintLines(QPainter &painter, const Frame &frame, const Content &content, QSize size,
                const Palette &colors)
{
    paper(painter, size, colors);
    const auto layout = linesLayout(frame, content.shot.percent);
    const qreal left = frame.x(9), width = 82 * frame.u;
    for (int index = 0; index < layout.segments.size(); ++index) {
        const int slot = index < layout.read ? index : index + 1;
        const qreal y = layout.top + slot * (layout.row + layout.gap);
        qreal x = left + (layout.indented[index] ? width * 0.08 : 0);
        for (const auto part : layout.segments[index]) {
            painter.fillRect(QRectF(x, y, width * part / 100, layout.row),
                             index < layout.read ? colors.ink : colors.soft);
            x += width * (part + 1.6) / 100;
        }
    }
    const QFont label = mono(2.4 * frame.u, 0.24);
    const qreal middle = layout.labelTop + layout.labelHeight / 2;
    const int height = lineHeight(label);
    const auto word = content.word().toUpper();
    const qreal wordWidth = ui::interfaceTextWidth(word, label) + 3.2 * frame.u;
    const QRectF tag(left, middle - height / 2.0 - 0.3 * frame.v, wordWidth,
                     height + 0.6 * frame.v);
    painter.fillRect(tag, colors.ink);
    painter.setFont(label);
    painter.setPen(colors.paper);
    ui::drawOpticalPrivateText(painter, tag.toAlignedRect(), word, Qt::AlignCenter);
    qreal x = tag.right() + 2 * frame.u;
    const auto percent = content.percent();
    painter.setPen(colors.ink);
    ui::drawSegmentPrivateText(
            painter,
            QRectF(x, middle - height / 2.0, ui::interfaceTextWidth(percent, label) + 2, height)
                    .toAlignedRect(),
            percent, Qt::AlignLeft | Qt::AlignVCenter);
    x += ui::interfaceTextWidth(percent, label) + 2 * frame.u;
    qreal end = left + width;
    if (content.charging()) {
        const QFont plain = mono(2.4 * frame.u, 0);
        const qreal battWidth = batteryWidth(plain, content.battery);
        battery(painter, plain, colors.ink, colors.paper, QPointF(end - battWidth, middle),
                content.battery, true);
        end -= battWidth + 2 * frame.u;
    }
    painter.fillRect(QRectF(x, middle - 0.125 * frame.u, qMax<qreal>(0, end - x), 0.25 * frame.u),
                     colors.ink);
    const QFont footer = mono(2.1 * frame.u, 0);
    const qreal footerTop = frame.bottom(5) - lineHeight(footer);
    text(painter, footer, colors.gray, QRectF(left, footerTop, width / 2, lineHeight(footer)),
         ui::elideInterfaceText(content.shot.details.title, footer, qRound(width / 2)));
    text(painter, footer, colors.gray,
         QRectF(left + width / 2, footerTop, width / 2, lineHeight(footer)), content.shot.remaining,
         Qt::AlignRight | Qt::AlignVCenter);
}

QVector<QRect> linesAreas(const Frame &frame, const sleep::Snapshot &shot, QSize size)
{
    const auto layout = linesLayout(frame, shot.percent);
    const QFont label = mono(2.4 * frame.u, 0.24);
    const qreal middle = layout.labelTop + layout.labelHeight / 2;
    const qreal half = lineHeight(label) / 2.0 + 0.3 * frame.v + 2;
    return { QRectF(0, middle - half, size.width(), 2 * half).toAlignedRect() };
}

// ---------------------------------------------------------------- C28b

// The letter's line height, more open than the mockup's 1.6 so the marked
// figures have room.
constexpr qreal letterLeading = 2.0;
constexpr qreal letterSize = 4.0;

// The figures in the letter are marked like highlights: paper on ink, which
// inverts with the theme. Spacing belongs outside the highlight.
QString mark(const QString &value, const Palette &colors)
{
    return QStringLiteral("&#8201;<span style=\"background-color:%1;color:%2\">%3</span>&#8201;")
            .arg(colors.ink.name(), colors.paper.name(), escaped(value));
}

QStringList letterParagraphs(const Content &content, const Palette &colors)
{
    const auto &details = content.shot.details;
    const bool off = content.status == SleepStatus::PoweredOff;
    const auto title = QStringLiteral("<i>%1</i>").arg(escaped(details.title));
    // Substitute only placeholders from the template, never from book metadata.
    const auto fill = [](const QString &text, const QStringList &values) {
        QString result;
        for (int i = 0; i < text.size(); ++i) {
            if (text[i] == QLatin1Char('%') && i + 1 < text.size()) {
                const int index = text[i + 1].digitValue() - 1;
                if (index >= 0 && index < values.size()) {
                    result += values[index];
                    ++i;
                    continue;
                }
            }
            result += text[i];
        }
        return result;
    };
    QString main;
    if (content.shot.percent >= 100) {
        main = fill(strings::get(off ? "sleep.letter_finished_off" : "sleep.letter_finished"),
                    { title });
    } else {
        const auto chapter = details.chapter.isEmpty()
                ? QString()
                : fill(strings::get("sleep.letter_chapter"), { escaped(details.chapter) });
        const auto percent = mark(
                strings::get("common.percent").arg(qBound(0, content.shot.percent, 100)), colors);
        main = fill(strings::get(off ? "sleep.letter_progress_off" : "sleep.letter_progress"),
                    { percent, title, chapter });
    }
    QStringList paragraphs{ escaped(strings::get("sleep.letter_greeting")), main };
    if (content.shot.percent < 100 && !details.left.isEmpty())
        paragraphs.append(
                fill(strings::get("sleep.letter_remaining"), { mark(details.left, colors) }));
    if (content.charging()) {
        // The battery level starts its own line.
        const auto level = mark(strings::get("common.percent").arg(content.battery), colors);
        paragraphs.append(escaped(strings::get("sleep.letter_charging")) + QStringLiteral("<br>")
                          + fill(strings::get("sleep.letter_battery"), { level }));
    }
    paragraphs.append(escaped(strings::get(off ? "sleep.letter_restart" : "sleep.letter_wake")));
    return paragraphs;
}

void paintLetter(QPainter &painter, const Frame &frame, const Content &content, QSize size,
                 const Palette &colors)
{
    paper(painter, size, colors);
    const qreal left = frame.x(10), width = 80 * frame.u;
    const QFont heading = mono(1.9 * frame.u, 0.24);
    const qreal headingTop = frame.y(9);
    QString headingText = strings::get("sleep.letter_heading").toUpper();
    if (!content.shot.details.clock.isEmpty())
        headingText += QStringLiteral(" · ") + content.shot.details.clock;
    text(painter, heading, colors.ink, QRectF(left, headingTop, width, lineHeight(heading)),
         headingText, Qt::AlignLeft | Qt::AlignTop);
    painter.fillRect(
            QRectF(left, headingTop + lineHeight(heading) + 1.2 * frame.v, width, 0.3 * frame.u),
            colors.ink);
    // The reader's chosen body face; Libron, the default, when none is known.
    const auto &family = content.shot.details.font;
    const QFont font =
            body(family.isEmpty() ? QStringLiteral("Libron") : family, letterSize * frame.u);
    const QFont signature = serif(11 * frame.u);
    const qreal signatureTop = frame.bottom(12) - lineHeight(signature) * 1.1;
    const qreal gap = 2.4 * frame.v;
    const qreal available = signatureTop - lineHeight(font) - gap - frame.y(20);
    auto shot = content.shot;
    const auto title = metadata(shot.details.title), chapter = metadata(shot.details.chapter);
    shot.details.left =
            ui::elideInterfaceText(metadata(shot.details.left), font, qRound(width / 3));
    const auto paragraphs = [&](int limit) {
        shot.details.title = shortened(title, limit);
        shot.details.chapter = shortened(chapter, limit);
        return letterParagraphs({ shot, content.status, content.battery }, colors);
    };
    const auto fits = [&](int limit) {
        qreal height = -gap;
        for (const auto &paragraph : paragraphs(limit))
            height += block(paragraph, font, width, letterSize * frame.u * letterLeading)->height
                    + gap;
        return height <= available;
    };
    int low = 0, high = qMax(title.size(), chapter.size());
    if (!fits(high)) {
        while (low < high) {
            const int middle = (low + high + 1) / 2;
            if (fits(middle))
                low = middle;
            else
                high = middle - 1;
        }
    }
    qreal y = frame.y(20);
    painter.save();
    painter.setClipRect(QRectF(left, y, width, available), Qt::IntersectClip);
    for (const auto &paragraph : paragraphs(high)) {
        const auto laid = block(paragraph, font, width, letterSize * frame.u * letterLeading);
        drawBlock(painter, *laid, QPointF(left, y), colors.ink);
        y += laid->height + gap;
    }
    painter.restore();
    text(painter, font, colors.ink,
         QRectF(left, signatureTop - lineHeight(font), width, lineHeight(font)),
         strings::get("sleep.letter_closing"));
    text(painter, signature, colors.ink, QRectF(left, signatureTop, width, lineHeight(signature)),
         QStringLiteral("Kobalt"), Qt::AlignLeft | Qt::AlignTop);
}

QVector<QRect> letterAreas(const Frame &frame, QSize size)
{
    // The charging line moves the paragraph after it, so the whole body repaints.
    const QFont signature = serif(11 * frame.u);
    const qreal bottom = frame.bottom(12) - lineHeight(signature) * 1.1;
    return {
        QRectF(0, frame.y(20), size.width(), qMax<qreal>(0, bottom - frame.y(20))).toAlignedRect()
    };
}

// ----------------------------------------------------------------

// Draws what a design adds to its backdrop before the status underlay is saved.
void paintGround(QPainter &painter, QSize size, const sleep::Snapshot &shot)
{
    if (shot.design == Design::Caption)
        paintCaptionFade(painter, Frame(size), size);
}

// Draws a design that is not tiled over a backdrop already on the painter.
void paintDesign(QPainter &painter, QSize size, const sleep::Snapshot &shot, const QImage &cover,
                 int batteryPercent, SleepStatus status)
{
    const Frame frame(size);
    const Content content{ shot, status, batteryPercent };
    const bool dark = shot.dark || status == SleepStatus::PoweredOff;
    const auto colors = palette(dark);
    switch (shot.design) {
    case Design::Seal:
        paintSeal(painter, frame, content, colors);
        break;
    case Design::Medallion:
        paintMedallion(painter, frame, content, colors);
        break;
    case Design::Pill:
        paintPill(painter, frame, content, colors);
        break;
    case Design::Caption:
        paintCaption(painter, frame, content);
        break;
    case Design::Widgets:
        paintWidgets(painter, frame, content, size, cover, colors);
        break;
    case Design::Night:
        paintNight(painter, frame, content, size, colors);
        break;
    case Design::Ring:
        paintRing(painter, frame, content, size, colors);
        break;
    case Design::Lines:
        paintLines(painter, frame, content, size, colors);
        break;
    case Design::Letter:
        paintLetter(painter, frame, content, size, colors);
        break;
    default:
        break;
    }
}

sleep::Snapshot displaySnapshot(sleep::Snapshot shot)
{
    shot.details.title = metadata(shot.details.title);
    shot.details.author = metadata(shot.details.author);
    shot.details.chapter = metadata(shot.details.chapter);
    shot.details.greeting = metadata(shot.details.greeting);
    shot.details.left = metadata(shot.details.left);
    shot.details.clock = metadata(shot.details.clock);
    shot.progress = metadata(shot.progress);
    shot.remaining = metadata(shot.remaining);
    return shot;
}

} // namespace

QImage sleepDesignImage(QImage backdrop, const sleep::Snapshot &source, int batteryPercent,
                        SleepStatus status, QVector<QImage> *underlay)
{
    const auto snapshot = displaySnapshot(source);
    if (underlay)
        underlay->clear();
    if (sleep::tiled(snapshot.design) || backdrop.isNull())
        return sleepScreenImage(std::move(backdrop), snapshot.progress, snapshot.remaining,
                                snapshot.dark, batteryPercent, status, snapshot.dimPage,
                                snapshot.tile, snapshot.percent);
    if (snapshot.dimPage)
        dimSleepPage(backdrop, snapshot.dark);
    {
        QPainter ground(&backdrop);
        paintGround(ground, backdrop.size(), snapshot);
    }
    if (underlay)
        *underlay = sleepStatusUnderlay(backdrop, sleepStatusAreas(backdrop.size(), snapshot));
    const QImage cover = snapshot.design == Design::Widgets && !snapshot.coverArea.isEmpty()
            ? backdrop.copy(snapshot.coverArea)
            : QImage();
    QPainter painter(&backdrop);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    paintDesign(painter, backdrop.size(), snapshot, cover, batteryPercent, status);
    painter.end();
    return backdrop;
}

QVector<QRect> sleepStatusAreas(QSize size, const sleep::Snapshot &source)
{
    const auto snapshot = displaySnapshot(source);
    if (size.isEmpty())
        return {};
    if (sleep::tiled(snapshot.design)) {
        const auto area = sleepScreenCardRect(size, !snapshot.remaining.isEmpty(), snapshot.tile);
        return area.isEmpty() ? QVector<QRect>() : QVector<QRect>{ area };
    }
    const Frame frame(size);
    const QRect screen(QPoint(), size);
    QVector<QRect> areas;
    switch (snapshot.design) {
    case Design::Seal:
    case Design::Medallion:
        areas = { sealBox(frame, snapshot.design == Design::Medallion)
                          .adjusted(-frame.u, -frame.u, frame.u, frame.u)
                          .toAlignedRect() };
        break;
    case Design::Pill:
        areas = pillAreas(frame, snapshot, size);
        break;
    case Design::Caption:
        areas = captionAreas(frame, snapshot, size);
        break;
    case Design::Widgets:
        areas = widgetsAreas(frame, snapshot);
        break;
    case Design::Night:
        areas = nightAreas(frame, size);
        break;
    case Design::Ring:
        areas = ringAreas(frame, size);
        break;
    case Design::Lines:
        areas = linesAreas(frame, snapshot, size);
        break;
    case Design::Letter:
        areas = letterAreas(frame, size);
        break;
    default:
        break;
    }
    for (auto &area : areas)
        area = area.intersected(screen);
    return areas;
}

QVector<QImage> sleepStatusUnderlay(const QImage &backdrop, const QVector<QRect> &areas)
{
    QVector<QImage> result;
    for (const auto &area : areas)
        result.append(backdrop.copy(area));
    return result;
}

QRect repaintSleepStatus(QImage &screen, const QVector<QImage> &underlay,
                         const sleep::Snapshot &source, int batteryPercent, SleepStatus status)
{
    const auto snapshot = displaySnapshot(source);
    const auto areas = sleepStatusAreas(screen.size(), snapshot);
    if (areas.isEmpty())
        return {};
    if (sleep::tiled(snapshot.design)) {
        screen = sleepScreenImage(std::move(screen), snapshot.progress, snapshot.remaining,
                                  snapshot.dark, batteryPercent, status, false, snapshot.tile,
                                  snapshot.percent);
        return areas.first();
    }
    // Each area is drawn into its own image, starting from the saved backdrop
    // and shifted by whole pixels, then copied back. The image edges do the
    // clipping: a painter clip changes how Qt smooths some shapes, and the
    // result must match a fresh screen pixel for pixel.
    QRect bounds;
    QPainter screenPainter(&screen);
    for (int index = 0; index < areas.size(); ++index) {
        const auto &area = areas[index];
        QImage patch = index < underlay.size() && underlay[index].size() == area.size()
                ? underlay[index].copy()
                : screen.copy(area);
        QPainter painter(&patch);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(-area.topLeft());
        paintDesign(painter, screen.size(), snapshot, {}, batteryPercent, status);
        painter.end();
        screenPainter.drawImage(area.topLeft(), patch);
        bounds |= area;
    }
    screenPainter.end();
    return bounds;
}

void paintSleepPreview(QPainter &painter, const QRectF &target, QSize size, const QImage &cover,
                       const QImage &page, const QImage &overlay, const sleep::Snapshot &source)
{
    const auto snapshot = displaySnapshot(source);
    if (size.isEmpty() || target.isEmpty())
        return;
    painter.save();
    painter.setClipRect(target);
    painter.translate(target.topLeft());
    painter.scale(target.width() / size.width(), target.height() / size.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRect screen(QPoint(), size);
    const auto colors = palette(snapshot.dark);
    sleep::Snapshot shot = snapshot;
    switch (sleep::backdrop(snapshot.design)) {
    case sleep::Mode::Cover:
        painter.fillRect(screen, Qt::black);
        if (!cover.isNull()) {
            shot.coverArea = sleep::coverArea(cover.size(), size);
            painter.drawImage(shot.coverArea, cover);
        }
        break;
    case sleep::Mode::Page:
    case sleep::Mode::Overlay:
        if (!page.isNull())
            painter.drawImage(screen, page);
        else
            painter.fillRect(screen, colors.paper);
        painter.fillRect(screen, ui::themeColors(snapshot.dark).sleepDim);
        if (snapshot.design == Design::Overlay && !overlay.isNull()) {
            const QSize fitted = overlay.size().scaled(size, Qt::KeepAspectRatio);
            painter.drawImage(QRect(QPoint((size.width() - fitted.width()) / 2,
                                           (size.height() - fitted.height()) / 2),
                                    fitted),
                              overlay);
        }
        break;
    case sleep::Mode::Plain:
    case sleep::Mode::Stock:
        painter.fillRect(screen, colors.paper);
        break;
    }
    if (sleep::tiled(snapshot.design)) {
        paintSleepTile(painter, size, snapshot.progress, snapshot.remaining, snapshot.dark, -1,
                       SleepStatus::Sleeping, snapshot.tile, snapshot.percent);
    } else {
        QImage tileCover;
        if (snapshot.design == Design::Widgets)
            tileCover = cover;
        paintGround(painter, size, shot);
        paintDesign(painter, size, shot, tileCover, -1, SleepStatus::Sleeping);
    }
    painter.restore();
}

} // namespace kobalt::reader
