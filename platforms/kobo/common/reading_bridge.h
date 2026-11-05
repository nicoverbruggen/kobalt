#pragma once
#include "reading/reading_bridge.h"
#include <memory>

namespace kobalt {
class NativeProgress;

namespace nickel {
// Construct on the GUI thread after the firmware and native APIs are ready.
std::unique_ptr<ReadingBridge> makeReadingBridge(QString identity,
                                                 std::unique_ptr<NativeProgress> progress);
} // namespace nickel
} // namespace kobalt
