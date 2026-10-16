#pragma once
#include <QString>
#include "reading_estimates.h"

namespace kobalt {
// Construct only after firmwareSupported() accepts Nickel, on the GUI thread.
class NativeReadingSession {
public:
    explicit NativeReadingSession(QString book);
    ~NativeReadingSession();
    static bool available();
    void start(const ReadingActivity &activity);
    void page(const ReadingActivity &activity);
    bool estimates(const ReadingActivity &activity, ReadingEstimates *result);
    bool estimates(ReadingEstimates *result) {
        return estimates(activity_.current(), result);
    }
    void stop();
#ifndef KOBALT_READER_HARDWARE
    static void testCommand();
#endif
private:
    QString book, account;
    double percent = 0;
    bool running = false;
    nickel::PageReadingActivity activity_;
    double secondsPerWord_ = -1;
    bool paceDirty_ = true;
};
}
