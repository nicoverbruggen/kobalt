#pragma once

namespace kobalt {
// Call only on the thread that owns CREngine and its glyph caches.
void setExtraFontWeight(int steps);
} // namespace kobalt
