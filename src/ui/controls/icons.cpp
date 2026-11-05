#include "icons.h"
#include "buttons.h"
#include "ui/style/metrics.h"
#include "ui/style/theme.h"
#include <QIconEngine>
#include <QCache>
#include <QDataStream>
#include <QIODevice>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QTransform>
#include <QVariant>
#include "phosphor_geometry.h"

namespace kobalt::ui {
QStringList bookmarkIconNames() { return {
        "map", "compass", "home", "flag",
        "heart", "lightbulb", "people", "tree",
        "sun", "moon", "crown", "key",
        "sword", "warning", "scroll", "eye",
        "castle", "anchor", "paw", "flower",
        "coffee", "rocket", "lightning", "flame",
        "planet", "star", "bookmark", "dictionary",
        "chapters", "link", "note", "globe",
        "mountains", "clock", "calendar", "person",
        "pencil", "quotes", "question", "check",
        "chat", "image", "search", "lock",
        "books", "music", "skull", "diamond"}; }
ControlIcon bookmarkIcon(const QString &name) {
    const ControlIcon icons[] = {
        ControlIcon::Map, ControlIcon::Compass, ControlIcon::Home, ControlIcon::Flag,
        ControlIcon::Heart, ControlIcon::Lightbulb, ControlIcon::People, ControlIcon::Tree,
        ControlIcon::Light, ControlIcon::Moon, ControlIcon::Crown, ControlIcon::Key,
        ControlIcon::Sword, ControlIcon::Warning, ControlIcon::Scroll, ControlIcon::Display,
        ControlIcon::Castle, ControlIcon::Anchor, ControlIcon::Paw, ControlIcon::Flower,
        ControlIcon::Coffee, ControlIcon::Rocket, ControlIcon::Lightning, ControlIcon::Flame,
        ControlIcon::Planet, ControlIcon::Star, ControlIcon::Bookmark, ControlIcon::Dictionary,
        ControlIcon::Chapters, ControlIcon::Link, ControlIcon::Note, ControlIcon::Globe,
        ControlIcon::Mountains, ControlIcon::Clock, ControlIcon::Calendar, ControlIcon::Person,
        ControlIcon::EditHighlight, ControlIcon::Quotes, ControlIcon::Question, ControlIcon::CheckCircle,
        ControlIcon::Chat, ControlIcon::Image, ControlIcon::Search, ControlIcon::Lock,
        ControlIcon::Books, ControlIcon::Music, ControlIcon::Skull, ControlIcon::Diamond};
    const int index = bookmarkIconNames().indexOf(name);
    return index < 0 ? ControlIcon::Star : icons[index];
}
namespace {
struct Geometry {
    QPainterPath path;
    qreal canvas = 64;
    qreal stroke = 3.5;
    Qt::PenCapStyle cap = Qt::RoundCap;
    Qt::PenJoinStyle join = Qt::RoundJoin;
    bool filled = false;
    QRectF inkBounds() const {
        if (filled)
            return path.boundingRect();
        QPainterPathStroker stroker;
        stroker.setWidth(stroke);
        stroker.setCapStyle(cap);
        stroker.setJoinStyle(join);
        return stroker.createStroke(path).boundingRect();
    }
};

Geometry geometry(ControlIcon icon, bool sideButtons) {
    Geometry result;
    auto &path = result.path;
    const auto line = [&path](qreal x1, qreal y1, qreal x2, qreal y2) {
        path.moveTo(x1, y1);
        path.lineTo(x2, y2);
    };
    if (icon == ControlIcon::WidowsOrphans) {
        line(11, 18, 34, 18);
        line(11, 32, 37, 32);
        line(11, 46, 29, 46);
        path.moveTo(44, 14);
        path.lineTo(51, 14);
        path.lineTo(51, 50);
        path.lineTo(44, 50);
    } else if (icon == ControlIcon::HangingPunctuation) {
        line(11, 20, 36, 20);
        line(11, 35, 40, 35);
        line(11, 49, 30, 49);
        path.moveTo(51.8, 21);
        path.cubicTo(51.8, 26, 49.6, 29, 47, 30);
        QPainterPathStroker stroke;
        stroke.setWidth(result.stroke);
        stroke.setCapStyle(result.cap);
        stroke.setJoinStyle(result.join);
        QPainterPath dot;
        dot.addEllipse(QPointF(49, 20), 3.5, 3.5);
        path = stroke.createStroke(path).united(dot);
        result.filled = true;
    } else if (icon == ControlIcon::FitImage) {
        line(22, 9, 9, 9); line(9, 9, 9, 22);
        line(42, 9, 55, 9); line(55, 9, 55, 22);
        line(9, 42, 9, 55); line(9, 55, 22, 55);
        line(42, 55, 55, 55); line(55, 55, 55, 42);
        path.addRect(20, 23, 24, 18);
    } else if (icon == ControlIcon::Door) {
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(QStringLiteral("door-open"), path);
    } else if (icon == ControlIcon::BookmarkAdd || icon == ControlIcon::BookmarkRemove || icon == ControlIcon::ExportAction) {
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(icon == ControlIcon::ExportAction ? QStringLiteral("export")
                                                          : QStringLiteral("bookmark-simple"), path);
        if (icon == ControlIcon::BookmarkAdd) {
            QPainterPath horizontal, vertical;
            horizontal.addRect(96, 104, 64, 16);
            vertical.addRect(120, 80, 16, 64);
            path = path.united(horizontal.united(vertical));
        } else if (icon == ControlIcon::BookmarkRemove) {
            QPainterPath slash;
            slash.moveTo(40, 32);
            slash.lineTo(216, 224);
            QPainterPathStroker stroke;
            stroke.setCapStyle(Qt::RoundCap);
            stroke.setWidth(28);
            path = path.subtracted(stroke.createStroke(slash));
            stroke.setWidth(16);
            path = path.united(stroke.createStroke(slash));
        }
    } else if (icon >= ControlIcon::Flag && icon <= ControlIcon::Diamond) {
        const char *names[] = {"flag", "heart", "note", "lightbulb", "globe", "mountains", "tree", "moon",
                               "clock", "calendar", "user", "users", "crown", "key", "sword",
                               "quotes", "question", "check-circle", "warning", "chat-circle", "scroll", "image", "books",
                               "castle-turret", "anchor", "paw-print", "flower", "coffee", "music-note", "rocket", "planet",
                               "lightning", "flame", "skull", "diamond"};
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(QString::fromLatin1(names[int(icon) - int(ControlIcon::Flag)]), path);
    } else if (icon == ControlIcon::Pin || icon == ControlIcon::PinOff) {
        result.canvas = 24;
        result.stroke = 1.5;
        path.moveTo(8, 3); path.lineTo(16, 3); path.lineTo(15, 10);
        path.lineTo(19, 14); path.lineTo(5, 14); path.lineTo(9, 10); path.closeSubpath();
        line(12, 14, 12, 22);
        if (icon == ControlIcon::PinOff) line(3, 3, 21, 21);
    } else if (icon == ControlIcon::Star) {
        result.canvas = 24;
        result.stroke = 1.5;
        path.moveTo(12, 2); path.lineTo(15, 8.5); path.lineTo(22, 9.5);
        path.lineTo(17, 14.5); path.lineTo(18, 22); path.lineTo(12, 18.5);
        path.lineTo(6, 22); path.lineTo(7, 14.5); path.lineTo(2, 9.5);
        path.lineTo(9, 8.5); path.closeSubpath();
    } else if (icon == ControlIcon::Map) {
        result.canvas = 24;
        result.stroke = 1.5;
        path.moveTo(2, 5); path.lineTo(8, 2); path.lineTo(16, 5); path.lineTo(22, 2);
        path.lineTo(22, 19); path.lineTo(16, 22); path.lineTo(8, 19); path.lineTo(2, 22);
        path.closeSubpath(); line(8, 2, 8, 19); line(16, 5, 16, 22);
    } else if (icon == ControlIcon::KeyboardShift || icon == ControlIcon::KeyboardCapsLock) {
        result.canvas = 24;
        result.stroke = 1.5;
        path.moveTo(12, 3);
        path.lineTo(21, 12);
        path.lineTo(16, 12);
        path.lineTo(16, icon == ControlIcon::KeyboardCapsLock ? 17 : 21);
        path.lineTo(8, icon == ControlIcon::KeyboardCapsLock ? 17 : 21);
        path.lineTo(8, 12);
        path.lineTo(3, 12);
        path.closeSubpath();
        if (icon == ControlIcon::KeyboardCapsLock) line(8, 21, 16, 21);
    } else if (icon == ControlIcon::KeyboardBackspace) {
        result.canvas = 24;
        result.stroke = 1.5;
        path.moveTo(8, 5);
        path.lineTo(22, 5);
        path.lineTo(22, 19);
        path.lineTo(8, 19);
        path.lineTo(1, 12);
        path.closeSubpath();
        line(11, 9, 17, 15);
        line(17, 9, 11, 15);
    } else if (icon == ControlIcon::KeyboardReturn) {
        result.canvas = 24;
        result.stroke = 1.5;
        path.moveTo(21, 4);
        path.lineTo(21, 14);
        path.lineTo(3, 14);
        line(3, 14, 9, 8);
        line(3, 14, 9, 20);
    } else if (icon == ControlIcon::Export) {
        // Regular-weight Phosphor icons retain their original 256-unit outlines.
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(QStringLiteral("file-zip"), path);
    } else if (icon >= ControlIcon::WifiNone && icon <= ControlIcon::WifiDisconnected) {
        static const char *names[] = {"wifi-none", "wifi-low", "wifi-medium", "wifi-high", "wifi-slash"};
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(QString::fromLatin1(names[int(icon) - int(ControlIcon::WifiNone)]), path);
    } else if (icon == ControlIcon::Sync) {
        result.canvas = 24;
        result.stroke = 1.5;
        path.moveTo(4, 9);
        path.cubicTo(7, 2, 17, 2, 20, 9);
        line(20, 4, 20, 9);
        line(20, 9, 15, 9);
        path.moveTo(20, 15);
        path.cubicTo(17, 22, 7, 22, 4, 15);
        line(4, 20, 4, 15);
        line(4, 15, 9, 15);
    } else if (icon == ControlIcon::ArrowDownRight) {
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(QStringLiteral("arrow-down-right"), path);
    } else if (icon >= ControlIcon::SpreadHorizontal && icon <= ControlIcon::Hidden) {
        const char *names[] = {"arrows-horizontal", "paragraph", "arrows-vertical", "magic-wand", "eye-slash"};
        result.canvas = 256;
        result.filled = true;
        static_assert(sizeof(names) / sizeof(*names) == size_t(int(ControlIcon::Hidden) - int(ControlIcon::SpreadHorizontal) + 1));
        phosphorGeometry(QString::fromLatin1(names[int(icon) - int(ControlIcon::SpreadHorizontal)]), path);
    } else if (icon == ControlIcon::OneColumn || icon == ControlIcon::TwoColumns || icon == ControlIcon::Sort) {
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(icon == ControlIcon::Sort ? QStringLiteral("sort-ascending")
                         : icon == ControlIcon::OneColumn ? QStringLiteral("text-align-justify")
                                                         : QStringLiteral("text-columns"), path);
    } else if (icon == ControlIcon::FolderPlus || icon == ControlIcon::Folders) {
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(icon == ControlIcon::FolderPlus ? QStringLiteral("folder-plus") : QStringLiteral("folders"), path);
    } else if (icon == ControlIcon::SelectionStart || icon == ControlIcon::SelectionEnd) {
        const bool start = icon == ControlIcon::SelectionStart;
        const int caret = start ? 12 : 52;
        line(caret, 12, caret, 52);
        line(caret - 6, 12, caret + 6, 12);
        line(caret - 6, 52, caret + 6, 52);
        const int left = start ? 26 : 8, right = start ? 56 : 38;
        line(left, 20, right, 20);
        line(left, 32, right, 32);
        line(left, 44, right - 8, 44);
    } else if (icon == ControlIcon::Annotate || icon == ControlIcon::Trash ||
               (icon >= ControlIcon::SaveHighlight && icon <= ControlIcon::RemoveHighlight)) {
        const char *name = "highlighter";
        switch (icon) {
        case ControlIcon::SaveHighlight:
            name = "floppy-disk";
            break;
        case ControlIcon::ClearSelection:
            name = "eraser";
            break;
        case ControlIcon::ExtendSelection:
            name = "selection";
            break;
        case ControlIcon::EditHighlight:
            name = "pencil-simple";
            break;
        case ControlIcon::RemoveHighlight:
        case ControlIcon::Trash:
            name = "trash";
            break;
        default:
            break;
        }
        result.canvas = 256;
        result.filled = true;
        phosphorGeometry(QString::fromLatin1(name), path);
    } else if (icon == ControlIcon::PreviousChapter || icon == ControlIcon::NextChapter) {
        result.canvas = 256;
        result.filled = true;
        QPainterPath caret;
        phosphorGeometry(
            icon == ControlIcon::PreviousChapter ? QStringLiteral("caret-left") : QStringLiteral("caret-right"), caret);
        path = QTransform::fromTranslate(-36, 0).map(caret);
        path.addPath(QTransform::fromTranslate(36, 0).map(caret));
    } else if (icon == ControlIcon::Swipe) {
        result.canvas = 24;
        result.stroke = 1.5;
        line(4, 2, 4, 12);
        path.moveTo(2, 4);
        path.lineTo(4, 2);
        path.lineTo(6, 4);
        path.moveTo(2, 10);
        path.lineTo(4, 12);
        path.lineTo(6, 10);
        path.moveTo(10, 21);
        path.lineTo(7, 16);
        path.cubicTo(6, 14, 8, 13, 10, 15);
        path.lineTo(10, 7);
        path.cubicTo(10, 5, 13, 5, 13, 7);
        path.lineTo(13, 12);
        path.lineTo(18, 13);
        path.cubicTo(21, 13, 22, 15, 21, 18);
        path.lineTo(20, 21);
        path.closeSubpath();
    } else if (icon >= ControlIcon::Rows && icon <= ControlIcon::Check) {
        // Kobalt's own drawings, on Phosphor's 24-unit grid with its stroke.
        result.canvas = 24;
        result.stroke = 1.5;
        if (icon == ControlIcon::Rows) {
            path.addRect(QRectF(3, 4, 4, 6));
            path.addRect(QRectF(3, 14, 4, 6));
            line(10, 5.5, 21, 5.5);
            line(10, 8.5, 17, 8.5);
            line(10, 15.5, 21, 15.5);
            line(10, 18.5, 17, 18.5);
        } else if (icon == ControlIcon::Grid) {
            path.addRect(QRectF(4, 3, 6.5, 8));
            path.addRect(QRectF(13.5, 3, 6.5, 8));
            path.addRect(QRectF(4, 13, 6.5, 8));
            path.addRect(QRectF(13.5, 13, 6.5, 8));
        } else if (icon == ControlIcon::Home) {
            path.moveTo(3, 11);
            path.lineTo(12, 3.5);
            path.lineTo(21, 11);
            path.moveTo(5.5, 9.5);
            path.lineTo(5.5, 20.5);
            path.lineTo(18.5, 20.5);
            path.lineTo(18.5, 9.5);
            path.moveTo(10, 20.5);
            path.lineTo(10, 14.5);
            path.lineTo(14, 14.5);
            path.lineTo(14, 20.5);
        } else {
            path.moveTo(4, 12.5);
            path.lineTo(9.5, 18);
            path.lineTo(20, 6.5);
        }
    } else if (icon == ControlIcon::RotationBoth) {
        path.addRoundedRect(QRectF(6, 10, 20, 36), 2, 2);
        path.addRoundedRect(QRectF(32, 24, 28, 22), 2, 2);
    } else if (icon >= ControlIcon::Portrait && icon <= ControlIcon::InvertedLandscape) {
        result.canvas = 24;
        result.filled = true;
        path.addRoundedRect(sideButtons ? QRectF(2, 2, 20, 20) : QRectF(4, 2, 16, 20), 1.5, 1.5);
        QPainterPath holes;
        holes.addRect(sideButtons ? QRectF(3.5, 3.5, 14, 17) : QRectF(5.5, 3.5, 13, 15));
        if (sideButtons) {
            holes.addRoundedRect(QRectF(19, 7, 1.5, 3), .75, .75);
            holes.addRoundedRect(QRectF(19, 14, 1.5, 3), .75, .75);
        }
        path = path.subtracted(holes);
        QTransform transform;
        transform.translate(12, 12);
        // Qt rotates the screen axes; the physical bezel moves the other way.
        transform.rotate(-90 * (int(icon) - int(ControlIcon::Portrait)));
        transform.translate(-12, -12);
        path = transform.map(path);
    } else {
        static const char *const names[] = {"magnifying-glass",
                                            "sun",
                                            "gear-fine",
                                            "layout",
                                            "x",
                                            "list",
                                            "compass",
                                            "chart-bar",
                                            "caret-left",
                                            "caret-right",
                                            "caret-up",
                                            "caret-down",
                                            "minus",
                                            "plus",
                                            "text-align-left",
                                            "text-align-justify",
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            nullptr,
                                            "arrow-left",
                                            "text-aa",
                                            "dots-three",
                                            "bug",
                                            "book-bookmark",
                                            "lock",
                                            "file-x",
                                            "link",
                                            "book-open",
                                            "arrow-u-up-left",
                                            "download-simple",
                                            "bluetooth",
                                            "eye",
                                            "wrench",
                                            "paint-brush-broad",
                                            "desktop",
                                            "translate",
                                            "info",
                                            "toolbox"};
        static_assert(sizeof(names) / sizeof(*names) == size_t(ControlIcon::System) + 1);
        const int index = int(icon);
        if (index >= 0 && index <= int(ControlIcon::System) && names[index]) {
            result.canvas = 256;
            result.filled = true;
            phosphorGeometry(QString::fromLatin1(names[index]), path);
        }
    }
    return result;
}

void paintGeometry(QPainter &painter, const Geometry &shape, ControlIcon glyph, const QRectF &rect, QColor color,
                   IconRendition rendition) {
    if (rect.isEmpty() || shape.path.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal side = qMin(rect.width(), rect.height());
    const QRectF square(rect.center().x() - side / 2, rect.center().y() - side / 2, side, side);
    painter.translate(square.topLeft());
    if (rendition == IconRendition::Action) {
        const QRectF bounds = shape.inkBounds();
        const qreal fraction =
            qreal(glyph == ControlIcon::Close ? int(metrics::control::closeInkSize) : int(metrics::control::actionInkSize)) /
            int(metrics::control::iconCanvas);
        // Phosphor regular assets share a 256-unit canvas and 16-unit strokes.
        // Fitting each silhouette separately changes that stroke weight. Use
        // the same scale, anchored to the Settings icon's 224-unit extent.
        const qreal extent = shape.canvas == 256 && glyph != ControlIcon::Close
                                 ? 224.0 : qMax(bounds.width(), bounds.height());
        const qreal factor = side * fraction / extent;
        painter.translate(side / 2, side / 2);
        painter.scale(factor, factor);
        painter.translate(-bounds.center());
    } else if (shape.canvas == 256 && (glyph < ControlIcon::WifiNone || glyph > ControlIcon::WifiMedium)) {
        // Phosphor artwork can sit off-center in its canvas: the moon is high
        // and to the right, the open book low. Keep the asset's size and
        // center its visible ink, so icons line up with their text. Partial
        // Wi-Fi bars keep the full signal's baseline.
        painter.translate(side / 2, side / 2);
        painter.scale(side / shape.canvas, side / shape.canvas);
        painter.translate(-shape.inkBounds().center());
    } else
        painter.scale(side / shape.canvas, side / shape.canvas);
    painter.setPen(shape.filled ? QPen(Qt::NoPen) : QPen(color, shape.stroke, Qt::SolidLine, shape.cap, shape.join));
    painter.setBrush(shape.filled ? QBrush(color) : QBrush(Qt::NoBrush));
    painter.drawPath(shape.path);
    painter.restore();
}

class VectorIconEngine final : public QIconEngine {
public:
    VectorIconEngine(ControlIcon glyph, QColor color, bool sideButtons,
                     IconRendition rendition = IconRendition::OriginalAsset, QColor active = QColor())
        : glyph(glyph), color(color), active(active.isValid() ? active : color), rendition(rendition),
          shape(geometry(glyph, rendition == IconRendition::Action ? false : sideButtons)) {
        QDataStream stream(&identity, QIODevice::WriteOnly);
        stream << qint32(glyph) << color << this->active << qint32(rendition)
               << (rendition == IconRendition::Action ? false : sideButtons);
    }
    QIconEngine *clone() const override {
        return new VectorIconEngine(*this);
    }
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override {
        const int side = qMin(size.width(), size.height());
        return QSize(side, side);
    }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        const QColor ink = state == QIcon::On ? active : color;
        const QTransform transform = painter->deviceTransform();
        const QRectF pixels = transform.mapRect(QRectF(rect));
        const auto whole = [](qreal value) {
            return qAbs(value - qRound(value)) < .000001;
        };
        // Reuse only a one-to-one pixel mapping. Fractional placement and
        // transformed composition keep the original vector painting contract.
        // Colored ink also stays direct: premultiplied bitmap composition can
        // round its antialiased edges differently by one channel value.
        if (!rect.isEmpty() && transform.type() <= QTransform::TxScale && transform.m11() > 0 &&
            qFuzzyCompare(transform.m11(), transform.m22()) && qFuzzyCompare(painter->opacity(), qreal(1)) &&
            (ink == QColor(Qt::black) || ink == QColor(Qt::white)) &&
            painter->compositionMode() == QPainter::CompositionMode_SourceOver && whole(pixels.x()) &&
            whole(pixels.y()) && whole(pixels.width()) && whole(pixels.height())) {
            const auto image = pixmap(QSize(qRound(pixels.width()), qRound(pixels.height())), mode, state);
            painter->drawPixmap(QRectF(rect), image, QRectF(QPointF(), image.size()));
            return;
        }
        render(painter, rect, mode, state);
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        if (size.isEmpty())
            return {};
        // Shared across short-lived icons too. The budget charges at least one
        // KiB per entry, bounding both bitmap storage and the number of entries.
        static QCache<QByteArray, QPixmap> cache(512);
        QByteArray key = identity;
        QDataStream stream(&key, QIODevice::Append);
        stream << qint32(density()) << size << qint32(mode) << qint32(state);
        if (const auto *cached = cache.object(key))
            return *cached;
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        render(&painter, QRect(QPoint(), size), mode, state);
        painter.end();
        const qint64 bytes = qint64(size.width()) * size.height() * 4;
        if (bytes <= 512 * 1024)
            cache.insert(key, new QPixmap(result), int((bytes + 1023) / 1024));
        return result;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QSize pixels = size;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        // Qt 6.8 changed this argument from physical to logical pixels.
        pixels *= scale;
#endif
        QPixmap result = pixmap(pixels, mode, state);
        result.setDevicePixelRatio(scale);
        return result;
    }
#elif QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    void virtual_hook(int id, void *data) override {
        if (id == QIconEngine::ScaledPixmapHook) {
            auto *argument = static_cast<QIconEngine::ScaledPixmapArgument *>(data);
            argument->pixmap = pixmap(argument->size, argument->mode, argument->state);
            argument->pixmap.setDevicePixelRatio(argument->scale);
            return;
        }
        QIconEngine::virtual_hook(id, data);
    }
#endif

private:
    void render(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) {
        painter->save();
        if (mode == QIcon::Disabled)
            painter->setOpacity(painter->opacity() * .4);
        paintGeometry(*painter, shape, glyph, rect, state == QIcon::On ? active : color, rendition);
        painter->restore();
    }
    QByteArray identity;
    ControlIcon glyph;
    QColor color, active;
    IconRendition rendition;
    Geometry shape;
};
}

