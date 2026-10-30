// Checks the runtime screens on the host. Covers the readout against the proof renderer byte for
// byte (settled, rolling and muted), its rows and edges, the key names with their flats, the voice
// ribbon and the muted word. The proof PGMs go to build/oled-*.pgm.

#include "support.h"

#include <algorithm>
#include <array>
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

// The outermost ring of pixels must stay dark, which keeps text 1 px clear of every edge.
bool edgesDark(const Canvas& c) {
  for (int x = 0; x < kCanvasWidth; ++x) {
    if (lit(c, x, 0) || lit(c, x, kCanvasHeight - 1)) return false;
  }
  for (int y = 0; y < kCanvasHeight; ++y) {
    if (lit(c, 0, y) || lit(c, kCanvasWidth - 1, y)) return false;
  }
  return true;
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
      {4, 1, 210.0f, "settled-210ms", "oled-readout-210ms.pgm"},
      {7, 0, 65.0f, "settled-65pct", "oled-readout-65pct.pgm"},
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
    const Canvas canvas = readoutOf(4, 1, proofShown(120.0f, 210.0f, false, frame));
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
  const Canvas c = readoutOf(4, 1, 210.0f);
  Canvas head;
  head.drawText(1, 1, font5x7(), "5 SLAPBACK");
  check(sameRows(c, head, 0, 9), "readout: row one is the slot, a space and the card in 5x7 at (1, 1)");
  Canvas name;
  name.drawText((kCanvasWidth - Canvas::textWidth(font9x12(), "TIME")) / 2, 15, font9x12(), "TIME");
  check(sameRows(c, name, 9, 30), "readout: row two is the parameter name in 9X12 centred, top at 15");
  check(rowsBlank(c, 27, 34) && rowsBlank(c, 58, kCanvasHeight), "readout: nothing between the rows or under the value");
  check(!rowsBlank(c, 34, 58) && edgesDark(c), "readout: the value fills the band from row 34 to 57 and the edges stay dark");
}

// ---------------------------------------------------------------------------------------------
// The value field. These tests build the row from the spec's wording and take nothing from the
// renderer: a sign cell when the range is signed, one cell for each digit of the widest number, a
// space and the unit, the whole field centred once. A number is right-aligned in the digit cells and
// its sign sits in the sign cell.

constexpr int kValueTop = 34;
constexpr int kValueBottom = kValueTop + 24;

struct Field {
  bool signedRange = false;
  int digits = 1;
  int cells() const { return digits + (signedRange ? 1 : 0); }
};

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

// Runs through every reading the knob can make to find its widest number and whether any is negative.
Field fieldOf(const KnobDesc& k) {
  Field f;
  const int count = k.kind == KnobKind::Stepped ? k.positions : 101;
  for (int i = 0; i < count; ++i) {
    const float reading = k.kind == KnobKind::Stepped ? static_cast<float>(i) : static_cast<float>(i) / 100.0f;
    const int shown = knobShown(k, reading);
    f.signedRange = f.signedRange || shown < 0;
    f.digits = std::max(f.digits, static_cast<int>(std::to_string(std::abs(shown)).size()));
  }
  return f;
}

// One character per cell for the whole number v.
std::string cellsFor(const Field& f, int v) {
  std::string cells(static_cast<size_t>(f.digits), ' ');
  const std::string magnitude = std::to_string(std::abs(v));
  for (size_t i = 0; i < magnitude.size() && i < cells.size(); ++i) {
    cells[cells.size() - 1 - i] = magnitude[magnitude.size() - 1 - i];
  }
  if (f.signedRange) cells.insert(cells.begin(), v < 0 ? '-' : (v > 0 ? '+' : ' '));
  return cells;
}

int cellPitch() { return font16x24().width + font16x24().spacing; }

// Left edge of the first cell. The field is as wide as all its cells and the unit with its space.
// The unit as the readout draws it: a leading space, in capitals, "" when the knob has none.
std::string unitOf(const KnobDesc& k) {
  char unit[16];
  unitText(k, unit, sizeof(unit));
  return unit;
}

