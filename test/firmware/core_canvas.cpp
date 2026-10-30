// Checks the OLED canvas on the host. Covers text drawing, clipping, the dirty-page API, glyph
// fallback and the flat sign, the three fonts, rectangle fills, text spaced by ink, tabular digits,
// the 5x7 fixed pitch, and the panel link that decides when the display backend writes.

#include "support.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/display.h"
#include "core/fonts.h"

#include "cubevox_fonts.h"

namespace {

void checkCanvasText() {
  namespace f = cubevox::fonts::Myrtle5x7;
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
  namespace f = cubevox::fonts::Myrtle5x7;
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

  // A restarted panel needs the whole frame again, pixels unchanged.
  clearAllDirty(c);
  std::array<std::array<uint8_t, kCanvasWidth>, kCanvasPages> pixels{};
  for (int p = 0; p < kCanvasPages; ++p) std::memcpy(pixels[p].data(), c.page(p), kCanvasWidth);
  c.markAllDirty();
  bool same = true;
  for (int p = 0; p < kCanvasPages; ++p) same = same && std::memcmp(pixels[p].data(), c.page(p), kCanvasWidth) == 0;
  check(c.dirtyPages() == 0xFF && c.firstDirtyPage() == 0 && same, "canvas: markAllDirty marks every page and keeps the pixels");
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

  check(glyphIndex(capsOnly, 'A') == 0 && glyphIndex(capsOnly, 'a') == 0, "glyph: a capital and its lowercase find the same glyph");
  check(glyphIndex(capsOnly, 'C') < 0 && glyphIndex(capsOnly, 'c') < 0 && glyphIndex(capsOnly, '\x01') < 0,
        "glyph: a letter the font lacks finds nothing");
  check(glyphIndex(font5x7(), 'u') >= 0 && glyphIndex(font5x7(), 'u') == glyphIndex(font5x7(), 'U'), "glyph: 5x7 u is the glyph of U");
}

// The key names write a flat as a lowercase b and minor as a lowercase m. The 9X12 and 16X24 fonts
// ink a flat sign in the b cell, and 16X24 inks a minor-key m in the m cell, so those keep their own
// glyphs. The unit letters d, s and t are inked lowercase in both fonts, and every other blank
// lowercase cell still draws as its capital.
void checkFlatGlyph() {
  const auto own = [](const FontDesc& font, char c) { return glyphIndex(font, c) == static_cast<int>(c) - 0x20; };
  check(own(font9x12(), 'b') && own(font16x24(), 'b'), "flat: 9X12 and 16X24 draw b from its own inked cell");
  check(own(font9x12(), 'm') && own(font16x24(), 'm'), "minor: 9X12 and 16X24 draw m from its own inked cell");
  bool unitsOwn = true;
  for (const char c : {'d', 's', 't'}) unitsOwn = unitsOwn && own(font9x12(), c) && own(font16x24(), c);
  check(unitsOwn, "units: 9X12 and 16X24 draw d, s and t from their own inked cells");
  check(glyphIndex(font5x7(), 'b') == glyphIndex(font5x7(), 'B'), "flat: 5x7 has no flat, so b draws as B");
  bool othersMap = true;
  for (char c = 'a'; c <= 'z'; ++c) {
    if (c == 'b' || c == 'm' || c == 'd' || c == 's' || c == 't') continue;
    othersMap = othersMap && glyphIndex(font9x12(), c) == glyphIndex(font9x12(), static_cast<char>(c - 'a' + 'A'));
    othersMap = othersMap && glyphIndex(font16x24(), c) == glyphIndex(font16x24(), static_cast<char>(c - 'a' + 'A'));
  }
  check(othersMap, "flat: every other lowercase letter still draws as its capital");

  const FontDesc& big = font16x24();
  const int flat = glyphIndex(big, 'b');
  const int sharp = glyphIndex(big, '#');
  check(big.inkWidth[flat] > 0 && big.inkWidth[flat] <= big.inkWidth[sharp] && big.inkWidth[flat] < big.inkWidth[glyphIndex(big, 'B')],
        "flat: the 16X24 flat is inked, no wider than the sharp and narrower than B");
  Canvas flatKey;
  flatKey.drawText(0, 0, big, "Bb");
  Canvas capitals;
  capitals.drawText(0, 0, big, "BB");
  check(std::memcmp(flatKey.page(1), capitals.page(1), kCanvasWidth) != 0, "flat: Bb draws a flat after the B, not a second B");
}

void checkCanvasFonts() {
  Canvas lower5;
  Canvas upper5;
  lower5.drawText(0, 0, font5x7(), "u");
  upper5.drawText(0, 0, font5x7(), "U");
  check(lower5.page(0)[1] != 0 && std::memcmp(lower5.page(0), upper5.page(0), kCanvasWidth) == 0,
        "canvas: 5x7 u draws the bytes of U");

  namespace f9 = cubevox::fonts::Myrtle9X12;
  Canvas big9;
  big9.drawText(0, 0, font9x12(), "u");
  bool ok9 = true;
  for (int col = 0; col < f9::kWidth; ++col) {
    ok9 = ok9 && big9.page(0)[col] == f9::kGlyphs['U' - 0x20][col];
    ok9 = ok9 && big9.page(1)[col] == (f9::kGlyphs['U' - 0x20][f9::kWidth + col] & 0x0F);
  }
  check(ok9 && big9.page(0)[1] != 0, "canvas: 9x12 u draws the glyph bytes of U");

  namespace f16 = cubevox::fonts::Myrtle16X24;
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

// The 9X12 and 16X24 fonts keep this many blank columns between neighbouring inks.
constexpr int kInkGap = 3;

// Holds the first and last column with a lit pixel. first is -1 when nothing is lit.
struct Ink {
  int first = -1;
  int last = -1;
  int width() const { return first < 0 ? 0 : last - first + 1; }
};

bool columnLit(const Canvas& c, int x) {
  for (int p = 0; p < kCanvasPages; ++p) {
    if (c.page(p)[x]) return true;
  }
  return false;
}

bool sameColumn(const Canvas& a, int ax, const Canvas& b, int bx) {
  for (int p = 0; p < kCanvasPages; ++p) {
    if (a.page(p)[ax] != b.page(p)[bx]) return false;
  }
  return true;
}

bool samePages(const Canvas& a, const Canvas& b) {
  for (int p = 0; p < kCanvasPages; ++p) {
    if (std::memcmp(a.page(p), b.page(p), kCanvasWidth) != 0) return false;
  }
  return true;
}

Ink inkOf(const Canvas& c) {
  Ink ink;
  for (int x = 0; x < kCanvasWidth; ++x) {
    if (!columnLit(c, x)) continue;
    if (ink.first < 0) ink.first = x;
    ink.last = x;
  }
  return ink;
}

// Reads the ink of glyph `index` from its bitmap, so tests check the exported tables against pixels.
Ink bitmapInk(const FontDesc& font, int index) {
  Ink ink;
  const int bands = (font.height + 7) / 8;
  for (int col = 0; col < font.width; ++col) {
    bool lit = false;
    for (int band = 0; band < bands; ++band) lit = lit || glyphAt(font, index)[band * font.width + col] != 0;
    if (!lit) continue;
    if (ink.first < 0) ink.first = col;
    ink.last = col;
  }
  return ink;
}

struct NamedFont {
  const char* name;
  const FontDesc* font;
};

std::array<NamedFont, 2> inkFonts() { return {{{"9X12", &font9x12()}, {"16X24", &font16x24()}}}; }

// The GUI draws values in 16X24 and labels in 9X12. The gap rule holds for both fonts, so every
// line is checked in both, and a label is wider than the panel in 16X24.
constexpr const char* kValues[] = {"MUTED", "210 MS", "-24 DB"};
constexpr const char* kLabels[] = {"SEMITONES", "THRESHOLD"};

std::string said(const NamedFont& nf, const char* text, const char* what) {
  return std::string("canvas: ") + nf.name + " \"" + text + "\" " + what;
}

std::vector<const char*> allLines() {
  std::vector<const char*> lines(std::begin(kValues), std::end(kValues));
  lines.insert(lines.end(), std::begin(kLabels), std::end(kLabels));
  return lines;
}

// Returns the lines that fit the panel in this font, which is all of them in 9X12.
std::vector<const char*> fittingLines(const NamedFont& nf) {
  std::vector<const char*> lines;
  for (const char* text : allLines()) {
    if (Canvas::textWidth(*nf.font, text) <= kCanvasWidth - 2) lines.push_back(text);
  }
  return lines;
}

// True when `a`, `blanks` spaces and `b` are drawn with each ink whole, starting on its own pen, and
// the blank columns between the two inks equal `gap` plus one space advance per blank.
bool pairIsSpaced(const FontDesc& font, char a, int blanks, char b, int gap) {
  char pair[8] = {a};
  for (int i = 0; i < blanks; ++i) pair[1 + i] = ' ';
  pair[1 + blanks] = b;
  const char loneA[2] = {a, 0};
  const char loneB[2] = {b, 0};
  Canvas line;
  Canvas first;
  Canvas second;
  line.drawText(0, 0, font, pair);
  first.drawText(0, 0, font, loneA);
  second.drawText(0, 0, font, loneB);
  const Ink inkA = inkOf(first);
  const Ink inkB = inkOf(second);
  if (inkA.first != 0 || inkB.first != 0) return false;

  int start = inkA.width();
  while (start < kCanvasWidth && !columnLit(line, start)) ++start;
  for (int k = 0; k < inkA.width(); ++k) {
    if (!sameColumn(line, k, first, k)) return false;
  }
  for (int k = 0; k < inkB.width(); ++k) {
    if (start + k >= kCanvasWidth || !sameColumn(line, start + k, second, k)) return false;
  }
  return start - inkA.width() == gap + blanks * font.spaceAdvance;
}

// True when every neighbouring pair of inked glyphs in `text` passes pairIsSpaced.
bool pairsAreSpaced(const FontDesc& font, const char* text, int gap) {
  bool any = false;
  for (const char* p = text; *p; ++p) {
    if (*p == ' ') continue;
    const char* q = p + 1;
    while (*q == ' ') ++q;
    if (!*q) break;
    any = true;
    if (!pairIsSpaced(font, *p, static_cast<int>(q - p) - 1, *q, gap)) return false;
  }
  return any;
}

// Checks the same rule on one drawing of a line that fits the panel, where each glyph sits whole
// after the one before it.
bool lineIsSpaced(const FontDesc& font, const char* text, int gap) {
  Canvas line;
  line.drawText(0, 0, font, text);
  int prevEnd = -1;
  int blanks = 0;
  for (const char* p = text; *p; ++p) {
    if (*p == ' ') {
      ++blanks;
      continue;
    }
    const char one[2] = {*p, 0};
    Canvas alone;
    alone.drawText(0, 0, font, one);
    const Ink glyph = inkOf(alone);
    int start = prevEnd + 1;
    while (start < kCanvasWidth && !columnLit(line, start)) ++start;
    for (int k = 0; k < glyph.width(); ++k) {
      if (start + k >= kCanvasWidth || !sameColumn(line, start + k, alone, k)) return false;
    }
    const int expected = prevEnd < 0 ? 0 : gap + blanks * font.spaceAdvance;
    if (start - prevEnd - 1 != expected) return false;
    prevEnd = start + glyph.width() - 1;
    blanks = 0;
  }
  return prevEnd >= 0;
}

void checkInkTables() {
  const NamedFont all[] = {{"5x7", &font5x7()}, {"9X12", &font9x12()}, {"16X24", &font16x24()}};
  for (const NamedFont& nf : all) {
    const FontDesc& font = *nf.font;
    bool ok = font.inkLeft != nullptr && font.inkWidth != nullptr;
    for (int i = 0; ok && i < font.glyphCount; ++i) {
      const Ink ink = bitmapInk(font, i);
      ok = font.inkLeft[i] == (ink.first < 0 ? 0 : ink.first) && font.inkWidth[i] == ink.width();
    }
    check(ok, (std::string("canvas: ") + nf.name + " ink tables match the bitmaps, 0 for a blank glyph").c_str());
    check(font.spaceAdvance == font.width / 2, (std::string("canvas: ") + nf.name + " space advance is half the cell").c_str());
  }
  check(font5x7().inkGap == 0 && font9x12().inkGap == kInkGap && font16x24().inkGap == kInkGap,
        "canvas: only 5x7 stays on the fixed pitch, the others use a 3 px gap");
}

void checkInkSpacing() {
  for (const NamedFont& nf : inkFonts()) {
    for (const char* text : allLines()) {
      check(pairsAreSpaced(*nf.font, text, kInkGap), said(nf, text, "adjacent inks sit exactly the gap apart").c_str());
    }
    for (const char* text : fittingLines(nf)) {
      check(lineIsSpaced(*nf.font, text, kInkGap), said(nf, text, "drawn whole keeps the same gaps").c_str());
    }
  }
  check(fittingLines({"9X12", &font9x12()}).size() == 5 && fittingLines({"16X24", &font16x24()}).size() == 3,
        "canvas: every line fits the panel in 9X12, the three values fit in 16X24");
}

void checkInkWidthAndCentring() {
  for (const NamedFont& nf : inkFonts()) {
    const FontDesc& font = *nf.font;
    for (const char* text : fittingLines(nf)) {
      const int width = Canvas::textWidth(font, text);

      Canvas flush;
      flush.drawText(0, 0, font, text);
      check(inkOf(flush).first == 0 && inkOf(flush).width() == width, said(nf, text, "textWidth is the ink width from x").c_str());

      const int x = (kCanvasWidth - width) / 2;
      Canvas mid;
      const int pen = mid.drawText(x, 0, font, text);
      const Ink ink = inkOf(mid);
      check(std::abs(ink.first - (kCanvasWidth - 1 - ink.last)) <= 1 && pen == x + width + kInkGap,
            said(nf, text, "centred margins differ by at most 1 px").c_str());

      Canvas padded;
      padded.drawText(1, 0, font, text);
      check(!columnLit(padded, 0) && inkOf(padded).first == 1 && width <= kCanvasWidth - 2,
            said(nf, text, "at x = 1 keeps 1 px clear and fits with 1 px each side").c_str());
    }
  }
}

void checkInkPen() {
  const char unknown[] = "A\x01" "B";  // A code no font has advances like a blank.
  for (const NamedFont& nf : inkFonts()) {
    const FontDesc& font = *nf.font;
    Canvas whole;
    whole.drawText(2, 0, font, "MUTED");
    Canvas split;
    const int pen = split.drawText(2, 0, font, "MU");
    split.drawText(pen, 0, font, "TED");
    check(samePages(whole, split), said(nf, "MU + TED", "a second call continues the line").c_str());

    const int blank = Canvas::textWidth(font, "A") + kInkGap + font.spaceAdvance + Canvas::textWidth(font, "B");
    check(Canvas::textWidth(font, "A B") == blank && Canvas::textWidth(font, unknown) == blank,
          said(nf, "A B", "a blank glyph advances by the space advance").c_str());
    check(Canvas::textWidth(font, "") == 0, said(nf, "", "has no width").c_str());
  }
}

void checkInkClipping() {
  for (const NamedFont& nf : inkFonts()) {
    Canvas ref;
    ref.drawText(30, 0, *nf.font, "MUTED");
    Canvas left;
    left.drawText(-20, 0, *nf.font, "MUTED");
    Canvas right;
    right.drawText(100, 0, *nf.font, "MUTED");
    bool ok = true;
    for (int c = 0; c < kCanvasWidth - 50; ++c) ok = ok && sameColumn(left, c, ref, c + 50);
    for (int k = 0; k < kCanvasWidth - 100; ++k) ok = ok && sameColumn(right, 100 + k, ref, 30 + k);
    check(ok && inkOf(left).width() > 0 && inkOf(right).width() > 0,
          said(nf, "MUTED", "clips at both edges without moving the rest").c_str());
  }
}

void checkTabularDigits() {
  for (const NamedFont& nf : inkFonts()) {
    const FontDesc& font = *nf.font;
    const int pitch = font.width + font.spacing;

    bool centred = true;
    for (const char* d = "0123456789-"; *d; ++d) {
      const char one[2] = {*d, 0};
      Canvas c;
      c.drawDigits(0, 0, font, one);
      const Ink ink = inkOf(c);
      centred = centred && ink.first >= 0 && std::abs(ink.first - (font.width - 1 - ink.last)) <= 1;
    }
    check(centred, said(nf, "0123456789-", "tabular ink sits mid-cell, slack within 1 px").c_str());

    Canvas one;
    one.drawDigits(0, 0, font, "1");
    const Ink ink1 = inkOf(one);
    check(std::abs(ink1.first - (font.width - 1 - ink1.last)) <= 1 && ink1.first > 0,
          said(nf, "1", "left and right slack differ by at most 1 px").c_str());

    check(Canvas::digitsWidth(font, "111") == 3 * pitch - font.spacing && Canvas::digitsWidth(font, "111") == Canvas::digitsWidth(font, "888") &&
              Canvas::digitsWidth(font, "") == 0,
          said(nf, "111", "digitsWidth depends on the length only").c_str());

    Canvas ten;
    Canvas eighty;
    const int pen = ten.drawDigits(0, 0, font, "10");
    eighty.drawDigits(0, 0, font, "80");
    bool tailStill = true;
    for (int col = pitch; col < pitch + font.width; ++col) tailStill = tailStill && sameColumn(ten, col, eighty, col);
    check(tailStill && !sameColumn(ten, 0, eighty, 0) && !samePages(ten, eighty) && pen == 2 * pitch,
          said(nf, "10 and 80", "a changed digit leaves its neighbour in place").c_str());

    Canvas gap;
    gap.drawDigits(0, 0, font, "1 1");
    bool blankCell = true;
    for (int col = pitch; col < 2 * pitch; ++col) blankCell = blankCell && !columnLit(gap, col);
    check(blankCell && columnLit(gap, 2 * pitch + font.width / 2), said(nf, "1 1", "a space holds a whole cell").c_str());
  }
}

// Lays glyph cells on a width + spacing grid, which is the 5x7 pitch the menu screens rely on.
void fixedPitchRow(const char* text, uint8_t (&row)[kCanvasWidth]) {
  namespace f = cubevox::fonts::Myrtle5x7;
  std::memset(row, 0, sizeof row);
  int x = 0;
  for (; *text; ++text) {
    int code = static_cast<unsigned char>(*text);
    if (code >= 'a' && code <= 'z') code -= 'a' - 'A';
    if (code >= 0x20 && code <= 0x7E) {
      for (int col = 0; col < f::kWidth; ++col) {
        if (x + col < kCanvasWidth) row[x + col] |= f::kGlyphs[code - 0x20][col];
      }
    }
    x += f::kWidth + f::kSpacing;
  }
}

bool fixedPitchMatches(const char* text) {
  uint8_t want[kCanvasWidth];
  fixedPitchRow(text, want);
  Canvas c;
  const int n = static_cast<int>(std::strlen(text));
  const int pen = c.drawText(0, 0, font5x7(), text);
  bool ok = std::memcmp(c.page(0), want, kCanvasWidth) == 0 && pen == n * kFont5x7Advance;
  ok = ok && Canvas::textWidth(font5x7(), text) == (n == 0 ? 0 : n * kFont5x7Advance - 1);
  for (int p = 1; p < kCanvasPages; ++p) {
    for (int x = 0; x < kCanvasWidth; ++x) ok = ok && c.page(p)[x] == 0;
  }
  return ok;
}

void checkFixedPitch5x7() {
  bool ok = true;
  char chunk[22];
  for (int start = 0x20; start <= 0x7E; start += 21) {
    int n = 0;
    for (int code = start; code <= 0x7E && n < 21; ++code) chunk[n++] = static_cast<char>(code);
    chunk[n] = '\0';
    ok = ok && fixedPitchMatches(chunk);
  }
  check(ok, "canvas: 5x7 prints every character on the 6 px grid, byte for byte");

  const char* ui[] = {"Update the firmware?", "You have v2.00.19.", "Click to start.", "Turn to cancel.",
                      ">Out LINE", "Chain order", "1 INPUT GATE", "I", "i", "", " "};
  bool menuOk = true;
  for (const char* s : ui) menuOk = menuOk && fixedPitchMatches(s);
  check(menuOk, "canvas: 5x7 menu and update strings are byte for byte the fixed pitch");
}

bool pixelLit(const Canvas& c, int x, int y) { return ((c.page(y / 8)[x] >> (y % 8)) & 1) != 0; }

int litPixels(const Canvas& c) {
  int n = 0;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) n += pixelLit(c, x, y) ? 1 : 0;
  }
  return n;
}

