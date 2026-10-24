// Checks the OLED canvas on the host. Covers text drawing, clipping, the dirty-page API, glyph
// fallback, the three fonts and rectangle fills.

#include "support.h"

#include <cstdint>
#include <cstring>

#include "core/canvas.h"
#include "core/fonts.h"

#include "cubevox_fonts.h"

namespace {

void checkCanvasText() {
  namespace f = cubevox::fonts::StylizedHyperlegible5x7;
  const FontDesc& font = font5x7();
  const uint8_t* glyphA = &f::kGlyphs['A' - 0x20][0];

  Canvas c;
  check(c.dirtyPages() == 0xFF, "canvas: starts fully dirty");
  clearAllDirty(c);
  const int pen = c.drawText(0, 0, font, "A");
  check(std::memcmp(c.page(0), glyphA, f::kWidth) == 0, "canvas: 5x7 A matches the header bytes");
  check(c.page(0)[f::kWidth] == 0 && c.page(1)[0] == 0, "canvas: nothing past the glyph");
  check(c.dirtyPages() == 0x01, "canvas: drawing sets only the touched page dirty");
  check(pen == f::kWidth + f::kSpacing && pen == kFont5x7Advance, "canvas: drawText returns the pen after the spacing");
  c.clearDirty(0);
  check(c.dirtyPages() == 0, "canvas: clearDirty clears the bit");

  Canvas shifted;
  shifted.drawText(0, 3, font, "A");
  bool shiftOk = true;
  for (int col = 0; col < f::kWidth; ++col) {
    shiftOk = shiftOk && shifted.page(0)[col] == static_cast<uint8_t>(glyphA[col] << 3);
    shiftOk = shiftOk && shifted.page(1)[col] == static_cast<uint8_t>(glyphA[col] >> 5);
  }
  check(shiftOk, "canvas: glyph at y = 3 splits across pages 0 and 1");

  Canvas above;
  above.drawText(0, -4, font, "A");
  bool aboveOk = true;
  for (int col = 0; col < f::kWidth; ++col) aboveOk = aboveOk && above.page(0)[col] == static_cast<uint8_t>(glyphA[col] >> 4);
  check(aboveOk, "canvas: glyph above the top edge clips to its lower rows");
}

void checkCanvasWidth() {
  namespace f = cubevox::fonts::StylizedHyperlegible5x7;
  const FontDesc& font = font5x7();
  check(Canvas::textWidth(font, "AB") == 2 * (f::kWidth + f::kSpacing) - f::kSpacing, "canvas: textWidth drops the trailing gap");
  check(Canvas::textWidth(font, "") == 0, "canvas: textWidth of an empty string is 0");
}

void checkCanvasEdges() {
  const FontDesc& font = font5x7();
  Canvas edge;
  edge.drawText(125, 60, font, "AB");
  edge.drawText(-3, -3, font, "A");
  edge.drawText(120, 62, font, "AAA");
  edge.fillRect(-5, -5, 300, 300, true);
  edge.invertRect(100, 50, 100, 100);
  check(edge.page(7)[127] == 0x00 && edge.page(6)[127] == 0x03 && edge.page(0)[0] == 0xFF, "canvas: fill and invert clip at the edges");

  Canvas c;
  c.fillRect(0, 0, kCanvasWidth, kCanvasHeight, true);
  clearAllDirty(c);
  c.clear();
  bool blank = true;
  for (int p = 0; p < kCanvasPages; ++p) {
    for (int x = 0; x < kCanvasWidth; ++x) blank = blank && c.page(p)[x] == 0;
  }
  check(blank && c.dirtyPages() == 0xFF, "canvas: clear blanks every page and marks them dirty");
}

void checkCanvasDirtyApi() {
  Canvas c;
  check(c.firstDirtyPage() == 0, "canvas: a fresh canvas has page 0 dirty first");
  clearAllDirty(c);
  check(c.firstDirtyPage() == -1, "canvas: no dirty page reports -1");
  c.fillRect(0, 30, 4, 2, true);
  c.fillRect(0, 50, 4, 2, true);
  check(c.firstDirtyPage() == 3, "canvas: the lowest dirty page comes first");
  c.clearDirty(3);
  check(c.firstDirtyPage() == 6, "canvas: clearing a page moves on to the next");
  const uint8_t before = c.dirtyPages();
  c.clearDirty(-1);
  c.clearDirty(kCanvasPages);
  c.clearDirty(40);
  check(c.dirtyPages() == before, "canvas: clearDirty ignores a page out of range");
  check(c.page(-1) == nullptr && c.page(kCanvasPages) == nullptr && c.page(0) != nullptr, "canvas: page() rejects an index out of range");
}

void checkCanvasFallback() {
  // A capitals-only font, so lowercase has no glyph of its own.
  const uint8_t caps[2][5] = {{1, 2, 3, 4, 5}, {6, 7, 8, 9, 10}};
  const uint8_t codes[2] = {'A', 'B'};
  const FontDesc capsOnly{5, 7, 1, 5, codes, 2, &caps[0][0]};
  Canvas low;
  Canvas up;
  low.drawText(0, 0, capsOnly, "ab");
  up.drawText(0, 0, capsOnly, "AB");
  check(low.page(0)[0] == 1 && std::memcmp(low.page(0), up.page(0), kCanvasWidth) == 0, "canvas: lowercase falls back to uppercase");
  Canvas gap;
  gap.drawText(0, 0, capsOnly, "?B");
  Canvas direct;
  direct.drawText(capsOnly.width + capsOnly.spacing, 0, capsOnly, "B");
  check(std::memcmp(gap.page(0), direct.page(0), kCanvasWidth) == 0, "canvas: a missing glyph is blank and keeps its advance");

  check(findGlyph(capsOnly, 'A') == &caps[0][0] && findGlyph(capsOnly, 'a') == &caps[0][0], "glyph: a capital and its lowercase find the same bytes");
  check(findGlyph(capsOnly, 'C') == nullptr && findGlyph(capsOnly, 'c') == nullptr && findGlyph(capsOnly, '\x01') == nullptr,
        "glyph: a letter the font lacks finds nothing");
  check(findGlyph(font5x7(), 'u') != nullptr && findGlyph(font5x7(), 'u') == findGlyph(font5x7(), 'U'), "glyph: 5x7 u is the glyph of U");
}

void checkCanvasFonts() {
  Canvas lower5;
  Canvas upper5;
  lower5.drawText(0, 0, font5x7(), "u");
  upper5.drawText(0, 0, font5x7(), "U");
  check(lower5.page(0)[1] != 0 && std::memcmp(lower5.page(0), upper5.page(0), kCanvasWidth) == 0,
        "canvas: 5x7 u draws the bytes of U");

  namespace f9 = cubevox::fonts::StylizedHyperlegible9X12;
  Canvas big9;
  big9.drawText(0, 0, font9x12(), "u");
  bool ok9 = true;
  for (int col = 0; col < f9::kWidth; ++col) {
    ok9 = ok9 && big9.page(0)[col] == f9::kGlyphs['U' - 0x20][col];
    ok9 = ok9 && big9.page(1)[col] == (f9::kGlyphs['U' - 0x20][f9::kWidth + col] & 0x0F);
  }
  check(ok9 && big9.page(0)[1] != 0, "canvas: 9x12 u draws the glyph bytes of U");

  namespace f16 = cubevox::fonts::StylizedHyperlegible16X24;
  Canvas big16;
  big16.drawText(0, 0, font16x24(), "H");
  bool ok16 = true;
  for (int col = 0; col < f16::kWidth; ++col)
    for (int band = 0; band < 3; ++band)
      ok16 = ok16 && big16.page(band)[col] == f16::kGlyphs['H' - 0x20][band * f16::kWidth + col];
  check(ok16 && big16.page(0)[0] != 0, "canvas: 16x24 H draws the glyph bytes");
}

void checkCanvasRects() {
  Canvas rect;
  clearAllDirty(rect);
  rect.fillRect(10, 10, 4, 3, true);
  check(rect.dirtyPages() == 0x02 && rect.page(1)[10] == 0x1C, "canvas: fillRect sets the rows inside page 1");
  rect.invertRect(10, 10, 4, 3);
  check(rect.page(1)[10] == 0 && rect.page(1)[13] == 0, "canvas: invertRect undoes a fill");
}

}  // namespace

void checkCanvas() {
  checkCanvasText();
  checkCanvasWidth();
  checkCanvasEdges();
  checkCanvasDirtyApi();
  checkCanvasFallback();
  checkCanvasFonts();
  checkCanvasRects();
}
