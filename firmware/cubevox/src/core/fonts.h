// Bitmap fonts over the font editor's export (tools/font-editor/fonts/cubevox_fonts.h).
// Each glyph is ceil(height / 8) pages, top page first, one byte per column, bit 0 on top.

#pragma once

#include <stdint.h>

struct FontDesc {
  uint8_t width;
  uint8_t height;
  uint8_t spacing;  // blank columns between glyphs
  uint8_t bytesPerGlyph;
  const uint8_t* codes;  // ascending character codes, one per glyph
  int glyphCount;
  const uint8_t* glyphs;  // glyphCount * bytesPerGlyph bytes
};

// Pen advance of the 5x7 font: glyph width plus spacing. fonts.cpp checks it against the export.
constexpr int kFont5x7Advance = 6;

const FontDesc& font5x7();
const FontDesc& font9x12();
const FontDesc& font16x24();

// Glyph bytes for `c`. The project fonts carry blank lowercase cells, so a lowercase letter draws
// as its capital. Returns nullptr when the font has no glyph for the code, and the caller advances
// over a blank cell.
const uint8_t* findGlyph(const FontDesc& font, char c);
