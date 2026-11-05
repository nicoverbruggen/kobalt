#pragma once

#include <QObject>
#include <QVariantMap>

namespace kobalt::nickel_qt5 {
// Older firmware has no PluginState argument. Keep the two calling conventions
// separate and require a complete pair before starting a native session.
template <class Volume, class Position>
class SessionCalls
{
public:
    //nb lookup libnickel 4.38.21908 4.99.99999 _ZN11EventEngine19startReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE _ZN11EventEngine19startReadingSessionER6Volume4QMapI7QString8QVariantE
    //nb lookup libnickel 4.38.21908 4.99.99999 _ZN11EventEngine17endReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE _ZN11EventEngine17endReadingSessionER6Volume4QMapI7QString8QVariantE
    explicit SessionCalls(void *(*lookup)(const char *))
    {
        start_ = reinterpret_cast<decltype(start_)>(lookup(
                "_ZN11EventEngine19startReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE"));
        end_ = reinterpret_cast<decltype(end_)>(lookup(
                "_ZN11EventEngine17endReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE"));
        legacyStart_ = reinterpret_cast<decltype(legacyStart_)>(
                lookup("_ZN11EventEngine19startReadingSessionER6Volume4QMapI7QString8QVariantE"));
        legacyEnd_ = reinterpret_cast<decltype(legacyEnd_)>(
                lookup("_ZN11EventEngine17endReadingSessionER6Volume4QMapI7QString8QVariantE"));
    }

    bool ready() const { return (start_ && end_) || (legacyStart_ && legacyEnd_); }

    void start(QObject *engine, Volume &volume, const Position &position,
               const QVariantMap &values) const
    {
        if (start_ && end_) {
            start_(engine, volume, position, values);
        } else if (legacyStart_ && legacyEnd_) {
            legacyStart_(engine, volume, values);
        }
    }

    int end(QObject *engine, Volume &volume, const Position &position,
            const QVariantMap &values) const
    {
        if (start_ && end_) {
            return end_(engine, volume, position, values);
        }
        return legacyStart_ && legacyEnd_ ? legacyEnd_(engine, volume, values) : 0;
    }

private:
    void (*start_)(QObject *, Volume &, const Position &, QVariantMap) = nullptr;
    int (*end_)(QObject *, Volume &, const Position &, QVariantMap) = nullptr;
    void (*legacyStart_)(QObject *, Volume &, QVariantMap) = nullptr;
    int (*legacyEnd_)(QObject *, Volume &, QVariantMap) = nullptr;
};
} // namespace kobalt::nickel_qt5
