#pragma once
#include "publication/opening_result.h"
#include "support/strings.h"

namespace kobalt {
// Opening producers supply stable reasons. Only the presentation boundary
// resolves copy. Legacy resource/parser detail keeps its existing wording
// until those lower-level APIs have structured errors of their own.
inline QString openingMessage(const OpeningResult &result) {
    const auto *failure = result.failure();
    return !failure                     ? QString()
           : !failure->detail.isEmpty() ? failure->detail
                                        : strings::get(failure->reason.constData());
}
}