void checkSetPixel() {
  Canvas c;
  clearAllDirty(c);
  c.setPixel(5, 10, true);
  check(pixelLit(c, 5, 10) && litPixels(c) == 1 && c.dirtyPages() == 0x02, "canvas: setPixel lights one pixel and dirties its page");
  clearAllDirty(c);
  c.setPixel(5, 10, true);
  check(c.dirtyPages() == 0, "canvas: lighting a lit pixel changes nothing");
  c.setPixel(5, 10, false);
  check(litPixels(c) == 0 && c.dirtyPages() == 0x02, "canvas: setPixel false clears it");
  clearAllDirty(c);
  c.setPixel(-1, 0, true);
  c.setPixel(0, -1, true);
  c.setPixel(kCanvasWidth, 0, true);
  c.setPixel(0, kCanvasHeight, true);
  check(litPixels(c) == 0 && c.dirtyPages() == 0, "canvas: setPixel outside the canvas does nothing");
  c.setPixel(kCanvasWidth - 1, kCanvasHeight - 1, true);
  check(pixelLit(c, kCanvasWidth - 1, kCanvasHeight - 1) && c.dirtyPages() == 0x80, "canvas: setPixel reaches the last pixel");
}

// Draws "8" in 16X24 at y = 10 clipped to the rows [clipTop, clipBottom) and returns the canvas.
Canvas clippedEight(int clipTop, int clipBottom) {
  Canvas c;
  c.drawDigitsClipped(4, 10, font16x24(), "8", clipTop, clipBottom);
  return c;
}

