// The project fonts for C code. fonts.cpp fills these from tools/font-editor/fonts/cubevox_fonts.h,
// the only font source. A glyph is ceil(height / 8) pages, top page first, one byte per column
// left to right, bit 0 on top. The 5x7 font, the only one here, is single case, so a..z are blank cells.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CvFont {
  uint8_t width;
  uint8_t height;
  // Blank columns between glyphs.
  uint8_t spacing;
  uint8_t bytesPerGlyph;
  uint16_t glyphCount;
  // Ascending character codes, one per glyph.
  const uint8_t* codes;
  // glyphCount * bytesPerGlyph bytes.
  const uint8_t* glyphs;
};

// Myrtle 5x7, the project's prose font and the only one the update screens draw.
extern const struct CvFont cvFont5x7;

#ifdef __cplusplus
}
#endif
