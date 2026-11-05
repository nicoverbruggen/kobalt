#include "platforms/kobo/nickel-qt5/touch_rotation.h"

#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

QTransform staleTransform;

QPoint transform(const QTransform *matrix, const QPoint &point)
{
    return matrix->map(point);
}

QPoint panel(const void *, const QPoint &point)
{
    return kobalt_touch_transform(&staleTransform, point);
}

QByteArray record(const char *model, const char *firmware = "4.45.23697")
{
    return QByteArray("serial,kernel,") + firmware + ",date,build,00000000-0000-0000-0000-000000000"
            + model;
}
} // namespace

// Checks the Qt5 touch-rotation fix: which models and firmware get it, the mapping in each orientation, clamping and delegation.
int main()
{
    using namespace kobalt::nickel_qt5;
    require(reversedTouchForVersion(record("391")), "Clara BW must use corrected mapping");
    require(reversedTouchForVersion(record("395")),
            "Clara BW alternate ID must use corrected mapping");
    require(reversedTouchForVersion(record("393")), "Clara Colour must use corrected mapping");
    require(!reversedTouchForVersion(record("376", "4.38.23828")),
            "Reported Clara HD firmware keeps native mapping");
    require(!reversedTouchForVersion(record("376", "4.38.23684")),
            "Clara HD keeps native mapping at former boundary");
    require(!reversedTouchForVersion(record("376", "4.38.23683")), "Earlier HD firmware");
    require(!reversedTouchForVersion(record("376", "4.46.23836")),
            "Later Clara HD firmware keeps native mapping");
    require(!reversedTouchForVersion(record("390")), "Libra Colour keeps native mapping");
    require(!reversedTouchForVersion(record("388")), "Libra 2 keeps native mapping");
    require(!reversedTouchForVersion(record("386")), "Clara 2E keeps native mapping");
    require(!reversedTouchForVersion(record("391", "6.0.0")), "Qt6 keeps native mapping");
    require(!reversedTouchForVersion(record("391", "4.44.23697")), "Earlier BW firmware");
    require(!reversedTouchForVersion(record("999")), "Unknown model keeps native mapping");
    require(!reversedTouchForVersion(record("391") + '\0'), "Reject NUL");
    require(!reversedTouchForVersion(record("391", "4.+45.23697")), "Reject malformed firmware");
    require(!reversedTouchForVersion(record("391", "4.999999999999999.1")), "Reject overflow");
    require(!reversedTouchForVersion(record("391") + "\nextra"), "Reject extra line");
    require(reversedTouchForVersion(record("391") + "\r\n"), "Accept trailing newline");
    nativeMapFromDevice = panel;
    nativeTouchTransform = transform;
    staleTransform.translate(100, 200);
    const QPoint point(20, 30);
    configureTouchRotation(false, Qt::PortraitOrientation, 1072, 1448);
    require(kobalt_touch_map_from_device(nullptr, point) == QPoint(120, 230),
            "Inactive mapping delegates");
    const Qt::ScreenOrientation modes[] = { Qt::PortraitOrientation, Qt::LandscapeOrientation,
                                            Qt::InvertedPortraitOrientation,
                                            Qt::InvertedLandscapeOrientation };
    const QPoint expected[] = { point, { 1417, 20 }, { 1051, 1417 }, { 30, 1051 } };
    for (const char *model : { "376", "386", "388", "390", "999" }) {
        for (const auto mode : modes) {
            configureTouchRotation(reversedTouchForVersion(record(model, "4.46.23836")), mode, 0,
                                   0);
            require(kobalt_touch_map_from_device(nullptr, point) == QPoint(120, 230),
                    "Unaffected devices retain the driver transform in every orientation");
        }
    }
    for (int i = 0; i < 4; ++i) {
        configureTouchRotation(true, modes[i], 0, 0);
        require(kobalt_touch_map_from_device(nullptr, point) == expected[i], "Orientation mapping");
        require(kobalt_touch_transform(&staleTransform, point) == QPoint(120, 230),
                "Unrelated transforms delegate");
    }
    configureTouchRotation(true, Qt::LandscapeOrientation, 0, 0);
    require(kobalt_touch_map_from_device(nullptr, { -2, 9999 }) == QPoint(0, 0),
            "Clamp panel bounds");
    configureTouchRotation(true, Qt::PortraitOrientation, 0, 0);
    require(kobalt_touch_map_from_device(nullptr, point) == point,
            "Discard stale transform on portrait return");
    configureTouchRotation(false, Qt::PortraitOrientation, 0, 0);
    require(kobalt_touch_map_from_device(nullptr, point) == QPoint(120, 230),
            "Release ownership restores delegate");
    std::puts(
            "PASS: device guards, all orientations, bounds, transform scope and ownership release");
}
