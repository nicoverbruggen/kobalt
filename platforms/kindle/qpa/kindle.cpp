#include "touch_state.h"
#include "pointer_delivery.h"
#include "sleep_frame.h"
#include "mtk_display.h"
#include <QHash>
#include <QDebug>
#include <QPointer>
#include <memory>

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSocketNotifier>
#include <QVariant>
#include <QWindow>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qgenericunixfontdatabase_p.h>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformintegrationplugin.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

class KindleScreen final : public QObject, public QPlatformScreen {
public:
    KindleScreen()
    {
        if (!display.open())
            qFatal("kindle: %s", qPrintable(display.error()));
        qApp->setProperty("kobalt.swipeAvailable", display.swipeAvailable());
        const auto &layout = display.layout();
        frame = kindle::mtk::initialFrame(QSize(layout.width, layout.height));
        queue = std::make_unique<kindle::RefreshQueue>([this](const kindle::RefreshFrame &value) {
            return display.present(value);
        });
        connect(queue.get(), &kindle::RefreshQueue::completed, this, [this](quint64 serial, quint64 epoch) {
            const auto token = submitted.take(serial);
            if (epoch == queue->epoch() && token.window) {
                kindle::sleepFramePresented(*token.window, token.sleepGeneration);
                if (token.exitFrame && token.window->property("kobalt.exitRequested").toBool())
                    token.window->setProperty("kobalt.exitPresented", true);
            }
        }, Qt::QueuedConnection);
        connect(queue.get(), &kindle::RefreshQueue::discarded, this,
                [this](quint64 serial) { submitted.remove(serial); }, Qt::QueuedConnection);
        connect(queue.get(), &kindle::RefreshQueue::failed, this, [](const QString &error) {
            qCritical().noquote() << error;
            qApp->setProperty("kobalt.displayFailure", error);
        }, Qt::QueuedConnection);
        qApp->installEventFilter(this);
    }
    ~KindleScreen() override
    {
        if (qApp)
            qApp->removeEventFilter(this);
        queue->stop();
    }
    QRect geometry() const override { return QRect(QPoint(), frame.size()); }
    int depth() const override { return 32; }
    QImage::Format format() const override { return QImage::Format_RGB32; }
    QDpi logicalDpi() const override { return QDpi(96, 96); }
    QSizeF physicalSize() const override { return QSizeF(frame.width() * 25.4 / 300, frame.height() * 25.4 / 300); }
    QPixmap grabWindow(WId, int x, int y, int width, int height) const override
    {
        return QPixmap::fromImage(frame.copy(x, y, width < 0 ? frame.width() - x : width,
                                           height < 0 ? frame.height() - y : height));
    }
    bool present(QWindow *window, const QRect &damage, bool full)
    {
        if (damage.isEmpty())
            return false;
        if (!++serial)
            ++serial;
        const auto generation = window->property("kobalt.displaySleeping").toBool()
            ? window->property("kobalt.sleepRequested").toULongLong() : 0;
        const int direction = window->property("kobalt.pageTurnDirection").toInt();
        window->setProperty("kobalt.pageTurnDirection", 0);
        submitted.insert(serial, {window, generation, window->property("kobalt.exitRequested").toBool()});
        // QImage detaches on the next GUI paint, retaining this exact snapshot
        // until the worker's completion fence. No worker touches a QWindow.
        if (!queue->enqueue({frame, damage, serial, queue->epoch(), full,
                             generation || window->property("kobalt.exitRequested").toBool() ? 0 : direction})) {
            submitted.remove(serial);
            return false;
        }
        return true;
    }
    QImage frame;
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::DynamicPropertyChange && qobject_cast<QWindow *>(object)) {
            const auto name = static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName();
            if (name == "kobalt.cancelAnimation")
                queue->cancelAnimation();
            if (name == "kobalt.displaySleeping" || name == "kobalt.sleepRequested" || name == "kobalt.exitRequested")
                queue->invalidate();
        }
        return false;
    }
private:
    struct Token { QPointer<QWindow> window; quint64 sleepGeneration; bool exitFrame; };
    kindle::mtk::Display display;
    std::unique_ptr<kindle::RefreshQueue> queue;
    QHash<quint64, Token> submitted;
    quint64 serial = 0;
};

class KindleWindow final : public QPlatformWindow {
public:
    explicit KindleWindow(QWindow *window) : QPlatformWindow(window) {}
    void requestActivateWindow() override
    {
        QWindowSystemInterface::handleFocusWindowChanged(window());
        QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationActive);
    }
    void setVisible(bool visible) override
    {
        if (visible) {
            setGeometry(screen()->geometry());
            QWindowSystemInterface::handleGeometryChange(window(), geometry());
            requestActivateWindow();
        }
        QWindowSystemInterface::handleExposeEvent(window(), visible ? QRegion(QRect(QPoint(), geometry().size())) : QRegion());
    }
};