int fieldLeft(const KnobDesc& k, const Field& f) {
  const FontDesc& big = font16x24();
  const std::string unit = unitOf(k);
  const int unitWidth = unit.empty() ? 0 : big.spacing + Canvas::textWidth(big, unit.c_str());
  return (kCanvasWidth - (f.cells() * cellPitch() - big.spacing + unitWidth)) / 2;
}

void drawUnit(Canvas& c, const KnobDesc& k, int pen) {
  const std::string unit = unitOf(k);
  if (unit.empty()) return;
  c.drawText(pen, kValueTop, font16x24(), unit.c_str());
}

// The value rows of a number at rest, cell by cell, with `rolled` px of roll into the next number.
// Cells that differ between the two numbers roll up out of the band while the next ones roll in.
Canvas valueBand(const KnobDesc& k, int v, int rolled = 0) {
  const FontDesc& big = font16x24();
  const Field f = fieldOf(k);
  const int x0 = fieldLeft(k, f);
  const std::string from = cellsFor(f, v);
  const std::string to = cellsFor(f, v + 1);
  Canvas c;
  for (size_t j = 0; j < from.size(); ++j) {
    const int x = x0 + static_cast<int>(j) * cellPitch();
    const char a[2] = {from[j], '\0'};
    const char b[2] = {to[j], '\0'};
    if (from[j] == to[j] || rolled == 0) {
      c.drawDigits(x, kValueTop, big, a);
    } else {
      c.drawDigitsClipped(x, kValueTop - rolled, big, a, kValueTop, kValueBottom);
      c.drawDigitsClipped(x, kValueBottom - rolled, big, b, kValueTop, kValueBottom);
    }
  }
  drawUnit(c, k, x0 + f.cells() * cellPitch());
  return c;
}

// Every number a knob can show lands in the same cells, whatever its digit count or sign.
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
  check(inPlace && numbers > 2000, "readout: every number of every knob sits in its fixed field, cell by cell");
}

