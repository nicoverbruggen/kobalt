#pragma once
#include <QString>

class QPainterPath;

namespace kobalt::ui {
// Sets `path` to the named Phosphor icon, a filled outline on a 256 unit
// canvas, and returns true. Returns false for an unknown name. The geometry
// is generated from assets/icons/phosphor; see doc/building.md.
bool phosphorGeometry(const QString &name, QPainterPath &path);
} // namespace kobalt::ui
