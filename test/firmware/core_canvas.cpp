// Checks the OLED canvas on the host. Covers text drawing, clipping, the dirty-page API, glyph
// fallback, the three fonts, rectangle fills, text spaced by ink, tabular digits and the 5x7
// fixed pitch.

#include "support.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

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
  namespace f = cubevox::fonts::StylizedHyperlegible5x7;
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

}  // namespace

void checkCanvas() {
  checkCanvasText();
  checkCanvasWidth();
  checkCanvasEdges();
  checkCanvasDirtyApi();
  checkCanvasFallback();
  checkCanvasFonts();
  checkCanvasRects();
  checkInkTables();
  checkInkSpacing();
  checkInkWidthAndCentring();
  checkInkPen();
  checkInkClipping();
  checkTabularDigits();
  checkFixedPitch5x7();
}
