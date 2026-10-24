// Bitmap fonts over the font editor's export (tools/font-editor/fonts/cubevox_fonts.h).
// Each glyph is ceil(height / 8) pages, top page first, one byte per column, bit 0 on top.
// The export also measures the ink of every glyph, so text can be spaced and centred by ink.

#pragma once

#include <stdint.h>

struct FontDesc {
  uint8_t width;
  uint8_t height;
  uint8_t spacing;  // blank columns between glyph cells on the fixed pitch
  uint8_t bytesPerGlyph;
  const uint8_t* codes;  // ascending character codes, one per glyph
  int glyphCount;
  const uint8_t* glyphs;  // glyphCount * bytesPerGlyph bytes

  // Ink metrics from the export, one entry per glyph. inkLeft is the first inked column and
  // inkWidth the columns from there to the last inked one. A blank glyph has both at 0. A
  // descriptor built without them lays out on the fixed pitch.
  const uint8_t* inkLeft = nullptr;
  const uint8_t* inkWidth = nullptr;

  // Pen advance over a blank glyph when text is spaced by ink.
  uint8_t spaceAdvance = 0;

  // Blank columns between neighbouring inks. Zero keeps the fixed pitch of width + spacing, which
  // the 5x7 menu font needs. Above zero the font is spaced by ink.
  uint8_t inkGap = 0;
};

// Pen advance of the 5x7 font: glyph width plus spacing. fonts.cpp checks it against the export.
constexpr int kFont5x7Advance = 6;

const FontDesc& font5x7();    // fixed pitch
const FontDesc& font9x12();   // spaced by ink
const FontDesc& font16x24();  // spaced by ink

// Table index of the glyph for `c`, or -1 when the font has none. The project fonts are single case,
// so a lowercase letter whose cell is blank maps to its capital. A lowercase cell the font inks keeps
// its own glyph: the 9X12 and 16X24 fonts draw the flat sign in the "b" cell, for key names such as
// "Bb / Gm". A descriptor without ink metrics maps every lowercase letter.
int glyphIndex(const FontDesc& font, char c);

inline const uint8_t* glyphAt(const FontDesc& font, int index) { return font.glyphs + index * font.bytesPerGlyph; }