class KindleBackingStore final : public QPlatformBackingStore {
public:
    KindleBackingStore(QWindow *window, KindleScreen *screen) : QPlatformBackingStore(window), screen_(screen) {}
    QPaintDevice *paintDevice() override { return &image_; }
    void resize(const QSize &size, const QRegion &) override
    {
        image_ = QImage(size, QImage::Format_RGB32);
        image_.fill(Qt::white);
    }
    void flush(QWindow *window, const QRegion &region, const QPoint &offset) override
    {
        // A native dialog has its own backing store. It must not draw over a
        // sleeping root surface or consume that surface's final-frame token.
        for (auto *surface : QGuiApplication::topLevelWindows())
            if (!kindle::displayFlushAllowed(*window, surface))
                return;
        if (!kindle::displayFlushAllowed(*window))
            return;
        const bool full = first_ || window->property("kobalt.fullRefresh").toBool() ||
            window->property("kobalt.displaySleeping").toBool();
        window->setProperty("kobalt.fullRefresh", false);
        const QRect source = full ? image_.rect() : region.boundingRect().translated(offset).intersected(image_.rect());
        const QPoint destination = window->geometry().topLeft() + source.topLeft() - offset;
        {
            QPainter painter(&screen_->frame);
            painter.drawImage(destination, image_, source);
        }
        if (screen_->present(window, QRect(destination, source.size()), full)) {
            first_ = false;
        } else {
            window->setProperty("kobalt.fullRefresh", true);
        }
    }
private:
    KindleScreen *screen_;
    QImage image_;
    bool first_ = true;
};

class KindleTouch final : public QObject {
public:
    explicit KindleTouch(KindleScreen *screen) : screen_(screen)
    {
        qApp->installEventFilter(this);
        QStringList devices;
        const QString selected = qEnvironmentVariable("KOBALT_KINDLE_TOUCH_DEVICE");
        if (!selected.isEmpty()) devices << selected;
        else {
            const QDir input(QStringLiteral("/dev/input"));
            for (const QString &name : input.entryList({QStringLiteral("event*")}, QDir::System | QDir::Files, QDir::Name))
                devices << input.filePath(name);
        }
        for (const QString &device : devices) {
            const int candidate = ::open(QFile::encodeName(device).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (candidate < 0) continue;
            input_absinfo x{}, y{}, slotInfo{};
            const bool mt = ioctl(candidate, EVIOCGABS(ABS_MT_POSITION_X), &x) == 0
                         && ioctl(candidate, EVIOCGABS(ABS_MT_POSITION_Y), &y) == 0;
            if (!mt || x.maximum <= x.minimum || y.maximum <= y.minimum) {
                ::close(candidate);
                continue;
            }
            if (ioctl(candidate, EVIOCGRAB, 1) < 0) {
                qWarning("kindle: cannot exclusively grab touch device %s", qPrintable(device));
                ::close(candidate);
                continue;
            }
            fd_ = candidate;
            x_ = x;
            y_ = y;
            slots_ = ioctl(fd_, EVIOCGABS(ABS_MT_SLOT), &slotInfo) == 0;
            if (slots_) {
                // Protocol B omits unchanged axes, including on a reused
                // slot. Snapshot every slot before consuming its deltas.
                if (slotInfo.minimum != 0 || slotInfo.maximum < 0 || slotInfo.maximum >= kindle::TouchState::maximumSlots)
                    qFatal("kindle: unsupported multitouch slot range");
                slotCount_ = slotInfo.maximum + 1;
                snapshotSlots();
            }
            char name[128]{};
            ioctl(fd_, EVIOCGNAME(sizeof(name)), name);
            qInfo("kindle: touch %s (%s), x %d..%d y %d..%d, %s", qPrintable(device), name,
                  x.minimum, x.maximum, y.minimum, y.maximum, slots_ ? "slots" : "packets");
            auto *notifier = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
            connect(notifier, &QSocketNotifier::activated, this, [this, notifier] {
                input_event events[32];
                for (;;) {
                    const ssize_t count = ::read(fd_, events, sizeof(events));
                    if (count < 0 && errno == EINTR) continue;
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    if (count <= 0 || count % sizeof(input_event)) {
                        release();
                        notifier->setEnabled(false);
                        qWarning("kindle: touch device disconnected or invalid input record");
                        break;
                    }
                    for (size_t i = 0; i < size_t(count) / sizeof(input_event); ++i) consume(events[i]);
                }
            });
            return;
        }
        qFatal("kindle: no usable multitouch device; set KOBALT_KINDLE_TOUCH_DEVICE to its event path");
    }
    ~KindleTouch() override
    {
        // Qt can destroy the platform integration after clearing qApp.
        if (qApp)
            qApp->removeEventFilter(this);
        if (fd_ >= 0) {
            ioctl(fd_, EVIOCGRAB, 0);
            ::close(fd_);
        }
    }
private:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::DynamicPropertyChange &&
            static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "kobalt.inputBlocked") {
            auto *window = qobject_cast<QWindow *>(object);
            if (window) {
                const bool blocked = window->property("kobalt.inputBlocked").toBool();
                const bool entering = blocked && !gate_.blocked;
                gate_.setBlocked(blocked, touch_.down);
                if (entering) {
                    release();
                }
            }
        }
        return QObject::eventFilter(object, event);
    }
    void snapshotSlots()
    {
        input_absinfo slotInfo{};
        if (ioctl(fd_, EVIOCGABS(ABS_MT_SLOT), &slotInfo) < 0)
            qFatal("kindle: cannot read current multitouch slot");
        touch_.slot = slotInfo.value;
        int values[kindle::TouchState::maximumSlots + 1]{};
        const size_t length = size_t(slotCount_ + 1) * sizeof(int);
        for (const auto axis : {ABS_MT_POSITION_X, ABS_MT_POSITION_Y}) {
            values[0] = axis;
            if (ioctl(fd_, EVIOCGMTSLOTS(length), values) < 0)
                qFatal("kindle: cannot read multitouch slot state");
            for (int slot = 0; slot < slotCount_; ++slot)
                (axis == ABS_MT_POSITION_X ? touch_.positions[slot].x : touch_.positions[slot].y) = values[slot + 1];
        }
    }
    void send(ulong timestamp, bool cancelled = false)
    {
        if (!touch_.down && !touch_.sentDown) return;
        double x = kindle::axisPosition(touch_.x, x_.minimum, x_.maximum);
        double y = kindle::axisPosition(touch_.y, y_.minimum, y_.maximum);
        // The Kindle input driver reports logical panel coordinates. Its
        // axes do not follow the framebuffer controller's native rotation.
        const QPointF point(x * (screen_->frame.width() - 1), y * (screen_->frame.height() - 1));
        QWindow *window = QGuiApplication::focusWindow();
        if (!window) return;
        if (cancelled)
            kindle::cancelPointer(window, point - window->position(), point, timestamp);
        else
            kindle::deliverPointer(window, point - window->position(), point, touch_.down, touch_.sentDown, timestamp);
    }
    void release()
    {
        touch_.down = false;
        send(lastTimestamp_, true);
        touch_.frameSent();
    }
    void consume(const input_event &event)
    {
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            release();
            touch_.dropped = true;
            return;
        }
        // Wait for a fresh contact after a lost frame instead of inventing a drag.
        if (touch_.dropped) {
            if (event.type == EV_SYN && event.code == SYN_REPORT) {
                touch_.dropped = false;
                if (slots_) snapshotSlots();
            }
            return;
        }
        if (event.type == EV_ABS) {
            if (event.code == ABS_MT_SLOT) touch_.slot = event.value;
            else if (!touch_.skipPacket && event.code == ABS_MT_TRACKING_ID) touch_.tracking(event.value);
            else if (event.code == ABS_MT_POSITION_X || event.code == ABS_MT_POSITION_Y) {
                if (!slots_ && !touch_.skipPacket) touch_.contact(true);
                touch_.position(event.code == ABS_MT_POSITION_X, event.value);
            }
        } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
            touch_.contact(event.value != 0, slots_);
        } else if (event.type == EV_SYN && event.code == SYN_MT_REPORT && !slots_) {
            touch_.skipPacket = true;
        } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
            lastTimestamp_ = ulong(event.time.tv_sec) * 1000 + ulong(event.time.tv_usec) / 1000;
            if (gate_.deliver(touch_.down))
                send(lastTimestamp_);
            touch_.frameSent();
            if (!slots_) {
                touch_.slot = 0;
                // An empty protocol A frame means every contact was lifted.
                touch_.down = false;
            }
        }
    }
    KindleScreen *screen_;
    int fd_ = -1;
    input_absinfo x_{}, y_{};
    bool slots_ = false;
    kindle::ContactGate gate_;
    int slotCount_ = 0;
    ulong lastTimestamp_ = 0;
    kindle::TouchState touch_;
};

