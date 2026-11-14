#pragma once
class ldomDocument;

namespace kobalt {
// Classify once from the initial styles. Only attributes change, so source
// XPointers, Kobo spans and text remain the same when the mode is toggled.
void markBeautifierContent(ldomDocument *document);
} // namespace kobalt
