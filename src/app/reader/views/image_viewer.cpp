#include "image_viewer.h"
#include "ui/controls/buttons.h"
#include "ui/style/fonts.h"
#include "ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include "support/strings.h"
#include "support/qt_compat.h"
#include "ui/input/input.h"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineF>
#include <QMap>
#include <QElapsedTimer>
#include <QPainter>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QTimer>
#include <cmath>

namespace kobalt {
namespace chrome = ui::metrics::chrome;

class ImageCanvas final : public QWidget
{
public:
    explicit ImageCanvas(QWidget *parent) : QWidget(parent)
    {
        setObjectName("rrImageCanvas");
        setProperty("kobalt_native_touch", true);
        setAttribute(Qt::WA_AcceptTouchEvents);
        setAttribute(Qt::WA_OpaquePaintEvent);
        previewTimer.setSingleShot(true);
        previewTimer.setTimerType(Qt::PreciseTimer);
        connect(&previewTimer, &QTimer::timeout, this, [this] { present(false); });
        wheelTimer.setSingleShot(true);
        wheelTimer.setInterval(previewMilliseconds);
        connect(&wheelTimer, &QTimer::timeout, this, [this] {
            wheelActive = false;
            present(true);
        });
    }

    std::function<void(QJsonObject)> rememberView;
    std::function<void(bool, qreal)> controlsChanged;

    QJsonObject view() const
    {
        if (image.isNull())
            return {};
        const auto shown = image.size() * scale(zoom);
        return qt::object({ { "zoom", zoom },
                            { "x", .5 - offset.x() / shown.width() },
                            { "y", .5 - offset.y() / shown.height() } });
    }

    void setImage(QImage value, const QJsonObject &view)
    {
        image = std::move(value);
        ready = true;
        zoom = 1;
        offset = {};
        const qreal level = view.value("zoom").toDouble(1);
        const qreal x = view.value("x").toDouble(.5), y = view.value("y").toDouble(.5);
        if (!image.isNull() && std::isfinite(level) && level >= 1 && level <= 8 && std::isfinite(x)
            && x >= 0 && x <= 1 && std::isfinite(y) && y >= 0 && y <= 1) {
            zoom = level;
            const auto shown = image.size() * scale(zoom);
            offset = QPointF((.5 - x) * shown.width(), (.5 - y) * shown.height());
        }
        changed();
    }

    void clear()
    {
        previewTimer.stop();
        wheelTimer.stop();
        previewClock.invalidate();
        wheelActive = false;
        image = {};
        ready = false;
        contacts.clear();
        mouseDown = false;
        tapPossible = false;
        lastTap.invalidate();
        fit();
    }

    void fit()
    {
        zoom = 1;
        offset = {};
        changed();
    }

    void zoomBy(qreal factor, QPointF anchor)
    {
        if (image.isNull())
            return;
        const qreal next = qBound(qreal(1), zoom * factor, qreal(8));
        const QPointF center = QRectF(rect()).center();
        offset = anchor - center - (anchor - center - offset) * (next / zoom);
        zoom = next;
        changed();
    }