// While the value changes or rolls, nothing in the value rows moves sideways. The unit stays where it
// is and every pixel stays inside the field.
void checkReadoutNeverMoves() {
  const Layout layout = defaultLayout();
  const float fractions[] = {0.0f, 0.25f, 0.5f, 0.75f, 0.96f};
  bool unitStill = true;
  bool inField = true;
  for (int slot = 0; slot < kSlotCount; ++slot) {
    for (int knob = 0; knob < kKnobsPerCard; ++knob) {
      const KnobDesc* k = knobDesc(layout.card[static_cast<size_t>(slot)], knob);
      if (!k || k->labels) continue;
      const Field f = fieldOf(*k);
      const int x0 = fieldLeft(*k, f);
      const int pen = x0 + f.cells() * cellPitch();
      int lo = 0;
      int hi = 0;
      shownRange(*k, lo, hi);
      const Canvas unitOnly = valueBand(*k, lo);
      std::vector<int> bases;
      for (int b = lo; b < hi; b += std::max(1, (hi - lo) / 40)) bases.push_back(b);
      for (int b : {-100, -10, -9, -1, 0, 9, 10, 99, 100, 999}) {
        if (b >= lo && b < hi) bases.push_back(b);
      }
      for (int b : bases) {
        for (float t : fractions) {
          const Canvas c = readoutOf(slot, knob, static_cast<float>(b) + t);
          for (int y = kValueTop; y < kValueBottom; ++y) {
            for (int x = 0; x < kCanvasWidth; ++x) {
              if (x >= pen) unitStill = unitStill && lit(c, x, y) == lit(unitOnly, x, y);
              if (x < x0) inField = inField && !lit(c, x, y);
            }
          }
        }
      }
    }
  }
  check(unitStill, "readout: the unit never moves while any number changes or rolls");
  check(inField, "readout: nothing in the value rows lies left of the field");
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

// What a sweep remembers from the frames before: the column each cell and the unit first took, and
// the first and the previous frame.
struct SweepMemory {
  std::vector<int> cellX;
  Extent unit;
  bool haveUnit = false;
  Canvas first;
  Canvas previous;
};

// The field the spec gives a knob whose numbers are `values`.
Field fieldOfValues(const std::vector<int>& values) {
  Field f;
  for (int v : values) {
    f.signedRange = f.signedRange || v < 0;
    f.digits = std::max(f.digits, static_cast<int>(std::to_string(std::abs(v)).size()));
  }
  return f;
}

// Each character must sit where the first frame that showed it put it. Fills `at` with the columns found.
std::string cellsInPlace(const Columns& cols, const std::string& cells, std::vector<std::vector<uint32_t>>& pictures,
                         SweepMemory& memory, std::vector<int>& at) {
  if (!placeCells(cols, cells, pictures, at)) return "the characters \"" + cells + "\" are not in their cells";
  for (size_t j = 0; j < at.size(); ++j) {
    if (at[j] < 0) continue;
    if (memory.cellX[j] < 0) memory.cellX[j] = at[j];
    if (at[j] != memory.cellX[j]) {
      return "cell " + std::to_string(j + 1) + " of " + std::to_string(at.size()) + " moved from x " +
             std::to_string(memory.cellX[j]) + " to x " + std::to_string(at[j]);
    }
  }
  return "";
}

// The last cell always holds a digit, so the unit lies to the right of it, on the columns it first took.
std::string unitInPlace(const Columns& cols, int lastCell, SweepMemory& memory) {
  const Extent seen = inkFrom(cols, lastCell + font16x24().width);
  if (!memory.haveUnit) {
    memory.unit = seen;
    memory.haveUnit = true;
  }
  if (seen.left == memory.unit.left && seen.right == memory.unit.right) return "";
  return "the unit moved from columns " + std::to_string(memory.unit.left) + " to " + std::to_string(memory.unit.right) +
         " to columns " + std::to_string(seen.left) + " to " + std::to_string(seen.right);
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

// Sweeps a numeric knob and returns what moved first, or "" when nothing did. `steps` counts the
// frames measured. Every frame must put each digit, the sign and the unit where the first frame that
// showed them put them. The rest of the screen must not change either, and each step must look new.
std::string sweepNumber(Card cardId, int slot, int knob, const KnobDesc& k, const std::vector<int>& values,
                        std::vector<std::vector<uint32_t>>& pictures, int& steps) {
  const Field f = fieldOfValues(values);
  SweepMemory memory;
  memory.cellX.assign(static_cast<size_t>(f.cells()), -1);
  for (size_t n = 0; n < values.size(); ++n) {
    const int v = values[n];
    const Canvas frame = settledReadout(cardId, slot, knob, v);
    ++steps;
    const Columns cols = bandColumns(frame);
    const std::string cells = cellsFor(f, v);
    std::vector<int> at;
    std::string moved = cellsInPlace(cols, cells, pictures, memory, at);
    if (moved.empty()) moved = unitInPlace(cols, at.back(), memory);
    if (moved.empty() && !inkAccounted(frame, k, cells, at)) moved = "the value rows hold ink that is no cell and not the unit";
    if (moved.empty()) moved = restStill(frame, n, memory);
    if (!moved.empty()) return "at " + valueText(k, v) + ", " + moved;
  }
  return "";
}

// Sweeps a labelled knob the same way. Each name must start at the column the first one started at.
std::string sweepLabelled(Card cardId, int slot, int knob, const KnobDesc& k, const std::vector<int>& values,
                          int& steps) {
  int left = -1;
  SweepMemory memory;
  for (size_t n = 0; n < values.size(); ++n) {
    const int v = values[n];
    const std::string where = "at " + valueText(k, v) + ", ";
    const Canvas frame = settledReadout(cardId, slot, knob, v);
    ++steps;

    const Extent ink = inkFrom(bandColumns(frame), 0);
    if (ink.left < 0) return where + "the name draws nothing";
    if (n == 0) left = ink.left;
    if (ink.left != left) {
      return where + "the name moved from x " + std::to_string(left) + " to x " + std::to_string(ink.left);
    }
    Canvas rebuilt;
    rebuilt.drawText(ink.left, kValueTop, font16x24(), knobLabel(k, v));
    if (!sameRows(frame, rebuilt, kValueTop, kValueBottom)) return where + "the value rows hold ink that is not the name";
    const std::string still = restStill(frame, n, memory);
    if (!still.empty()) return where + still;
  }
  return "";
}

// Hard gate (owner 2026-10-24, "I don't want anything bouncing around laterally as values change").
// Prints how many parameters and steps it covered.
void checkReadoutSweep() {
  std::vector<std::vector<uint32_t>> pictures(128);
  int expected = 0;
  int parameters = 0;
  int steps = 0;
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
      const std::string moved = k->labels ? sweepLabelled(cardId, slot, knob, *k, values, done)
                                          : sweepNumber(cardId, slot, knob, *k, values, pictures, done);
      check(moved.empty(), (who + " " + moved).c_str());

      // A sweep that found nothing must also have reached every step between the ends of the travel.
      const int reach = k->kind == KnobKind::Stepped ? k->positions : std::abs(knobShown(*k, 1.0f) - knobShown(*k, 0.0f)) + 1;
      if (moved.empty()) check(done == reach && reach >= 2, (who + " was not swept through its whole range").c_str());
      ++parameters;
      steps += done;
    }
  }
  check(parameters == expected && parameters > 0, "readout sweep: every knob of every card was swept");
  std::printf("readout sweep: %d parameters, %d settled steps\n", parameters, steps);
}

