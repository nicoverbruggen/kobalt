#include "check.h"
#include "engine_fixtures.h"
#include "reader_inputs.h"
#include "engine/crengine/column_view.h"
#include "engine/crengine/engine_document.h"
#include "engine/crengine/reader_locations.h"
#include "publication/page_estimate.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <cmath>
#include <cstdio>
#include <lvdocview.h>

using namespace kobalt;

namespace kobalt {
// Reads the spine items of an open book, for the comparison with real pages.
struct ReaderLocationProbe
{
    static QStringList items(const EngineDocument &model)
    {
        QStringList result;
        auto *root = model.engine_->getDocument()->nodeFromXPath(U"/body");
        for (int i = 0; root && i < root->getChildCount(); ++i) {
            auto *fragment = root->getChildNode(i);
            if (fragment->isElement() && enginelocation::body(fragment))
                result.append(QString::fromUtf8(
                        UnicodeToUtf8(fragment->getAttributeValue("Source")).c_str()));
        }
        return result;
    }

    static QByteArray source(const EngineDocument &model, const QString &href)
    {
        auto stream = model.engine_->getDocument()->getContainer()->OpenStream(
                Utf8ToUnicode(href.toUtf8().constData()).c_str(), LVOM_READ);
        if (stream.isNull() || stream->GetSize() > 64 * 1024 * 1024)
            return {};
        QByteArray bytes(int(stream->GetSize()), '\0');
        lvsize_t read = 0;
        if (stream->Read(bytes.data(), bytes.size(), &read) != LVERR_OK
            || read != lvsize_t(bytes.size()))
            return {};
        return bytes;
    }
};
} // namespace kobalt