void checkClippedDigits() {
  Canvas whole;
  whole.drawDigits(4, 10, font16x24(), "8");
  check(samePages(whole, clippedEight(0, kCanvasHeight)) && samePages(whole, clippedEight(-100, 1000)),
        "canvas: a clip covering the canvas draws what drawDigits draws");

  bool band = true;
  for (int top = 8; top <= 30; top += 3) {
    const Canvas c = clippedEight(top, top + 7);
    for (int y = 0; y < kCanvasHeight; ++y) {
      for (int x = 0; x < kCanvasWidth; ++x) {
        const bool want = y >= top && y < top + 7 && pixelLit(whole, x, y);
        band = band && pixelLit(c, x, y) == want;
      }
    }
  }
  check(band, "canvas: a clipped digit keeps exactly the rows inside the clip, at any alignment to a page");

  check(litPixels(clippedEight(20, 20)) == 0 && litPixels(clippedEight(30, 20)) == 0, "canvas: an empty or reversed clip draws nothing");
  check(litPixels(clippedEight(0, 10)) == 0 && litPixels(clippedEight(34, 64)) == 0, "canvas: a clip clear of the glyph draws nothing");
  Canvas two;
  const int pen = two.drawDigitsClipped(0, 10, font16x24(), "12", 10, 34);
  check(pen == 2 * (font16x24().width + font16x24().spacing), "canvas: a clipped line returns the pen after its cells");
}

