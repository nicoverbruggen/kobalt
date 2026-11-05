#pragma once
#include <QObject>
#include <QPointer>
#include <QWidget>
#include <QTime>

namespace kobalt {
#ifndef KOBALT_READER_HARDWARE
extern int (*lightTestIoctl)(int, unsigned long, void *);
#endif
// Call only after firmwareSupported() accepts Nickel.
class NativeLight : public QObject
{
    Q_OBJECT
public:
    explicit NativeLight(QWidget *anchor);
    ~NativeLight() override;
    static bool available();
    bool initialize();
    bool hasAutomaticBrightness() const;
    bool automaticBrightness() const;
    bool setAutomaticBrightness(bool enabled);
    bool hasWarmth() const;
    int warmth() const;
    int warmthMinimum() const;
    int warmthMaximum() const;
    int warmthStep() const;
    bool automaticWarmth() const;
    QTime bedtime() const;
    int setWarmth(int value);
    bool saveWarmth();
    bool setAutomaticWarmth(bool enabled);
    bool beginGesture();
    int setGestureLevel(int percent);
    void finishGesture(bool save);
    int brightness() const;

    bool gesturing() const { return gestureStart >= 0; }

private:
    QPointer<QWidget> anchor;
    QPointer<QObject> frontLight;
    int gestureStart = -1;
    bool reportedGestureFailure = false, gestureAutomatic = false;
};
} // namespace kobalt
