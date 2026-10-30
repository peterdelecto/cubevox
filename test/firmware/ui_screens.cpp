// Checks the runtime screens on the host. Covers the readout against the proof renderer byte for
// byte (settled, rolling and muted), its title bar, rows and edges, the value row centred on what is
// drawn with its unit in lowercase, the key names centred each on its own width with their flats, the
// voice ribbon, the boxed input level meter with its 16 segments, the muted word and the DEBUG load figure.
// The proof PGMs go to build/oled-*.pgm.

#include "support.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/fonts.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/registry.h"
#include "ui/screens.h"

namespace {

bool lit(const Canvas& c, int x, int y) { return ((c.page(y / 8)[x] >> (y % 8)) & 1) != 0; }

// True when rows y0 up to y1 - 1 of the two canvases hold the same pixels.
bool sameRows(const Canvas& a, const Canvas& b, int y0, int y1) {
  for (int y = y0; y < y1; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (lit(a, x, y) != lit(b, x, y)) return false;
    }
  }
  return true;
}

bool rowsBlank(const Canvas& c, int y0, int y1) {
  for (int y = y0; y < y1; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (lit(c, x, y)) return false;
    }
  }
  return true;
}

// The readout's title bar (owner 2026-10-30): rows 0 to 13 lit across the full width, the slot
// number and card name on it in black 7X12 with their top at row 1, so 1 px of bar lies above and
// below the 12 px glyphs. The parameter name's top is at row 16.
constexpr int kBarRows = 14;
constexpr int kTitleTop = 1;
constexpr int kNameTop = 16;

// The outermost ring of pixels must stay dark, which keeps text 1 px clear of every edge. The title
// bar lights rows 0 to 13 edge to edge, so a readout asks from row `top` down, with row 0 skipped.
bool edgesDark(const Canvas& c, int top = 0) {
  for (int x = 0; x < kCanvasWidth; ++x) {
    if ((top == 0 && lit(c, x, 0)) || lit(c, x, kCanvasHeight - 1)) return false;
  }
  for (int y = top; y < kCanvasHeight; ++y) {
    if (lit(c, 0, y) || lit(c, kCanvasWidth - 1, y)) return false;
  }
  return true;
}

// The title bar built from the pick: `text` in 7X12 centred with its top at row 1, then rows 0 to 13
// inverted over it, so the glyphs come out black on the bar.
Canvas titleBarOf(const char* text) {
  Canvas c;
  c.drawText((kCanvasWidth - Canvas::textWidth(font7x12(), text)) / 2, kTitleTop, font7x12(), text);
  c.invertRect(0, 0, kCanvasWidth, kBarRows);
  return c;
}

// True when the bar's first and last rows and both side columns are lit end to end and the two rows
// under it are blank, so nothing but the title's glyphs is dark inside the bar.
bool barSpansWidth(const Canvas& c) {
  for (int x = 0; x < kCanvasWidth; ++x) {
    if (!lit(c, x, 0) || !lit(c, x, kBarRows - 1)) return false;
  }
  for (int y = 0; y < kBarRows; ++y) {
    if (!lit(c, 0, y) || !lit(c, kCanvasWidth - 1, y)) return false;
  }
  for (int y = kBarRows; y < kNameTop; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (lit(c, x, y)) return false;
    }
  }
  return true;
}

// First and last column holding a dark pixel inside the bar, -1 when the bar is solid.
struct DarkSpan {
  int left = -1;
  int right = -1;
};

DarkSpan darkInBar(const Canvas& c) {
  DarkSpan span;
  for (int x = 0; x < kCanvasWidth; ++x) {
    for (int y = 0; y < kBarRows; ++y) {
      if (lit(c, x, y)) continue;
      if (span.left < 0) span.left = x;
      span.right = x;
      break;
    }
  }
  return span;
}

// The readout of knob `knob` on the card in `slot` of the default layout.
ReadoutView viewOf(int slot, int knob, float shown) {
  const Card c = defaultLayout().card[static_cast<size_t>(slot)];
  ReadoutView v;
  v.slot = slot;
  v.card = cardDesc(c).name;
  v.knob = knobDesc(c, knob);
  v.shown = shown;
  v.target = std::isfinite(shown) ? static_cast<int>(std::lround(shown)) : 0;
  return v;
}

Canvas readoutOf(int slot, int knob, float shown) {
  Canvas c;
  renderReadout(c, viewOf(slot, knob, shown));
  return c;
}

// ---------------------------------------------------------------------------------------------
// The proof's knob turn, copied from tools/oled-proof/oled_motion.cpp: a cubic ease from one value
// to another over 2 s, sampled at 25 fps, with the readout easing toward it at a 20 ms time constant.

constexpr float kProofDt = 1.0f / 25;
constexpr float kProofTurnS = 2.0f;
constexpr float kProofTauS = 0.02f;

float proofEase(float p) {
  p = std::min(1.0f, std::max(0.0f, p));
  return p < 0.5f ? 4.0f * p * p * p : 1.0f - std::pow(-2.0f * p + 2.0f, 3.0f) / 2.0f;
}

float proofShown(float from, float to, bool stepped, int frame) {
  float shown = 0.0f;
  for (int i = 0; i <= frame; ++i) {
    const float t = static_cast<float>(i) * kProofDt;
    float target = from + (to - from) * proofEase(std::min(t, kProofTurnS) / kProofTurnS);
    if (stepped) target = std::round(target);
    if (i == 0) shown = target;
    shown += (target - shown) * (1.0f - std::exp(-kProofDt / kProofTauS));
  }
  return shown;
}

void checkReadoutSettled() {
  struct Case {
    int slot;
    int knob;
    float value;
    const char* golden;
    const char* pgm;
  };
  const Case cases[] = {
      {4, 0, 210.0f, "settled-210ms", "oled-readout-210ms.pgm"},
      {7, 1, 65.0f, "settled-65pct", "oled-readout-65pct.pgm"},
      {2, 0, -12.0f, "settled-m12st", "oled-readout-m12st.pgm"},
      {6, 0, -24.0f, "settled-m24db", "oled-readout-m24db.pgm"},
  };
  for (const Case& c : cases) {
    const Canvas canvas = readoutOf(c.slot, c.knob, c.value);
    check(matchesGolden(canvas, c.golden), (std::string("readout: ") + c.golden + " matches the proof frame").c_str());
    writePgm(canvas, c.pgm);
  }
}

void checkReadoutRolling() {
  for (int frame : {8, 12, 16, 20, 24, 28, 32, 40, 52}) {
    const Canvas canvas = readoutOf(4, 0, proofShown(120.0f, 210.0f, false, frame));
    const std::string name = "roll-time-" + std::to_string(frame);
    check(matchesGolden(canvas, name.c_str()), ("readout: " + name + " matches the proof frame").c_str());
  }
  for (int frame : {6, 12, 16, 20, 24, 28, 32, 40}) {
    const Canvas canvas = readoutOf(2, 0, proofShown(0.0f, -12.0f, true, frame));
    const std::string name = "roll-semi-" + std::to_string(frame);
    check(matchesGolden(canvas, name.c_str()), ("readout: " + name + " matches the proof frame").c_str());
    if (frame == 32) writePgm(canvas, "oled-readout-roll-m10-m9.pgm");  // -9.9, the number gains a digit
  }
}

void checkReadoutRows() {
  const Canvas c = readoutOf(4, 0, 210.0f);
  check(sameRows(c, titleBarOf("5 SLAPBACK"), 0, kNameTop),
        "readout: the first rows are the slot, a space and the card in black 7X12 on a bar, top at row 1");
  Canvas name;
  name.drawText((kCanvasWidth - Canvas::textWidth(font9x12(), "TIME")) / 2, kNameTop, font9x12(), "TIME");
  check(sameRows(c, name, kNameTop, 30), "readout: the parameter name is 9X12 centred, top at 16");
  check(rowsBlank(c, 28, 34) && rowsBlank(c, 58, kCanvasHeight), "readout: nothing between the rows or under the value");
  check(!rowsBlank(c, 34, 58) && edgesDark(c, kBarRows), "readout: the value fills the band from row 34 to 57 and the edges below the bar stay dark");
}

// ---------------------------------------------------------------------------------------------
// The value field (owner 2026-10-30). These tests build the row from the spec's wording and take
// nothing from the renderer. A number is its digits, one cell each, with a sign cell in front when
// the knob's range reaches below zero and the number is not zero. A roll sits between two numbers.
// Its field holds the cells of the wider one, the digits right-aligned and a sign cell first when
// either number has a sign, and a digit or sign that only one number has is a blank in the other. A
// value at rest is one number, so its field is that number's own width. The field, a space and the
// unit are centred on the panel, and the unit is written as in print (st, ms, dB, %).

constexpr int kValueTop = 34;
constexpr int kValueBottom = kValueTop + 24;
constexpr int kValueHeight = kValueBottom - kValueTop;

// The lowest and highest number a numeric knob can show. A continuous knob reaches both ends at the
// readings 0 and 1, and a stepped one at its first and last positions.
void shownRange(const KnobDesc& k, int& lo, int& hi) {
  lo = knobShown(k, 0.0f);
  hi = lo;
  const int count = k.kind == KnobKind::Stepped ? k.positions : 2;
  for (int i = 0; i < count; ++i) {
    const int shown = knobShown(k, static_cast<float>(i));
    lo = std::min(lo, shown);
    hi = std::max(hi, shown);
  }
}

// True when any reading of the knob is a negative number, which gives its numbers a sign cell.
bool signedRange(const KnobDesc& k) {
  int lo = 0;
  int hi = 0;
  shownRange(k, lo, hi);
  return lo < 0;
}

// The two numbers of a roll, one character per cell, both as long as the field.
struct Pair {
  std::string from;
  std::string to;
};

Pair pairCells(bool signedKnob, int from, int to) {
  const std::string digitsFrom = std::to_string(std::abs(from));
  const std::string digitsTo = std::to_string(std::abs(to));
  const size_t digits = std::max(digitsFrom.size(), digitsTo.size());
  const bool signCell = signedKnob && (from != 0 || to != 0);
  const auto lay = [&](const std::string& d, int v) {
    std::string cells = std::string(digits - d.size(), ' ') + d;
    if (signCell) cells.insert(cells.begin(), v < 0 ? '-' : (v > 0 ? '+' : ' '));
    return cells;
  };
  return {lay(digitsFrom, from), lay(digitsTo, to)};
}

// The number alone: its sign if it has one, then its digits.
std::string ownCells(bool signedKnob, int v) { return pairCells(signedKnob, v, v).from; }

int cellPitch() { return font16x24().width + font16x24().spacing; }

// The unit as it is written in print, with the space in front of it. "" when the knob has none.
std::string unitOf(const KnobDesc& k) { return k.unit[0] == '\0' ? std::string() : std::string(" ") + k.unit; }

