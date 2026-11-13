#include "rendering_mode.h"
#include "engine/engine.h"
#include <atomic>

namespace kobalt::renderingMode {
namespace {
std::atomic_int override_{ -1 };
std::atomic_int preferred_{ int(Mode::Hybrid) };

Mode forced(int value)
{
    return value == 2 ? Mode::Hybrid : value > 0 ? Mode::Fast : Mode::Accurate;
}

// The engine reads one value when a book opens. This is the only caller.
void select()
{
    kobalt::setChapterModeOverride(effective(Mode(preferred_.load())) == Mode::Accurate ? 0 : 1);
}
} // namespace

Mode fromPreferences(const QJsonObject &preferences)
{
    const auto value = preferences.value(preferenceKey).toString();
    return value == QLatin1String("accurate") ? Mode::Accurate
            : value == QLatin1String("fast")  ? Mode::Fast
                                              : Mode::Hybrid;
}

QString value(Mode mode)
{
    return mode == Mode::Accurate ? QStringLiteral("accurate")
            : mode == Mode::Fast  ? QStringLiteral("fast")
                                  : QStringLiteral("hybrid");
}

Mode effective(Mode preferred)
{
    const int override = override_.load();
    if (override >= 0)
        return forced(override);
    const auto environment = qgetenv("KOBALT_CHAPTER_MODE");
    if (!environment.isEmpty())
        return forced(environment.toInt());
    return preferred;
}

bool countsInBackground()
{
    return effective(Mode(preferred_.load())) == Mode::Hybrid;
}

void apply(const QJsonObject &preferences)
{
    preferred_.store(int(fromPreferences(preferences)));
    select();
}

void setOverride(int mode)
{
    override_.store(qBound(-1, mode, 2));
    select();
}
} // namespace kobalt::renderingMode
