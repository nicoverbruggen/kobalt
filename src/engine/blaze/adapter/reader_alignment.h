#pragma once
class ldomDocument;

namespace kobalt {
// Mark text-only body blocks for alignment and ordinary paragraphs for spacing,
// without changing source text or anchor structure.
void markBodyAlignment(ldomDocument *document);
} // namespace kobalt
