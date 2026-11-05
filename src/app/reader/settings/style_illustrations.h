#pragma once
#include "reading/style_sample.h"

namespace kobalt::reader {
// Draw the fixed page-break sample with private font faces on the GUI thread.
StyleSample drawStyleIllustration(const StyleSampleRequest &request);
}