void paintOpeningIcon(QPainter &painter, ControlIcon icon, const QRectF &bounds, QColor color) {
    paintGeometry(painter, geometry(icon, false), icon, bounds, color, IconRendition::OriginalAsset);
}
void paintWifiStatusIcon(QPainter &painter, ControlIcon icon, const QRectF &bounds, QColor color) {
    const auto reference = geometry(ControlIcon::Bluetooth, false);
    const auto shape = geometry(icon, false);
    const auto full = geometry(icon == ControlIcon::WifiDisconnected ? icon : ControlIcon::WifiHigh, false).inkBounds();
    const qreal side = qMin(bounds.width(), bounds.height());
    const qreal scale = 0.9 * side * reference.inkBounds().height() / reference.canvas / full.height();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(bounds.center());
    painter.scale(scale, scale);
    painter.translate(-full.center());
    painter.fillPath(shape.path, color);
    painter.restore();
}
QIcon controlIcon(ControlIcon icon, QColor color, bool sideButtons) {
    if (int(icon) < 0 ||
        (int(icon) > int(ControlIcon::System) && icon != ControlIcon::BookmarkAdd && icon != ControlIcon::BookmarkRemove && icon != ControlIcon::ExportAction && icon != ControlIcon::FitImage && icon != ControlIcon::Door &&
         icon != ControlIcon::PreviousChapter &&
         icon != ControlIcon::NextChapter && icon != ControlIcon::Sync && icon != ControlIcon::Sort &&
         icon != ControlIcon::ArrowDownRight && icon != ControlIcon::EditHighlight &&
         (icon < ControlIcon::Rows || icon > ControlIcon::Export) &&
         (icon < ControlIcon::KeyboardShift || icon > ControlIcon::KeyboardReturn) &&
         (icon < ControlIcon::SpreadHorizontal || icon > ControlIcon::Diamond)))
        return {};
    return QIcon(new VectorIconEngine(icon, color, sideButtons));
}
void Button::setIconConfiguration(ButtonIcon configuration) {
    if (!hasConfiguredIcon || !(configuredIcon == configuration)) {
        configuredIcon = configuration;
        hasConfiguredIcon = true;
        hasIconTheme = false;
    }
    refreshIcon();
}

