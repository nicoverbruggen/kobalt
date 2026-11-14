#pragma once

// The bundled faces that every engine uses for generic families and for
// characters that the reading font lacks.
namespace kobalt {
// Bundled defaults for generic families.
inline constexpr auto publisherSansFontFace = "NV Legible Next";
// Bundled face for the generic monospace family.
inline constexpr auto monospaceFontFace = "IBM Plex Mono";
// Bundled face for MathML and the generic math family.
inline constexpr auto mathFontFace = "Noto Sans Math";
// Bundled faces, in order, for characters that the reading font lacks.
// Text faces precede Math so its letter glyphs cannot interrupt script shaping.
// Math precedes CJK so shared mathematical symbols keep their math metrics.
// The CJK face has full coverage and localized glyphs selected by HarfBuzz
// from the book or element language. Untagged text uses its Japanese forms.
inline constexpr auto fallbackFontFaces =
        "Noto Serif|Noto Naskh Arabic|Noto Serif Hebrew|Noto Serif Armenian|Noto Serif Georgian|Noto Serif Devanagari|Noto Serif Bengali|Noto Serif Tamil|Noto Serif Thai|Noto Sans Math|Noto Serif CJK JP";
// The largest embedded font that a publication may give to an engine.
inline constexpr long long maxEmbeddedFontBytes = 16 * 1024 * 1024;
} // namespace kobalt
