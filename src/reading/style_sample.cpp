#include "style_sample.h"
#include "engine/crengine/engine_worker.h"
#include "engine/crengine/engine_activity.h"
#include "appearance_settings.h"
#include "reading_density.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QPointer>
#include <QPainter>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <atomic>
#include <memory>

namespace kobalt {
namespace {
// The XHTML files are the editable source for Styling previews.
// Keep their top-level paragraphs and headings as blocks so page-break
// previews can try another starting paragraph when the first break is unchanged.
struct SampleBlock
{
    QByteArray html;
    QString text;
    bool paragraph = false;
};

struct SampleDocument
{
    QByteArray prefix;
    QVector<SampleBlock> blocks;
};

SampleDocument sampleDocument(bool words, bool hanging)
{
    QFile file(hanging         ? ":/kobalt/samples/hanging-punctuation.html"
                       : words ? ":/kobalt/samples/words-and-letters.html"
                               : ":/kobalt/samples/layout.html");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto html = file.readAll();
    const int body = html.indexOf("<body>");
    if (body < 0)
        return {};
    SampleDocument document;
    document.prefix = html.left(body + 6);
    QXmlStreamReader reader(html);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QLatin1String("body"))
            continue;
        while (reader.readNextStartElement()) {
            SampleBlock block;
            block.paragraph = reader.name() == QLatin1String("p");
            QXmlStreamWriter writer(&block.html);
            writer.writeCurrentToken(reader);
            int depth = 1;
            while (depth && !reader.atEnd()) {
                reader.readNext();
                writer.writeCurrentToken(reader);
                if (reader.isStartElement())
                    ++depth;
                if (reader.isEndElement())
                    --depth;
                if (reader.isCharacters())
                    block.text += reader.text().toString();
            }
            block.text = block.text.simplified();
            document.blocks.append(block);
        }
    }
    return reader.hasError() ? SampleDocument{} : document;
}

constexpr int searchedPages = 10;
constexpr int sampleVariants = 6;
constexpr int paragraphSampleCount = 3;

