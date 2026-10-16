#include "icons.h"
#include "buttons.h"
#include "../style/metrics.h"
#include "../style/theme.h"
#include <QIconEngine>
#include <QCache>
#include <QDataStream>
#include <QIODevice>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QTransform>
#include <QVariant>
#include <reader_icon_vectors.h>

namespace kobalt::ui {
namespace m = metrics;
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
    // Phosphor has no vertical swipe gesture, and the device drawings show
    // the side buttons, so these stay Kobalt's own artwork.
    if (icon == ControlIcon::Swipe) {
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
                                            "bookmark-simple",
                                            "lock",
                                            "file-x",
                                            "link",
                                            "book-open",
                                            "arrow-u-up-left",
                                            "download-simple",
                                            "bluetooth",
                                            "eye",
                                            "wrench",
                                            "desktop",
                                            "translate",
                                            "info",
                                            "flask"};
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
            qreal(glyph == ControlIcon::Close ? int(m::control::closeInkSize) : int(m::control::actionInkSize)) /
            int(m::control::iconCanvas);
        const qreal factor = side * fraction / qMax(bounds.width(), bounds.height());
        painter.translate(side / 2, side / 2);
        painter.scale(factor, factor);
        painter.translate(-bounds.center());
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
QIcon controlIcon(ControlIcon icon, QColor color, bool sideButtons) {
    if (int(icon) < 0 || int(icon) > int(ControlIcon::System))
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

void Button::refreshIcon() {
    if (!hasConfiguredIcon)
        return;
    const bool dark = darkTheme(this);
    if (hasIconTheme && iconDark == dark)
        return;
    const auto &configuration = configuredIcon;
    const QColor foreground = dark ? Qt::white : Qt::black;
    const QColor background = dark ? Qt::black : Qt::white;
    setIcon(QIcon(new VectorIconEngine(configuration.glyph, foreground, configuration.rotationSideButtons,
                                       configuration.rendition, background)));
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
    control->setIconSize(QSize(m::control::iconSize, m::control::iconSize));
}

}