namespace {
using Probe = ReaderLocationProbe;

void blocks()
{
    const auto item = measurePageEstimate(
            "<html><head><title>Skipped title</title><style>p{}</style></head><body>"
            "<h1>Head</h1><p>One  two\n three</p><p>Line<br/>next</p><blockquote><p>Quoted</p>"
            "</blockquote><hr/><p><img src=\"a.png\"/></p></body></html>");
    require(item.parsed, "Well-formed XHTML must parse");
    require(item.characters == 4 + 11 + 8 + 6, "Whitespace and the head must not count");
    require(item.blocks.size() == 7, "Each paragraph, line, rule and image is one block");
    require(item.blocks[0].scale > 1 && item.blocks[1].scale == 1, "A heading has a larger font");
    require(item.blocks[2].kind == PageEstimateBlock::LineBreak
                    && item.blocks[3].kind == PageEstimateBlock::Text,
            "Text before a line break has no paragraph gap");
    require(item.blocks[4].depth == 1, "A quotation is indented");
    require(item.blocks[5].kind == PageEstimateBlock::Rule
                    && item.blocks[6].kind == PageEstimateBlock::Image,
            "Rules and images are blocks");
    require(!measurePageEstimate("<html><body><p>Open").parsed, "Malformed XHTML must be reported");

    PageEstimateLayout layout;
    layout.fontSize = 20;
    layout.lineHeight = 30;
    layout.width = 400;
    layout.height = 600;
    const auto prose = measurePageEstimate("<body><p>" + QByteArray(4000, 'a') + "</p></body>");
    const auto verse =
            measurePageEstimate("<body><p>" + QByteArray("a<br/>").repeated(4000) + "</p></body>");
    require(estimatePages(verse, layout) > 10 * estimatePages(prose, layout),
            "Short lines need more pages than the same characters as prose");
    auto wide = layout;
    wide.width = 800;
    require(estimatePages(prose, wide) < estimatePages(prose, layout),
            "A wider page needs fewer pages");
    require(estimatePages(prose, PageEstimateLayout()) == 0, "No layout, no estimate");
}

struct Result
{
    int items = 0, pages = 0, unparsed = 0;
    double estimated = 0;
    // The largest and the mean distance, in percent of the book, between the
    // real position of a spine item start and its estimate.
    double worstLayout = 0, meanLayout = 0, worstCharacters = 0, meanCharacters = 0;
    qint64 measureMs = 0, estimateUs = 0, engineMs = 0;
};

Result compare(const QByteArray &bytes)
{
    const int font = 44, margin = 80, spacing = 150;
    const QSize viewport(1264, 1680);
    EngineDocument model(test::readerFonts());
    QString error;
    QElapsedTimer timer;
    timer.start();
    require(model.open(bytes, viewport,
                       { { "font_pixels", font },
                         { "margin_pixels", margin },
                         { "top_margin_pixels", margin },
                         { "bottom_margin_pixels", margin },
                         { "line_spacing", spacing },
                         { "line_spacing_version", 1 } },
                       &error),
            qPrintable(error));
    Result result;
    result.engineMs = timer.elapsed();
    const auto items = Probe::items(model);
    QStringList starts;
    for (const auto &href : items) {
        Location location;
        location.href = href;
        starts.append(model.positionOf(location));
    }
    const auto pages = model.pagesOf(starts);
    result.items = items.size();
    result.pages = model.pages();

    PageEstimateLayout layout;
    layout.fontSize = font;
    layout.lineHeight = font * spacing / 100.0;
    layout.width = viewport.width() - 2 * margin;
    layout.height = viewport.height() - 2 * margin;
    layout.indent = font;
    QVector<QByteArray> sources;
    for (const auto &href : items)
        sources.append(Probe::source(model, href));
    timer.restart();
    QVector<PageEstimateItem> measured;
    for (const auto &source : sources)
        measured.append(measurePageEstimate(source));
    result.measureMs = timer.elapsed();
    timer.restart();
    QVector<double> byLayout, byCharacters;
    double totalLayout = 0, totalCharacters = 0;
    for (const auto &item : measured) {
        result.unparsed += !item.parsed;
        byLayout.append(std::ceil(estimatePages(item, layout)));
        byCharacters.append(double(item.characters));
        totalLayout += byLayout.last();
        totalCharacters += byCharacters.last();
    }
    result.estimateUs = timer.nsecsElapsed() / 1000;
    result.estimated = totalLayout;
    double beforeLayout = 0, beforeCharacters = 0;
    for (int i = 0; i < items.size(); ++i) {
        if (pages[i] >= 0 && result.pages > 0 && totalLayout > 0 && totalCharacters > 0) {
            const double real = 100.0 * pages[i] / result.pages;
            const double layoutError = std::fabs(100.0 * beforeLayout / totalLayout - real);
            const double characterError =
                    std::fabs(100.0 * beforeCharacters / totalCharacters - real);
            result.worstLayout = qMax(result.worstLayout, layoutError);
            result.worstCharacters = qMax(result.worstCharacters, characterError);
            result.meanLayout += layoutError / items.size();
            result.meanCharacters += characterError / items.size();
        }
        beforeLayout += byLayout[i];
        beforeCharacters += byCharacters[i];
    }
    return result;
}

void report(const QString &name, const Result &r)
{
    std::printf("%s|%d|%d|%.0f|%.2f|%.2f|%.2f|%.2f|%lld|%lld|%lld|%d\n", qPrintable(name), r.items,
                r.pages, r.estimated, r.worstLayout, r.meanLayout, r.worstCharacters,
                r.meanCharacters, (long long)r.measureMs, (long long)r.estimateUs,
                (long long)r.engineMs, r.unparsed);
}
} // namespace

// With a folder argument, reports each EPUB in it and requires nothing.
// Columns: name, spine items, real pages, estimated pages, worst and mean
// error of the layout estimate, worst and mean error of the character count
// (all in percent of the book), measure ms, estimate microseconds, engine
// open ms, spine items that did not parse.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc > 1) {
        const QDir folder(QString::fromLocal8Bit(argv[1]));
        for (const auto &name : folder.entryList({ "*.epub" }, QDir::Files, QDir::Name)) {
            QFile file(folder.filePath(name));
            if (file.open(QIODevice::ReadOnly))
                report(name, compare(file.readAll()));
        }
        return 0;
    }
    blocks();
    QFile file(QStringLiteral(KOBALT_TEST_FIXTURES) + "/trevelyans-type-tester.kepub.epub");
    require(file.open(QIODevice::ReadOnly), "Cannot read the book fixture");
    const auto result = compare(file.readAll());
    report("trevelyans-type-tester", result);
    require(result.items > 1 && result.pages > 0 && result.unparsed == 0,
            "The fixture must have several spine items that parse");
    std::printf("PASS: page estimate blocks and comparison with real pages\n");
    return 0;
}