void checkBayer() {
  int unique = 0;
  bool seen[64] = {};
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      int below = 0;
      for (int level = 0; level <= 64; ++level) below += bayerLit(x, y, level) ? 1 : 0;
      // The threshold is the lowest level that lights the pixel, 64 minus the levels that do.
      const int threshold = 64 - below;
      if (threshold >= 0 && threshold < 64 && !seen[threshold]) {
        seen[threshold] = true;
        ++unique;
      }
    }
  }
  check(unique == 64, "bayer: the 64 pixels of a tile hold the 64 thresholds once each");

  int half = 0;
  int none = 0;
  int all = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      half += bayerLit(x, y, 32) ? 1 : 0;
      none += bayerLit(x, y, 0) ? 1 : 0;
      all += bayerLit(x, y, 64) ? 1 : 0;
    }
  }
  check(half == 32 && none == 0 && all == 64, "bayer: level 0 lights none, 32 half and 64 all");
  check(bayerLit(0, 0, 1) && !bayerLit(1, 0, 32) && bayerLit(1, 0, 33) && bayerLit(8, 8, 1) == bayerLit(0, 0, 1),
        "bayer: thresholds follow the standard 8 x 8 matrix and repeat every 8 pixels");

  bool nested = true;
  for (int level = 0; level < 64; ++level) {
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) nested = nested && (!bayerLit(x, y, level) || bayerLit(x, y, level + 1));
    }
  }
  check(nested, "bayer: a pixel lit at one level stays lit at every higher level");
}

