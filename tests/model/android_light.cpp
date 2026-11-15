#include "platforms/android/light.h"
#include <QCoreApplication>

#define CHECK(value)                                 \
    do {                                             \
        if (!(value))                                \
            qFatal("line %d: %s", __LINE__, #value); \
    } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int level = 40, saved = -1, writes = 0;
    bool system = true;
    kobalt::android::Light light([&](int operation, int type, int value) {
        if (type != 7)
            return -1;
        switch (operation) {
        case 0:
            return level;
        case 1:
            return 100;
        case 2:
            system = false;
            return level = value;
        case 3:
            return 1;
        case 4:
            return system ? 1 : 0;
        case 5:
            system = value;
            saved = system ? -1 : level;
            ++writes;
            return 1;
        case 6:
            saved = level;
            ++writes;
            return 1;
        case 7:
            system = value;
            return 1;
        default:
            return -1;
        }
    });
    CHECK(light.initialize() && light.hasSystemBrightness() && !light.hasWarmth());
    CHECK(light.hasAutomaticBrightness() && light.automaticBrightness());
    CHECK(light.beginGesture());
    CHECK(light.setGestureLevel(73) == 73 && !light.automaticBrightness());
    CHECK(light.beginGesture()); // Repeated updates retain the first snapshot.
    light.finishGesture(false);
    CHECK(level == 40 && system && writes == 0 && !light.gesturing());
    CHECK(light.beginGesture() && light.setGestureLevel(65) == 65);
    light.finishGesture(true);
    CHECK(saved == 65 && writes == 1 && !system);
    light.finishGesture(true);
    CHECK(writes == 1); // Hiding a finished panel cannot save another gesture.
    CHECK(light.beginGesture() && light.setGestureLevel(22) == 22);
    light.finishGesture(false);
    CHECK(level == 65 && !system && saved == 65 && writes == 1);
    CHECK(light.setAutomaticBrightness(true) && system && saved == -1);
    // BOOX keeps its raw range and does not receive screen-policy or save commands.
    kobalt::android::Light boox([](int operation, int, int value) {
        if (operation == 0)
            return 128;
        if (operation == 1)
            return 255;
        if (operation == 2)
            return value;
        CHECK(operation == 3);
        return 0;
    });
    CHECK(boox.initialize() && !boox.hasAutomaticBrightness());
    CHECK(boox.beginGesture() && boox.setGestureLevel(100) == 100);
    boox.finishGesture(true);
}