std::shared_ptr<ResourceCache> sampleBook(const SampleDocument &document, int variant,
                                          QMap<QString, qint64> *sizes, bool paragraphs)
{
    QByteArray body;
    int count = 0;
    for (int index = 0; index < document.blocks.size(); ++index) {
        const auto &block = document.blocks[(index + variant) % document.blocks.size()];
        body += block.html;
        if (paragraphs && block.paragraph && ++count == paragraphSampleCount)
            break;
    }
    const QMap<QString, QByteArray> files{
        { "mimetype", "application/epub+zip" },
        { "META-INF/container.xml",
          "<container xmlns='urn:oasis:names:tc:opendocument:xmlns:container' version='1.0'><rootfiles>"
          "<rootfile full-path='book.opf' media-type='application/oebps-package+xml'/></rootfiles></container>" },
        { "book.opf",
          "<package xmlns='http://www.idpf.org/2007/opf' version='2.0'><metadata><title>Sample</title>"
          "<language xmlns='http://purl.org/dc/elements/1.1/'>en</language></metadata><manifest>"
          "<item id='c' href='c.xhtml' media-type='application/xhtml+xml'/></manifest>"
          "<spine><itemref idref='c'/></spine></package>" },
        { "c.xhtml", document.prefix + body + "</body></html>" }
    };
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        (*sizes)[it.key()] = it.value().size();
    return std::make_shared<ResourceCache>(
            files.keys(),
            [files](const QString &name, const ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
}

bool cancelled(const StyleSampleRequest &request)
{
    engineActivity().checkpoint();
    return request.cancelled && request.cancelled->load();
}

// Applying an option is the costly step, so it also checks for cancellation.
bool apply(EngineDocument &engine, const StyleSampleRequest &request, const QJsonValue &value)
{
    if (cancelled(request))
        return false;
    // Qt 5.2 has no initializer-list constructor for JSON objects.
    QJsonObject settings;
    settings.insert(request.key, value);
    return engine.applyAppearance(settings);
}

QStringList pageStarts(EngineDocument &engine)
{
    QStringList starts;
    for (int page = 0; page < qMin(engine.pages(), searchedPages + 1); ++page)
        starts.append(engine.pageStart(page));
    return starts;
}

// The first page whose start differs between options, so the spread before it
// shows the changed break. Returns -1 when every option breaks the same way.
int firstChangedBreak(const QVector<QStringList> &starts)
{
    int limit = starts.first().size() - 1;
    for (const auto &option : starts)
        limit = qMin(limit, option.size() - 1);
    // Leave a following page beyond the spread so its second page is full.
    for (int page = 1; page < limit; ++page)
        for (const auto &option : starts)
            if (option.value(page) != starts.first().value(page))
                return page;
    return -1;
}

// The first page that the options draw differently, or -1. Page spacing moves
// lines within a page without changing where pages start.
int firstChangedPage(EngineDocument &engine, const StyleSampleRequest &request, int completeSpreads)
{
    for (int page = 0; page < qMin(completeSpreads, searchedPages); ++page) {
        QImage first;
        for (const auto &value : request.values) {
            if (!apply(engine, request, value))
                return -1;
            const auto image = engine.image(page);
            if (first.isNull())
                first = image;
            else if (image != first)
                return page;
        }
    }
    return -1;
}

// The height that ends a crop halfway through the gap below its last whole
// line, found from the rendered pixels. Rows that differ from the paper
// colour hold ink.
int wholeLineHeight(const QImage &crop)
{
    const auto paper = qGray(crop.pixel(0, 0));
    const auto inked = [&crop, paper](int y) {
        for (int x = 0; x < crop.width(); ++x)
            if (qAbs(qGray(crop.pixel(x, y)) - paper) > 64)
                return true;
        return false;
    };
    int y = crop.height() - 1;
    // Skip a line cut by the bottom edge, then find the last line's bottom.
    while (y > 0 && inked(y))
        --y;
    const int gapBottom = y;
    while (y > 0 && !inked(y))
        --y;
    return y > 0 ? y + 1 + (gapBottom - y) / 2 : crop.height();
}

// The vertical extent of each line of ink in `area`.
QVector<QPair<int, int>> lineBands(const QImage &page, const QRect &area)
{
    const auto paper = qGray(page.pixel(0, 0));
    QVector<QPair<int, int>> bands;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        bool inked = false;
        for (int x = area.left(); x <= area.right() && !inked; ++x)
            inked = qAbs(qGray(page.pixel(x, y)) - paper) > 64;
        if (!inked)
            continue;
        // Accents and dots a pixel or two above their letters join their line.
        if (!bands.isEmpty() && y - bands.last().second <= 2)
            bands.last().second = y;
        else
            bands.append({ y, y });
    }
    return bands;
}

// Height that `bands` leave empty compared with `baseline`, the same lines
// drawn by the first option: above the first line, between lines and below
// the last line down to the end of the reading area.
QVector<QRect> spareSpace(const QVector<QPair<int, int>> &bands,
                          const QVector<QPair<int, int>> &baseline, const QRect &area)
{
    QVector<QRect> spare;
    if (bands.isEmpty() || bands.size() != baseline.size())
        return spare;
    const auto band = [&area](int top, int bottom) {
        return QRect(area.left(), top, area.width(), bottom - top);
    };
    if (bands.first().first - baseline.first().first > 1)
        spare.append(band(baseline.first().first, bands.first().first));
    for (int index = 1; index < bands.size(); ++index) {
        const int added = (bands[index].first - bands[index - 1].first)
                - (baseline[index].first - baseline[index - 1].first);
        if (added > 1)
            spare.append(band(bands[index].first - added, bands[index].first));
    }
    // A full line's pitch below the last line is still text height.
    const int pitch = baseline.size() > 1
            ? (baseline.last().first - baseline.first().first) / (baseline.size() - 1)
            : baseline.last().second - baseline.last().first;
    const int end = bands.last().first + pitch;
    if (area.bottom() + 1 - end > 1)
        spare.append(band(end, area.bottom() + 1));
    return spare;
}

QVector<QRect> paragraphGaps(EngineDocument &engine, const QRect &area,
                             const QStringList &paragraphs)
{
    const auto line = [&engine](const QString &query, bool last) {
        const auto hits = engine.searchPage(query, 0).hits;
        if (hits.isEmpty())
            return QRect();
        const auto rects = engine.rangeRects(hits.first().start, hits.first().end);
        QRect result;
        for (const auto &rect : rects)
            if (result.isEmpty() || (last ? rect.top() > result.top() : rect.top() < result.top()))
                result = rect;
        return result;
    };
    QVector<QRect> gaps;
    QRect previous;
    for (const auto &text : paragraphs) {
        const auto first = line(text.left(24), false);
        if (first.isEmpty())
            break;
        if (!previous.isEmpty())
            gaps.append(QRect(area.left(), previous.bottom() + 1, area.width(),
                              qMax(0, first.top() - previous.bottom() - 1)));
        previous = line(text.right(24), true);
        if (previous.isEmpty())
            break;
    }
    return gaps;
}

StyleSample render(const EngineFontInputs &fonts, const StyleSampleRequest &request)
{
    StyleSample result;
    result.cropRatio = request.cropRatio;
    if (request.values.isEmpty() || request.page.isEmpty())
        return result;
    const auto document =
            sampleDocument(request.key == "word_spacing" || request.key == "justification_tracking",
                           request.key == "hanging_punctuation");
    if (document.blocks.isEmpty())
        return result;
    QStringList paragraphTexts;
    for (const auto &block : document.blocks)
        if (block.paragraph && paragraphTexts.size() < paragraphSampleCount)
            paragraphTexts.append(block.text);
    auto settings = request.settings;
    settings[request.key] = request.values.first();
    const bool pages = request.crop.isEmpty();
    std::unique_ptr<EngineDocument> engine;
    int page = 0;
    result.differs = false;
    for (int variant = 0; variant < (pages ? sampleVariants : 1) && !result.differs; ++variant) {
        if (cancelled(request))
            return {};
        engine = std::make_unique<EngineDocument>(fonts);
        QMap<QString, qint64> sizes;
        QString error;
        if (!engine->openResources(
                    sampleBook(document, variant, &sizes, request.key == "paragraph_spacing"),
                    sizes, request.page, settings, &error)
            || engine->pages() < 1)
            return {};
        if (!pages) {
            result.differs = true;
            break;
        }
        QVector<QStringList> starts;
        for (const auto &value : request.values) {
            if (!apply(*engine, request, value))
                return {};
            starts.append(pageStarts(*engine));
        }
        const int changedBreak = firstChangedBreak(starts);
        int completeSpreads = engine->pages() - 2;
        for (const auto &option : starts)
            completeSpreads = qMin(completeSpreads, option.size() - 2);
        const int changedPage =
                changedBreak < 0 ? firstChangedPage(*engine, request, completeSpreads) : -1;
        result.differs = changedBreak > 0 || changedPage >= 0;
        page = changedBreak > 0 ? changedBreak - 1 : qMax(0, changedPage);
    }
    // Keep the selected spread within the sample.
    result.page = qMax(0, qMin(page, engine->pages() - 2));
    const QRect area = request.readingArea.intersected(QRect(QPoint(), request.page));
    const bool detail = !request.detail.isEmpty() && !area.isEmpty();
    // The bottom edge of the reading area, with a little paper beside and below.
    const int side = qMax(0, qMin(area.left(), request.page.width() - 1 - area.right()));
    QRect view = QRect(area.left() - side, area.bottom() + 1 + side - request.detail.height(),
                       area.width() + 2 * side, request.detail.height())
                         .intersected(QRect(QPoint(), request.page));
    if (!request.page.isEmpty()) {
        const qreal scale = qreal(request.thumbnail.width()) / request.page.width();
        result.readingArea = QRectF(area.topLeft() * scale, area.size() * scale);
    }
    QVector<QPair<int, int>> baseline;
    QVector<QRect> paragraphBaseline;
    const bool paragraphs = request.key == "paragraph_spacing";
    const QRect cropArea(request.cropOrigin, request.crop);
    if (request.marginGuides) {
        const int margin = settings.value("margin_pixels").toInt();
        result.cropReadingArea =
                QRect(margin, 0, request.page.width() - 2 * margin, request.page.height())
                        .translated(-request.cropOrigin);
    }
    const int paragraphMargin = settings.value("margin_pixels").toInt();
    const QRect paragraphArea(paragraphMargin, 0, request.page.width() - 2 * paragraphMargin,
                              request.page.height());
    if (paragraphs) {
        if (!apply(*engine, request, 0))
            return {};
        engine->image(0);
        paragraphBaseline = paragraphGaps(*engine, paragraphArea, paragraphTexts);
    }
    for (const auto &value : request.values) {
        if (!apply(*engine, request, value))
            return {};
        QVector<QImage> spread;
        for (int index = result.page; index < qMin(result.page + 2, engine->pages()); ++index) {
            const auto image = engine->image(index);
            if (index == 0 && !pages) {
                result.crops.append(image.copy(QRect(request.cropOrigin, request.crop)));
                if (paragraphs) {
                    const auto gaps = paragraphGaps(*engine, paragraphArea, paragraphTexts);
                    QVector<QRect> extra;
                    if (gaps.size() == paragraphBaseline.size())
                        for (int gap = 0; gap < gaps.size(); ++gap) {
                            const int height = gaps[gap].height() - paragraphBaseline[gap].height();
                            const QRect added(gaps[gap].left(), gaps[gap].bottom() + 1 - height,
                                              gaps[gap].width(), height);
                            if (height > 0 && added.intersects(cropArea))
                                extra.append(added.intersected(cropArea).translated(
                                        -request.cropOrigin));
                        }
                    result.cropGaps.append(extra);
                }
            }
            if (detail && index == page) {
                const auto bands = lineBands(image, area);
                if (baseline.isEmpty()) {
                    baseline = bands;
                    // Start the view in the gap above its first whole line.
                    for (int line = 1; line < bands.size(); ++line)
                        if (bands[line].first >= view.top()) {
                            view.setTop((bands[line - 1].second + bands[line].first) / 2);
                            break;
                        }
                    result.detailArea = area.translated(-view.topLeft());
                }
                QVector<QRect> spare;
                for (const auto &rect : spareSpace(bands, baseline, area))
                    spare.append(rect.translated(-view.topLeft())
                                         .intersected(QRect(QPoint(), view.size())));
                result.details.append(image.copy(view));
                result.spare.append(spare);
            }
            // Scale right away so only one full page is held at a time.
            spread.append(
                    image.scaled(request.thumbnail, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        result.spreads.append(spread);
    }
    if (!pages) {
        // One height for every option keeps the preview steady while choosing.
        if (paragraphs) {
            // Keep the canvas steady while changing the sliders, but clear a
            // partial bottom line instead of showing fragments of its glyphs.
            for (int index = 0; index < result.crops.size(); ++index) {
                auto &crop = result.crops[index];
                const int visible = wholeLineHeight(crop);
                QPainter painter(&crop);
                painter.fillRect(QRect(0, visible, crop.width(), crop.height() - visible),
                                 QColor::fromRgb(crop.pixel(0, 0)));
                QVector<QRect> gaps;
                for (const auto &gap : result.cropGaps[index]) {
                    const auto clipped = gap.intersected(QRect(0, 0, crop.width(), visible));
                    if (!clipped.isEmpty())
                        gaps.append(clipped);
                }
                result.cropGaps[index] = gaps;
            }
        } else {
            const int height = wholeLineHeight(result.crops.first());
            for (auto &crop : result.crops)
                crop = crop.copy(0, 0, crop.width(), height);
        }
        result.differs = request.key == "paragraph_spacing";
        for (const auto &crop : result.crops)
            result.differs = result.differs || crop != result.crops.first();
    }
    return result;
}
} // namespace

QJsonObject styleSampleSettings(QJsonObject state)
{
    for (const auto *key :
         { "position", "dom_version", "imported_native_location", "language", "pages" })
        state.remove(key);
    // The sample shows one column, even when the book uses two in landscape.
    state["columns"] = 1;
    state["two_columns_landscape"] = false;
    return state;
}

QSize styleSamplePageSize()
{
    // A six-inch page at 300 ppi, scaled to the screen's density.
    return { readingPixels(1072), readingPixels(1448) };
}

QRect styleSampleReadingArea()
{
    const auto page = styleSamplePageSize();
    const int side = defaultMarginPixels(), top = readingPixels(36), bottom = readingPixels(36);
    return { side, top, page.width() - 2 * side, page.height() - top - bottom };
}

QJsonObject styleSamplePageSettings(const QJsonObject &reading)
{
    const auto area = styleSampleReadingArea();
    QJsonObject settings;
    for (const auto *key :
         { "dark_mode", "widows_orphans", "vertical_justification", "vertical_spacing_scope" })
        if (reading.contains(key))
            settings[key] = reading.value(key);
    settings["font_face"] = QString(defaultFontFace);
    settings["default_font_face"] = QString(defaultFontFace);
    settings["font_pixels"] = defaultFontPixels();
    settings["line_spacing"] = 140;
    settings["line_spacing_version"] = 1;
    settings["margin_pixels"] = area.left();
    settings["top_margin_pixels"] = area.top();
    settings["bottom_margin_pixels"] = styleSamplePageSize().height() - area.bottom() - 1;
    settings["text_alignment"] = QStringLiteral("justify");
    settings["hyphenation"] = QStringLiteral("auto");
    settings["weight_steps"] = 0;
    settings["bold_reading"] = QStringLiteral("off");
    settings["word_spacing"] = 100;
    settings["justification_tracking"] = 0;
    settings["normalize_page_margins"] = true;
    settings["reduce_chapter_spacing"] = true;
    settings["columns"] = 1;
    settings["two_columns_landscape"] = false;
    return settings;
}

void renderStyleSample(EngineFontInputs fonts, StyleSampleRequest request, QObject *context,
                       std::function<void(StyleSample)> finished)
{
    QPointer<QObject> guard(context);
    // The sample document shares CREngine's global fonts and caches, so it is
    // created, used and destroyed on the reader's engine worker.
    kobalt::qt::invoke(
            engineWorker(),
            [guard, fonts = std::move(fonts), request = std::move(request),
             finished = std::move(finished)] {
                StyleSample result;
                {
                    EngineJob activity;
                    result = render(fonts, request);
                }
                kobalt::qt::invoke(
                        qApp,
                        [guard, finished, result = std::move(result)]() mutable {
                            if (guard)
                                finished(std::move(result));
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}
} // namespace kobalt
