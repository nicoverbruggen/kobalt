#include "platforms/kobo/common/native_ui.h"
#include "platforms/kobo/common/native_wifi_power.h"
#include <QDBusError>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QObject>
#include <dlfcn.h>
#include <utility>

namespace kobalt {
namespace {
QString bluetoothServiceName()
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (qgetenv("PLATFORM") == "mt8113t-ntx")
        return QStringLiteral("com.kobo.mtk.bluedroid");
#endif
    return QStringLiteral("org.bluez");
}

class BluetoothStatus final : public QObject
{
    Q_OBJECT
public:
    BluetoothStatus(QObject *context, std::function<void(bool)> changed)
        : QObject(context), changed(std::move(changed)), serviceName(bluetoothServiceName())
    {
        // Qt5.2 does not initialize a default-constructed elapsed timer.
        lastHeartbeat.invalidate();
        const auto manager = reinterpret_cast<QObject *(*)()>(
                dlsym(RTLD_DEFAULT, "_ZN16BluetoothManager14sharedInstanceEv"));
        if (manager) {
            if (auto *object = manager())
                connect(object, SIGNAL(adapterPowered(bool)), this, SLOT(powered(bool)));
        }
        auto *service = new QDBusServiceWatcher(serviceName, QDBusConnection::systemBus(),
                                                QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(service, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &owner) {
                    powered(false);
                    if (!owner.isEmpty())
                        query();
                });
        query();
    }

    void activity(QKeyEvent *event)
    {
        if (!enabled || !event || event->type() != QEvent::KeyPress)
            return;
        const int key = event->key();
        if (key != Qt::Key_Left && key != Qt::Key_Right && key != Qt::Key_Up && key != Qt::Key_Down)
            return;
        if (!event->nativeScanCode())
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const auto classify = reinterpret_cast<bool (*)(const void *, QKeyEvent *)>(
                dlsym(RTLD_DEFAULT, "_ZNK17AVRCPButtonFilter16isBluetoothEventEP9QKeyEvent"));
        // The supported native classifier reads only the event's scan code.
        if (!classify || !classify(nullptr, event))
            return;
#endif
        const auto eventType = reinterpret_cast<int (*)()>(
                dlsym(RTLD_DEFAULT, "_ZN23BluetoothHeartbeatEvent9eventTypeEv"));
        if (!eventType || !QCoreApplication::instance())
            return;
        const int type = eventType();
        if (type < QEvent::User || type > QEvent::MaxUser)
            return;
        if (lastHeartbeat.isValid() && lastHeartbeat.elapsed() < 5000)
            return;
        QCoreApplication::postEvent(QCoreApplication::instance(),
                                    new QEvent(static_cast<QEvent::Type>(type)));
        lastHeartbeat.start();
    }

private slots:

    void powered(bool value)
    {
        ++generation;
        if (enabled == value)
            return;
        enabled = value;
        if (!enabled)
            lastHeartbeat.invalidate();
        changed(value);
    }

private:
    void query()
    {
        const auto requestGeneration = generation;
        // QDBusInterface introspection and QObject::property both wait for
        // replies. Send the firmware's adapter query without either wrapper.
        auto request = QDBusMessage::createMethodCall(
                serviceName, QStringLiteral("/org/bluez/hci0"),
                QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        request << QStringLiteral("org.bluez.Adapter1") << QStringLiteral("Powered");
        auto *pending = new QDBusPendingCallWatcher(
                QDBusConnection::systemBus().asyncCall(request, 1500), this);
        connect(pending, &QDBusPendingCallWatcher::finished, this,
                [this, requestGeneration](QDBusPendingCallWatcher *call) {
                    const QDBusPendingReply<QDBusVariant> reply = *call;
                    call->deleteLater();
                    // A live notification is newer than this initial snapshot.
                    if (requestGeneration != generation)
                        return;
                    powered(!reply.isError() && reply.value().variant().toBool());
                });
    }

    std::function<void(bool)> changed;
    const QString serviceName;
    quint64 generation = 0;
    bool enabled = false;
    QElapsedTimer lastHeartbeat;
};
} // namespace

void observeNativeBluetooth(QObject *context, std::function<void(bool)> changed)
{
    new BluetoothStatus(context, std::move(changed));
}

void queryNativeBluetoothPower(QObject *context, std::function<void(BluetoothPower)> answer)
{
    if (!QDBusConnection::systemBus().isConnected()) {
        answer(BluetoothPower::Unknown);
        return;
    }
    auto request = QDBusMessage::createMethodCall(
            bluetoothServiceName(), QStringLiteral("/org/bluez/hci0"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    request << QStringLiteral("org.bluez.Adapter1") << QStringLiteral("Powered");
    // An opened book and the power saver at sleep wait for this answer, so
    // keep the timeout short.
    auto *pending = new QDBusPendingCallWatcher(
            QDBusConnection::systemBus().asyncCall(request, 500), context);
    QObject::connect(pending, &QDBusPendingCallWatcher::finished, context,
                     [answer](QDBusPendingCallWatcher *call) {
                         const QDBusPendingReply<QDBusVariant> reply = *call;
                         call->deleteLater();
                         if (!reply.isError()) {
                             answer(reply.value().variant().toBool() ? BluetoothPower::On
                                                                     : BluetoothPower::Off);
                             return;
                         }
                         // A stopped stack or a missing adapter cannot be powered.
                         // A timeout or another error leaves the state unknown.
                         const auto type = reply.error().type();
                         answer(type == QDBusError::ServiceUnknown
                                                || type == QDBusError::UnknownObject
                                        ? BluetoothPower::Off
                                        : BluetoothPower::Unknown);
                     });
}

void reportNativeBluetoothActivity(QObject *context, QKeyEvent *event)
{
    if (!context)
        return;
    if (auto *status = context->findChild<BluetoothStatus *>(QString(), Qt::FindDirectChildrenOnly))
        status->activity(event);
}
} // namespace kobalt

#include "native_bluetooth.moc"