// A number that gains a digit lights its leading cell in place while the other digits roll.
void checkReadoutGainsDigit() {
  // 99 to 100 on a percent knob. The field has 3 cells, so 99 is a blank cell and two 9s.
  const KnobDesc& percent = *knobDesc(defaultLayout().card[7], 0);
  check(cellsFor(fieldOf(percent), 99) == " 99" && cellsFor(fieldOf(percent), 100) == "100", "readout: 99 is a blank cell and two 9s, 100 fills the cells");
  const Canvas settled99 = readoutOf(7, 0, 99.0f);
  const Canvas settled100 = readoutOf(7, 0, 100.0f);
  const Field f = fieldOf(percent);
  const int x0 = fieldLeft(percent, f);
  bool leadingBlank = true;
  for (int y = kValueTop; y < kValueBottom; ++y) {
    for (int x = x0; x < x0 + cellPitch(); ++x) leadingBlank = leadingBlank && !lit(settled99, x, y);
  }
  check(leadingBlank && sameRows(settled99, valueBand(percent, 99), kValueTop, kValueBottom), "readout: 99 leaves the leading cell blank");
  check(sameRows(settled100, valueBand(percent, 100), kValueTop, kValueBottom), "readout: 100 fills all three cells");

  // The leading cell lights while the 9s roll to 0 in the cells they already sit in.
  const Canvas half = readoutOf(7, 0, 99.5f);
  check(sameRows(half, valueBand(percent, 99, 12), kValueTop, kValueBottom), "readout: 99.5 rolls the 1 in and the 9s to 0, in place");
  bool leadingTopBlank = true;
  for (int y = kValueTop; y < kValueTop + 12; ++y) {
    for (int x = x0; x < x0 + cellPitch(); ++x) leadingTopBlank = leadingTopBlank && !lit(half, x, y);
  }
  check(leadingTopBlank && !rowsBlank(half, kValueTop + 12, kValueBottom), "readout: the 1 rises into the leading cell from below");
  writePgm(half, "oled-readout-roll-99-100.pgm");

  // -9 to -10 on a signed knob. The sign cell keeps its minus and does not roll.
  const KnobDesc& semitones = *knobDesc(defaultLayout().card[2], 0);
  const Field s = fieldOf(semitones);
  check(s.signedRange && s.digits == 2 && cellsFor(s, -9) == "- 9" && cellsFor(s, 5) == "+ 5" && cellsFor(s, 0) == "  0" &&
            cellsFor(s, -10) == "-10",
        "readout: a signed knob has a sign cell, a blank tens cell for one digit, and no sign for zero");
  check(sameRows(readoutOf(2, 0, -9.5f), valueBand(semitones, -10, 12), kValueTop, kValueBottom),
        "readout: -9.5 rolls the 1 in and the 0 to 9, with the minus in its own cell");
}

