#include "style_illustrations.h"
#include "ui/style/private_text.h"
#include "reading/appearance_settings.h"
#include "style_illustration_layouts.h"
#include <QPainter>
#include <QtMath>

namespace kobalt::reader {
namespace {
using namespace illustrations;

// Positions are those of the original fixed sample. Shape the actual text at
// the output size, including the enlarged edge, instead of scaling a bitmap.
QImage drawPage(const Page &page, const QRectF &view, const QSize &size, bool dark)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(dark ? Qt::black : Qt::white);
    QPainter painter(&image);
    const qreal scale = qreal(size.width()) / view.width();
    for (int lineIndex = 0; lineIndex < page.count; ++lineIndex) {
        const auto &line = page.lines[lineIndex];
        const qreal baseline = line.baseline;
        const qreal pixels = fontPixels;
        if (baseline + pixels < view.top() || baseline - pixels > view.bottom())
            continue;
        for (int wordIndex = 0; wordIndex < line.count; ++wordIndex) {
            const auto &word = line.words[wordIndex];
            QFont font(QString::fromLatin1(defaultFontFace));
            font.setPixelSize(qMax(1, qRound(pixels * scale)));
            font.setItalic(word.italic);
            auto text = ui::renderPrivateText(QString::fromUtf8(texts[word.text]), font, 0, {}, {},
                                              ui::PrivateTextEmphasis::FontFace);
            if (text.image.isNull())
                continue;
            QPainter color(&text.image);
            color.setCompositionMode(QPainter::CompositionMode_SourceIn);
            color.fillRect(text.image.rect(), dark ? Qt::white : Qt::black);
            color.end();
            const QPointF origin((word.x - view.left()) * scale, (baseline - view.top()) * scale);
            painter.drawImage(origin + text.origin, text.image);
        }
    }
    return image;
}

QVector<QRect> spareSpace(Bands bands, Bands baseline)
{
    QVector<QRect> result;
    if (bands.count == 0 || bands.count != baseline.count)
        return result;
    const auto top = [](const Band &band) { return int(band.top); };
    const auto add = [&](int start, int height) {
        if (height > 1)
            result.append(QRect(48, start, 976, height));
    };
    add(top(baseline.data[0]), top(bands.data[0]) - top(baseline.data[0]));
    for (int i = 1; i < bands.count; ++i) {
        const int added = top(bands.data[i]) - top(bands.data[i - 1]) - top(baseline.data[i])
                + top(baseline.data[i - 1]);
        add(top(bands.data[i]) - added, added);
    }
    const int pitch = baseline.count > 1
            ? (top(baseline.data[baseline.count - 1]) - top(baseline.data[0]))
                    / (baseline.count - 1)
            : baseline.data[0].bottom - top(baseline.data[0]);
    const int end = top(bands.data[bands.count - 1]) + pitch;
    add(end, 1412 - end);
    return result;
}
} // namespace

StyleSample drawStyleIllustration(const StyleSampleRequest &request)
{
    StyleSample result;
    if (request.thumbnail.isEmpty() || request.page.isEmpty())
        return result;
    auto settings = request.settings;
    // The varied option does not select the sample; the other two settings do.
    settings[request.key] = request.key == "widows_orphans" ? QStringLiteral("publisher")
            : request.key == "vertical_justification"       ? QStringLiteral("off")
                                                            : QStringLiteral("all");
    const QString key = request.key + "/" + settings.value("widows_orphans").toString("publisher")
            + "/" + settings.value("vertical_justification").toString("off") + "/"
            + settings.value("vertical_spacing_scope").toString("all");
    const Sample *sample = nullptr;
    for (const auto &candidate : samples) {
        if (key == QLatin1String(candidate.key)) {
            sample = &candidate;
            break;
        }
    }
    if (!sample || sample->count != request.values.size())
        return result;
    const bool dark = settings.value("dark_mode").toBool();
    result.page = sample->page;
    result.differs = sample->differs;
    const qreal density = qreal(request.page.width()) / 1072;
    const bool detail = !request.detail.isEmpty();
    QSize thumbnail(1072, 1448);
    thumbnail.scale(request.thumbnail, Qt::KeepAspectRatio);
    const qreal thumbnailScale = qreal(request.thumbnail.width()) / 1072;
    const QRect area(48, 36, 976, 1376);
    if (detail)
        result.readingArea = QRectF(QPointF(area.topLeft()) * thumbnailScale,
                                    QSizeF(area.size()) * thumbnailScale);
    QRect view(0, 1460 - qRound(request.detail.height() / density), 1072,
               qRound(request.detail.height() / density));
    view = view.intersected(QRect(0, 0, 1072, 1448));
    const auto baseline = sample->options[0].bands;
    if (detail) {
        for (int line = 1; line < baseline.count; ++line) {
            const int top = baseline.data[line].top;
            if (top >= view.top()) {
                view.setTop((baseline.data[line - 1].bottom + top) / 2);
                break;
            }
        }
        result.detailArea = QRect(qRound((area.x() - view.x()) * density),
                                  qRound((area.y() - view.y()) * density),
                                  qRound(area.width() * density), qRound(area.height() * density));
    }
    for (int option = 0; option < sample->count; ++option) {
        QVector<QImage> spread;
        const auto &choice = sample->options[option];
        for (const auto index : { choice.left, choice.right })
            spread.append(drawPage(pages[index], QRectF(0, 0, 1072, 1448), thumbnail, dark));
        result.spreads.append(spread);
        if (detail) {
            result.details.append(drawPage(
                    pages[choice.left], view,
                    QSize(qRound(view.width() * density), qRound(view.height() * density)), dark));
            QVector<QRect> spare;
            for (const auto &rect : spareSpace(choice.bands, baseline)) {
                const auto clipped = rect.intersected(view).translated(-view.topLeft());
                spare.append(QRect(qRound(clipped.x() * density), qRound(clipped.y() * density),
                                   qRound(clipped.width() * density),
                                   qRound(clipped.height() * density)));
            }
            result.spare.append(spare);
        }
    }
    return result;
}
} // namespace kobalt::reader