// Left edge of the first of `cells` cells when the cells, the space and the unit are centred.
int fieldLeft(const KnobDesc& k, int cells) {
  const FontDesc& big = font16x24();
  const std::string unit = unitOf(k);
  const int unitWidth = unit.empty() ? 0 : big.spacing + Canvas::textWidth(big, unit.c_str());
  return (kCanvasWidth - (cells * cellPitch() - big.spacing + unitWidth)) / 2;
}

void drawUnit(Canvas& c, const KnobDesc& k, int pen) {
  const std::string unit = unitOf(k);
  if (unit.empty()) return;
  c.drawText(pen, kValueTop, font16x24(), unit.c_str());
}

// The value rows when the readout stands `rolled` px of the way from `base` to base + 1, which is
// shown = base + rolled / 24. At 0 px the number is base alone and at 24 px it is base + 1 alone.
// In between, the cells that differ between the two numbers roll up out of the band while the next
// ones roll in.
Canvas valueBand(const KnobDesc& k, int base, int rolled = 0) {
  const FontDesc& big = font16x24();
  const bool signedKnob = signedRange(k);
  const bool atRest = rolled <= 0 || rolled >= kValueHeight;
  const int from = rolled >= kValueHeight ? base + 1 : base;
  const Pair p = pairCells(signedKnob, from, atRest ? from : base + 1);
  const int lift = atRest ? 0 : rolled;
  const int x0 = fieldLeft(k, static_cast<int>(p.from.size()));
  Canvas c;
  for (size_t j = 0; j < p.from.size(); ++j) {
    const int x = x0 + static_cast<int>(j) * cellPitch();
    const char a[2] = {p.from[j], '\0'};
    const char b[2] = {p.to[j], '\0'};
    if (p.from[j] == p.to[j]) {
      c.drawDigits(x, kValueTop, big, a);
    } else {
      c.drawDigitsClipped(x, kValueTop - lift, big, a, kValueTop, kValueBottom);
      c.drawDigitsClipped(x, kValueBottom - lift, big, b, kValueTop, kValueBottom);
    }
  }
  drawUnit(c, k, x0 + static_cast<int>(p.from.size()) * cellPitch());
  return c;
}

// Every number a knob can show draws as its own cells, centred, whatever its digit count or sign.
void checkReadoutField() {
  const Layout layout = defaultLayout();
  bool inPlace = true;
  int numbers = 0;
  for (int slot = 0; slot < kSlotCount; ++slot) {
    for (int knob = 0; knob < kKnobsPerCard; ++knob) {
      const KnobDesc* k = knobDesc(layout.card[static_cast<size_t>(slot)], knob);
      if (!k || k->labels) continue;
      int lo = 0;
      int hi = 0;
      shownRange(*k, lo, hi);
      for (int v = lo; v <= hi; ++v) {
        inPlace = inPlace && sameRows(readoutOf(slot, knob, static_cast<float>(v)), valueBand(*k, v), kValueTop, kValueBottom);
        ++numbers;
      }
    }
  }
  check(inPlace && numbers > 2000, "readout: every number of every knob draws as its own cells, centred, cell by cell");
}

// Between two neighbouring numbers the readout draws every roll position as the spec says: the
// wider number's field while a digit or sign is in motion, the one number alone at 0 px and 24 px.
void checkReadoutRolls() {
  const Layout layout = defaultLayout();
  bool same = true;
  int frames = 0;
  for (int slot = 0; slot < kSlotCount; ++slot) {
    for (int knob = 0; knob < kKnobsPerCard; ++knob) {
      const KnobDesc* k = knobDesc(layout.card[static_cast<size_t>(slot)], knob);
      if (!k || k->labels) continue;
      int lo = 0;
      int hi = 0;
      shownRange(*k, lo, hi);
      std::vector<int> bases;
      for (int b = lo; b < hi; b += std::max(1, (hi - lo) / 30)) bases.push_back(b);
      for (int b : {-100, -11, -10, -9, -2, -1, 0, 1, 8, 9, 10, 98, 99, 100, 999}) {
        if (b >= lo && b < hi) bases.push_back(b);
      }
      for (int b : bases) {
        for (int rolled = 0; rolled <= kValueHeight; ++rolled) {
          const float shown = static_cast<float>(b) + static_cast<float>(rolled) / static_cast<float>(kValueHeight);
          same = same && sameRows(readoutOf(slot, knob, shown), valueBand(*k, b, rolled), kValueTop, kValueBottom);
          ++frames;
        }
      }
    }
  }
  check(same && frames > 5000, "readout: every roll position between neighbouring numbers draws as the wider field, or one number alone at 0 and 24 px");
}

// ---------------------------------------------------------------------------------------------
// The lateral sweep. Every knob of every card in the registry runs through its whole range one step
// at a time, and each settled frame is measured on its pixels. The test takes no position from the
// renderer and none from a model of the layout. It finds a glyph by matching its picture, and any
// column that differs from the first frame fails, however small the change.

// The value rows as one word per column. Bit r of a word is row kValueTop + r.
using Columns = std::array<uint32_t, kCanvasWidth>;

Columns bandColumns(const Canvas& c) {
  Columns cols{};
  for (int x = 0; x < kCanvasWidth; ++x) {
    for (int r = 0; r < kValueBottom - kValueTop; ++r) {
      if (lit(c, x, kValueTop + r)) cols[static_cast<size_t>(x)] |= 1u << r;
    }
  }
  return cols;
}

// The picture of one character in a tabular cell whose left edge is column 0.
std::vector<uint32_t> cellPicture(char ch) {
  const char text[2] = {ch, '\0'};
  Canvas c;
  c.drawDigits(0, kValueTop, font16x24(), text);
  const Columns cols = bandColumns(c);
  return std::vector<uint32_t>(cols.begin(), cols.begin() + font16x24().width);
}

// Every column from which the band shows exactly that picture, left to right.
std::vector<int> picturesIn(const Columns& cols, const std::vector<uint32_t>& picture) {
  std::vector<int> found;
  const int width = static_cast<int>(picture.size());
  for (int x = 0; x + width <= kCanvasWidth; ++x) {
    bool same = true;
    for (int i = 0; same && i < width; ++i) same = cols[static_cast<size_t>(x + i)] == picture[static_cast<size_t>(i)];
    if (same) found.push_back(x);
  }
  return found;
}

// The first and last lit columns of the band from column `from` on. Both are -1 when none is lit.
struct Extent {
  int left = -1;
  int right = -1;
};

Extent inkFrom(const Columns& cols, int from) {
  Extent e;
  for (int x = from; x < kCanvasWidth; ++x) {
    if (cols[static_cast<size_t>(x)] == 0) continue;
    if (e.left < 0) e.left = x;
    e.right = x;
  }
  return e;
}

// The settled readout of a knob on any card, showing the number `shown`.
Canvas settledReadout(Card cardId, int slot, int knob, int shown) {
  ReadoutView v;
  v.slot = slot;
  v.card = cardDesc(cardId).name;
  v.knob = knobDesc(cardId, knob);
  v.shown = static_cast<float>(shown);
  v.target = shown;
  Canvas c;
  renderReadout(c, v);
  return c;
}

// Every number the knob can show, in order. A stepped knob has one for each position. A continuous
// knob has every whole step from the lowest number its travel reaches to the highest, found over the
// 4096 readings of the pot's 12-bit converter.
std::vector<int> sweepValues(const KnobDesc& k) {
  std::vector<int> values;
  if (k.kind == KnobKind::Stepped) {
    for (int i = 0; i < k.positions; ++i) values.push_back(knobShown(k, static_cast<float>(i)));
    return values;
  }
  constexpr int kReadings = 4096;
  int lo = knobShown(k, 0.0f);
  int hi = lo;
  for (int i = 0; i < kReadings; ++i) {
    const int shown = knobShown(k, static_cast<float>(i) / static_cast<float>(kReadings - 1));
    lo = std::min(lo, shown);
    hi = std::max(hi, shown);
  }
  for (int shown = lo; shown <= hi; ++shown) values.push_back(shown);
  return values;
}

// The number with its unit, or the name of a labelled knob, for a failure message.
std::string valueText(const KnobDesc& k, int shown) {
  if (k.labels) return knobLabel(k, shown);
  char cells[kMaxKnobCells + 1];
  formatCells(k, shown, cells, sizeof(cells));
  std::string text(cells);
  text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
  return text + unitOf(k);
}

// Finds each non-blank character of `cells` in the band by its picture, left to right. A cell takes
// the leftmost match that clears the cell before it, and a blank cell has no ink to find and stays at
// -1. False when a character has no picture or no match.
bool placeCells(const Columns& cols, const std::string& cells, std::vector<std::vector<uint32_t>>& pictures,
                std::vector<int>& at) {
  at.assign(cells.size(), -1);
  int next = 0;
  for (size_t j = 0; j < cells.size(); ++j) {
    if (cells[j] == ' ') continue;
    std::vector<uint32_t>& picture = pictures[static_cast<size_t>(cells[j])];
    if (picture.empty()) picture = cellPicture(cells[j]);
    if (std::all_of(picture.begin(), picture.end(), [](uint32_t word) { return word == 0; })) return false;
    int pick = -1;
    for (int x : picturesIn(cols, picture)) {
      if (x >= next) {
        pick = x;
        break;
      }
    }
    if (pick < 0) return false;
    at[j] = pick;
    next = pick + font16x24().width;
  }
  return true;
}

// What a sweep remembers: the first and the previous settled frame, and, for each count of cells a
// number can have, where the first frame with that many cells put each cell and the unit. Later
// frames with the same count must put them in the same columns.
struct Grid {
  bool known = false;
  std::vector<int> cellX;
  Extent unit;
};

struct SweepMemory {
  std::vector<Grid> grid = std::vector<Grid>(kMaxKnobCells + 1);
  Canvas first;
  Canvas previous;
};

// Every cell and the unit must sit where the first frame with this many cells put them, and the
// cells must stand edge to edge on the cell pitch. Fills `at` with the columns found.
std::string cellsInPlace(const Columns& cols, const std::string& cells, std::vector<std::vector<uint32_t>>& pictures,
                         SweepMemory& memory, std::vector<int>& at) {
  if (!placeCells(cols, cells, pictures, at)) return "the characters \"" + cells + "\" are not in their cells";
  for (size_t j = 1; j < at.size(); ++j) {
    if (at[j] - at[j - 1] != cellPitch()) {
      return "cell " + std::to_string(j + 1) + " of " + std::to_string(at.size()) + " does not stand a cell pitch after the one before";
    }
  }
  // The last cell always holds a digit, so the unit lies to the right of it.
  const Extent unit = inkFrom(cols, at.back() + font16x24().width);
  Grid& grid = memory.grid[cells.size()];
  if (!grid.known) {
    grid.known = true;
    grid.cellX = at;
    grid.unit = unit;
    return "";
  }
  for (size_t j = 0; j < at.size(); ++j) {
    if (at[j] != grid.cellX[j]) {
      return "cell " + std::to_string(j + 1) + " of " + std::to_string(at.size()) + " moved from x " +
             std::to_string(grid.cellX[j]) + " to x " + std::to_string(at[j]);
    }
  }
  if (unit.left != grid.unit.left || unit.right != grid.unit.right) {
    return "the unit moved from columns " + std::to_string(grid.unit.left) + " to " + std::to_string(grid.unit.right) +
           " to columns " + std::to_string(unit.left) + " to " + std::to_string(unit.right);
  }
  return "";
}

