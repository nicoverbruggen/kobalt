#include "platforms/kobo/nickel-fixes/wifi-idle/wifi_idle_log.h"
#include "platforms/kobo/common/native_wifi_power.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

namespace kobalt {
void diagnostic(const char *, QJsonObject, bool) { }

bool diagnosticsActive()
{
    return false;
}

bool sampleDiagnostics(std::function<void()>)
{
    return false;
}

void queryNativeBluetoothPower(QObject *, std::function<void(BluetoothPower)> done)
{
    done(BluetoothPower::Unknown);
}

namespace mainthread {
void blocking(const char *) { }
} // namespace mainthread
} // namespace kobalt

static void check(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}

static int offCalls = 0;

static void off(QObject *)
{
    ++offCalls;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qputenv("WIFI_MODULE", "wlan_drv_gen4m");
    QTemporaryDir root;
    check(root.isValid(), "temporary diagnostic root");
    QDir directory(root.path());
    check(directory.mkpath("proc") && directory.mkpath("sys/class/net/wlan0")
                  && directory.mkpath("mnt/onboard/.kobo")
                  && directory.mkpath("sys/class/rfkill/rfkill0"),
          "diagnostic fixture folders");
    write(root.path() + "/proc/modules", "wlan_drv_gen4m 123 0 - Live 0\nbt_drv 456 0 - Live 0\n");
    write(root.path() + "/sys/class/net/wlan0/operstate", "up\n");
    write(root.path() + "/sys/class/net/wlan0/carrier", "1\n");
    write(root.path() + "/sys/class/rfkill/rfkill0/type", "wlan\n");
    write(root.path() + "/sys/class/rfkill/rfkill0/state", "1\n");
    write(root.path() + "/mnt/onboard/.kobo/version", "private-serial,test,4.46.23852,391");
    auto fields = kobalt::nickel::collectWifiIdleHardware(root.path(), {});
    check(fields.value("modules_available").toBool()
                  && fields.value("modules").toArray().size() == 2
                  && fields.value("interface_exists").toBool()
                  && fields.value("wifi_driver_loaded").toInt() == 1
                  && fields.value("operstate").toString() == "up",
          "loaded radio and interface");
    check(fields.value("rfkill").toArray().at(0).toObject().value("state").toString() == "1",
          "record kernel radio state separately from module presence");
    check(fields.value("firmware").toString() == "4.46.23852" && !fields.contains("serial"),
          "record firmware without device identity");
    write(root.path() + "/sys/class/net/wlan0/operstate", "down\n");
    fields = kobalt::nickel::collectWifiIdleHardware(root.path(), {});
    check(fields.value("modules").toArray().size() == 2
                  && fields.value("interface_exists").toBool(),
          "a down interface is not an unloaded driver");
    QFile::remove(root.path() + "/proc/modules");
    fields = kobalt::nickel::collectWifiIdleHardware(root.path(), {});
    check(!fields.value("modules_available").toBool()
                  && fields.value("wifi_driver_loaded").toInt() == -1,
          "missing module evidence remains unknown");
    kobalt::nickel::nativeWifiIdleOff = off;
    QObject manager;
    kobalt_wifi_idle_off(&manager);
    check(offCalls == 1, "the observer must call the original shutdown exactly once");
    std::puts("Wi-Fi hardware diagnostic checks passed");
}