// A rolling digit stays inside the 24 px band of the value.
void checkReadoutRollClip() {
  bool clipped = true;
  bool dark = true;
  for (int k = 0; k <= 48; ++k) {
    const Canvas c = readoutOf(4, 1, 120.0f + static_cast<float>(k) / 24.0f);
    clipped = clipped && rowsBlank(c, 27, 34) && rowsBlank(c, 58, kCanvasHeight);
    dark = dark && edgesDark(c);
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
        dark = dark && edgesDark(readoutOf(slot, knob, shown)) && edgesDark(readoutOf(slot, knob, shown + 0.5f));
      }
    }
  }
  check(dark, "readout: every value of every default-layout knob keeps its edges dark");
}

// The autotune key is a name. It draws in 16X24 and does not roll. Its field is as wide as the widest
// name and sits centred once, and each name starts at the field's left edge.
const KnobDesc& keyKnob() { return *knobDesc(defaultLayout().card[1], 0); }

constexpr int kKeyFieldLeft = 6;

void checkKeyField() {
  const KnobDesc& key = keyKnob();
  int widest = 0;
  for (int i = 0; i < key.positions; ++i) widest = std::max(widest, Canvas::textWidth(font16x24(), key.labels[i]));
  check(widest == 116 && (kCanvasWidth - widest) / 2 == kKeyFieldLeft,
        "readout: the widest key name is 116 px, so its field starts at x 6");

  bool inPlace = true;
  bool startsAtLeft = true;
  for (int i = 0; i < key.positions; ++i) {
    const Canvas c = readoutOf(1, 0, static_cast<float>(i));
    Canvas want;
    want.drawText(kKeyFieldLeft, kValueTop, font16x24(), key.labels[i]);
    inPlace = inPlace && sameRows(c, want, kValueTop, kValueBottom);
    startsAtLeft = startsAtLeft && inkFrom(bandColumns(c), 0).left == kKeyFieldLeft;
  }
  check(inPlace, "readout: each key name is drawn in 16X24 from the left edge of the one fixed field");
  check(startsAtLeft, "readout: every key name starts at the same column, the shorter ones leave the right of the field blank");
}

void checkKeyRows() {
  const Canvas key = readoutOf(1, 0, 11.0f);
  Canvas head;
  head.drawText(1, 1, font5x7(), "2 AUTOTUNE");
  head.drawText((kCanvasWidth - Canvas::textWidth(font9x12(), "KEY")) / 2, 15, font9x12(), "KEY");
  check(sameRows(key, head, 0, kValueTop), "readout: the key's card line and parameter name are as for any knob");
  writePgm(key, "oled-readout-key.pgm");

  check(edgesDark(readoutOf(1, 0, 6.0f)), "readout: the widest key name, F# / D#m, keeps the edges dark");
  ReadoutView stray = viewOf(1, 0, 3.5f);
  stray.target = 99;
  Canvas clamped;
  renderReadout(clamped, stray);
  Canvas last;
  last.drawText(kKeyFieldLeft, kValueTop, font16x24(), "F / Dm");
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
  Canvas head;
  head.drawText(1, 1, font5x7(), "4 EMPTY");
  check(sameRows(c, head, 0, kCanvasHeight), "readout: a slot with no knob draws the card line only");

  // Values no knob produces still draw the card line and never fault.
  Canvas title;
  title.drawText(1, 1, font5x7(), "3 OCTAVE");
  bool stable = true;
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (float shown : {nan, inf, -inf, 1.0e9f, -1.0e9f}) stable = stable && sameRows(readoutOf(2, 0, shown), title, 0, 9);
  check(stable, "readout: a wild value still draws the card line");

  // A knob with decimals rolls through the point. No card has one yet.
  const KnobDesc rate = {"Rate", KnobKind::Continuous, 0, 0.1f, 5.0f, "hz", Curve::Linear, 1};
  ReadoutView fine;
  fine.slot = 3;
  fine.card = "Unison";
  fine.knob = &rate;
  fine.shown = 14.5f;
  Canvas d;
  renderReadout(d, fine);
  check(edgesDark(d) && !rowsBlank(d, 34, 58), "readout: a decimal value draws with its point");

  // Rolling from -1 to 0, the sign cell rolls through a blank.
  const Canvas across = readoutOf(2, 0, -0.5f);
  check(edgesDark(across) && !rowsBlank(across, 34, 58), "readout: a value rolling from -1 to 0 draws");
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
  check(ribbonHalfHeight(level::kSilence) == 1, "voice: the smallest level the meter reports is 1 px");
  check(ribbonHalfHeight(1.0f) == kRibbonHalfMax && kRibbonHalfMax == 22 && ribbonHalfHeight(5.0f) == 22,
        "voice: full level is 22 px each side of the horizon");
  check(ribbonHalfHeight(0.5f) == 13, "voice: half level lifts to 13 px on the 0.8 curve");
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
  check(r.half(0) == 0 && r.half(1) == 13 && r.half(2) == 22 && r.half(3) == 0, "ribbon: levels read newest first");
  check(r.half(-1) == 0 && r.half(kRibbonLevels) == 0 && r.half(1000) == 0, "ribbon: ages outside the history read zero");
  for (int i = 0; i < kRibbonLevels; ++i) r.push(1.0f);
  check(r.half(0) == 22 && r.half(kRibbonLevels - 1) == 22, "ribbon: the history holds 64 levels");
  r.push(0.0f);
  check(r.half(0) == 0 && r.half(1) == 22 && r.half(kRibbonLevels - 1) == 22, "ribbon: a push drops the oldest level");
}

