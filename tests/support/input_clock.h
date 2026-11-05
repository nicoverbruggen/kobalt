#pragma once
#include <QElapsedTimer>

namespace kobalt::test {
// The input clock for the reader's duplicate page-turn filter in tests: real
// time plus the time that tests skipped. Fixtures set it as
// ReaderHostServices::inputMilliseconds. A test that turns pages on purpose
// calls skipPageTurnWindow() instead of sleeping through the filter window.
struct InputClock
{
    QElapsedTimer started;
    qint64 skipped = 0;

    InputClock() { started.start(); }
};

inline InputClock &inputClock()
{
    static InputClock clock;
    return clock;
}

inline qint64 inputMilliseconds()
{
    return inputClock().started.elapsed() + inputClock().skipped;
}

// The filter drops a turn that arrives less than 200 ms after the previous one.
inline void skipPageTurnWindow()
{
    inputClock().skipped += 210;
}
} // namespace kobalt::test
