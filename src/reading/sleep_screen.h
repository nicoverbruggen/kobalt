#pragma once

#include <QJsonObject>
#include <QImage>
#include <QPainter>
#include <QPair>
#include <QRect>
#include <QStringList>
#include <QVector>

namespace kobalt::sleep {
// Sample an captured reader overlay without copying or scaling its pixels.
// A missing or translucent capture cannot rule out an image underneath it.
bool needsImageCleanup(const QImage &frame, bool colorDisplay);

// The backdrop. Plain is a blank page for designs that draw everything
// themselves. Wallpaper is one of the reader's own images, filling the screen.
// Neither is ever saved in the legacy `sleep_screen` preference.
enum class Mode { Stock, Cover, Page, Overlay, Plain, Wallpaper };
// The status tile drawn over the backdrop. It is independent of the backdrop.
enum class Tile { Card, Bookmark, None };
// What the sleep screen looks like. `designs` below lists the gallery order. Stock
// is Kobo's own screen. Bookmark, Card and Cover are the cover with today's
// tiles; Page and Overlay keep the tile saved in `sleep_tile`. Custom combines
// a backdrop with a status of the reader's choice, see `Custom` below.
enum class Design {
    Stock,
    Custom,
    Bookmark,
    Card,
    Cover,
    Seal,
    Medallion,
    Pill,
    Caption,
    Widgets,
    Night,
    Ring,
    Framed,
    Lines,
    Letter,
    Page,
    Overlay,
    Random // A Custom status choice. Resolve it before drawing.
};

// Text the newer designs draw besides the status, captured at sleep.
// Empty strings and a negative time mean unknown; designs leave those out.
struct Details
{
    // `left` is the time left without "to go", for the letter.
    // `font` is the reader's chosen body face, for the letter. `greeting`
    // replaces "Goodnight" on the Night screen when the reader set one.
    QString title{}, author{}, chapter{}, clock{}, left{}, font{}, greeting{};
    qint64 todayMilliseconds = -1;
};

// Where a design puts its status. Default is the design's own place. A design
// accepts only the places `places()` lists for it.
enum class Place { Default, TopLeft, TopRight, BottomLeft, BottomRight, Left, Right, Top, Bottom };

// Native cover preferences, read by the Kobo host when opening the gallery.
struct NativePreview
{
    bool showCover = true;
    bool fullScreen = false;
    bool showInfo = true;
};

// Keep the backdrop separate from the card so sleep and power-off can use
// the same saved page with their own current status.
struct Snapshot
{
    QImage image;
    Qt::ScreenOrientation orientation = Qt::PrimaryOrientation;
    QString progress, remaining;
    bool dark = false;
    Mode mode = Mode::Stock;
    bool dimPage = true;
    // Unknown in snapshots saved before the percentage was stored.
    int percent = -1;
    Tile tile = Tile::Bookmark;
    // The design that draws the status. A Custom screen is captured as the
    // status it chose, such as Seal, over its own `mode`.
    Design design = Design::Bookmark;
    Place place = Place::Default;
    Details details{};
    // Where the fitted cover sits in a Cover backdrop; empty when unknown.
    QRect coverArea{};
    // Set only when the native full-size cover supplied the decoded image.
    QString coverPath{};
    qint64 coverBytes = 0, coverModified = 0;
    bool privateBook = false;
};

// Safe fallback when the book or its chosen design is unavailable.
inline Snapshot privateSnapshot(QSize size, bool dark = false, bool followReading = false)
{
    if (size.isEmpty() || qint64(size.width()) * size.height() > 4 * 1024 * 1024)
        size = QSize(800, 1200);
    Snapshot snapshot;
    snapshot.image = QImage(followReading ? size
                                          : QSize(qMin(size.width(), size.height()),
                                                  qMax(size.width(), size.height())),
                            QImage::Format_RGB32);
    snapshot.image.fill(dark ? Qt::black : Qt::white);
    snapshot.orientation = Qt::PortraitOrientation;
    snapshot.dark = dark;
    snapshot.mode = Mode::Plain;
    snapshot.design = Design::Card;
    snapshot.tile = Tile::Card;
    snapshot.dimPage = false;
    snapshot.privateBook = true;
    return snapshot;
}

// Where a cover fits in `size` without cropping, centred.
inline QRect coverArea(QSize cover, QSize size)
{
    if (cover.isEmpty() || size.isEmpty())
        return {};
    const auto fitted = cover.scaled(size, Qt::KeepAspectRatio);
    return { QPoint((size.width() - fitted.width()) / 2, (size.height() - fitted.height()) / 2),
             fitted };
}

// Fit without cropping and keep the unused screen area black.
inline QImage coverBackground(const QImage &cover, QSize size)
{
    if (cover.isNull() || size.isEmpty() || qint64(size.width()) * size.height() > 4 * 1024 * 1024)
        return {};
    QImage background(size, QImage::Format_RGB32);
    if (background.isNull())
        return {};
    background.fill(Qt::black);
    const QRect area = coverArea(cover.size(), size);
    QPainter painter(&background);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(area, cover);
    painter.end();
    return background;
}

// Fit the artwork to the page without changing its aspect ratio.
inline void drawOverlay(QImage &page, const QImage &overlay)
{
    const QSize size = overlay.size().scaled(page.size(), Qt::KeepAspectRatio);
    const QRect area(QPoint((page.width() - size.width()) / 2, (page.height() - size.height()) / 2),
                     size);
    QPainter painter(&page);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(area, overlay);
}

inline constexpr Mode modes[] = { Mode::Cover, Mode::Page, Mode::Overlay, Mode::Stock };
inline constexpr auto preferenceKey = "sleep_screen";

inline QString value(Mode mode)
{
    switch (mode) {
    case Mode::Stock:
        return QStringLiteral("stock");
    case Mode::Cover:
        return QStringLiteral("cover");
    case Mode::Page:
        return QStringLiteral("page");
    case Mode::Overlay:
        return QStringLiteral("overlay");
    case Mode::Plain:
        return QStringLiteral("plain");
    case Mode::Wallpaper:
        return QStringLiteral("wallpaper");
    }
    return {};
}

inline Mode savedMode(const QJsonObject &preferences)
{
    const auto saved = preferences.value(preferenceKey);
    for (const auto option : modes)
        if (saved == value(option))
            return option;
    return Mode::Cover;
}

inline constexpr Tile tiles[] = { Tile::Bookmark, Tile::Card, Tile::None };
inline constexpr auto tileKey = "sleep_tile";

inline QString value(Tile tile)
{
    switch (tile) {
    case Tile::Card:
        return QStringLiteral("card");
    case Tile::Bookmark:
        return QStringLiteral("bookmark");
    case Tile::None:
        return QStringLiteral("none");
    }
    return {};
}

inline Tile savedTile(const QJsonObject &preferences)
{
    const auto saved = preferences.value(tileKey);
    for (const auto option : tiles)
        if (saved == value(option))
            return option;
    return Tile::Bookmark;
}

// Gallery order: Kobo's screen, Custom, the reading page, then the presets.
inline constexpr Design designs[] = { Design::Stock,    Design::Custom,    Design::Page,
                                      Design::Bookmark, Design::Card,      Design::Cover,
                                      Design::Seal,     Design::Medallion, Design::Pill,
                                      Design::Caption,  Design::Widgets,   Design::Night,
                                      Design::Ring,     Design::Framed,    Design::Lines,
                                      Design::Letter,   Design::Overlay };

inline constexpr auto designKey = "sleep_design";
inline constexpr auto followOrientationKey = "sleep_follow_orientation";

inline bool followsReading(const QJsonObject &preferences)
{
    return preferences.value(followOrientationKey).toBool(true);
}

// The Night screen's own greeting. Long-pressing Night in the gallery sets it.
inline constexpr auto greetingKey = "sleep_greeting";
inline constexpr int greetingLength = 40;

inline QString value(Design design)
{
    switch (design) {
    case Design::Stock:
        return QStringLiteral("stock");
    case Design::Custom:
        return QStringLiteral("custom");
    case Design::Random:
        return QStringLiteral("random");
    case Design::Bookmark:
        return QStringLiteral("bookmark");
    case Design::Card:
        return QStringLiteral("card");
    case Design::Cover:
        return QStringLiteral("cover");
    case Design::Seal:
        return QStringLiteral("seal");
    case Design::Medallion:
        return QStringLiteral("medallion");
    case Design::Pill:
        return QStringLiteral("pill");
    case Design::Caption:
        return QStringLiteral("caption");
    case Design::Widgets:
        return QStringLiteral("widgets");
    case Design::Night:
        return QStringLiteral("night");
    case Design::Ring:
        return QStringLiteral("ring");
    case Design::Framed:
        return QStringLiteral("framed");
    case Design::Lines:
        return QStringLiteral("lines");
    case Design::Letter:
        return QStringLiteral("letter");
    case Design::Page:
        return QStringLiteral("page");
    case Design::Overlay:
        return QStringLiteral("overlay");
    }
    return {};
}

inline QStringList designValues()
{
    QStringList values;
    for (const auto design : designs)
        values.append(value(design));
    return values;
}

// The backdrop each design is drawn over.
inline Mode backdrop(Design design)
{
    switch (design) {
    case Design::Stock:
        return Mode::Stock;
    case Design::Page:
        return Mode::Page;
    case Design::Overlay:
        return Mode::Overlay;
    case Design::Night:
    case Design::Ring:
    case Design::Lines:
    case Design::Letter:
        return Mode::Plain;
    default:
        return Mode::Cover;
    }
}

// Designs that predate the gallery and draw one of today's tiles.
inline bool tiled(Design design)
{
    switch (design) {
    case Design::Bookmark:
    case Design::Card:
    case Design::Cover:
    case Design::Page:
    case Design::Overlay:
        return true;
    default:
        return false;
    }
}

// The legacy pair an older Kobalt reads for a design. Newer designs show
// the cover with the bookmark there. Stock, Page and Overlay keep the tile.
inline QPair<Mode, Tile> legacyPair(Design design, Tile saved)
{
    switch (design) {
    case Design::Stock:
        return { Mode::Stock, saved };
    case Design::Card:
        return { Mode::Cover, Tile::Card };
    case Design::Cover:
        return { Mode::Cover, Tile::None };
    case Design::Page:
        return { Mode::Page, saved };
    case Design::Overlay:
        return { Mode::Overlay, saved };
    default:
        return { Mode::Cover, Tile::Bookmark };
    }
}

// The design from the legacy pair alone.
inline Design legacyDesign(Mode mode, Tile tile)
{
    switch (mode) {
    case Mode::Stock:
        return Design::Stock;
    case Mode::Page:
        return Design::Page;
    case Mode::Overlay:
        return Design::Overlay;
    case Mode::Cover:
    case Mode::Plain:
    case Mode::Wallpaper:
        break;
    }
    switch (tile) {
    case Tile::Card:
        return Design::Card;
    case Tile::None:
        return Design::Cover;
    case Tile::Bookmark:
        break;
    }
    return Design::Bookmark;
}

// `sleep_design` wins while the legacy pair still matches it. An older
// Kobalt changes only the pair, so a pair that no longer matches is newer.
inline Design design(const QJsonObject &preferences)
{
    const auto mode = savedMode(preferences);
    const auto tile = savedTile(preferences);
    const auto saved = preferences.value(designKey);
    for (const auto option : designs)
        if (saved == value(option)) {
            const auto pair = legacyPair(option, tile);
            const bool matches =
                    option == Design::Stock || option == Design::Page || option == Design::Overlay
                    ? pair.first == mode
                    : pair == qMakePair(mode, tile);
            if (matches)
                return option;
            break;
        }
    return legacyDesign(mode, tile);
}

// Writes the design and its legacy pair, keeping the saved tile where the
// design does not choose one.
inline void setDesign(QJsonObject &preferences, Design design)
{
    const auto pair = legacyPair(design, savedTile(preferences));
    preferences[designKey] = value(design);
    preferences[preferenceKey] = value(pair.first);
    preferences[tileKey] = value(pair.second);
}

// ---------------------------------------------------------------- Places

inline constexpr auto placesKey = "sleep_positions";

inline QString value(Place place)
{
    switch (place) {
    case Place::Default:
        return {};
    case Place::TopLeft:
        return QStringLiteral("top-left");
    case Place::TopRight:
        return QStringLiteral("top-right");
    case Place::BottomLeft:
        return QStringLiteral("bottom-left");
    case Place::BottomRight:
        return QStringLiteral("bottom-right");
    case Place::Left:
        return QStringLiteral("left");
    case Place::Right:
        return QStringLiteral("right");
    case Place::Top:
        return QStringLiteral("top");
    case Place::Bottom:
        return QStringLiteral("bottom");
    }
    return {};
}

// The places a design can move its status to, in the order the editor
// offers them. Designs that cannot move return none.
inline QVector<Place> places(Design design)
{
    switch (design) {
    case Design::Seal:
    case Design::Medallion:
    case Design::Card:
        return { Place::TopLeft, Place::TopRight, Place::BottomLeft, Place::BottomRight };
    case Design::Bookmark:
        return { Place::Left, Place::Right };
    case Design::Pill:
    case Design::Caption:
        return { Place::Top, Place::Bottom };
    default:
        return {};
    }
}

// Where a design puts its status when the reader has not moved it.
inline Place defaultPlace(Design design)
{
    switch (design) {
    case Design::Seal:
    case Design::Medallion:
        return Place::BottomRight;
    case Design::Card:
        return Place::BottomLeft;
    case Design::Bookmark:
        return Place::Right;
    case Design::Pill:
    case Design::Caption:
        return Place::Bottom;
    default:
        return Place::Default;
    }
}

// The saved place of `design` in a `sleep_positions` object, or Default.
inline Place savedPlace(const QJsonObject &positions, Design design)
{
    const auto saved = positions.value(value(design));
    for (const auto option : places(design))
        if (saved == value(option))
            return option;
    return Place::Default;
}

// The book's own place wins over the global one. Default keeps the design's
// own place.
inline Place place(const QJsonObject &preferences, const QJsonObject &bookPositions, Design design)
{
    const auto own = savedPlace(bookPositions, design);
    if (own != Place::Default)
        return own;
    return savedPlace(preferences.value(placesKey).toObject(), design);
}

// Saves `place` for `design` in a `sleep_positions` object. The design's
// default place removes the entry.
inline void setPlace(QJsonObject &positions, Design design, Place place)
{
    if (place == Place::Default || place == defaultPlace(design) || !places(design).contains(place))
        positions.remove(value(design));
    else
        positions[value(design)] = value(place);
}

// ---------------------------------------------------------------- Custom

// A Custom screen is a backdrop with one status over it. Sleeping and charging
// use `sleep`. Power off uses `off` unless `same` is set.
inline constexpr auto customKey = "sleep_custom";

// The backdrops and statuses Custom offers, in the editor's order. The Cover
// design stands for no status: it draws the backdrop alone.
inline constexpr Mode customBackdrops[] = { Mode::Cover, Mode::Page, Mode::Overlay, Mode::Wallpaper,
                                            Mode::Plain };
inline constexpr Design customStatuses[] = { Design::Cover, Design::Random, Design::Bookmark,
                                             Design::Card,  Design::Seal,   Design::Medallion,
                                             Design::Pill,  Design::Caption };

// Only visible statuses participate in a random draw.
inline bool eligibleStatus(Design status)
{
    for (const auto choice : customStatuses)
        if (choice == status)
            return choice != Design::Random && choice != Design::Cover;
    return false;
}

Design pickStatus(Design previous);

struct Layer
{
    Mode backdrop = Mode::Cover;
    Design status = Design::Bookmark;
    // A file name in the wallpaper folders. Empty picks one at random.
    QString wallpaper{};