void checkVoiceSilence() {
  Canvas c;
  renderVoice(c, Ribbon(), Walk{0, 0});
  Canvas line;
  line.fillRect(0, 32, kCanvasWidth, 1, true);
  check(sameRows(c, line, 0, kCanvasHeight), "voice: in silence the screen is the horizon line at row 32 and nothing else");
}

void checkVoiceShape() {
  Ribbon r;
  for (int i = 0; i < kRibbonLevels; ++i) r.push(0.0f);
  r.push(1.0f);  // 22 px, columns 124 and 125 after the next push
  r.push(0.5f);  // 13 px, columns 126 and 127
  Canvas c;
  renderVoice(c, r, Walk{0, 0});

  bool solid = true;
  for (int y = 32 - 13; y <= 32 + 13; ++y) solid = solid && lit(c, 126, y) && lit(c, 127, y);
  for (int y = 32 - 22; y <= 32 + 22; ++y) solid = solid && lit(c, 124, y) && lit(c, 125, y);
  check(solid, "voice: each level spans two columns and is solid from the horizon out, the newest at the right edge");
  check(lit(c, 100, 32) && !lit(c, 100, 31) && !lit(c, 100, 33), "voice: older silence is the line");

  // A 1 px rim of 50 % Bayer dither caps each column, and nothing lies beyond it.
  bool rim = true;
  for (int x = 126; x <= 127; ++x) {
    rim = rim && lit(c, x, 32 - 14) == bayerLit(x, 32 - 14, 32) && lit(c, x, 32 + 14) == bayerLit(x, 32 + 14, 32);
    rim = rim && !lit(c, x, 32 - 15) && !lit(c, x, 32 + 15);
  }
  for (int x = 124; x <= 125; ++x) {
    rim = rim && lit(c, x, 32 - 23) == bayerLit(x, 32 - 23, 32) && lit(c, x, 32 + 23) == bayerLit(x, 32 + 23, 32);
    rim = rim && !lit(c, x, 32 - 24) && !lit(c, x, 32 + 24);
  }
  check(rim, "voice: a 1 px 50 % dither caps the column top and bottom");

  check(rowsBlank(c, 0, 9) && rowsBlank(c, 56, kCanvasHeight),
        "voice: the tallest column with its rim stays 9 px clear of the top and bottom");
}

