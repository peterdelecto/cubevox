// Bitmap fonts over the font editor's export (tools/font-editor/fonts/cubevox_fonts.h).
// Each glyph is ceil(height / 8) pages, top page first, one byte per column, bit 0 on top.

#pragma once

#include <stdint.h>

struct FontDesc {
  uint8_t width;
  uint8_t height;
  uint8_t spacing;  // blank columns between glyphs
  uint8_t bytesPerGlyph;
  const char* codes;  // ascending ASCII codes, one per glyph
  int glyphCount;
  const uint8_t* glyphs;  // glyphCount * bytesPerGlyph bytes
  bool singleCase;  // a..z draw as A..Z even though the export carries blank lowercase cells
};

const FontDesc& font5x7();
const FontDesc& font9x12();
const FontDesc& font16x24();

// Glyph bytes for `c`. In a singleCase font, and for any font lacking a lowercase glyph,
// a lowercase letter maps to its capital.
// Returns nullptr when neither exists, and the caller advances over a blank cell.
const uint8_t* findGlyph(const FontDesc& font, char c);
