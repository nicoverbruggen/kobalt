#pragma once

#include <QJsonObject>
#include <QImage>
#include <QPainter>

namespace kobalt::sleep {
enum class Mode { Stock, Cover, Page, Overlay };
// The status tile drawn over the backdrop. It is independent of the backdrop.
enum class Tile { Card, Bookmark, None };

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
    }
    return {};
}

inline Mode mode(const QJsonObject &preferences)
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

inline Tile tile(const QJsonObject &preferences)
{
    const auto saved = preferences.value(tileKey);
    for (const auto option : tiles)
        if (saved == value(option))
            return option;
    return Tile::Bookmark;
}
} // namespace kobalt::sleep
