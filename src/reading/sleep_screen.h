#pragma once

#include <QJsonObject>
#include <QImage>
#include <QPainter>

namespace kobalt::sleep {
enum class Mode { Stock, Cover, Page };
// The status tile drawn over the backdrop. It is independent of the backdrop.
enum class Tile { Card, Band, Bookmark };

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
    Tile tile = Tile::Card;
    // Set only when the native full-size cover supplied the decoded image.
    QString coverPath{};
    qint64 coverBytes = 0, coverModified = 0;
};

// Fit without cropping and keep the unused screen area black.
inline QImage coverBackground(const QImage &cover, QSize size)
{
    if (cover.isNull() || size.isEmpty() || qint64(size.width()) * size.height() > 4 * 1024 * 1024)
        return {};
    QImage background(size, QImage::Format_RGB32);
    if (background.isNull())
        return {};
    background.fill(Qt::black);
    const auto fitted = cover.size().scaled(size, Qt::KeepAspectRatio);
    const QRect area(
            QPoint((size.width() - fitted.width()) / 2, (size.height() - fitted.height()) / 2),
            fitted);
    QPainter painter(&background);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(area, cover);
    painter.end();
    return background;
}

inline constexpr Mode modes[] = { Mode::Stock, Mode::Cover, Mode::Page };
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
    }
    return {};
}

inline Mode mode(const QJsonObject &preferences)
{
    const auto saved = preferences.value(preferenceKey);
    if (saved.isUndefined()) {
        return preferences.value("kobalt_sleep_screen").toBool(false) ? Mode::Cover : Mode::Stock;
    }
    if (saved == value(Mode::Cover)) {
        return Mode::Cover;
    }
    if (saved == value(Mode::Page)) {
        return Mode::Page;
    }
    return Mode::Stock;
}

inline constexpr Tile tiles[] = { Tile::Card, Tile::Band, Tile::Bookmark };
inline constexpr auto tileKey = "sleep_tile";

inline QString value(Tile tile)
{
    switch (tile) {
    case Tile::Card:
        return QStringLiteral("card");
    case Tile::Band:
        return QStringLiteral("band");
    case Tile::Bookmark:
        return QStringLiteral("bookmark");
    }
    return {};
}

inline Tile tile(const QJsonObject &preferences)
{
    const auto saved = preferences.value(tileKey);
    if (saved == value(Tile::Band)) {
        return Tile::Band;
    }
    if (saved == value(Tile::Bookmark)) {
        return Tile::Bookmark;
    }
    return Tile::Card;
}
} // namespace kobalt::sleep