void Button::clearIconConfiguration() {
    hasConfiguredIcon = hasIconTheme = false;
    setIcon({});
}

void Button::refreshIcon() {
    if (!hasConfiguredIcon)
        return;
    const bool dark = darkTheme(this);
    if (hasIconTheme && iconDark == dark)
        return;
    const auto &configuration = configuredIcon;
    const auto &colors = themeColors(dark);
    setIcon(QIcon(new VectorIconEngine(configuration.glyph, colors.ink, configuration.rotationSideButtons,
                                       configuration.rendition, colors.paper)));
    iconDark = dark;
    hasIconTheme = true;
}

void buttonIcon(QPushButton *button, ControlIcon icon) {
    auto *control = dynamic_cast<Button *>(button);
    Q_ASSERT(control);
    if (control)
        control->setIconConfiguration({icon});
}

void refreshButtonIcon(QPushButton *button) {
    if (auto *control = dynamic_cast<Button *>(button))
        control->refreshIcon();
}

void iconAction(QPushButton *button, ControlIcon icon, const QString &label) {
    auto *control = dynamic_cast<Button *>(button);
    Q_ASSERT(control);
    if (!control)
        return;
    control->setPaintMode(ButtonPaintMode::OutlinedAction);
    control->setText({});
    control->setAccessibleName(label);
    control->setIconConfiguration({icon, IconRendition::Action});
    control->setIconSize(QSize(metrics::control::iconSize, metrics::control::iconSize));
}

}
