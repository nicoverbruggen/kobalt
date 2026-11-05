#include "platforms/kobo/nickel-qt5/session_calls.h"
#include "check.h"

#include <cstring>

namespace {
struct Volume
{
};

struct Position
{
    double percent = 37.5;
};

QObject engine;
Volume volume;
Position position;
int exports = 0, calls = 0;

void checkArguments(QObject *object, Volume &book, const QVariantMap &values, bool starting)
{
    require(object == &engine && &book == &volume, "Session arguments changed");
    require(values
                    == (starting ? QVariantMap{ { "ViewType", "ReadingView" } }
                                 : QVariantMap{ { "ended", true } }),
            "Session event values changed");
}

void start(QObject *object, Volume &book, const Position &state, QVariantMap values)
{
    checkArguments(object, book, values, true);
    require(&state == &position && state.percent == 37.5, "Session position changed");
    calls |= 1;
}

int end(QObject *object, Volume &book, const Position &state, QVariantMap values)
{
    checkArguments(object, book, values, false);
    require(&state == &position && state.percent == 37.5, "Session position changed");
    calls |= 2;
    return 42;
}

void legacyStart(QObject *object, Volume &book, QVariantMap values)
{
    checkArguments(object, book, values, true);
    calls |= 4;
}

int legacyEnd(QObject *object, Volume &book, QVariantMap values)
{
    checkArguments(object, book, values, false);
    calls |= 8;
    return 23;
}

void *lookup(const char *name)
{
    if (std::strcmp(
                name,
                "_ZN11EventEngine19startReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE")
        == 0)
        return exports & 1 ? reinterpret_cast<void *>(start) : nullptr;
    if (std::strcmp(
                name,
                "_ZN11EventEngine17endReadingSessionER6VolumeRK11PluginState4QMapI7QString8QVariantE")
        == 0)
        return exports & 2 ? reinterpret_cast<void *>(end) : nullptr;
    if (std::strcmp(name, "_ZN11EventEngine19startReadingSessionER6Volume4QMapI7QString8QVariantE")
        == 0)
        return exports & 4 ? reinterpret_cast<void *>(legacyStart) : nullptr;
    if (std::strcmp(name, "_ZN11EventEngine17endReadingSessionER6Volume4QMapI7QString8QVariantE")
        == 0)
        return exports & 8 ? reinterpret_cast<void *>(legacyEnd) : nullptr;
    require(false, "Unexpected session symbol lookup");
    return nullptr;
}
} // namespace

int main()
{
    for (exports = 0; exports < 16; ++exports) {
        calls = 0;
        const kobalt::nickel_qt5::SessionCalls<Volume, Position> api(lookup);
        const bool modern = (exports & 3) == 3;
        const bool legacy = (exports & 12) == 12;
        require(api.ready() == (modern || legacy), "An incomplete session pair was accepted");
        api.start(&engine, volume, position, { { "ViewType", "ReadingView" } });
        const auto seconds = api.end(&engine, volume, position, { { "ended", true } });
        require(calls == (modern ? 3 : legacy ? 12 : 0), "Session calling conventions were mixed");
        require(seconds == (modern ? 42 : legacy ? 23 : 0), "Native elapsed time changed");
    }
}
