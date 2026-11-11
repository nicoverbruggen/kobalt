#include "sync_choice.h"
#include "support/strings.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/private_text.h"
#include "ui/style/theme.h"
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace kobalt::reader {
namespace metrics = ui::metrics::syncChoice;

namespace {
QFont labelFont(bool bold = false)
{
    auto font = ui::interfaceFont(ui::metrics::chrome::captionText);
    font.setBold(bold);
    return font;
}

QFont valueFont(bool bold = false)
{
    auto font = ui::interfaceFont(ui::metrics::chrome::descriptionText);
    font.setBold(bold);
    return font;
}

QString counted(int count, const QString &many, const QString &one)
{
    return count == 1 ? one : many.arg(QLocale().toString(count));
}

QString pagesText(int direction, int distance)
{
    return direction > 0 ? counted(distance, strings::get("kosync.pages_ahead"),
                                   strings::get("kosync.pages_ahead_one"))
                         : counted(distance, strings::get("kosync.pages_back"),
                                   strings::get("kosync.pages_back_one"));
}

// A filled triangle whose tip is at `tip`, pointing along (dx, dy).
void arrowHead(QPainter &painter, QPointF tip, qreal dx, qreal dy, qreal length, qreal width,
               const QColor &ink)
{
    const QPointF base(tip.x() - dx * length, tip.y() - dy * length);
    const QPointF side(-dy * width / 2, dx * width / 2);
    QPainterPath path;
    path.moveTo(tip);
    path.lineTo(base + side);
    path.lineTo(base - side);
    path.closeSubpath();
    painter.fillPath(path, ink);
}

// One line of text centered on `center`, kept inside [0, width].
QRect centeredText(const QString &text, const QFont &font, int center, int top, int width)
{
    const int textWidth = ui::interfaceTextWidth(text, font);
    const int left = qBound(0, center - textWidth / 2, qMax(0, width - textWidth));
    return QRect(left, top, textWidth, ui::interfaceLineHeight(font));
}

void drawText(QPainter &painter, const QRect &area, const QString &text, const QFont &font,
              const QColor &ink, Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignTop)
{
    painter.setFont(font);
    painter.setPen(ink);
    ui::drawInterfaceText(painter, area, text, alignment);
}
} // namespace

bool SyncJump::sameChapter() const
{
    // Equal titles alone do not make one chapter: books without contents
    // have no titles, and different chapters can share one.
    return drawable() && !fromChapter.trimmed().isEmpty() && fromChapter == toChapter
            && fromParent == toParent && startsCrossed() == 0;
}

int SyncJump::startsCrossed() const
{
    if (!drawable())
        return 0;
    const int low = qMin(here, synced), high = qMax(here, synced);
    int count = 0;
    for (const auto &start : starts)
        count += start.page > low && start.page <= high;
    return count;
}

QVector<SyncJump::Start> syncChapterStarts(const QVector<ChapterLink> &links)
{
    QVector<SyncJump::Start> result;
    const bool nested = std::any_of(links.cbegin(), links.cend(),
                                    [](const ChapterLink &link) { return link.depth > 0; });
    for (int i = 0; i < links.size(); ++i) {
        const auto &link = links[i];
        if (link.page < 0)
            continue;
        QString parent;
        for (int j = i - 1; j >= 0 && link.depth > 0; --j)
            if (links[j].depth < link.depth) {
                parent = links[j].title.simplified();
                break;
            }
        SyncJump::Start start{ link.page, link.title.simplified(), parent,
                               nested && link.depth == 0 };
        // A Book heading and its first chapter often share a page. The
        // chapter names the start, and the start still marks the Book.
        if (!result.isEmpty() && result.last().page == link.page) {
            start.top = start.top || result.last().top;
            result.last() = start;
        } else if (result.isEmpty() || result.last().page < link.page) {
            result.append(start);
        }
    }
    return result;
}

QString syncChapterParent(const QVector<ChapterLink> &links, const QString &title, int page)
{
    const auto wanted = title.simplified();
    for (int i = links.size() - 1; i >= 0; --i) {
        if (links[i].page > page || links[i].title.simplified() != wanted)
            continue;
        for (int j = i - 1; j >= 0 && links[i].depth > 0; --j)
            if (links[j].depth < links[i].depth)
                return links[j].title.simplified();
        return {};
    }
    return {};
}

QString syncTimeText(const QDateTime &time, const QDateTime &now)
{
    if (!time.isValid())
        return strings::get("kosync.time_unavailable");
    const auto clock = QLocale().toString(time.time(), QLocale::ShortFormat);
    const auto days = time.date().daysTo(now.date());
    if (days == 0)
        return strings::get("kosync.today_at").arg(clock);
    if (days == 1)
        return strings::get("kosync.yesterday_at").arg(clock);
    return QLocale().toString(time, QLocale::ShortFormat);
}

QString syncPercentText(double percent)
{
    return percent < 0 ? QString()
                       : strings::get("kosync.percent").arg(QLocale().toString(percent, 'f', 1));
}

SyncChoiceView::SyncChoiceView(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrKosyncChoiceView");
    // The height follows the width, because the page boxes scale with it. A
    // fixed vertical policy would cap the height at a hint for an older width.
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void SyncChoiceView::setJump(const SyncJump &jump)
{
    value = jump;
    updateGeometry();
    update();
}

SyncChoiceView::Scale SyncChoiceView::scale() const
{
    const int distance = std::abs(value.synced - value.here);
    if (distance <= metrics::pageJump)
        return Scale::Pages;
    // From a quarter of the book, the whole book gives the arrow enough length.
    return 4 * distance >= value.pages ? Scale::Book : Scale::Track;
}

QString SyncChoiceView::jumpLabel() const
{
    const auto pages = pagesText(value.direction(), std::abs(value.synced - value.here));
    const int crossed = value.startsCrossed();
    return crossed ? strings::get("kosync.jump_label")
                             .arg(pages,
                                  counted(crossed, strings::get("kosync.chapters_crossed"),
                                          strings::get("kosync.chapters_crossed_one")))
                   : pages;
}

// The pages part is bold. The chapter count after it is grey.
void SyncChoiceView::paintJumpLabel(QPainter &painter, int center, int top, int width) const
{
    const auto label = jumpLabel();
    auto pages = pagesText(value.direction(), std::abs(value.synced - value.here));
    const auto bold = labelFont(true), normal = labelFont();
    // A label that does not start with the pages, as in a translation that
    // reorders it, is drawn whole in bold.
    const bool split = label.startsWith(pages);
    const auto rest = split ? label.mid(pages.size()) : QString();
    if (!split)
        pages = label;
    const int pagesWidth = ui::interfaceTextWidth(pages, bold);
    const int total = pagesWidth + ui::interfaceTextWidth(rest, normal);
    const int left = qBound(0, center - total / 2, qMax(0, width - total));
    const int height = ui::interfaceLineHeight(bold);
    drawText(painter, QRect(left, top, pagesWidth, height), pages, bold,
             palette().color(QPalette::WindowText));
    if (!rest.isEmpty())
        drawText(painter, QRect(left + pagesWidth, top, total - pagesWidth, height), rest, normal,
                 ui::noteColor(this));
}

int SyncChoiceView::pagesGraph(int width, QPainter *painter) const
{
    const int last = value.pages - 1;
    int low = qMin(value.here, value.synced) - 2, high = qMax(value.here, value.synced) + 2;
    while (high - low + 1 < 9) {
        --low;
        ++high;
    }
    if (low < 0) {
        high -= low;
        low = 0;
    }
    if (high > last) {
        low = qMax(0, low - (high - last));
        high = last;
    }
    const int count = high - low + 1;
    const qreal slot = qreal(width) / count;
    const int pageWidth = qMax(4, qMin<int>(metrics::pageWidth, int(slot) - metrics::pageGap));
    const int pageHeight = qRound(pageWidth * qreal(metrics::pageHeight) / metrics::pageWidth);
    const int line = ui::interfaceLineHeight(labelFont(true));
    const int bar = line + metrics::labelGap + metrics::jumpLine / 2;
    const int top = bar + metrics::jumpLift;
    const int numbers = top + pageHeight + metrics::labelGap;
    QVector<SyncJump::Start> shown;
    for (const auto &start : value.starts)
        if (start.page > low && start.page <= high)
            shown.append(start);
    // The dashed lines mark chapter starts; the jump label counts them and the
    // To row names the chapter.
    const int height = numbers + line;
    if (!painter)
        return height;

    const auto ink = palette().color(QPalette::WindowText);
    const auto paper = palette().color(QPalette::Window);
    const auto grey = ui::noteColor(this);
    const auto center = [&](int page) { return qRound(slot * (page - low + 0.5)); };
    const int from = qMin(value.here, value.synced), to = qMax(value.here, value.synced);
    for (int page = low; page <= high; ++page) {
        const QRect box(center(page) - pageWidth / 2, top, pageWidth, pageHeight);
        if (page == value.synced) {
            painter->fillRect(box, ink);
            continue;
        }
        painter->fillRect(box, page > from && page < to ? ui::sectionColor(this) : paper);
        if (page == value.here)
            ui::paintEdges(*painter, box,
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           ink, metrics::currentPageLine);
        else
            ui::paintEdges(*painter, box,
                           ui::Edges(Qt::TopEdge) | Qt::BottomEdge | Qt::LeftEdge | Qt::RightEdge,
                           grey, metrics::pageLine);
    }
    // A dashed line between the pages where a chapter starts.
    for (const auto &start : shown) {
        const int x = (center(start.page - 1) + center(start.page)) / 2 - metrics::startLine / 2;
        for (int y = top - metrics::startReach; y < top + pageHeight + metrics::startReach;
             y += 2 * metrics::startDash)
            painter->fillRect(
                    x, y, metrics::startLine,
                    qMin<int>(metrics::startDash, top + pageHeight + metrics::startReach - y), ink);
    }
    // A squared arrow: up from the current page, across, and down onto the synced page.
    const int xh = center(value.here), xs = center(value.synced);
    const int half = metrics::jumpLine / 2;
    const int gap = metrics::pageLine * 2;
    const int head = metrics::jumpHead;
    painter->fillRect(QRect(xh - half, bar - half, metrics::jumpLine, top - gap - bar + half), ink);
    painter->fillRect(QRect(qMin(xh, xs) - half, bar - half, std::abs(xs - xh) + metrics::jumpLine,
                            metrics::jumpLine),
                      ink);
    painter->fillRect(
            QRect(xs - half, bar - half, metrics::jumpLine, top - gap - head - bar + half), ink);
    painter->setRenderHint(QPainter::Antialiasing, true);
    arrowHead(*painter, QPointF(xs + (metrics::jumpLine % 2 ? 0.5 : 0), top - gap), 0, 1, head,
              head * 1.1, ink);
    painter->setRenderHint(QPainter::Antialiasing, false);
    paintJumpLabel(*painter, (xh + xs) / 2, 0, width);
    // Page numbers count from one. Neighbouring pages push their numbers apart.
    const auto number = [](int page) {
        return strings::get("kosync.page_number").arg(QLocale().toString(page + 1));
    };
    const auto hereText = number(value.here), syncedText = number(value.synced);
    auto hereArea = centeredText(hereText, labelFont(), xh, numbers, width);
    auto syncedArea = centeredText(syncedText, labelFont(true), xs, numbers, width);
    if (hereArea.intersects(syncedArea.adjusted(-metrics::labelGap, 0, metrics::labelGap, 0))) {
        const int middle = (xh + xs) / 2, space = metrics::labelGap / 2;
        auto &left = xh < xs ? hereArea : syncedArea;
        auto &right = xh < xs ? syncedArea : hereArea;
        left.moveRight(middle - space);
        right.moveLeft(middle + space);
        // Keep the pair inside the graph, as at the last page of a long book.
        const int shift = left.left() < 0 ? -left.left() : qMin(0, width - 1 - right.right());
        left.translate(shift, 0);
        right.translate(shift, 0);
    }
    drawText(*painter, hereArea, hereText, labelFont(), grey);
    drawText(*painter, syncedArea, syncedText, labelFont(true), ink);
    return height;
}

int SyncChoiceView::trackGraph(int width, QPainter *painter) const
{
    const int line = ui::interfaceLineHeight(labelFont(true));
    const int y = line + metrics::labelGap + metrics::tickReach;
    const int labels = y + metrics::tickReach + metrics::labelGap;
    const int height = labels + line;
    if (!painter)
        return height;

    const auto ink = palette().color(QPalette::WindowText);
    const auto paper = palette().color(QPalette::Window);
    const auto grey = ui::noteColor(this);
    const int last = value.pages - 1;
    int low = 0, high = last;
    if (scale() == Scale::Track) {
        const int distance = std::abs(value.synced - value.here);
        const int pad = qMax(1, qRound(distance * 0.4));
        low = qMax(0, qMin(value.here, value.synced) - pad);
        high = qMin(last, qMax(value.here, value.synced) + pad);
    }
    const bool openLeft = low > 0, openRight = high < last;
    const int margin = metrics::here + metrics::hereLine;
    const int x0 = margin + (openLeft ? int(metrics::trackEnd) : 0);
    const int x1 = width - margin - (openRight ? int(metrics::trackEnd) : 0);
    const auto x = [&](qreal page) {
        return qRound(x0 + (page - low) / qMax<qreal>(1, high - low) * (x1 - x0));
    };
    // Chapter starts as ticks. A tick too close to the one before or to an
    // end is left out, so a few crowded starts lose their ticks. When the
    // starts are too dense along the whole track, the ticks no longer count
    // anything: the track shows the top-level entries of nested contents
    // instead, or no ticks.
    const auto spaced = [&](bool topOnly, QVector<int> *result) {
        int candidates = 0, previous = x0;
        for (const auto &start : value.starts) {
            if (start.page <= low || start.page > high || (topOnly && !start.top))
                continue;
            ++candidates;
            const int at = x(start.page - 0.5);
            if (at - previous >= metrics::minimumTickGap && x1 - at >= metrics::minimumTickGap) {
                result->append(at);
                previous = at;
            }
        }
        return candidates * metrics::minimumTickGap <= x1 - x0;
    };
    QVector<int> ticks;
    if (!spaced(false, &ticks)) {
        ticks.clear();
        if (!value.nested || !spaced(true, &ticks))
            ticks.clear();
    }
    const int half = metrics::track / 2;
    painter->fillRect(QRect(x0, y - half, x1 - x0, metrics::track), grey);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(grey);
    for (int i = 0; i < 3; ++i) {
        const int step = metrics::dotPitch * (i + 1);
        if (openLeft)
            painter->drawEllipse(QPointF(x0 - step, y), metrics::dot, metrics::dot);
        if (openRight)
            painter->drawEllipse(QPointF(x1 + step, y), metrics::dot, metrics::dot);
    }
    painter->setRenderHint(QPainter::Antialiasing, false);
    for (const int at : ticks)
        painter->fillRect(QRect(at - metrics::tick / 2, y - metrics::tickReach, metrics::tick,
                                2 * metrics::tickReach),
                          grey);
    // The arrow runs from the edge of the circle to the synced position.
    const int xh = x(value.here), xs = x(value.synced), direction = value.direction();
    const int head = metrics::arrowHead;
    const int start = xh + direction * (metrics::here + metrics::hereLine / 2);
    const int end = xs - direction * head;
    const int arrowHalf = metrics::arrow / 2;
    if ((end - start) * direction > 0)
        painter->fillRect(
                QRect(qMin(start, end), y - arrowHalf, std::abs(end - start), metrics::arrow), ink);
    painter->setRenderHint(QPainter::Antialiasing, true);
    arrowHead(*painter, QPointF(xs, y + (metrics::arrow % 2 ? 0.5 : 0)), direction, 0, head,
              head * 0.95, ink);
    QPen ring(ink, metrics::hereLine);
    painter->setPen(ring);
    painter->setBrush(paper);
    painter->drawEllipse(QPointF(xh, y), metrics::here, metrics::here);
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setBrush(Qt::NoBrush);
    paintJumpLabel(*painter, (xh + xs) / 2, 0, width);
    const auto hereText = strings::get("kosync.here"),
               syncedText = strings::get("kosync.synced_label");
    drawText(*painter, centeredText(hereText, labelFont(), xh, labels, width), hereText,
             labelFont(), grey);
    drawText(*painter, centeredText(syncedText, labelFont(true), xs, labels, width), syncedText,
             labelFont(true), ink);
    return height;
}

int SyncChoiceView::rows(int width, int top, QPainter *painter) const
{
    const auto caption = ui::captionFont();
    const auto normal = valueFont(), bold = valueFont(true);
    const int valueLine = ui::interfaceLineHeight(normal);
    const int captionLine = ui::interfaceLineHeight(caption);
    const int parentLine = ui::interfaceLineHeight(labelFont());
    const QStringList captions{ strings::get("kosync.from"), strings::get("kosync.to"),
                                strings::get("kosync.device"), strings::get("kosync.synced") };
    int captionWidth = 0;
    for (const auto &text : captions)
        captionWidth = qMax(captionWidth, ui::interfaceTextWidth(text, caption));
    const int valueLeft = captionWidth + metrics::captionGap;
    const int percentWidth = ui::interfaceTextWidth(syncPercentText(100), bold);
    const int chapterWidth = qMax(1, width - valueLeft - percentWidth - metrics::captionGap);
    const int wideWidth = qMax(1, width - valueLeft);
    const auto ink = palette().color(QPalette::WindowText);
    const auto grey = ui::noteColor(this);
    // Captions are smaller than values. They share the baseline of the
    // first value line.
    const int captionShift = ui::interfaceLineAscent(normal) - ui::interfaceLineAscent(caption);
    int y = top;
    const auto chapterRow = [&](const QString &name, QString title, const QString &parent,
                                double percent, bool destination, bool same) {
        if (title.trimmed().isEmpty())
            title = strings::get("kosync.chapter_unavailable");
        const auto font = destination ? bold : normal;
        if (painter) {
            drawText(*painter, QRect(0, y + captionShift, captionWidth, captionLine), name, caption,
                     grey);
            const auto shown = same
                    ? strings::get("kosync.same_chapter")
                    : ui::elideInterfaceText(title.simplified(), font, chapterWidth);
            drawText(*painter, QRect(valueLeft, y, chapterWidth, valueLine), shown, font,
                     same ? grey : ink);
            drawText(*painter, QRect(width - percentWidth, y, percentWidth, valueLine),
                     syncPercentText(percent), font, destination ? ink : grey,
                     Qt::AlignRight | Qt::AlignTop);
            if (!same && !parent.isEmpty())
                drawText(*painter, QRect(valueLeft, y + valueLine, chapterWidth, parentLine),
                         ui::elideInterfaceText(parent, labelFont(), chapterWidth), labelFont(),
                         grey);
        }
        y += valueLine + (!same && !parent.isEmpty() ? parentLine : 0) + metrics::rowGap;
    };
    const bool same = value.sameChapter();
    chapterRow(captions[0], value.fromChapter, value.fromParent, value.fromPercent, false, same);
    chapterRow(captions[1], value.toChapter, value.toParent, value.toPercent, true, false);
    // A dotted rule separates the two positions from the device and time of the sync.
    y += metrics::ruleGap - metrics::rowGap;
    if (painter) {
        const int dot = ui::metrics::control::border;
        for (int x = 0; x < width; x += 3 * dot)
            painter->fillRect(x, y, dot, dot, grey);
    }
    y += ui::metrics::control::border + metrics::ruleGap;
    const auto device = value.device;
    const int deviceHeight = ui::wrappedInterfaceTextHeight(device, normal, wideWidth, 2);
    if (painter) {
        drawText(*painter, QRect(0, y + captionShift, captionWidth, captionLine), captions[2],
                 caption, grey);
        painter->setFont(normal);
        painter->setPen(ink);
        ui::drawWrappedInterfaceText(*painter, QRect(valueLeft, y, wideWidth, deviceHeight), device,
                                     Qt::AlignLeft | Qt::AlignTop, 2);
    }
    y += qMax(deviceHeight, valueLine) + metrics::rowGap;
    if (painter) {
        drawText(*painter, QRect(0, y + captionShift, captionWidth, captionLine), captions[3],
                 caption, grey);
        drawText(*painter, QRect(valueLeft, y, wideWidth, valueLine),
                 ui::elideInterfaceText(value.time, normal, wideWidth), normal, ink);
    }
    y += valueLine;
    return y - top;
}

int SyncChoiceView::layout(int width, QPainter *painter) const
{
    int graph = 0;
    if (value.drawable()) {
        const int graphWidth = qMin<int>(width, metrics::graphWidth);
        if (painter) {
            painter->save();
            painter->translate((width - graphWidth) / 2, 0);
        }
        graph = scale() == Scale::Pages ? pagesGraph(graphWidth, painter)
                                        : trackGraph(graphWidth, painter);
        if (painter)
            painter->restore();
        graph += metrics::graphGap;
    }
    return graph + rows(width, graph, painter);
}

int SyncChoiceView::heightForWidth(int width) const
{
    return layout(qMax(1, width), nullptr);
}

QSize SyncChoiceView::sizeHint() const
{
    const int width = qMax(1, this->width());
    return QSize(width, heightForWidth(width));
}

void SyncChoiceView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    layout(width(), &painter);
}
} // namespace kobalt::reader