// The ribbon drawn pixel by pixel from its definition, as an independent check of the whole frame:
// a column of half-height h is solid within h rows of the horizon, a 1 px rim lies one row beyond
// it where the Bayer threshold of the pixel is under 32, and a column of height 0 is the horizon
// pixel alone. The walk moves the finished picture.
Canvas voiceFromDefinition(const Ribbon& ribbon, Walk walk) {
  Canvas c;
  for (int x = 0; x < kCanvasWidth; ++x) {
    const int h = ribbon.half((kCanvasWidth - 1 - x) / 2);
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
  for (int frame = 0; frame < 6; ++frame) {
    Ribbon r;
    for (int i = 0; i < kRibbonLevels + frame; ++i) {
      seed = seed * 1664525u + 1013904223u;
      r.push(static_cast<float>(seed >> 8) / 16777216.0f);
    }
    for (uint32_t step = 0; step < 9; ++step) {
      Canvas c;
      renderVoice(c, r, walkAt(step));
      same = same && sameRows(c, voiceFromDefinition(r, walkAt(step)), 0, kCanvasHeight);
    }
  }
  check(same, "voice: whole frames match a pixel-by-pixel drawing of the ribbon, at every step of the walk");
}

void checkVoiceScroll() {
  Ribbon r = ribbonOf({0.2f, 0.9f, 0.4f, 0.7f, 0.0f, 0.6f, 1.0f, 0.3f});
  Canvas before;
  renderVoice(before, r, Walk{0, 0});
  r.push(0.8f);
  Canvas after;
  renderVoice(after, r, Walk{0, 0});

  // The rim dither is anchored to the panel, so it does not move with the ribbon. The rest does.
  bool moved = true;
  for (int x = 0; x + kRibbonColumnsPerLevel < kCanvasWidth; ++x) {
    const int half = r.half((kCanvasWidth - 1 - x) / kRibbonColumnsPerLevel);
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
  renderVoice(base, r, Walk{0, 0});
  bool ok = true;
  for (int i = 1; i <= 8; ++i) {
    const Walk w = walkAt(static_cast<uint32_t>(i));
    Canvas moved;
    renderVoice(moved, r, w);
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

// Slot 6 (Distortion) with its toggle at `on`.
Canvas toggleOf(bool on) {
  ReadoutView v = viewOf(5, 0, 0.0f);
  v.knob = nullptr;
  v.toggle = true;
  v.on = on;
  Canvas c;
  renderReadout(c, v);
  return c;
}

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

    Canvas head;
    head.drawText(1, 1, font5x7(), "6 DISTORTION");
    check(sameRows(c, head, 0, 9), (what + "row one is the slot and card in 5x7 at (1, 1), as on a knob readout").c_str());
    check(rowsBlank(c, 9, 34), (what + "row two is empty").c_str());

    Canvas word;
    word.drawText((kCanvasWidth - Canvas::textWidth(font16x24(), t.word)) / 2, 34, font16x24(), t.word);
    check(sameRows(c, word, 34, 58) && !rowsBlank(c, 34, 58), (what + "the word is 16X24 centred with its top at row 34").c_str());
    check(rowsBlank(c, 58, kCanvasHeight) && edgesDark(c), (what + "nothing under the word and the edges stay dark").c_str());

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

  // The word does not depend on a knob descriptor, so the slot's card line alone changes with the slot.
  Canvas other;
  ReadoutView v = viewOf(7, 1, 0.0f);
  v.knob = nullptr;
  v.toggle = true;
  v.on = true;
  renderReadout(other, v);
  check(sameRows(other, toggleOf(true), 34, kCanvasHeight) && !sameRows(other, toggleOf(true), 0, 9),
        "toggle: only the card line differs between two slots' readouts");
}

}  // namespace

void checkScreens() {
  checkReadoutSettled();
  checkReadoutRolling();
  checkReadoutRows();
  checkReadoutField();
  checkReadoutNeverMoves();
  checkReadoutSweep();
  checkReadoutGainsDigit();
  checkReadoutRollClip();
  checkReadoutEdges();
  checkKeyField();
  checkKeyRows();
  checkKeyFlats();
  checkReadoutEdgeCases();
  checkRibbonHeight();
  checkRibbonHistory();
  checkVoiceSilence();
  checkVoiceShape();
  checkVoiceWholeFrame();
  checkVoiceScroll();
  checkVoiceWalk();
  checkMuted();
  checkToggle();
}