// The cells, the space and the unit stand centred: the margin left of the first cell and the one
// right of the unit's ink (or of the last cell, for a knob without a unit) differ by 1 px at most.
std::string fieldCentred(const Columns& cols, const std::vector<int>& at) {
  const Extent unit = inkFrom(cols, at.back() + font16x24().width);
  const int rightEdge = unit.right >= 0 ? unit.right + 1 : at.back() + font16x24().width;
  const int left = at.front();
  const int right = kCanvasWidth - rightEdge;
  if (std::abs(left - right) <= 1) return "";
  return "the field is not centred, " + std::to_string(left) + " px left of it and " + std::to_string(right) + " px right";
}

// The cells at the places found, and the unit after the last one, account for every lit pixel.
bool inkAccounted(const Canvas& frame, const KnobDesc& k, const std::string& cells, const std::vector<int>& at) {
  const FontDesc& big = font16x24();
  Canvas rebuilt;
  for (size_t j = 0; j < at.size(); ++j) {
    if (at[j] < 0) continue;
    const char one[2] = {cells[j], '\0'};
    rebuilt.drawDigits(at[j], kValueTop, big, one);
  }
  const std::string unit = unitOf(k);
  if (!unit.empty()) rebuilt.drawText(at.back() + cellPitch(), kValueTop, big, unit.c_str());
  return sameRows(frame, rebuilt, kValueTop, kValueBottom);
}

// Nothing outside the value rows changes, and each step looks new. Remembers the frame for the next.
std::string restStill(const Canvas& frame, size_t n, SweepMemory& memory) {
  if (n == 0) memory.first = frame;
  if (!sameRows(memory.first, frame, 0, kValueTop) || !sameRows(memory.first, frame, kValueBottom, kCanvasHeight)) {
    return "something outside the value rows changed";
  }
  if (n > 0 && sameRows(memory.previous, frame, kValueTop, kValueBottom)) return "the step looks like the one before";
  memory.previous = frame;
  return "";
}

// The settled pass of a numeric knob. Every number draws as its own cells, and each cell and the
// unit must sit where the first number with the same count of cells put them, so within one digit
// count nothing moves. The field stands centred, the rest of the screen holds still, and each step
// must look new. Returns what failed first, or "".
std::string sweepSettled(Card cardId, int slot, int knob, const KnobDesc& k, const std::vector<int>& values, bool signedKnob,
                         std::vector<std::vector<uint32_t>>& pictures, SweepMemory& memory, int& steps) {
  for (size_t n = 0; n < values.size(); ++n) {
    const int v = values[n];
    const Canvas frame = settledReadout(cardId, slot, knob, v);
    ++steps;
    const Columns cols = bandColumns(frame);
    const std::string cells = ownCells(signedKnob, v);
    std::vector<int> at;
    std::string moved = cellsInPlace(cols, cells, pictures, memory, at);
    if (moved.empty()) moved = fieldCentred(cols, at);
    if (moved.empty() && !inkAccounted(frame, k, cells, at)) moved = "the value rows hold ink that is no cell and not the unit";
    if (moved.empty()) moved = restStill(frame, n, memory);
    if (!moved.empty()) return "at " + valueText(k, v) + ", " + moved;
  }
  return "";
}

// The row re-centres by half a cell, within half a pixel, between neighbouring counts of cells: the
// first cell moves left by that much and the unit moves right by the rest of a cell pitch.
std::string recentresByHalfCell(const SweepMemory& memory) {
  const int pitch = cellPitch();
  for (size_t n = 1; n + 1 < memory.grid.size(); ++n) {
    const Grid& a = memory.grid[n];
    const Grid& b = memory.grid[n + 1];
    if (!a.known || !b.known) continue;
    const int cellMoved = a.cellX.front() - b.cellX.front();
    const int unitMoved = b.unit.left - a.unit.left;
    const bool half = cellMoved == pitch / 2 || cellMoved == (pitch + 1) / 2;
    if (!half || cellMoved + unitMoved != pitch) {
      return "from " + std::to_string(n) + " cells to " + std::to_string(n + 1) + " the first cell moved " + std::to_string(cellMoved) +
             " px left and the unit " + std::to_string(unitMoved) + " px right, not half of the " + std::to_string(pitch) + " px cell";
    }
  }
  return "";
}

// The roll pass of a numeric knob. Between every two neighbouring numbers, at five roll positions,
// each cell and the unit must sit on the columns the settled pass found for a number with as many
// cells as the wider of the two, and the value rows must hold the rolling cells there and nothing
// else. The positions come from the settled frames, never from a model of the layout.
std::string sweepRolls(Card cardId, int slot, int knob, const KnobDesc& k, const std::vector<int>& values, bool signedKnob,
                       const SweepMemory& memory, int& frames) {
  const FontDesc& big = font16x24();
  const int lifts[] = {1, 6, 12, 18, 23};
  for (size_t i = 0; i + 1 < values.size(); ++i) {
    const int v = values[i];
    if (values[i + 1] != v + 1) continue;
    const Pair p = pairCells(signedKnob, v, v + 1);
    const Grid& grid = memory.grid[p.from.size()];
    const std::string where = "between " + valueText(k, v) + " and " + valueText(k, v + 1) + ", ";
    if (!grid.known) return where + "no settled number has the " + std::to_string(p.from.size()) + " cells of the roll";
    const Canvas reference = settledReadout(cardId, slot, knob, v);
    for (int lift : lifts) {
      const float shown = static_cast<float>(v) + static_cast<float>(lift) / static_cast<float>(kValueHeight);
      ReadoutView view;
      view.slot = slot;
      view.card = cardDesc(cardId).name;
      view.knob = knobDesc(cardId, knob);
      view.shown = shown;
      view.target = v;
      Canvas frame;
      renderReadout(frame, view);
      ++frames;

      Canvas rebuilt;
      for (size_t j = 0; j < p.from.size(); ++j) {
        const char a[2] = {p.from[j], '\0'};
        const char b[2] = {p.to[j], '\0'};
        if (p.from[j] == p.to[j]) {
          rebuilt.drawDigits(grid.cellX[j], kValueTop, big, a);
        } else {
          rebuilt.drawDigitsClipped(grid.cellX[j], kValueTop - lift, big, a, kValueTop, kValueBottom);
          rebuilt.drawDigitsClipped(grid.cellX[j], kValueBottom - lift, big, b, kValueTop, kValueBottom);
        }
      }
      const std::string unit = unitOf(k);
      if (!unit.empty()) rebuilt.drawText(grid.cellX.back() + cellPitch(), kValueTop, big, unit.c_str());
      const std::string at = "at " + std::to_string(lift) + " px, ";
      if (!sameRows(frame, rebuilt, kValueTop, kValueBottom)) {
        return where + at + "the value rows are not the " + std::to_string(p.from.size()) + " cells and the unit on the columns of the settled numbers";
      }
      const Extent unitInk = inkFrom(bandColumns(frame), grid.cellX.back() + big.width);
      if (unitInk.left != grid.unit.left || unitInk.right != grid.unit.right) return where + at + "the unit is not where it sits when settled";
      if (!sameRows(frame, reference, 0, kValueTop) || !sameRows(frame, reference, kValueBottom, kCanvasHeight)) {
        return where + at + "something outside the value rows changed";
      }
    }
  }
  return "";
}

// Sweeps a labelled knob. Each name is drawn on its own width and stands centred, its left and right
// margins within 1 px, and names of different widths start at different columns.
std::string sweepLabelled(Card cardId, int slot, int knob, const KnobDesc& k, const std::vector<int>& values,
                          int& steps) {
  SweepMemory memory;
  bool startsDiffer = false;
  int firstLeft = -1;
  for (size_t n = 0; n < values.size(); ++n) {
    const int v = values[n];
    const std::string where = "at " + valueText(k, v) + ", ";
    const Canvas frame = settledReadout(cardId, slot, knob, v);
    ++steps;

    const Extent ink = inkFrom(bandColumns(frame), 0);
    if (ink.left < 0) return where + "the name draws nothing";
    const int leftMargin = ink.left;
    const int rightMargin = kCanvasWidth - 1 - ink.right;
    if (std::abs(leftMargin - rightMargin) > 1) {
      return where + "the name is not centred, " + std::to_string(leftMargin) + " px left of it and " + std::to_string(rightMargin) + " px right";
    }
    if (ink.right - ink.left + 1 != Canvas::textWidth(font16x24(), knobLabel(k, v))) return where + "the name is not drawn on its own width";
    if (n == 0) firstLeft = ink.left;
    startsDiffer = startsDiffer || ink.left != firstLeft;
    Canvas rebuilt;
    rebuilt.drawText(ink.left, kValueTop, font16x24(), knobLabel(k, v));
    if (!sameRows(frame, rebuilt, kValueTop, kValueBottom)) return where + "the value rows hold ink that is not the name";
    const std::string still = restStill(frame, n, memory);
    if (!still.empty()) return where + still;
  }
  if (!startsDiffer) return "every name starts at the same column, so none is centred on its own width";
  return "";
}

// Hard gate (owner 2026-10-24, "I don't want anything bouncing around laterally as values change",
// and of 2026-10-30, each value centred on what is drawn). Within one digit count the unit and every
// cell keep their columns, settled or rolling, and between digit counts the row re-centres by half a
// cell. Prints how many parameters and steps it covered.
void checkReadoutSweep() {
  std::vector<std::vector<uint32_t>> pictures(128);
  int expected = 0;
  int parameters = 0;
  int steps = 0;
  int rolledFrames = 0;
  for (int index = 1; index < kCardCount; ++index) {
    const Card cardId = card(index);
    const CardDesc& desc = cardDesc(cardId);
    for (int knob = 0; knob < desc.knobCount; ++knob) {
      ++expected;
      const KnobDesc* k = knobDesc(cardId, knob);
      if (!k) {
        check(false, (std::string("readout sweep: ") + desc.name + " has no knob " + std::to_string(knob + 1)).c_str());
        continue;
      }
      const std::string who = std::string("readout sweep: ") + desc.name + " " + k->name;
      if (k->decimals != 0) {
        check(false, (who + " declares decimals, which the sweep does not model yet").c_str());
        continue;
      }
      const std::vector<int> values = sweepValues(*k);
      const int slot = (index - 1) % kSlotCount;
      int done = 0;
      std::string moved;
      if (k->labels) {
        moved = sweepLabelled(cardId, slot, knob, *k, values, done);
      } else {
        const bool signedKnob = values.front() < 0;
        SweepMemory memory;
        moved = sweepSettled(cardId, slot, knob, *k, values, signedKnob, pictures, memory, done);
        if (moved.empty()) moved = recentresByHalfCell(memory);
        if (moved.empty()) moved = sweepRolls(cardId, slot, knob, *k, values, signedKnob, memory, rolledFrames);
      }
      check(moved.empty(), (who + " " + moved).c_str());

      // A sweep that found nothing must also have reached every step between the ends of the travel.
      const int reach = k->kind == KnobKind::Stepped ? k->positions : std::abs(knobShown(*k, 1.0f) - knobShown(*k, 0.0f)) + 1;
      if (moved.empty()) check(done == reach && reach >= 2, (who + " was not swept through its whole range").c_str());
      ++parameters;
      steps += done;
    }
  }
  check(parameters == expected && parameters > 0, "readout sweep: every knob of every card was swept");
  std::printf("readout sweep: %d parameters, %d settled steps, %d rolling frames\n", parameters, steps, rolledFrames);
}