class KindleIntegration final : public QPlatformIntegration {
public:
    KindleIntegration() : screen_(new KindleScreen) {}
    ~KindleIntegration() override
    {
        delete touch_;
        QWindowSystemInterface::handleScreenRemoved(screen_);
    }
    void initialize() override
    {
        QWindowSystemInterface::handleScreenAdded(screen_);
        touch_ = new KindleTouch(screen_);
    }
    bool hasCapability(Capability capability) const override
    {
        if (capability == ThreadedPixmaps) return true;
        if (capability == WindowManagement || capability == RhiBasedRendering) return false;
        return QPlatformIntegration::hasCapability(capability);
    }
    QPlatformWindow *createPlatformWindow(QWindow *window) const override { return new KindleWindow(window); }
    QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override
    { return new KindleBackingStore(window, screen_); }
    QAbstractEventDispatcher *createEventDispatcher() const override { return createUnixEventDispatcher(); }
    QPlatformFontDatabase *fontDatabase() const override { return &fonts_; }
private:
    KindleScreen *screen_;
    KindleTouch *touch_ = nullptr;
    mutable QGenericUnixFontDatabase fonts_;
};

class KindlePlugin final : public QPlatformIntegrationPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "kindle.json")
public:
    QPlatformIntegration *create(const QString &key, const QStringList &) override
    { return key.compare(QStringLiteral("kindle"), Qt::CaseInsensitive) == 0 ? new KindleIntegration : nullptr; }
};

} // namespace

#include "kindle.moc"