    void zoomBy(qreal factor) { zoomBy(factor, QRectF(rect()).center()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());
        if (image.isNull()) {
            painter.setPen(palette().windowText().color());
            painter.setFont(ui::interfaceFont(chrome::labelText));
            ui::drawInterfaceText(painter, rect(),
                                  strings::get(ready ? "image.unavailable" : "image.loading"),
                                  Qt::AlignCenter);
            return;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSizeF shown = image.size() * scale(presentedZoom);
        const QPointF top = QRectF(rect()).center() + presentedOffset
                - QPointF(shown.width(), shown.height()) / 2;
        painter.drawImage(QRectF(top, shown), image);
    }

    void resizeEvent(QResizeEvent *) override { changed(); }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (!ui::isPointerMouse(event) || event->button() != Qt::LeftButton)
            return;
        mouseDown = true;
        platform::pageRefresh(this, false);
        lastMouse = ui::mousePosition(event);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!mouseDown || !ui::isPointerMouse(event))
            return;
        const auto point = ui::mousePosition(event);
        offset += point - lastMouse;
        lastMouse = point;
        changed();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!ui::isPointerMouse(event))
            return;
        mouseDown = false;
        present(true);
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (!ui::isPointerMouse(event) || event->button() != Qt::LeftButton)
            return;
        if (zoom > 1)
            fit();
        else
            zoomBy(2, ui::mousePosition(event));
        event->accept();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        wheelActive = true;
        zoomBy(event->angleDelta().y() > 0 ? 1.25 : .8);
        wheelTimer.start();
        event->accept();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::TouchCancel) {
            contacts.clear();
            tapPossible = false;
            lastTap.invalidate();
            present(true);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate
            || event->type() == QEvent::TouchEnd) {
            const auto points = ui::touchContacts(static_cast<QTouchEvent *>(event));
            if (event->type() == QEvent::TouchBegin) {
                platform::pageRefresh(this, false);
                tapPossible = points.size() == 1;
                tapOrigin = points.isEmpty() ? QPointF() : points.first().position;
                touchTime.start();
            }
            QMap<int, QPointF> next;
            for (const auto &contact : points) {
                if (points.size() != 1
                    || (contact.position - tapOrigin).manhattanLength()
                            > ui::metrics::lookup::moveTolerance)
                    tapPossible = false;
                if (!contact.released || event->type() == QEvent::TouchEnd)
                    next.insert(contact.id, contact.position);
            }
            if (next.size() == 1 && contacts.size() == 1
                && next.firstKey() == contacts.firstKey()) {
                offset += next.first() - contacts.first();
                changed();
            } else if (next.size() == 2 && contacts.size() == 2 && next.keys() == contacts.keys()) {
                const auto oldPoints = contacts.values(), newPoints = next.values();
                const qreal before = QLineF(oldPoints[0], oldPoints[1]).length();
                const qreal after = QLineF(newPoints[0], newPoints[1]).length();
                const auto oldCenter = (oldPoints[0] + oldPoints[1]) / 2;
                const auto newCenter = (newPoints[0] + newPoints[1]) / 2;
                if (before > 1 && after > 1)
                    zoomBy(after / before, oldCenter);
                offset += newCenter - oldCenter;
                changed();
            }
            contacts = std::move(next);
            if (event->type() == QEvent::TouchEnd) {
                contacts.clear();
                if (tapPossible && touchTime.elapsed() < 300) {
                    if (lastTap.isValid() && lastTap.elapsed() < 350
                        && (tapOrigin - previousTap).manhattanLength()
                                < ui::metrics::lookup::moveTolerance * 2) {
                        if (zoom > 1)
                            fit();
                        else
                            zoomBy(2, tapOrigin);
                        lastTap.invalidate();
                    } else {
                        lastTap.start();
                        previousTap = tapOrigin;
                    }
                } else
                    lastTap.invalidate();
                present(true);
            }
            event->accept();
            return true;
        }
        return QWidget::event(event);
    }

private:
    qreal scale(qreal factor) const
    {
        return image.isNull()
                ? 1
                : qMin(qreal(width()) / image.width(), qreal(height()) / image.height()) * factor;
    }

    void changed()
    {
        const QSizeF shown = image.size() * scale(zoom);
        const qreal x = qMax(qreal(0), (shown.width() - width()) / 2);
        const qreal y = qMax(qreal(0), (shown.height() - height()) / 2);
        offset.setX(qBound(-x, offset.x(), x));
        offset.setY(qBound(-y, offset.y(), y));
        setProperty("imageZoom", zoom);
        setProperty("imageOffset", offset);
        setProperty("imageReady", !image.isNull());
        setProperty("imageSize", image.size());
        if (controlsChanged)
            controlsChanged(!image.isNull(), zoom);
        if (!mouseDown && contacts.isEmpty() && !wheelActive) {
            present(true);
        } else if (!previewClock.isValid() || previewClock.elapsed() >= previewMilliseconds) {
            present(false);
        } else if (!previewTimer.isActive()) {
            previewTimer.start(qMax(1, int(previewMilliseconds - previewClock.elapsed())));
        }
    }

    void present(bool clean)
    {
        previewTimer.stop();
        presentedZoom = zoom;
        presentedOffset = offset;
        setProperty("imagePreviewZoom", presentedZoom);
        setProperty("imagePreviewOffset", presentedOffset);
        previewClock.start();
        if (isVisible())
            platform::pageRefresh(this, clean);
        update();
        if (clean && rememberView && !image.isNull())
            rememberView(view());
    }

    // Keep the latest input, but resample the image at most ten times a second
    // during a gesture. Other paints must also use this last presented state.
    static constexpr int previewMilliseconds = 100;
    QImage image;
    qreal zoom = 1, presentedZoom = 1;
    QPointF offset, presentedOffset, lastMouse;
    QPointF tapOrigin, previousTap;
    QElapsedTimer touchTime, lastTap;
    QElapsedTimer previewClock;
    QTimer previewTimer, wheelTimer;
    QMap<int, QPointF> contacts;
    bool ready = false, mouseDown = false, tapPossible = false, wheelActive = false;
};