    bool operator==(const Layer &other) const
    {
        return backdrop == other.backdrop && status == other.status && wallpaper == other.wallpaper;
    }
};

struct Custom
{
    bool same = true;
    Layer sleep{}, off{};
};

inline QString statusValue(Design status)
{
    return status == Design::Cover ? QStringLiteral("none") : value(status);
}

inline Layer layer(const QJsonObject &saved)
{
    Layer result;
    const auto backdrop = saved.value("background");
    for (const auto option : customBackdrops)
        if (backdrop == value(option))
            result.backdrop = option;
    const auto status = saved.value("status");
    for (const auto option : customStatuses)
        if (status == statusValue(option))
            result.status = option;
    // A name, never a path: the folders are fixed by the host.
    const auto name = saved.value("wallpaper").toString();
    if (!name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) && name.size() <= 255)
        result.wallpaper = name;
    return result;
}

inline QJsonObject value(const Layer &layer)
{
    QJsonObject saved;
    saved["background"] = value(layer.backdrop);
    saved["status"] = statusValue(layer.status);
    if (!layer.wallpaper.isEmpty())
        saved["wallpaper"] = layer.wallpaper;
    return saved;
}

// Missing or invalid fields use the defaults: the same screen for both, the
// cover with the bookmark.
inline Custom custom(const QJsonObject &preferences)
{
    const auto saved = preferences.value(customKey).toObject();
    Custom result;
    result.same = saved.value("same").toBool(true);
    result.sleep = layer(saved.value("sleep").toObject());
    result.off = saved.contains("off") ? layer(saved.value("off").toObject()) : result.sleep;
    return result;
}