// Where the unit's ink sits in the value rows, found by its picture. Both ends are -1 unless the
// picture appears exactly once.
Extent unitPicture(const Canvas& frame, const std::string& unitWithSpace) {
  const std::string unit = unitWithSpace.substr(1);
  Canvas alone;
  alone.drawText(0, kValueTop, font16x24(), unit.c_str());
  const Columns ink = bandColumns(alone);
  const int width = Canvas::textWidth(font16x24(), unit.c_str());
  const std::vector<uint32_t> picture(ink.begin(), ink.begin() + width);
  const std::vector<int> found = picturesIn(bandColumns(frame), picture);
  Extent e;
  if (found.size() == 1) {
    e.left = found[0];
    e.right = found[0] + width - 1;
  }
  return e;
}

// A number that gains a digit makes room for it as the roll starts, and the new digit lights its
// cell from below while the other digits roll.
void checkReadoutGainsDigit() {
  // 99 to 100 on a percent knob. 99 is two 9s, and the roll holds the three cells of 100.
  const KnobDesc& percent = *knobDesc(defaultLayout().card[7], 1);
  const Pair roll = pairCells(false, 99, 100);
  check(ownCells(false, 99) == "99" && ownCells(false, 100) == "100" && roll.from == " 99" && roll.to == "100",
        "readout: 99 is two 9s, 100 is three cells, and the roll between them is a blank and two 9s into 100");
  const Canvas settled99 = readoutOf(7, 1, 99.0f);
  const Canvas settled100 = readoutOf(7, 1, 100.0f);
  check(sameRows(settled99, valueBand(percent, 99), kValueTop, kValueBottom), "readout: 99 draws two cells, centred");
  check(sameRows(settled100, valueBand(percent, 100), kValueTop, kValueBottom), "readout: 100 draws three cells, centred");

  // The leading cell lights while the 9s roll to 0 in the cells they sit in during the roll.
  const Canvas half = readoutOf(7, 1, 99.5f);
  check(sameRows(half, valueBand(percent, 99, 12), kValueTop, kValueBottom), "readout: 99.5 rolls the 1 in and the 9s to 0 in the three cells of 100");
  const int x0 = fieldLeft(percent, 3);
  bool leadingTopBlank = true;
  for (int y = kValueTop; y < kValueTop + 12; ++y) {
    for (int x = x0; x < x0 + cellPitch(); ++x) leadingTopBlank = leadingTopBlank && !lit(half, x, y);
  }
  check(leadingTopBlank && !rowsBlank(half, kValueTop + 12, kValueBottom), "readout: the 1 rises into the leading cell from below");
  writePgm(half, "oled-readout-roll-99-100.pgm");

  // -9 to -10 on a signed knob. The minus stays in its own cell and does not roll.
  const KnobDesc& semitones = *knobDesc(defaultLayout().card[2], 0);
  check(signedRange(semitones) && ownCells(true, -9) == "-9" && ownCells(true, 5) == "+5" && ownCells(true, 0) == "0" &&
            ownCells(true, -10) == "-10" && pairCells(true, -10, -9).from == "-10" && pairCells(true, -10, -9).to == "- 9",
        "readout: a signed knob puts a sign in front of every number but zero, and the roll from -10 to -9 holds a blank beside the minus");
  check(sameRows(readoutOf(2, 0, -9.5f), valueBand(semitones, -10, 12), kValueTop, kValueBottom),
        "readout: -9.5 rolls the 1 out and the 0 to 9, with the minus in its own cell");
}

// The unit's left edge at each of the 25 roll positions from `base` to base + 1, 0 px to 24 px.
std::vector<int> unitLeftAcross(int slot, int knob, int base) {
  const KnobDesc& k = *knobDesc(defaultLayout().card[static_cast<size_t>(slot)], knob);
  std::vector<int> lefts;
  for (int rolled = 0; rolled <= kValueHeight; ++rolled) {
    const float shown = static_cast<float>(base) + static_cast<float>(rolled) / static_cast<float>(kValueHeight);
    lefts.push_back(unitPicture(readoutOf(slot, knob, shown), unitOf(k)).left);
  }
  return lefts;
}

// Half a cell, rounded either way, is how far the row re-centres when a digit or sign comes or goes.
bool isHalfCell(int px) { return px == cellPitch() / 2 || px == (cellPitch() + 1) / 2; }

// Across one roll the unit moves at most once. It moves by half a cell, to the right when the wider
// number is the one the roll ends on or starts from, at the start of the roll when the roll gains a
// digit or sign and at its end when the roll loses one.
std::string recentresOnce(int slot, int knob, int base, bool signedKnob) {
  const std::vector<int> lefts = unitLeftAcross(slot, knob, base);
  const size_t before = ownCells(signedKnob, base).size();
  const size_t after = ownCells(signedKnob, base + 1).size();
  int jumps = 0;
  for (size_t i = 0; i + 1 < lefts.size(); ++i) {
    if (lefts[i] < 0) return "the unit was not found";
    if (lefts[i] == lefts[i + 1]) continue;
    ++jumps;
    const bool atStart = i == 0;
    const bool atEnd = i + 2 == lefts.size();
    const int moved = lefts[i + 1] - lefts[i];
    if (before < after && !(atStart && isHalfCell(moved))) return "the unit moved at roll position " + std::to_string(i) + " by " + std::to_string(moved) + " px";
    if (before > after && !(atEnd && isHalfCell(-moved))) return "the unit moved at roll position " + std::to_string(i) + " by " + std::to_string(moved) + " px";
    if (before == after) return "the unit moved though both numbers have " + std::to_string(before) + " cells";
  }
  if (lefts.back() < 0) return "the unit was not found";
  if (before != after && jumps != 1) return "the unit moved " + std::to_string(jumps) + " times, not once";
  return "";
}

// The row re-centres by half a cell exactly when a digit or the sign appears or vanishes, once per
// roll, and never while two numbers of one digit count roll.
void checkReadoutRecentres() {
  const Layout layout = defaultLayout();
  std::string failure;
  int rolls = 0;
  int recentred = 0;
  for (int slot = 0; slot < kSlotCount && failure.empty(); ++slot) {
    for (int knob = 0; knob < kKnobsPerCard && failure.empty(); ++knob) {
      const KnobDesc* k = knobDesc(layout.card[static_cast<size_t>(slot)], knob);
      if (!k || k->labels) continue;
      int lo = 0;
      int hi = 0;
      shownRange(*k, lo, hi);
      const bool signedKnob = lo < 0;
      for (int b = lo; b < hi && failure.empty(); ++b) {
        const bool changes = ownCells(signedKnob, b).size() != ownCells(signedKnob, b + 1).size();
        if (!changes && (b - lo) % 7 != 0) continue;
        failure = recentresOnce(slot, knob, b, signedKnob);
        if (!failure.empty()) failure = std::string(cardDesc(layout.card[static_cast<size_t>(slot)]).name) + " " + k->name + " from " + std::to_string(b) + ", " + failure;
        ++rolls;
        recentred += changes ? 1 : 0;
      }
    }
  }
  check(failure.empty(), ("readout: " + failure).c_str());
  check(rolls > 200 && recentred > 20, "readout: the re-centring was measured on many rolls, those that change the digit count among them");

  // The cases the owner named, measured on the unit's pixels.
  const std::vector<int> nineToTen = unitLeftAcross(7, 1, 9);
  const std::vector<int> ninetyNineToHundred = unitLeftAcross(7, 1, 99);
  const std::vector<int> zeroToOne = unitLeftAcross(2, 0, 0);
  check(isHalfCell(nineToTen[1] - nineToTen[0]) && nineToTen[1] == nineToTen[12] && nineToTen[12] == nineToTen[24],
        "readout: 9 to 10 re-centres by half a cell as the roll starts and holds still after");
  check(isHalfCell(ninetyNineToHundred[1] - ninetyNineToHundred[0]) && ninetyNineToHundred[1] == ninetyNineToHundred[24],
        "readout: 99 to 100 re-centres by half a cell as the roll starts and holds still after");
  check(isHalfCell(zeroToOne[1] - zeroToOne[0]) && zeroToOne[1] == zeroToOne[24],
        "readout: 0 st to +1 st re-centres by half a cell as the sign appears");
  const std::vector<int> oneToZero = unitLeftAcross(2, 0, -1);
  check(oneToZero[0] == oneToZero[23] && isHalfCell(oneToZero[23] - oneToZero[24]),
        "readout: -1 st to 0 st holds the row until the roll ends, then re-centres by half a cell as the sign goes");
}

// The four readings the owner named draw their unit as written, lowercase, and stand centred. The
// margins are measured on the cell field and, for the record, on the ink itself.
void checkReadoutUnitsAndCentre() {
  struct Case {
    int slot;
    int knob;
    int value;
    const char* text;
    const char* capitals;
  };
  const Case cases[] = {
      {2, 0, -12, "-12 st", " ST"}, {4, 0, 250, "250 ms", " MS"}, {6, 0, -40, "-40 dB", " DB"}, {7, 1, 50, "50 %", " %"},
  };
  std::vector<std::vector<uint32_t>> pictures(128);
  for (const Case& c : cases) {
    const KnobDesc& k = *knobDesc(defaultLayout().card[static_cast<size_t>(c.slot)], c.knob);
    const Canvas frame = readoutOf(c.slot, c.knob, static_cast<float>(c.value));
    const std::string what = std::string("readout: ") + c.text + " ";

    // The unit is the lowercase string as written in print. The capitals are not on the screen.
    char unit[16];
    unitText(k, unit, sizeof(unit));
    const std::string tail = std::strchr(c.text, ' ');
    check(tail == unit, (what + "takes its unit from unitText as written").c_str());
    check(unitPicture(frame, tail).left >= 0, (what + "draws its unit lowercase as written").c_str());
    if (std::string(c.capitals) != tail) check(unitPicture(frame, c.capitals).left < 0, (what + "has no capital letters in its unit").c_str());

    // Centred within 1 px on the cells and the unit, and the cells are the number alone.
    const Columns cols = bandColumns(frame);
    std::vector<int> at;
    const bool signedKnob = signedRange(k);
    const std::string cells = ownCells(signedKnob, c.value);
    const bool placed = placeCells(cols, cells, pictures, at);
    check(placed && fieldCentred(cols, at).empty(), (what + "stands centred within 1 px").c_str());
    check(placed && inkAccounted(frame, k, cells, at), (what + "holds its cells and its unit and nothing else").c_str());
    check(sameRows(frame, valueBand(k, c.value), kValueTop, kValueBottom), (what + "is the model's row").c_str());

    const Extent ink = inkFrom(cols, 0);
    std::printf("readout: %s ink margins %d px left, %d px right\n", c.text, ink.left, kCanvasWidth - 1 - ink.right);
  }

  // A signed knob at zero draws no sign.
  const KnobDesc& semitones = *knobDesc(defaultLayout().card[2], 0);
  Canvas zero;
  zero.drawDigits(fieldLeft(semitones, 1), kValueTop, font16x24(), "0");
  drawUnit(zero, semitones, fieldLeft(semitones, 1) + cellPitch());
  check(sameRows(readoutOf(2, 0, 0.0f), zero, kValueTop, kValueBottom), "readout: 0 st is a lone 0 and its unit, centred, with no sign");
}