void checkCopyFrom() {
  Canvas src;
  src.drawText(3, 20, font9x12(), "COPY");
  Canvas dst;
  clearAllDirty(dst);
  dst.copyFrom(src);
  check(samePages(dst, src), "canvas: copyFrom copies every page");
  const Canvas blank;
  unsigned changed = 0;
  for (int p = 0; p < kCanvasPages; ++p) {
    if (std::memcmp(blank.page(p), src.page(p), kCanvasWidth) != 0) changed |= 1u << p;
  }
  check(changed != 0 && changed != 0xFF && dst.dirtyPages() == changed, "canvas: copyFrom dirties exactly the pages that differ");
  clearAllDirty(dst);
  dst.copyFrom(src);
  check(dst.dirtyPages() == 0, "canvas: copying an identical canvas dirties nothing");
}

// Per-pixel dissolve, the definition the per-byte masks must equal.
Canvas dissolvedPerPixel(const Canvas& from, const Canvas& to, int level) {
  Canvas r;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) r.setPixel(x, y, bayerLit(x, y, level) ? pixelLit(to, x, y) : pixelLit(from, x, y));
  }
  return r;
}

Canvas dissolveFixture(int seed) {
  Canvas c;
  for (int i = 0; i < 700; ++i) {
    const int v = (i * 2654435761u + static_cast<unsigned>(seed) * 40503u) >> 7;
    c.setPixel(v % kCanvasWidth, (v / 131) % kCanvasHeight, true);
  }
  return c;
}