inline void setCustom(QJsonObject &preferences, const Custom &custom)
{
    QJsonObject saved;
    saved["same"] = custom.same;
    saved["sleep"] = value(custom.sleep);
    saved["off"] = value(custom.same ? custom.sleep : custom.off);
    preferences[customKey] = saved;
}

inline bool randomStatus(const QJsonObject &preferences, bool poweredOff = false)
{
    if (design(preferences) != Design::Custom)
        return false;
    const auto saved = custom(preferences);
    return (poweredOff && !saved.same ? saved.off : saved.sleep).status == Design::Random;
}

// A design that is a backdrop with one status, as Custom can show it.
// Returns false for Kobo's screen, Custom and the designs that draw the whole screen.
inline bool asLayer(Design design, Tile tile, Layer &result)
{
    switch (design) {
    case Design::Bookmark:
    case Design::Card:
    case Design::Cover:
    case Design::Seal:
    case Design::Medallion:
    case Design::Pill:
    case Design::Caption:
        result = { Mode::Cover, design, {} };
        return true;
    case Design::Page:
    case Design::Overlay:
        result = { design == Design::Page ? Mode::Page : Mode::Overlay,
                   tile == Tile::Card           ? Design::Card
                           : tile == Tile::None ? Design::Cover
                                                : Design::Bookmark,
                   {} };
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- Look

// What one state of the chosen screen draws: the status design, its backdrop
// and, for the tiled designs, the tile.
struct Look
{
    Design design = Design::Bookmark;
    Mode mode = Mode::Cover;
    Tile tile = Tile::Bookmark;
};

inline Tile tileFor(Design status)
{
    return status == Design::Bookmark ? Tile::Bookmark
            : status == Design::Card  ? Tile::Card
                                      : Tile::None;
}

inline Look look(const Layer &layer, Design picked = Design::Bookmark)
{
    const auto status = layer.status == Design::Random
            ? (eligibleStatus(picked) ? picked : Design::Bookmark)
            : layer.status;
    return { status, layer.backdrop, tileFor(status) };
}

// Charging uses the sleeping look; only power off can differ.
inline Look look(const QJsonObject &preferences, bool poweredOff = false,
                 Design picked = Design::Bookmark)
{
    const auto chosen = design(preferences);
    if (chosen == Design::Custom) {
        const auto saved = custom(preferences);
        return look(poweredOff && !saved.same ? saved.off : saved.sleep, picked);
    }
    Tile tile = Tile::None;
    switch (chosen) {
    case Design::Bookmark:
        tile = Tile::Bookmark;
        break;
    case Design::Card:
        tile = Tile::Card;
        break;
    case Design::Page:
    case Design::Overlay:
    case Design::Stock:
        tile = savedTile(preferences);
        break;
    default:
        break;
    }
    return { chosen, backdrop(chosen), tile };
}

// The design whose place applies: the tile for Reading page, PNG over text
// and the cover designs with a tile, otherwise the design itself.
inline Design statusDesign(const Look &look)
{
    if (look.tile == Tile::Bookmark)
        return Design::Bookmark;
    if (look.tile == Tile::Card)
        return Design::Card;
    return look.design;
}

inline Place place(const QJsonObject &preferences, const QJsonObject &bookPositions,
                   const Look &look)
{
    return place(preferences, bookPositions, statusDesign(look));
}

// The backdrop while sleeping.
inline Mode mode(const QJsonObject &preferences)
{
    return look(preferences).mode;
}

// Whether sleeping or power off draws over `mode`, so its image is prepared.
inline bool uses(const QJsonObject &preferences, Mode mode)
{
    return look(preferences).mode == mode || look(preferences, true).mode == mode;
}

// The tile for the tiled designs; None for designs that draw their own status.
inline Tile tile(const QJsonObject &preferences)
{
    return look(preferences).tile;
}

// Keep the design and anonymous reading statistics, discarding all book pixels
// and identifying metadata. Stock needs a Kobalt fallback to cover Nickel's image.
// A Custom screen keeps its status over the neutral backdrop; its wallpaper
// and PNG are left out.
inline Snapshot privateSnapshot(QSize size, const QJsonObject &preferences, Snapshot source = {},
                                bool followReading = false, bool poweredOff = false,
                                Design picked = Design::Bookmark)
{
    auto snapshot = privateSnapshot(size, preferences.value("dark_mode").toBool(source.dark),
                                    followReading);
    const auto chosen = look(preferences, poweredOff, picked);
    snapshot.design = chosen.design;
    snapshot.tile = chosen.tile;
    if (snapshot.design == Design::Stock) {
        snapshot.design = Design::Card;
        snapshot.tile = Tile::Card;
    }
    if (chosen.mode == Mode::Cover)
        snapshot.image.fill(Qt::black);
    snapshot.place = source.place;
    snapshot.percent = source.percent;
    snapshot.progress = source.progress;
    snapshot.remaining = source.remaining;
    snapshot.details = source.details;
    snapshot.details.title.clear();
    snapshot.details.author.clear();
    snapshot.details.chapter.clear();
    snapshot.details.greeting = preferences.value(greetingKey).toString();
    return snapshot;
}

} // namespace kobalt::sleep