// A rolling digit stays inside the 24 px band of the value.
void checkReadoutRollClip() {
  bool clipped = true;
  bool dark = true;
  for (int k = 0; k <= 48; ++k) {
    const Canvas c = readoutOf(4, 0, 120.0f + static_cast<float>(k) / 24.0f);
    clipped = clipped && rowsBlank(c, 28, 34) && rowsBlank(c, 58, kCanvasHeight);
    dark = dark && edgesDark(c, kBarRows);
  }
  check(clipped, "readout: a rolling digit never leaves its band");
  check(dark, "readout: a rolling readout keeps the edges dark");
}

// Every number every knob can show keeps 1 px clear of the panel edge, rolling or settled.
void checkReadoutEdges() {
  const Layout layout = defaultLayout();
  bool dark = true;
  for (int slot = 0; slot < kSlotCount; ++slot) {
    for (int knob = 0; knob < kKnobsPerCard; ++knob) {
      const KnobDesc* k = knobDesc(layout.card[static_cast<size_t>(slot)], knob);
      if (!k) continue;
      const int count = k->kind == KnobKind::Stepped ? k->positions : 101;
      for (int i = 0; i < count; ++i) {
        const float reading = k->kind == KnobKind::Stepped ? static_cast<float>(i) : static_cast<float>(i) / 100.0f;
        const float shown = static_cast<float>(knobShown(*k, reading));
        dark = dark && edgesDark(readoutOf(slot, knob, shown), kBarRows) && edgesDark(readoutOf(slot, knob, shown + 0.5f), kBarRows);
      }
    }
  }
  check(dark, "readout: every value of every default-layout knob keeps its edges dark");
}

// The autotune key is a name. It draws in 16X24, does not roll, and sits centred on its own width.
const KnobDesc& keyKnob() { return *knobDesc(defaultLayout().card[1], 0); }

// Left column of a key name drawn centred on its own width.
int centredLeft(const char* name) { return (kCanvasWidth - Canvas::textWidth(font16x24(), name)) / 2; }

void checkKeyField() {
  const KnobDesc& key = keyKnob();
  int widest = 0;
  int narrowest = kCanvasWidth;
  for (int i = 0; i < key.positions; ++i) {
    widest = std::max(widest, Canvas::textWidth(font16x24(), key.labels[i]));
    narrowest = std::min(narrowest, Canvas::textWidth(font16x24(), key.labels[i]));
  }
  check(widest == 116 && centredLeft("F# / D#m") == 6, "readout: the widest key name is 116 px, so it starts at x 6");
  check(narrowest < widest && narrowest < 100, "readout: the key names differ in width, so centring each one gives different columns");

  bool inPlace = true;
  bool centred = true;
  for (int i = 0; i < key.positions; ++i) {
    const Canvas c = readoutOf(1, 0, static_cast<float>(i));
    Canvas want;
    want.drawText(centredLeft(key.labels[i]), kValueTop, font16x24(), key.labels[i]);
    inPlace = inPlace && sameRows(c, want, kValueTop, kValueBottom);
    const Extent ink = inkFrom(bandColumns(c), 0);
    centred = centred && ink.left == centredLeft(key.labels[i]) && std::abs(ink.left - (kCanvasWidth - 1 - ink.right)) <= 1;
  }
  check(inPlace, "readout: each key name is drawn in 16X24 on its own width, nothing else on the value rows");
  check(centred, "readout: every key name stands centred, its left and right margins within 1 px");
}

void checkKeyRows() {
  const Canvas key = readoutOf(1, 0, 11.0f);
  Canvas head = titleBarOf("2 AUTOTUNE");
  head.drawText((kCanvasWidth - Canvas::textWidth(font9x12(), "KEY")) / 2, kNameTop, font9x12(), "KEY");
  check(sameRows(key, head, 0, kValueTop), "readout: the key's title bar and parameter name are as for any knob");
  writePgm(key, "oled-readout-key.pgm");

  check(edgesDark(readoutOf(1, 0, 6.0f), kBarRows), "readout: the widest key name, F# / D#m, keeps the edges below the bar dark");
  ReadoutView stray = viewOf(1, 0, 3.5f);
  stray.target = 99;
  Canvas clamped;
  renderReadout(clamped, stray);
  Canvas last;
  last.drawText(centredLeft("F / Dm"), kValueTop, font16x24(), "F / Dm");
  check(sameRows(clamped, last, kValueTop, kValueBottom), "readout: a target past the last key clamps to the last name");
}

// How often the ink of character `c` in 16X24 appears in the value rows, column for column.
int inkTimes(const Columns& cols, char c) {
  const char text[2] = {c, '\0'};
  Canvas alone;
  alone.drawText(0, kValueTop, font16x24(), text);
  const Columns ink = bandColumns(alone);
  const int width = Canvas::textWidth(font16x24(), text);
  const std::vector<uint32_t> picture(ink.begin(), ink.begin() + width);
  return static_cast<int>(picturesIn(cols, picture).size());
}

// A key name writes a flat as a lowercase b, and the 16X24 font draws the flat sign for it.
void checkKeyFlats() {
  const KnobDesc& key = keyKnob();
  int flats = 0;
  for (int i = 0; i < key.positions; ++i) flats += inkTimes(bandColumns(readoutOf(1, 0, static_cast<float>(i))), 'b');
  check(flats == 5, "readout: the five flats of Db / Bbm, Ab / Fm, Eb / Cm and Bb / Gm draw as the flat sign");

  const Canvas flat = readoutOf(1, 0, 7.0f);
  check(std::strcmp(knobLabel(key, 7), "Db / Bbm") == 0 && inkTimes(bandColumns(flat), 'b') == 2 &&
            inkTimes(bandColumns(flat), 'B') == 1,
        "readout: Db / Bbm shows two flats and one B");
  writePgm(flat, "oled-readout-key-flat.pgm");
}

void checkReadoutEdgeCases() {
  ReadoutView none;
  none.slot = 3;
  none.card = "Empty";
  Canvas c;
  renderReadout(c, none);
  check(sameRows(c, titleBarOf("4 EMPTY"), 0, kCanvasHeight), "readout: a slot with no knob draws the title bar only");

  // Values no knob produces still draw the title bar and never fault.
  const Canvas title = titleBarOf("3 OCTAVE");
  bool stable = true;
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (float shown : {nan, inf, -inf, 1.0e9f, -1.0e9f}) stable = stable && sameRows(readoutOf(2, 0, shown), title, 0, kNameTop);
  check(stable, "readout: a wild value still draws the title bar");

  // A knob with decimals rolls through the point. No card has one yet.
  const KnobDesc rate = {"Rate", KnobKind::Continuous, 0, 0.1f, 5.0f, "hz", Curve::Linear, 1};
  ReadoutView fine;
  fine.slot = 3;
  fine.card = "Unison";
  fine.knob = &rate;
  fine.shown = 14.5f;
  Canvas d;
  renderReadout(d, fine);
  check(edgesDark(d, kBarRows) && !rowsBlank(d, 34, 58), "readout: a decimal value draws with its point");

  // Rolling from -1 to 0, the sign cell rolls through a blank.
  const Canvas across = readoutOf(2, 0, -0.5f);
  check(edgesDark(across, kBarRows) && !rowsBlank(across, 34, 58), "readout: a value rolling from -1 to 0 draws");
}

// ---------------------------------------------------------------------------------------------
// Voice

Ribbon ribbonOf(std::initializer_list<float> oldestFirst) {
  Ribbon r;
  for (float level : oldestFirst) r.push(level);
  return r;
}

void checkRibbonHeight() {
  check(ribbonHalfHeight(0.0f) == 0 && ribbonHalfHeight(-1.0f) == 0 && ribbonHalfHeight(0.005f) == 0,
        "voice: no level, a negative one and a hair above zero are the line alone");
  check(ribbonHalfHeight(0.02f) == 0 && ribbonHalfHeight(0.03f) == 1, "voice: a level of 0.02 is the line and one of 0.03 is 1 px");
  check(ribbonHalfHeight(1.0f) == kRibbonHalfMax && kRibbonHalfMax == 22 && ribbonHalfHeight(5.0f) == 22,
        "voice: full level is 22 px each side of the horizon");
  check(ribbonHalfHeight(0.5f) == 11 && ribbonHalfHeight(0.1f) == 2, "voice: the height is the level times 22 px, with no curve");
  bool rising = true;
  int prev = 0;
  for (int i = 0; i <= 100; ++i) {
    const int h = ribbonHalfHeight(static_cast<float>(i) / 100.0f);
    rising = rising && h >= prev;
    prev = h;
  }
  check(rising, "voice: a louder level is never shorter");
  check(ribbonHalfHeight(std::numeric_limits<float>::quiet_NaN()) == 0, "voice: NaN is the line");
}

void checkRibbonHistory() {
  Ribbon r = ribbonOf({1.0f, 0.5f, 0.0f});
  check(r.half(0) == 0 && r.half(1) == 11 && r.half(2) == 22 && r.half(3) == 0, "ribbon: levels read newest first");
  check(r.half(-1) == 0 && r.half(kRibbonLevels) == 0 && r.half(1000) == 0, "ribbon: ages outside the history read zero");
  for (int i = 0; i < kRibbonLevels; ++i) r.push(1.0f);
  check(r.half(0) == 22 && r.half(kRibbonLevels - 1) == 22, "ribbon: the history holds 61 levels");
  r.push(0.0f);
  check(r.half(0) == 0 && r.half(1) == 22 && r.half(kRibbonLevels - 1) == 22, "ribbon: a push drops the oldest level");
  check(kRibbonLevels == 57 && kRibbonLeftX == 13 && kRibbonRightX == 126 && kRibbonLeftX + kRibbonLevels * kRibbonColumnsPerLevel == 127,
        "ribbon: 57 levels of two columns fill columns 13 to 126, and column 127 stays blank");
}

