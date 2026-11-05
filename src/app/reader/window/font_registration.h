#pragma once

#include <QFontDatabase>
#include <QRawFont>
#include "ui/style/private_text.h"
#include "ui/style/fonts.h"

namespace kobalt::reader {

// Reading-font previews stay private, outside Qt's font database. Load just
// the edited face when a native editor needs it. The face stays registered
// for the whole process: removing a font makes Qt rebuild its database on the
// next text it draws, which made closing a book slow. Returns whether this
// call added the face.
inline bool registerEditorFont(const QFont &font)
{
    if (QRawFont::fromFont(ui::renewedFont(font)).familyName() == font.family())
        return false;
    const auto bytes = ui::nativeEditorFontData(font);
    return !bytes.isEmpty() && QFontDatabase::addApplicationFontFromData(bytes) >= 0;
}

} // namespace kobalt::reader