ImageViewer::ImageViewer(QWidget *parent) : Panel(parent)
{
    setObjectName("rrImageViewer");
    setFocusPolicy(Qt::StrongFocus);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *header = new ui::Panel(this, Qt::BottomEdge);
    header->setObjectName("rrImageHeader");
    const int rule = ui::metrics::control::border;
    header->setFixedHeight(chrome::actionHeight + rule);
    auto *row = new QHBoxLayout(header);
    // Children paint opaque backgrounds. Keep the shared rule outside their cells.
    row->setContentsMargins(0, 0, 0, rule);
    row->setSpacing(0);
    auto *title = new ui::InterfaceLabel(strings::get("image.title"), header);
    title->setObjectName("rrImageTitle");
    title->setFont(ui::brandFont(chrome::titleText));
    ui::readerStyle(title, ui::styles::brand(chrome::titleText) + "background:transparent;");
    title->setAlignment(Qt::AlignCenter);
    row->addSpacing(chrome::cellWidth);
    row->addWidget(title, 1);
    auto *close = new ui::CellButton(ui::ControlIcon::Close, header);
    close->setObjectName("rrImageClose");
    close->setRule(Qt::LeftEdge);
    close->setFixedSize(chrome::cellWidth, chrome::actionHeight);
    close->setAccessibleName(strings::get("image.close"));
    row->addWidget(close);
    connect(close, &QPushButton::clicked, this, [this] { closeImage(); });
    layout->addWidget(header);
    canvas = new ImageCanvas(this);
    canvas->rememberView = [this](QJsonObject view) {
        if (!imageId.isEmpty() && rememberView)
            rememberView(imageId, std::move(view));
    };
    layout->addWidget(canvas, 1);
    auto *controls = new ui::Panel(this, Qt::TopEdge);
    controls->setObjectName("rrImageControls");
    controls->setFixedHeight(chrome::actionHeight + rule);
    auto *buttons = new QHBoxLayout(controls);
    buttons->setContentsMargins(0, rule, 0, 0);
    buttons->setSpacing(0);
    auto *minus = new ui::CellButton(ui::ControlIcon::Minus, controls);
    minus->setObjectName("rrImageZoomOut");
    minus->setAccessibleName(strings::get("image.zoom_out"));
    minus->setRule(Qt::LeftEdge);
    minus->setFixedSize(chrome::cellWidth, chrome::actionHeight);
    auto *fit = new ui::CellButton(ui::ControlIcon::FitImage, controls);
    fit->setObjectName("rrImageFit");
    fit->setAccessibleName(strings::get("image.fit"));
    fit->setToolTip(strings::get("image.fit"));
    fit->setRule(Qt::RightEdge);
    fit->setFixedSize(chrome::cellWidth, chrome::actionHeight);
    auto *plus = new ui::CellButton(ui::ControlIcon::Plus, controls);
    plus->setObjectName("rrImageZoomIn");
    plus->setAccessibleName(strings::get("image.zoom_in"));
    plus->setRule(Qt::LeftEdge);
    plus->setFixedSize(chrome::cellWidth, chrome::actionHeight);
    buttons->addWidget(fit);
    buttons->addStretch(1);
    buttons->addWidget(minus);
    buttons->addWidget(plus);
    canvas->controlsChanged = [minus, plus, fit](bool ready, qreal zoom) {
        minus->setEnabled(ready && zoom > 1);
        plus->setEnabled(ready && zoom < 8);
        fit->setEnabled(ready);
    };
    canvas->controlsChanged(false, 1);
    connect(minus, &QPushButton::clicked, this, [this] { canvas->zoomBy(.8); });
    connect(plus, &QPushButton::clicked, this, [this] { canvas->zoomBy(1.25); });
    connect(fit, &QPushButton::clicked, this, [this] { canvas->fit(); });
    layout->addWidget(controls);
    platform::pageRefresh(this, true);
    hide();
}

void ImageViewer::open()
{
    imageId.clear();
    canvas->clear();
    show();
    raise();
    setFocus();
}

void ImageViewer::setImage(QImage image, const QJsonObject &view)
{
    imageId = image.text("kobalt.image_id");
    canvas->setImage(std::move(image), view);
}

void ImageViewer::closeImage()
{
    if (!imageId.isEmpty() && rememberView)
        rememberView(imageId, canvas->view());
    imageId.clear();
    hide();
    canvas->clear();
    if (dismissed)
        dismissed();
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Back)
        closeImage();
    else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)
        canvas->zoomBy(1.25);
    else if (event->key() == Qt::Key_Minus)
        canvas->zoomBy(.8);
    else if (event->key() == Qt::Key_0)
        canvas->fit();
    event->accept();
}
} // namespace kobalt