void checkDissolveSteps() {
  check(kDissolveSteps == 2, "dissolve: a dissolve has 2 mixed steps");
  const Canvas from = dissolveFixture(1);
  const Canvas to = dissolveFixture(2);
  check(litPixels(from) > 300 && litPixels(to) > 300 && !samePages(from, to), "dissolve: the fixtures are busy and differ");

  Canvas r;
  r.dissolve(from, to, 0);
  check(samePages(r, from), "dissolve: step 0 is the old screen");
  r.dissolve(from, to, kDissolveSteps + 1);
  check(samePages(r, to), "dissolve: the step after the last is the new screen");
  r.dissolve(from, to, -4);
  check(samePages(r, from), "dissolve: a step below 0 reads as 0");
  r.dissolve(from, to, 99);
  check(samePages(r, to), "dissolve: a step past the end reads as the new screen");

  r.dissolve(from, to, 1);
  check(samePages(r, dissolvedPerPixel(from, to, 21)), "dissolve: step 1 equals the per-pixel Bayer dissolve at 21 of 64");
  r.dissolve(from, to, 2);
  check(samePages(r, dissolvedPerPixel(from, to, 43)), "dissolve: step 2 equals the per-pixel Bayer dissolve at 43 of 64");
}

void checkDissolveCoverage() {
  Canvas black;
  Canvas white;
  white.fillRect(0, 0, kCanvasWidth, kCanvasHeight, true);
  Canvas third;
  third.dissolve(black, white, 1);
  Canvas twoThirds;
  twoThirds.dissolve(black, white, 2);
  check(litPixels(third) == kCanvasWidth * kCanvasHeight * 21 / 64 && litPixels(twoThirds) == kCanvasWidth * kCanvasHeight * 43 / 64,
        "dissolve: the steps light 21 and 43 of every 64 pixels, about a third and two thirds");
  bool nested = true;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) nested = nested && (!pixelLit(third, x, y) || pixelLit(twoThirds, x, y));
  }
  check(nested, "dissolve: every pixel the first step lights, the second keeps lit");
}