// The meter box and its segments, drawn from the owner's wording (2026-10-30) and nothing in the
// renderer. The outline is 1 px wide, its sides are columns 1 and 9 and its top and bottom are rows
// 8 and 56, so the interior is columns 2 to 8, rows 9 to 55. There are 16 segments, each 2 rows
// tall at columns 2 to 8, with 1 row of gap above. Segment 1 is rows 54 and 55, segment 2 rows 51
// and 52, up to segment 16 on rows 9 and 10. Columns 10 to 12 stay blank and the ribbon starts at
// column 13. The walk moves the finished picture.
constexpr int kBoxLeft = 1;
constexpr int kBoxRight = 9;
constexpr int kBoxTop = 8;
constexpr int kBoxBottom = 56;
constexpr int kBoxSegments = 16;
constexpr int kBoxFootprint = 13;  // columns 0 to 12 belong to the meter and its blank margin

int segmentBottomRow(int n) { return 55 - 3 * (n - 1); }
int segmentTopRow(int n) { return segmentBottomRow(n) - 1; }

Canvas meterFromDefinition(int segments, Walk walk) {
  Canvas c;
  for (int x = kBoxLeft; x <= kBoxRight; ++x) {
    c.setPixel(x + walk.x, kBoxTop + walk.y, true);
    c.setPixel(x + walk.x, kBoxBottom + walk.y, true);
  }
  for (int y = kBoxTop; y <= kBoxBottom; ++y) {
    c.setPixel(kBoxLeft + walk.x, y + walk.y, true);
    c.setPixel(kBoxRight + walk.x, y + walk.y, true);
  }
  for (int n = 1; n <= std::min(segments, kBoxSegments); ++n) {
    for (int y = segmentTopRow(n); y <= segmentBottomRow(n); ++y) {
      for (int x = kBoxLeft + 1; x < kBoxRight; ++x) c.setPixel(x + walk.x, y + walk.y, true);
    }
  }
  return c;
}

// True when columns x0 up to kCanvasWidth - 1 of rows y0 up to y1 - 1 are dark.
bool darkFrom(const Canvas& c, int x0, int y0, int y1) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < kCanvasWidth; ++x) {
      if (lit(c, x, y)) return false;
    }
  }
  return true;
}

// True when columns x0 up to x1 - 1 of rows y0 up to y1 - 1 are dark.
bool darkIn(const Canvas& c, int x0, int x1, int y0, int y1) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (lit(c, x, y)) return false;
    }
  }
  return true;
}

void checkVoiceSilence() {
  // In silence the screen is the horizon line from column 13 to column 126 at row 32 and the empty
  // meter box. Column 127 stays blank.
  Canvas line = meterFromDefinition(0, Walk{0, 0});
  line.fillRect(13, 32, 114, 1, true);
  for (int segments : {0, -7}) {
    Canvas c;
    renderVoice(c, Ribbon(), segments, Walk{0, 0});
    check(sameRows(c, line, 0, kCanvasHeight),
          ("voice: in silence, with " + std::to_string(segments) + " segments, the screen is the horizon line from column 13 to 126 at row 32 and the empty meter box").c_str());
  }
}

// The first level of the history, the oldest, sits in columns 13 and 14, the newest in columns 125 and
// 126, and nothing is left of the ribbon but the meter box and the blank columns 10 to 12 and 127.
void checkRibbonFirstColumn() {
  Ribbon r;
  for (int i = 0; i < kRibbonLevels; ++i) r.push(1.0f);
  Canvas c;
  renderVoice(c, r, 0, Walk{0, 0});
  bool solid = true;
  for (int x = 13; x <= 126; ++x) {
    for (int y = 32 - 22; y <= 32 + 22; ++y) solid = solid && lit(c, x, y);
  }
  check(solid, "ribbon: a full history is solid from column 13 to column 126");
  check(darkFrom(c, 127, 0, kCanvasHeight), "ribbon: column 127 stays blank under a full history");
  const Canvas box = meterFromDefinition(0, Walk{0, 0});
  bool boxOnly = true;
  for (int x = 0; x < kBoxFootprint; ++x) {
    for (int y = 0; y < kCanvasHeight; ++y) boxOnly = boxOnly && lit(c, x, y) == lit(box, x, y);
  }
  check(boxOnly, "ribbon: columns 0 to 12 hold the empty meter box and nothing else, columns 10 to 12 blank");

  // With only the oldest level tall, it fills columns 13 and 14 and the next column is the line.
  Ribbon oldest;
  oldest.push(1.0f);
  for (int i = 1; i < kRibbonLevels; ++i) oldest.push(0.0f);
  Canvas d;
  renderVoice(d, oldest, 0, Walk{0, 0});
  check(lit(d, 13, 32 - 22) && lit(d, 14, 32 + 22) && !lit(d, 15, 32 - 22) && !lit(d, 12, 32) && lit(d, 15, 32),
        "ribbon: the oldest level fills columns 13 and 14");
}

void checkVoiceShape() {
  Ribbon r;
  for (int i = 0; i < kRibbonLevels; ++i) r.push(0.0f);
  r.push(1.0f);  // 22 px, columns 123 and 124 after the next push
  r.push(0.5f);  // 11 px, columns 125 and 126
  Canvas c;
  renderVoice(c, r, 0, Walk{0, 0});

  bool solid = true;
  for (int y = 32 - 11; y <= 32 + 11; ++y) solid = solid && lit(c, 125, y) && lit(c, 126, y);
  for (int y = 32 - 22; y <= 32 + 22; ++y) solid = solid && lit(c, 123, y) && lit(c, 124, y);
  check(solid, "voice: each level spans two columns and is solid from the horizon out, the newest at column 126");
  check(lit(c, 100, 32) && !lit(c, 100, 31) && !lit(c, 100, 33), "voice: older silence is the line");
  check(darkFrom(c, 127, 0, kCanvasHeight), "voice: column 127 stays blank under the newest level");

  // A 1 px rim of 50 % Bayer dither caps each column, and nothing lies beyond it.
  bool rim = true;
  for (int x = 125; x <= 126; ++x) {
    rim = rim && lit(c, x, 32 - 12) == bayerLit(x, 32 - 12, 32) && lit(c, x, 32 + 12) == bayerLit(x, 32 + 12, 32);
    rim = rim && !lit(c, x, 32 - 13) && !lit(c, x, 32 + 13);
  }
  for (int x = 123; x <= 124; ++x) {
    rim = rim && lit(c, x, 32 - 23) == bayerLit(x, 32 - 23, 32) && lit(c, x, 32 + 23) == bayerLit(x, 32 + 23, 32);
    rim = rim && !lit(c, x, 32 - 24) && !lit(c, x, 32 + 24);
  }
  check(rim, "voice: a 1 px 50 % dither caps the column top and bottom");

  check(darkFrom(c, 13, 0, 9) && darkFrom(c, 13, 56, kCanvasHeight),
        "voice: the tallest column with its rim stays 9 px clear of the top and bottom");
}

// The voice screen drawn pixel by pixel from its definition, as an independent check of the whole
// frame. The ribbon runs from column 13 to 126. A column of half-height h is solid within h rows of
// the horizon, a 1 px rim lies one row beyond it where the Bayer threshold of the pixel is under
// 32, and a column of height 0 is the horizon pixel alone. The meter is its box and `segments`
// lit segments. The walk moves the finished picture.
Canvas voiceFromDefinition(const Ribbon& ribbon, int segments, Walk walk) {
  Canvas c = meterFromDefinition(segments, walk);
  for (int x = 13; x <= 126; ++x) {
    const int h = ribbon.half((126 - x) / 2);
    for (int y = 0; y < kCanvasHeight; ++y) {
      const int d = std::abs(y - 32);
      const bool on = h == 0 ? d == 0 : (d <= h || (d == h + 1 && bayerLit(x, y, 32)));
      if (on) c.setPixel(x + walk.x, y + walk.y, true);
    }
  }
  return c;
}

void checkVoiceWholeFrame() {
  uint32_t seed = 12345;
  bool same = true;
  const int counts[] = {0, 3, 6, 9, 12, 15, 16};
  for (int frame = 0; frame < 7; ++frame) {
    Ribbon r;
    for (int i = 0; i < kRibbonLevels + frame; ++i) {
      seed = seed * 1664525u + 1013904223u;
      r.push(static_cast<float>(seed >> 8) / 16777216.0f);
    }
    const int segments = counts[frame];
    for (uint32_t step = 0; step < 9; ++step) {
      Canvas c;
      renderVoice(c, r, segments, walkAt(step));
      same = same && sameRows(c, voiceFromDefinition(r, segments, walkAt(step)), 0, kCanvasHeight);
    }
  }
  check(same, "voice: whole frames match a pixel-by-pixel drawing of the meter box, its segments and the ribbon, at every step of the walk");
}

// The lit segments from the level, through the same map the face uses. The level is linear in dB of
// the input tick's RMS from -50 dBFS (0 segments) to 0 dBFS (16), and the count is round(level x 16).
void checkMeterSegments() {
  const auto segmentsAt = [](float db) { return meterSegments(level::displayLevel(std::pow(10.0f, db / 20.0f))); };
  check(kMeterSegments == 16 && kMeterSegmentRows == 2 && kMeterSegmentGap == 1, "meter: 16 segments of 2 rows with 1 row of gap");
  check(kMeterRows == 47 && kMeterTopY == 9 && kMeterTopY + kMeterRows - 1 == 55, "meter: the interior spans rows 9 to 55, 47 rows");
  check(segmentsAt(-50.0f) == 0, "meter: -50 dBFS lights no segment");
  check(segmentsAt(-25.0f) == 8, "meter: -25 dBFS lights 8 segments");
  check(segmentsAt(-6.0f) == 14, "meter: -6 dBFS lights 14 segments");
  check(segmentsAt(0.0f) == 16 && segmentsAt(6.0f) == 16, "meter: 0 dBFS lights all 16 segments and louder stays there");
  check(segmentsAt(-60.0f) == 0, "meter: under -50 dBFS lights no segment");

  // -25 dBFS is level 0.5. The level itself is checked in core_level.
  check(std::fabs(level::displayLevel(std::pow(10.0f, -25.0f / 20.0f)) - 0.5f) < 1e-4f && meterSegments(0.5f) == 8,
        "meter: -25 dBFS is the middle, 8 segments");

  check(meterSegments(0.0f) == 0 && meterSegments(-1.0f) == 0 && meterSegments(0.02f) == 0, "meter: no level, a negative one and a hair above zero light nothing");
  check(meterSegments(1.0f) == 16 && meterSegments(5.0f) == 16, "meter: full level and louder light all 16");
  check(meterSegments(std::numeric_limits<float>::quiet_NaN()) == 0, "meter: NaN lights nothing");

  bool formula = true;
  for (int i = 0; i <= 1000; ++i) {
    const float level = static_cast<float>(i) / 1000.0f;
    formula = formula && meterSegments(level) == static_cast<int>(std::lround(level * 16.0f));
  }
  check(formula, "meter: the segments are round(level x 16) at a thousand levels");
  bool rising = true;
  int prev = 0;
  for (int i = 0; i <= 400; ++i) {
    const int segments = meterSegments(static_cast<float>(i) / 400.0f);
    rising = rising && segments >= prev && segments - prev <= 1;
    prev = segments;
  }
  check(rising, "meter: a louder level never lights fewer segments and adds one at a time");
}

