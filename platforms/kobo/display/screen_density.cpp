#include "screen_density.h"
#include "support/diagnostics.h"
#include "reading/reading_density.h"
#include <QGuiApplication>
#include <QScreen>
#include <utility>

namespace kobalt {
namespace {
// Device codenames as PRODUCT prefixes; the value can carry a suffix such as
// "spaColour" or "monzaTolino". Densities are the panels' published values.
constexpr std::pair<const char *, int> products[] = {
    { "trilogy", 167 }, { "pika", 167 },     { "pixie", 200 },     { "kraken", 212 },
    { "phoenix", 212 }, { "star", 212 },     { "luna", 212 },      { "dragon", 265 },
    { "dahlia", 265 },  { "snow", 265 },     { "europa", 227 },    { "condor", 227 },
    { "alyssum", 300 }, { "nova", 300 },     { "goldfinch", 300 }, { "spa", 300 },
    { "storm", 300 },   { "io", 300 },       { "monza", 300 },     { "frost", 300 },
    { "cadmus", 300 },  { "daylight", 300 },
};
} // namespace

ScreenDensity screenDensity(const QByteArray &product, qreal logicalDpi)
{
    for (const auto &entry : products)
        if (product.startsWith(entry.first))
            return { entry.second, "product" };
    // Qt reports 96 or 100 when the platform knows nothing about the screen.
    if (logicalDpi >= 150 && logicalDpi <= 400)
        return { qRound(logicalDpi), "screen" };
    return { 300, "default" };
}

int currentScreenDensity()
{
    static int logged = 0;
    const auto product = qgetenv("PRODUCT");
    auto *screen = QGuiApplication::primaryScreen();
    const qreal logical = screen ? screen->logicalDotsPerInch() : 0;
    const auto density = screenDensity(product, logical);
    readingPpi = density.ppi;
    if (density.ppi != logged) {
        logged = density.ppi;
        diagnostic("ui-density",
                   { { "ppi", density.ppi },
                     { "source", density.source },
                     { "product", QString::fromLatin1(product) },
                     { "screen_dpi", logical } });
    }
    return density.ppi;
}
} // namespace kobalt