void checkDissolveInPlace() {
  const Canvas from = dissolveFixture(1);
  const Canvas to = dissolveFixture(2);

  // In place, the second step works from the first step's result because it nests inside it.
  Canvas live = from;
  live.dissolve(live, to, 1);
  live.dissolve(live, to, 2);
  Canvas direct;
  direct.dissolve(from, to, 2);
  check(samePages(live, direct), "dissolve: step 2 over step 1, in place, equals step 2 over the old screen");
  live.dissolve(live, to, kDissolveSteps + 1);
  check(samePages(live, to), "dissolve: and the last step lands on the new screen");

  Canvas dirty = from;
  clearAllDirty(dirty);
  dirty.dissolve(dirty, from, 1);
  check(dirty.dirtyPages() == 0 && samePages(dirty, from), "dissolve: mixing a screen with itself changes nothing and dirties nothing");
  Canvas topOnly;
  topOnly.drawText(0, 0, font5x7(), "TOP");
  Canvas blank;
  clearAllDirty(blank);
  blank.dissolve(blank, topOnly, kDissolveSteps + 1);
  check(blank.dirtyPages() == 0x01, "dissolve: only the pages that changed are dirty");
}

// The proof renderer's dissolve from the silent voice line to MUTED, byte for byte. The line is
// the ribbon in silence, which starts at column 4.
void checkDissolveAgainstProof() {
  Canvas line;
  line.fillRect(4, 32, kCanvasWidth - 4, 1, true);
  Canvas muted;
  muted.drawText((kCanvasWidth - Canvas::textWidth(font16x24(), "MUTED")) / 2, (kCanvasHeight - 24) / 2, font16x24(), "MUTED");
  Canvas r;
  r.dissolve(line, muted, 1);
  check(matchesGolden(r, "muted-1"), "dissolve: step 1 from the line to MUTED matches the proof frame");
  r.dissolve(line, muted, 2);
  check(matchesGolden(r, "muted-2"), "dissolve: step 2 matches the proof frame");
  r.dissolve(line, muted, 3);
  check(matchesGolden(r, "muted-3"), "dissolve: the last step matches the proof frame");
}