// The box outline stands exactly on columns 1 and 9 and rows 8 and 56, in silence and at every
// level. Its interior is columns 2 to 8, rows 9 to 55. Columns 0 and 10 to 12 stay blank, and nothing
// of the meter lies outside the box.
void checkMeterBox() {
  Canvas empty;
  renderVoice(empty, Ribbon(), 0, Walk{0, 0});
  bool outline = true;
  for (int x = 1; x <= 9; ++x) outline = outline && lit(empty, x, 8) && lit(empty, x, 56);
  for (int y = 8; y <= 56; ++y) outline = outline && lit(empty, 1, y) && lit(empty, 9, y);
  check(outline, "meter: the empty box has its top at row 8, its bottom at row 56 and its sides at columns 1 and 9, 1 px wide");

  bool interior = true;
  for (int y = 9; y <= 55; ++y) {
    for (int x = 2; x <= 8; ++x) interior = interior && !lit(empty, x, y);
  }
  check(interior, "meter: the interior, columns 2 to 8 over rows 9 to 55, is dark in silence");

  bool outside = true;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x : {0, 10, 11, 12}) outside = outside && !lit(empty, x, y);
    if (y < 8 || y > 56) {
      for (int x = 1; x <= 9; ++x) outside = outside && !lit(empty, x, y);
    }
  }
  check(outside, "meter: nothing lies in column 0, in columns 10 to 12, or above row 8 and below row 56");

  int boxPixels = 0;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kBoxFootprint; ++x) boxPixels += lit(empty, x, y) ? 1 : 0;
  }
  check(boxPixels == 2 * 9 + 2 * 47, "meter: the box is 2 x 9 px of top and bottom and 2 x 47 px of sides, 112 px in all");

  Canvas full;
  renderVoice(full, Ribbon(), 16, Walk{0, 0});
  Canvas over;
  renderVoice(over, Ribbon(), 999, Walk{0, 0});
  check(sameRows(over, full, 0, kCanvasHeight), "meter: segments past 16 stop at the 16th");

  // A full ribbon beside a full meter leaves columns 10 to 12 blank and the box whole.
  Ribbon loud;
  for (int i = 0; i < kRibbonLevels; ++i) loud.push(1.0f);
  Canvas both;
  renderVoice(both, loud, 16, Walk{0, 0});
  bool gap = true;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x : {0, 10, 11, 12, 127}) gap = gap && !lit(both, x, y);
  }
  check(gap && lit(both, 13, 32) && lit(both, 9, 32) && lit(both, 8, 31) && lit(both, 126, 32),
        "meter: columns 10 to 12 stay blank between a full meter and a full ribbon, and column 127 stays blank after it");
}

// Segment n stands on rows 54 and 55 for n = 1, and each next segment 3 rows higher, so the 16th is
// on rows 9 and 10. Every gap row between two segments is dark, and lit segments are the lowest ones.
void checkMeterSegmentRows() {
  check(segmentBottomRow(1) == 55 && segmentTopRow(1) == 54 && segmentBottomRow(2) == 52 && segmentTopRow(2) == 51 &&
            segmentBottomRow(16) == 10 && segmentTopRow(16) == 9,
        "meter: segment 1 is rows 54 and 55, segment 2 rows 51 and 52, segment 16 rows 9 and 10");

  bool segmentsOk = true;
  bool gapsDark = true;
  bool countsOk = true;
  for (int lit16 = 0; lit16 <= 16; ++lit16) {
    Canvas c;
    renderVoice(c, Ribbon(), lit16, Walk{0, 0});
    int counted = 0;
    for (int n = 1; n <= 16; ++n) {
      const bool wantLit = n <= lit16;
      bool top = true;
      bool bottom = true;
      for (int x = 2; x <= 8; ++x) {
        top = top && lit(c, x, segmentTopRow(n)) == wantLit;
        bottom = bottom && lit(c, x, segmentBottomRow(n)) == wantLit;
      }
      segmentsOk = segmentsOk && top && bottom;
      counted += (wantLit && top && bottom) ? 1 : 0;
      if (n < 16) {
        for (int x = 2; x <= 8; ++x) gapsDark = gapsDark && !lit(c, x, segmentTopRow(n) - 1);
      }
    }
    countsOk = countsOk && counted == lit16;
  }
  check(segmentsOk && countsOk, "meter: with n segments lit, the lowest n are lit at columns 2 to 8, both rows each, and the rest are dark");
  check(gapsDark, "meter: the gap rows 53, 50, down to 11 are dark whatever the count, since a gap holds no segment");

  Canvas all;
  renderVoice(all, Ribbon(), 16, Walk{0, 0});
  bool gapRows = true;
  for (int y = 11; y <= 53; y += 3) {
    for (int x = 2; x <= 8; ++x) gapRows = gapRows && !lit(all, x, y);
  }
  check(gapRows, "meter: with all 16 lit, rows 11, 14, 17 up to 53 at columns 2 to 8 are dark");

  bool whole = true;
  for (int lit16 = 0; lit16 <= 16; ++lit16) {
    Canvas c;
    renderVoice(c, Ribbon(), lit16, Walk{0, 0});
    const Canvas want = meterFromDefinition(lit16, Walk{0, 0});
    for (int x = 0; x < kBoxFootprint; ++x) {
      for (int y = 0; y < kCanvasHeight; ++y) whole = whole && lit(c, x, y) == lit(want, x, y);
    }
  }
  check(whole, "meter: columns 0 to 12 are the box and the lit segments and nothing else at every count");
}

// The box, the segments and the ribbon walk together: every step of the walk moves the whole picture
// by the same pixel.
void checkMeterWalks() {
  bool moves = true;
  const Ribbon r = ribbonOf({0.2f, 0.9f, 0.4f, 0.7f, 0.0f, 0.6f, 1.0f, 0.3f});
  for (int segments : {0, 9, 16}) {
    Canvas base;
    renderVoice(base, r, segments, Walk{0, 0});
    for (uint32_t step = 1; step < 9; ++step) {
      const Walk w = walkAt(step);
      Canvas c;
      renderVoice(c, r, segments, w);
      for (int y = 0; y < kCanvasHeight; ++y) {
        for (int x = 0; x < kCanvasWidth; ++x) {
          const int sx = x - w.x;
          const int sy = y - w.y;
          const bool want = sx >= 0 && sx < kCanvasWidth && sy >= 0 && sy < kCanvasHeight && lit(base, sx, sy);
          moves = moves && lit(c, x, y) == want;
        }
      }
      moves = moves && sameRows(c, voiceFromDefinition(r, segments, w), 0, kCanvasHeight);
    }
  }
  check(moves, "meter: at every step of the walk the box, its segments and the ribbon move together by one pixel");
}

Canvas toggleReadout(int slot, bool on);

// The screens that are not the voice screen carry no meter. The box's left side is column 1 and its
// first segments start at column 2, so a screen without the meter leaves columns 0 to 5 dark below
// its title bar. A key name may start at column 6, so the readout is held to that. The toggle readout,
// MUTED and BYPASS hold their word further in, from column 11 at the nearest, and leave columns 0 to
// 10, the box and its walk, dark.
void checkMeterAbsentOffVoice() {
  const Layout layout = defaultLayout();
  bool dark = true;
  for (int slot = 0; slot < kSlotCount; ++slot) {
    for (int knob = 0; knob < kKnobsPerCard; ++knob) {
      if (!knobDesc(layout.card[static_cast<size_t>(slot)], knob)) continue;
      dark = dark && darkIn(readoutOf(slot, knob, 0.0f), 0, 6, kBarRows, kCanvasHeight);
    }
    dark = dark && darkIn(toggleReadout(slot, true), 0, kBoxRight + 2, kBarRows, kCanvasHeight);
  }
  Canvas muted;
  renderMuted(muted, "MUTED");
  Canvas bypass;
  renderMuted(bypass, "BYPASS");
  dark = dark && darkIn(muted, 0, kBoxRight + 2, 0, kCanvasHeight) && darkIn(bypass, 0, kBoxRight + 2, 0, kCanvasHeight);
  check(dark, "meter: the readout below its title bar leaves columns 0 to 5 dark, and the toggle readout, MUTED and BYPASS leave columns 0 to 10 dark");
}

void checkVoiceScroll() {
  Ribbon r = ribbonOf({0.2f, 0.9f, 0.4f, 0.7f, 0.0f, 0.6f, 1.0f, 0.3f});
  Canvas before;
  renderVoice(before, r, 0, Walk{0, 0});
  r.push(0.8f);
  Canvas after;
  renderVoice(after, r, 0, Walk{0, 0});

  // The rim dither is anchored to the panel, so it does not move with the ribbon. The rest does.
  bool moved = true;
  for (int x = kRibbonLeftX; x + kRibbonColumnsPerLevel <= kRibbonRightX; ++x) {
    const int half = r.half((kRibbonRightX - x) / kRibbonColumnsPerLevel);
    for (int y = 0; y < kCanvasHeight; ++y) {
      if (std::abs(y - kRibbonHorizonY) == half + 1) continue;
      moved = moved && lit(after, x, y) == lit(before, x + kRibbonColumnsPerLevel, y);
    }
  }
  check(moved, "voice: a push scrolls the ribbon 2 px to the left");
  check(kRibbonColumnsPerLevel * kUiFps == 50, "voice: 2 px a tick at 25 fps is 50 px/s");
}

void checkVoiceWalk() {
  const Ribbon r = ribbonOf({0.2f, 0.9f, 0.4f, 0.7f, 0.0f, 0.6f, 1.0f, 0.3f, 0.5f, 0.8f});
  Canvas base;
  renderVoice(base, r, 9, Walk{0, 0});
  bool ok = true;
  for (int i = 1; i <= 8; ++i) {
    const Walk w = walkAt(static_cast<uint32_t>(i));
    Canvas moved;
    renderVoice(moved, r, 9, w);
    for (int y = 0; y < kCanvasHeight; ++y) {
      for (int x = 0; x < kCanvasWidth; ++x) {
        const int sx = x - w.x;
        const int sy = y - w.y;
        const bool want = sx >= 0 && sx < kCanvasWidth && sy >= 0 && sy < kCanvasHeight && lit(base, sx, sy);
        ok = ok && lit(moved, x, y) == want;
      }
    }
  }
  check(ok, "voice: every step of the walk is the whole screen moved 1 px, nothing redrawn");
}

