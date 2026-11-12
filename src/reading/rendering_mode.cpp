#include "rendering_mode.h"
#include "engine/engine.h"
#include <atomic>

namespace kobalt::renderingMode {
namespace {
std::atomic_int override_{ -1 };
std::atomic_int preferred_{ int(Mode::Fast) };

// The engine reads one value when a book opens. This is the only caller.
void select()
{
    kobalt::setChapterModeOverride(effective(Mode(preferred_.load())) == Mode::Fast ? 1 : 0);
}
} // namespace

Mode fromPreferences(const QJsonObject &preferences)
{
    return preferences.value(preferenceKey) == QJsonValue(QStringLiteral("accurate"))
            ? Mode::Accurate
            : Mode::Fast;
}

QString value(Mode mode)
{
    return mode == Mode::Accurate ? QStringLiteral("accurate") : QStringLiteral("fast");
}

Mode effective(Mode preferred)
{
    const int forced = override_.load();
    if (forced >= 0)
        return forced > 0 ? Mode::Fast : Mode::Accurate;
    const auto environment = qgetenv("KOBALT_CHAPTER_MODE");
    if (!environment.isEmpty())
        return environment.toInt() > 0 ? Mode::Fast : Mode::Accurate;
    return preferred;
}

void apply(const QJsonObject &preferences)
{
    preferred_.store(int(fromPreferences(preferences)));
    select();
}

void setOverride(int mode)
{
    override_.store(mode < 0 ? -1 : mode > 0 ? 1 : 0);
    select();
}
} // namespace kobalt::renderingMode