// ---------------------------------------------------------------------------------------------
// The panel link (core/display.h): when the display backend sends a page, restarts the panel or waits.

void checkPanelLinkHealthy() {
  PanelLink link;
  check(link.step(0) == PanelStep::Restart && !link.up(), "panel link: a fresh link starts the panel first");
  check(link.started(true, 10) == PanelEvent::None && link.up() && link.step(11) == PanelStep::Send,
        "panel link: a start that answers lets the pages go out");
  check(link.wrote(true, 12) == PanelEvent::None && link.shown(true, 13) == PanelEvent::None && link.failures() == 0,
        "panel link: pages and a display-on that answer report nothing");
}

// One failed page restarts the panel on the next pass, reports Lost once, and Back once a whole frame
// is in again.
void checkPanelLinkGlitch() {
  PanelLink link;
  link.started(true, 0);
  link.shown(true, 5);
  check(link.wrote(false, 100) == PanelEvent::Lost && !link.up(), "panel link: the first failed page reports Lost");
  check(link.step(100) == PanelStep::Restart, "panel link: the panel restarts on the next pass, without waiting");
  check(link.started(true, 101) == PanelEvent::None && link.step(102) == PanelStep::Send,
        "panel link: a restart that answers sends again");
  check(link.wrote(true, 103) == PanelEvent::None && link.shown(true, 130) == PanelEvent::Back && link.failures() == 1,
        "panel link: Back once the whole frame is in, after one failure");
  check(link.wrote(false, 5000) == PanelEvent::Lost, "panel link: a later failure starts a new run");
}

// A panel that stays away costs one start per retry window and reports once.
void checkPanelLinkDown() {
  PanelLink link;
  check(link.started(false, 1000) == PanelEvent::Lost && link.step(1000) == PanelStep::Wait,
        "panel link: a boot start that fails reports Lost and waits");
  check(link.step(1999) == PanelStep::Wait && link.step(2000) == PanelStep::Restart, "panel link: the next start comes 1 s later");
  check(link.started(false, 2000) == PanelEvent::None && link.step(2999) == PanelStep::Wait,
        "panel link: a second failure stays quiet and keeps the 1 s window");
  link.started(true, 3000);
  check(link.wrote(false, 3001) == PanelEvent::None && link.step(3002) == PanelStep::Wait && link.step(4001) == PanelStep::Restart,
        "panel link: a failure before the frame is back waits for the window too");
  check(link.started(true, 4001) == PanelEvent::None && link.shown(false, 4010) == PanelEvent::None &&
            link.step(4011) == PanelStep::Wait,
        "panel link: a display-on that fails counts as a failed write");
  link.started(true, 5010);
  check(link.shown(true, 5050) == PanelEvent::Back && link.failures() == 4, "panel link: Back counts the run's 4 failures");

  PanelLink wrap;
  wrap.started(false, 0xFFFFFF00u);
  check(wrap.step(0xFFFFFF00u + kPanelRetryMs - 1) == PanelStep::Wait && wrap.step(0xFFFFFF00u + kPanelRetryMs) == PanelStep::Restart,
        "panel link: the retry window survives a millis() wrap");
}

}  // namespace

void checkCanvas() {
  checkCanvasText();
  checkCanvasWidth();
  checkCanvasEdges();
  checkCanvasDirtyApi();
  checkCanvasFallback();
  checkFlatGlyph();
  checkCanvasFonts();
  checkCanvasRects();
  checkInkTables();
  checkInkSpacing();
  checkInkWidthAndCentring();
  checkInkPen();
  checkInkClipping();
  checkTabularDigits();
  checkFixedPitch5x7();
  checkSetPixel();
  checkClippedDigits();
  checkBayer();
  checkCopyFrom();
  checkDissolveSteps();
  checkDissolveCoverage();
  checkDissolveInPlace();
  checkDissolveAgainstProof();
  checkPanelLinkHealthy();
  checkPanelLinkGlitch();
  checkPanelLinkDown();
}