// ---------------------------------------------------------------------------------------------
// Muted

void checkMuted() {
  Canvas c;
  renderMuted(c, "MUTED");
  check(matchesGolden(c, "muted-3"), "muted: the word matches the proof frame");
  writePgm(c, "oled-muted.pgm");

  Canvas bypass;
  renderMuted(bypass, "BYPASS");
  int first = kCanvasWidth;
  int last = -1;
  int top = kCanvasHeight;
  int bottom = -1;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (!lit(bypass, x, y)) continue;
      first = std::min(first, x);
      last = std::max(last, x);
      top = std::min(top, y);
      bottom = std::max(bottom, y);
    }
  }
  check(std::abs(first - (kCanvasWidth - 1 - last)) <= 1 && edgesDark(bypass), "muted: BYPASS is centred sideways with the edges dark");
  check(top >= 20 && bottom <= 43 && std::abs(top - (kCanvasHeight - 1 - bottom)) <= 3, "muted: the word sits mid-panel in 16X24, nothing else on it");
  writePgm(bypass, "oled-bypass.pgm");

  check(std::strcmp(topSwitchWord(TopSwitch::Mute), "MUTED") == 0 && std::strcmp(topSwitchWord(TopSwitch::Bypass), "BYPASS") == 0,
        "muted: the word follows the top switch setting");
}

// ---------------------------------------------------------------------------------------------
// Toggle

// The toggle readout of the card in `slot` of the default layout, with its toggle at `on`.
Canvas toggleReadout(int slot, bool on) {
  ReadoutView v = viewOf(slot, 0, 0.0f);
  v.knob = nullptr;
  v.toggle = true;
  v.on = on;
  Canvas c;
  renderReadout(c, v);
  return c;
}

// Slot 6 (Distortion) with its toggle at `on`.
Canvas toggleOf(bool on) { return toggleReadout(5, on); }

void checkToggle() {
  struct Case {
    bool on;
    const char* word;
    const char* golden;
    const char* pgm;
  };
  const Case cases[] = {{true, "ON", "toggle-on", "oled-toggle-on.pgm"}, {false, "OFF", "toggle-off", "oled-toggle-off.pgm"}};
  for (const Case& t : cases) {
    const Canvas c = toggleOf(t.on);
    const std::string what = std::string("toggle ") + t.word + ": ";
    check(matchesGolden(c, t.golden), (what + "matches the proof frame").c_str());
    writePgm(c, t.pgm);

    check(sameRows(c, titleBarOf("6 DISTORTION"), 0, kNameTop), (what + "the title bar holds the slot and card, as on a knob readout").c_str());
    check(rowsBlank(c, kBarRows, 34), (what + "the parameter row is empty").c_str());

    Canvas word;
    word.drawText((kCanvasWidth - Canvas::textWidth(font16x24(), t.word)) / 2, 34, font16x24(), t.word);
    check(sameRows(c, word, 34, 58) && !rowsBlank(c, 34, 58), (what + "the word is 16X24 centred with its top at row 34").c_str());
    check(rowsBlank(c, 58, kCanvasHeight) && edgesDark(c, kBarRows), (what + "nothing under the word and the edges below the bar stay dark").c_str());

    int first = kCanvasWidth;
    int last = -1;
    for (int y = 34; y < 58; ++y) {
      for (int x = 0; x < kCanvasWidth; ++x) {
        if (!lit(c, x, y)) continue;
        first = std::min(first, x);
        last = std::max(last, x);
      }
    }
    check(std::abs(first - (kCanvasWidth - 1 - last)) <= 1, (what + "the ink is centred sideways").c_str());
  }
  check(!sameRows(toggleOf(true), toggleOf(false), 34, 58), "toggle: ON and OFF differ on the value row");

  // The word does not depend on a knob descriptor, so the slot's title bar alone changes with the slot.
  Canvas other;
  ReadoutView v = viewOf(7, 1, 0.0f);
  v.knob = nullptr;
  v.toggle = true;
  v.on = true;
  renderReadout(other, v);
  check(sameRows(other, toggleOf(true), kBarRows, kCanvasHeight) && !sameRows(other, toggleOf(true), 0, kBarRows),
        "toggle: only the title bar differs between two slots' readouts");
}

// ---------------------------------------------------------------------------------------------
// Title bar

// A card name in capitals, the form the title shows.
std::string capitals(const char* name) {
  std::string s = name;
  for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

// Every readout of the default layout, knob and toggle alike, carries the same bar and title for its
// slot: the bar lit across columns 0 to 127 of rows 0 to 13, the title centred in black with 1 px of
// bar above and below, and rows 14 and 15 blank.
void checkReadoutTitleBar() {
  const Layout layout = defaultLayout();
  bool spans = true;
  bool exact = true;
  bool centred = true;
  bool margins = true;
  bool toggleSame = true;
  int widest = 0;
  std::string widestTitle;
  for (int slot = 0; slot < kSlotCount; ++slot) {
    const Card cardId = layout.card[static_cast<size_t>(slot)];
    const std::string title = std::to_string(slot + 1) + " " + capitals(cardDesc(cardId).name);
    std::vector<Canvas> screens;
    for (int knob = 0; knob < kKnobsPerCard; ++knob) {
      if (knobDesc(cardId, knob)) screens.push_back(readoutOf(slot, knob, 0.0f));
    }
    screens.push_back(toggleReadout(slot, true));
    screens.push_back(toggleReadout(slot, false));
    const Canvas want = titleBarOf(title.c_str());
    for (const Canvas& c : screens) {
      spans = spans && barSpansWidth(c);
      exact = exact && sameRows(c, want, 0, kNameTop);
      const DarkSpan dark = darkInBar(c);
      const int left = dark.left;
      const int right = kCanvasWidth - 1 - dark.right;
      centred = centred && dark.left >= 0 && std::abs(left - right) <= 1;
      margins = margins && left >= kScreenPad && right >= kScreenPad;
    }
    toggleSame = toggleSame && sameRows(screens[screens.size() - 2], screens[0], 0, kNameTop) &&
                 sameRows(screens[screens.size() - 2], screens.back(), 0, kNameTop);
    const int width = Canvas::textWidth(font7x12(), title.c_str());
    if (width > widest) {
      widest = width;
      widestTitle = title;
    }
  }
  check(spans, "title bar: every readout lights columns 0 to 127 of rows 0 and 13 and the side columns of rows 0 to 13, with rows 14 and 15 blank");
  check(exact, "title bar: every readout holds its slot, a space and the card in capitals in black 7X12, top at row 1");
  check(centred, "title bar: the title is centred, its left and right margins within 1 px");
  check(widestTitle == "8 SPRING REVERB" && widest == 108, "title bar: the longest title of the default layout is 8 SPRING REVERB, 108 px wide");
  check(margins, "title bar: every title of the default layout keeps at least 1 px of bar on both sides");
  check(toggleSame, "title bar: a toggle readout, ON or OFF, carries the same bar and title as the knob readout of its slot");

  // Any card in any slot fits between the pad on both sides.
  bool allFit = true;
  for (int index = 1; index < kCardCount; ++index) {
    for (int slot = 0; slot < kSlotCount; ++slot) {
      const std::string title = std::to_string(slot + 1) + " " + capitals(cardDesc(card(index)).name);
      allFit = allFit && Canvas::textWidth(font7x12(), title.c_str()) <= kCanvasWidth - 2 * kScreenPad;
    }
  }
  check(allFit, "title bar: the title of every card in every slot fits with 1 px of bar on both sides");
}

// ---------------------------------------------------------------------------------------------
// Load figure

// The DEBUG figure in 5x7 with its last column at 126 and its top row at 56, 1 px from the right and
// bottom edges. The text goes at literal places, x 116, 110 and 104 for 7%, 42% and 100%.
void checkLoad() {
  struct Case {
    int percent;
    const char* text;
    int x;
  };
  const Case cases[] = {{0, "0%", 116}, {7, "7%", 116}, {42, "42%", 110}, {100, "100%", 104}, {999, "999%", 104}};
  for (const Case& c : cases) {
    Canvas drawn;
    drawLoad(drawn, c.percent);
    Canvas want;
    want.drawText(c.x, 56, font5x7(), c.text);
    const std::string what = std::string("load ") + c.text + ": ";
    check(sameRows(drawn, want, 0, kCanvasHeight), (what + "the text sits with its top at row 56, its last column at 126").c_str());
    check(rowsBlank(drawn, 0, 56) && edgesDark(drawn), (what + "nothing above row 56 and the edges stay dark").c_str());
    if (c.percent == 7 || c.percent == 42 || c.percent == 100) {
      writePgm(drawn, (std::string("oled-load-") + std::to_string(c.percent) + ".pgm").c_str());
    }
  }

  Canvas high;
  drawLoad(high, 1500);
  Canvas cap;
  drawLoad(cap, 999);
  Canvas low;
  drawLoad(low, -5);
  Canvas zero;
  drawLoad(zero, 0);
  check(sameRows(high, cap, 0, kCanvasHeight) && sameRows(low, zero, 0, kCanvasHeight),
        "load: a figure past 999 reads 999% and one below 0 reads 0%");

  // Over a lit canvas, the box behind the text is blanked first, so only the text's own pixels stay lit
  // there and the rest of the canvas is untouched.
  Canvas full;
  full.fillRect(0, 0, kCanvasWidth, kCanvasHeight, true);
  drawLoad(full, 42);
  Canvas text;
  text.drawText(110, 56, font5x7(), "42%");
  bool boxIsText = true;
  bool restLit = true;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      const bool inBox = x >= 110 && x <= 126 && y >= 56 && y <= 62;
      if (inBox) boxIsText = boxIsText && lit(full, x, y) == lit(text, x, y);
      else restLit = restLit && lit(full, x, y);
    }
  }
  check(boxIsText && restLit, "load: the box behind the figure is blanked and nothing outside it changes");
}

}  // namespace

void checkScreens() {
  checkReadoutSettled();
  checkReadoutRolling();
  checkReadoutRows();
  checkReadoutField();
  checkReadoutRolls();
  checkReadoutSweep();
  checkReadoutGainsDigit();
  checkReadoutRecentres();
  checkReadoutUnitsAndCentre();
  checkReadoutRollClip();
  checkReadoutEdges();
  checkKeyField();
  checkKeyRows();
  checkKeyFlats();
  checkReadoutEdgeCases();
  checkRibbonHeight();
  checkRibbonHistory();
  checkVoiceSilence();
  checkRibbonFirstColumn();
  checkVoiceShape();
  checkVoiceWholeFrame();
  checkMeterSegments();
  checkMeterBox();
  checkMeterSegmentRows();
  checkMeterWalks();
  checkMeterAbsentOffVoice();
  checkVoiceScroll();
  checkVoiceWalk();
  checkMuted();
  checkToggle();
  checkReadoutTitleBar();
  checkLoad();
}
