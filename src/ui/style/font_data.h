#pragma once
#include <QPainterPath>

namespace kobalt::ui {
// Applies FreeType's emboldening, centred on the glyph, to a Qt glyph path.
// Strength is in path coordinates; reading weight uses pixel size * steps / 256.
QPainterPath weightedOutline(const QPainterPath &path, qreal strength);
}
