#pragma once

#include <QJsonObject>
#include <QImage>
#include <QPainter>
#include <QPair>
#include <QRect>
#include <QStringList>

namespace kobalt::sleep {
// The backdrop. Plain is a blank page for designs that draw everything
// themselves. It is never saved in the legacy `sleep_screen` preference.
enum class Mode { Stock, Cover, Page, Overlay, Plain };
// The status tile drawn over the backdrop. It is independent of the backdrop.
enum class Tile { Card, Bookmark, None };
// What the sleep screen looks like. `designs` below lists the gallery order. Stock
// is Kobo's own screen. Bookmark, Card and Cover are the cover with today's
// tiles; Page and Overlay keep the tile saved in `sleep_tile`.
enum class Design {
    Stock,
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
    Overlay
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
    Design design = Design::Bookmark;
    Details details{};
    // Where the fitted cover sits in a Cover backdrop; empty when unknown.
    QRect coverArea{};
    // Set only when the native full-size cover supplied the decoded image.
    QString coverPath{};
    qint64 coverBytes = 0, coverModified = 0;
    bool privateBook = false;
};

// Safe fallback when the book or its chosen design is unavailable.
inline Snapshot privateSnapshot(QSize size, bool dark = false)
{
    if (size.isEmpty() || qint64(size.width()) * size.height() > 4 * 1024 * 1024)
        size = QSize(800, 1200);
    Snapshot snapshot;
    snapshot.image =
            QImage(QSize(qMin(size.width(), size.height()), qMax(size.width(), size.height())),
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

// The gallery's order: Kobo's own screen, the reading page, today's default,
// then the rest.
inline constexpr Design designs[] = { Design::Stock,     Design::Page,  Design::Bookmark,
                                      Design::Card,      Design::Cover, Design::Seal,
                                      Design::Medallion, Design::Pill,  Design::Caption,
                                      Design::Widgets,   Design::Night, Design::Ring,
                                      Design::Framed,    Design::Lines, Design::Letter,
                                      Design::Overlay };
inline constexpr auto designKey = "sleep_design";
// The Night screen's own greeting. Long-pressing Night in the gallery sets it.
inline constexpr auto greetingKey = "sleep_greeting";
inline constexpr int greetingLength = 40;

inline QString value(Design design)
{
    switch (design) {
    case Design::Stock:
        return QStringLiteral("stock");
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

inline Mode mode(const QJsonObject &preferences)
{
    return backdrop(design(preferences));
}

// The tile for the tiled designs; None for designs that draw their own status.
inline Tile tile(const QJsonObject &preferences)
{
    const auto chosen = design(preferences);
    switch (chosen) {
    case Design::Bookmark:
        return Tile::Bookmark;
    case Design::Card:
        return Tile::Card;
    case Design::Page:
    case Design::Overlay:
    case Design::Stock:
        return savedTile(preferences);
    default:
        return Tile::None;
    }
}

// Keep the design and anonymous reading statistics, discarding all book pixels
// and identifying metadata. Stock needs a Kobalt fallback to cover Nickel's image.
inline Snapshot privateSnapshot(QSize size, const QJsonObject &preferences, Snapshot source = {})
{
    auto snapshot = privateSnapshot(size, preferences.value("dark_mode").toBool(source.dark));
    snapshot.design = design(preferences);
    snapshot.tile = tile(preferences);
    if (snapshot.design == Design::Stock) {
        snapshot.design = Design::Card;
        snapshot.tile = Tile::Card;
    }
    if (backdrop(snapshot.design) == Mode::Cover)
        snapshot.image.fill(Qt::black);
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
