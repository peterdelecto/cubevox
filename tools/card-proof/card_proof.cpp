// Card proof. Candidate layouts for the screen that shows one touched effect (slot and card name,
// both parameter names, the readout of the control that moved), drawn through the firmware's own
// Canvas and fonts. Writes one 128 x 64 byte-per-pixel frame (0 or 1) per proof, then a tab
// separated table of names and captions. Prints every text that crosses the 1 px padding.
//
//   clang++ -std=c++17 -O2 -I firmware/cubevox/src -I tools/font-editor/fonts \
//     tools/card-proof/card_proof.cpp firmware/cubevox/src/core/canvas.cpp \
//     firmware/cubevox/src/core/fonts.cpp -o build/card_proof \
//   && build/card_proof build/card-proof.raw build/card-proof.tsv \
//   && python3 -I tools/card-proof/sheet.py build/card-proof.raw build/card-proof.tsv

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/fonts.h"

namespace {

constexpr int kW = kCanvasWidth;
constexpr int kH = kCanvasHeight;
constexpr int kValueY = 34;  // top of the 16X24 value row, as renderReadout
constexpr int kNameBarH = 9;

struct Rect {
  int x, y, w, h;
};

struct Item {
  std::string label;
  Rect r;
  int ownBar;  // index into bars, or -1
};

struct Bar {
  std::string label;
  Rect r;
};

struct Example {
  int slot;
  const char* card;
  const char* names[2];
  int moving;
  const char* value;  // the cells of the knob's field
  const char* unit;
};

const Example kOctave = {3, "OCTAVE", {"SEMITONES", "MIX"}, 0, "-12", "ST"};
const Example kSpring = {7, "SPRING REVERB", {"MIX", "TIME"}, 1, "1000", "MS"};
const Example kGate = {2, "INPUT GATE", {"THRESHOLD", "DECAY"}, 0, "-24", "DB"};

bool lit(const Canvas& c, int x, int y) {
  if (x < 0 || x >= kW || y < 0 || y >= kH) return false;
  return (c.page(y >> 3)[x] >> (y & 7)) & 1;
}

struct Scene {
  std::string name, caption;
  Canvas c;
  std::vector<Item> items;
  std::vector<Bar> bars;
  std::vector<Bar> rules;
  bool strict = false;  // report items closer than 1 px to each other, not only overlaps

  // Draws spaced text (5x7 fixed pitch, 9X12 and 16X24 by ink) and records its nominal box.
  int text(const char* label, int x, int y, const FontDesc& f, const char* s, int ownBar = -1) {
    const int w = Canvas::textWidth(f, s);
    c.drawText(x, y, f, s);
    items.push_back({label, {x, y, w, f.height}, ownBar});
    return w;
  }

  // Draws `s` thinned to a 50 % checkerboard, clearing every pixel where (x + y) is odd.
  void dithered(const char* label, int x, int y, const FontDesc& f, const char* s) {
    const int w = Canvas::textWidth(f, s);
    Canvas t;
    t.drawText(x, y, f, s);
    for (int j = y; j < y + f.height; ++j)
      for (int i = x; i < x + w; ++i)
        if (lit(t, i, j) && ((i + j) & 1) == 0) c.setPixel(i, j, true);
    items.push_back({label, {x, y, w, f.height}, -1});
  }

  // Inverted bar around a 5x7 name: text height + 2 px, x - 1 and width + 2.
  int bar(const char* label, int x, int y, int w) {
    c.invertRect(x - 1, y - 1, w + 2, kNameBarH);
    bars.push_back({label, {x - 1, y - 1, w + 2, kNameBarH}});
    return static_cast<int>(bars.size()) - 1;
  }

  void rule(const char* label, int x, int y, int w, int h) {
    c.fillRect(x, y, w, h, true);
    rules.push_back({label, {x, y, w, h}});
  }
};

// The value as the readout draws it: the field is the cells, a space and the unit, centred.
void valueField(Scene& s, const Example& e, int top = kValueY) {
  const FontDesc& big = font16x24();
  char unit[16] = "";
  if (e.unit[0] != '\0') snprintf(unit, sizeof(unit), " %s", e.unit);
  const int unitW = unit[0] != '\0' ? big.spacing + Canvas::textWidth(big, unit) : 0;
  const int w = Canvas::digitsWidth(big, e.value) + unitW;
  const int x = (kW - w) / 2;
  const int pen = s.c.drawDigits(x, top, big, e.value);
  s.c.drawText(pen, top, big, unit);
  s.items.push_back({"value", {x, top, w, big.height}, -1});
}

void wordRow(Scene& s, const char* word) {
  const FontDesc& big = font16x24();
  const int w = Canvas::textWidth(big, word);
  s.text("value", (kW - w) / 2, kValueY, big, word);
}

std::string head(const Example& e) { return std::to_string(e.slot) + " " + e.card; }

enum class Mark { Bar, None };

// A0 family: stacked names at fixed x, moving one in a bar.
Scene stackedBar(const char* name, const char* caption, const Example& e, bool toggle) {
  Scene s{name, caption, {}, {}, {}, {}};
  s.text("heading", 1, 1, font5x7(), head(e).c_str());
  const int xs[2] = {2, 66};
  for (int i = 0; i < 2; ++i) {
    const int w = Canvas::textWidth(font5x7(), e.names[i]);
    int b = -1;
    s.c.drawText(xs[i], 12, font5x7(), e.names[i]);
    if (!toggle && i == e.moving) b = s.bar(e.names[i], xs[i], 12, w);
    s.items.push_back({e.names[i], {xs[i], 12, w, 7}, b});
  }
  if (toggle) wordRow(s, "BYPASS");
  else valueField(s, e);
  return s;
}

// A1: the moving name in 9X12, the other in 5x7 on the same bottom row, 6 px after it.
Scene stackedSize(const char* name, const char* caption, const Example& e) {
  Scene s{name, caption, {}, {}, {}, {}};
  s.text("heading", 1, 1, font5x7(), head(e).c_str());
  const int mv = e.moving, ot = 1 - e.moving;
  int x = 2;
  const int wm = Canvas::textWidth(font9x12(), e.names[mv]);
  const int wo = Canvas::textWidth(font5x7(), e.names[ot]);
  const int xm = mv == 0 ? x : x + wo + 6;
  const int xo = mv == 0 ? x + wm + 6 : x;
  s.text(e.names[mv], xm, 11, font9x12(), e.names[mv]);
  s.text(e.names[ot], xo, 16, font5x7(), e.names[ot]);
  std::printf("info %s: %s 9X12 x=%d..%d, %s 5x7 x=%d..%d (last px %d)\n", name, e.names[mv], xm,
              xm + wm - 1, e.names[ot], xo, xo + wo - 1, xo + wo - 1);
  valueField(s, e);
  return s;
}

// A2: the name that is not moving thinned to a checkerboard.
Scene stackedDither(const char* name, const char* caption, const Example& e) {
  Scene s{name, caption, {}, {}, {}, {}};
  s.text("heading", 1, 1, font5x7(), head(e).c_str());
  const int xs[2] = {2, 66};
  for (int i = 0; i < 2; ++i) {
    if (i == e.moving) s.text(e.names[i], xs[i], 12, font5x7(), e.names[i]);
    else s.dithered(e.names[i], xs[i], 12, font5x7(), e.names[i]);
  }
  valueField(s, e);
  return s;
}

// B family: names in a left column, the digits and unit on the right.
Scene columns(const char* name, const char* caption, const Example& e, bool useDither) {
  Scene s{name, caption, {}, {}, {}, {}};
  const FontDesc& big = font16x24();
  s.text("heading", 1, 1, font5x7(), head(e).c_str());
  s.rule("rule y=9", 0, 9, kW, 1);
  s.rule("rule y=36", 0, 36, 56, 1);
  s.rule("rule x=56", 56, 10, 1, 54);
  const int ys[2] = {20, 47};
  for (int i = 0; i < 2; ++i) {
    const int w = Canvas::textWidth(font5x7(), e.names[i]);
    if (i == e.moving) {
      s.c.drawText(1, ys[i], font5x7(), e.names[i]);
      int b = useDither ? -1 : s.bar(e.names[i], 1, ys[i], w);
      s.items.push_back({e.names[i], {1, ys[i], w, 7}, b});
    } else if (useDither) {
      s.dithered(e.names[i], 1, ys[i], font5x7(), e.names[i]);
    } else {
      s.text(e.names[i], 1, ys[i], font5x7(), e.names[i]);
    }
  }
  const int dw = Canvas::digitsWidth(big, e.value);
  const int dx = 58 + (70 - dw) / 2;
  s.c.drawDigits(dx, 14, big, e.value);
  s.items.push_back({"digits", {dx, 14, dw, big.height}, -1});
  const int uw = Canvas::textWidth(font9x12(), e.unit);
  s.text("unit", 58 + (70 - uw) / 2, 42, font9x12(), e.unit);
  std::printf("info %s: digits \"%s\" are %d px in 70 px (overrun %d), x=%d..%d\n", name, e.value,
              dw, dw > 70 ? dw - 70 : 0, dx, dx + dw - 1);
  return s;
}

// C family: the slot number alone in 16X24, names to its right.
Scene bigSlot(const char* name, const char* caption, const Example& e, bool toggle) {
  Scene s{name, caption, {}, {}, {}, {}};
  const FontDesc& big = font16x24();
  const std::string slot = std::to_string(e.slot);
  const int sw = s.text("slot", 1, 1, big, slot.c_str());
  const int x0 = 1 + sw + 4;
  s.text("card", x0, 2, font5x7(), e.card);
  const int xs[2] = {x0, x0 + 64};
  for (int i = 0; i < 2; ++i) {
    const int w = Canvas::textWidth(font5x7(), e.names[i]);
    s.c.drawText(xs[i], 14, font5x7(), e.names[i]);
    int b = -1;
    if (!toggle && i == e.moving) b = s.bar(e.names[i], xs[i], 14, w);
    s.items.push_back({e.names[i], {xs[i], 14, w, 7}, b});
  }
  std::printf("info %s: slot digit %d px wide, names start x=%d, second name x=%d\n", name, sw, x0,
              x0 + 64);
  if (toggle) wordRow(s, "ON");
  else valueField(s, e);
  return s;
}

bool hit(const Rect& a, const Rect& b, int grow) {
  return a.x - grow < b.x + b.w && b.x < a.x + a.w + grow && a.y - grow < b.y + b.h &&
         b.y < a.y + a.h + grow;
}

// Reports every text that comes within 1 px of an edge, a rule, another text's bar, or overlaps
// another text. Overrun is px short of the 1 px clearance.
void check(const Scene& s) {
  for (size_t i = 0; i < s.items.size(); ++i) {
    const Item& t = s.items[i];
    const int left = 1 - t.r.x, right = t.r.x + t.r.w - (kW - 1);
    const int top = 1 - t.r.y, bottom = t.r.y + t.r.h - (kH - 1);
    if (left > 0) std::printf("%s: %s crosses left padding by %d px\n", s.name.c_str(), t.label.c_str(), left);
    if (right > 0) std::printf("%s: %s crosses right padding by %d px\n", s.name.c_str(), t.label.c_str(), right);
    if (top > 0) std::printf("%s: %s crosses top padding by %d px\n", s.name.c_str(), t.label.c_str(), top);
    if (bottom > 0) std::printf("%s: %s crosses bottom padding by %d px\n", s.name.c_str(), t.label.c_str(), bottom);
    for (const Bar& r : s.rules)
      if (hit(t.r, r.r, 1)) std::printf("%s: %s is under 1 px from %s\n", s.name.c_str(), t.label.c_str(), r.label.c_str());
    for (size_t b = 0; b < s.bars.size(); ++b)
      if (static_cast<int>(b) != t.ownBar && hit(t.r, s.bars[b].r, 1))
        std::printf("%s: %s is under 1 px from the bar on %s\n", s.name.c_str(), t.label.c_str(), s.bars[b].label.c_str());
    for (size_t j = i + 1; j < s.items.size(); ++j)
      if (hit(t.r, s.items[j].r, s.strict ? 1 : 0))
        std::printf("%s: %s touches or overlaps %s\n", s.name.c_str(), t.label.c_str(), s.items[j].label.c_str());
  }
}

// C1: the loosened big-slot layout. The digit sits in its 18 px cell, the card name is right
// justified, and each parameter name is centred in its own 52 px box. Prints the clearances in
// px (free columns, negative means overrun). `value` is drawn with drawText when `labelled`.
Scene loosened(const char* name, const char* caption, int slot, const char* card,
               const char* names[2], int moving, const char* value, const char* unit,
               bool labelled) {
  Scene s{name, caption, {}, {}, {}, {}};
  const FontDesc& big = font16x24();
  const std::string slotText = std::to_string(slot);
  s.c.drawDigits(1, 1, big, slotText.c_str());
  const int cellRight = 1 + Canvas::digitsWidth(big, slotText.c_str()) - 1;
  s.items.push_back({"slot", {1, 1, cellRight, big.height}, -1});

  const int cw = Canvas::textWidth(font5x7(), card);
  const int cx = 127 - cw;
  s.text("card", cx, 3, font5x7(), card);
  std::printf("clr %s: slot cell right edge x=%d, card name x=%d..126, clear to cell %d px\n", name,
              cellRight, cx, cx - cellRight - 1);

  const int boxX[2] = {22, 75};
  int nx[2], nw[2];
  for (int i = 0; i < 2; ++i) {
    nw[i] = Canvas::textWidth(font5x7(), names[i]);
    nx[i] = boxX[i] + (52 - nw[i]) / 2;
    s.c.drawText(nx[i], 15, font5x7(), names[i]);
    int b = -1;
    if (i == moving) b = s.bar(names[i], nx[i], 15, nw[i]);
    s.items.push_back({names[i], {nx[i], 15, nw[i], 7}, b});
    const int tl = nx[i] - boxX[i], tr = boxX[i] + 51 - (nx[i] + nw[i] - 1);
    if (i == moving) {
      const int bl = (nx[i] - 1) - boxX[i], br = boxX[i] + 51 - (nx[i] + nw[i]);
      std::printf("clr %s: %s bar x=%d..%d in box %d..%d, clear left %d right %d px\n", name,
                  names[i], nx[i] - 1, nx[i] + nw[i], boxX[i], boxX[i] + 51, bl, br);
    } else {
      std::printf("clr %s: %s text x=%d..%d in box %d..%d, clear left %d right %d px\n", name,
                  names[i], nx[i], nx[i] + nw[i] - 1, boxX[i], boxX[i] + 51, tl, tr);
    }
  }
  // Gap between the two names' ink, and between the bar (if any) and the other name's ink.
  std::printf("clr %s: names %d px apart", name, nx[1] - (nx[0] + nw[0]));
  if (moving == 0) std::printf(", bar to %s %d px", names[1], nx[1] - (nx[0] + nw[0] + 1));
  if (moving == 1) std::printf(", bar to %s %d px", names[0], (nx[1] - 1) - (nx[0] + nw[0]));
  std::printf("\n");

  if (!value) {
    wordRow(s, "BYPASS");
  } else if (labelled) {
    const int w = Canvas::textWidth(big, value);
    s.text("value", (kW - w) / 2, kValueY, big, value);
  } else {
    Example e = {slot, card, {names[0], names[1]}, moving, value, unit};
    valueField(s, e);
  }
  return s;
}

// C2: the parameter names stacked, A above B as the panel's pots sit under the toggle. The value
// row starts at y=35. `moving` is 0 (A), 1 (B) or -1; `cardBar` puts the bar on the card name.
Scene vstack(const char* name, const char* caption, int slot, const char* card,
             const char* names[2], int moving, const char* value, const char* unit, bool labelled,
             bool centred, bool cardBar) {
  constexpr int kTop = 35;
  constexpr int kNameY[2] = {13, 24};
  Scene s{name, caption, {}, {}, {}, {}};
  const FontDesc& big = font16x24();
  const std::string slotText = std::to_string(slot);
  s.c.drawDigits(1, 1, big, slotText.c_str());
  const int cellRight = 1 + Canvas::digitsWidth(big, slotText.c_str()) - 1;
  s.items.push_back({"slot", {1, 1, cellRight, big.height}, -1});

  const int cw = Canvas::textWidth(font5x7(), card);
  const int cx = 127 - cw;
  s.c.drawText(cx, 2, font5x7(), card);
  const int cb = cardBar ? s.bar("card", cx, 2, cw) : -1;
  s.items.push_back({"card", {cx, 2, cw, 7}, cb});

  for (int i = 0; i < 2; ++i) {
    const int w = Canvas::textWidth(font5x7(), names[i]);
    const int x = centred ? 22 + (105 - w) / 2 : 23;
    s.c.drawText(x, kNameY[i], font5x7(), names[i]);
    int b = -1;
    if (i == moving) {
      b = s.bar(names[i], x, kNameY[i], w);
      std::printf("clr %s: %s bar x=%d..%d, clear left of x=22 %d px, right of x=126 %d px\n", name,
                  names[i], x - 1, x + w, x - 1 - 22, 126 - (x + w));
    }
    s.items.push_back({names[i], {x, kNameY[i], w, 7}, b});
  }
  // Rows between box edges; bars are nominal (text top - 1, 9 rows) when not drawn.
  std::printf("clr %s: card name x=%d..126, clear to cell edge x=18 %d px; card bottom to bar A %d rows; "
              "bar A to bar B %d rows; bar B bottom to value top %d rows\n",
              name, cx, cx - 19, (kNameY[0] - 1) - (2 + 7), (kNameY[1] - 1) - (kNameY[0] - 1 + 9),
              kTop - (kNameY[1] - 1 + 9));

  if (!value) {
    wordRow(s, "BYPASS");
    // wordRow draws at y=34; redraw on the 35 row.
    s.c.fillRect(0, 34, kW, 30, false);
    const int w = Canvas::textWidth(big, "BYPASS");
    s.c.drawText((kW - w) / 2, kTop, big, "BYPASS");
    s.items.back() = {"value", {(kW - w) / 2, kTop, w, big.height}, -1};
  } else if (labelled) {
    const int w = Canvas::textWidth(big, value);
    s.text("value", (kW - w) / 2, kTop, big, value);
  } else {
    Example e = {slot, card, {names[0], names[1]}, moving, value, unit};
    valueField(s, e, kTop);
  }
  return s;
}

// ---------------------------------------------------------------------------------------------
// D and E: the card draws the controls. Each parameter name hangs off a knob picture.

// 9x9 knob, radius-4 midpoint circle. Pointer from the centre to the rim at pot position p, a
// 270 degree sweep from 7 o'clock (p = 0) through straight up (0.5) to 5 o'clock (1). The moving
// knob is a lit disc with a dark pointer, the other a hollow ring with a lit pointer.
void drawKnob(Scene& s, const char* label, int x, int y, float p, bool moving) {
  bool ring[9][9] = {};
  int dx = 0, dy = 4, d = 3 - 2 * 4;
  auto plot8 = [&](int a, int b) {
    const int px[8] = {a, -a, a, -a, b, -b, b, -b};
    const int py[8] = {b, b, -b, -b, a, a, -a, -a};
    for (int i = 0; i < 8; ++i) ring[py[i] + 4][px[i] + 4] = true;
  };
  plot8(dx, dy);
  while (dy >= dx) {
    ++dx;
    if (d > 0) { --dy; d += 4 * (dx - dy) + 10; } else { d += 4 * dx + 6; }
    plot8(dx, dy);
  }
  const bool kDisc = moving;
  for (int j = 0; j < 9; ++j) {
    int lo = 9, hi = -1;
    for (int i = 0; i < 9; ++i)
      if (ring[j][i]) { lo = i < lo ? i : lo; hi = i > hi ? i : hi; }
    for (int i = 0; i < 9; ++i) {
      const bool on = kDisc ? (i >= lo && i <= hi) : ring[j][i];
      if (on) s.c.setPixel(x + i, y + j, true);
    }
  }
  const float theta = (-135.0f + 270.0f * p) * 3.14159265f / 180.0f;
  const int ex = static_cast<int>(std::lround(4.0f * std::sin(theta)));
  const int ey = static_cast<int>(std::lround(-4.0f * std::cos(theta)));
  // Bresenham from the centre to the rim point.
  int ax = 0, ay = 0;
  const int adx = std::abs(ex), ady = -std::abs(ey), sx = ex > 0 ? 1 : -1, sy = ey > 0 ? 1 : -1;
  int err = adx + ady;
  for (;;) {
    s.c.setPixel(x + 4 + ax, y + 4 + ay, !moving);
    if (ax == ex && ay == ey) break;
    const int e2 = 2 * err;
    if (e2 >= ady) { err += ady; ax += sx; }
    if (e2 <= adx) { err += adx; ay += sy; }
  }
  s.items.push_back({label, {x, y, 9, 9}, -1});
}

// 5x7 switch picture, dark on the lit strip: outline and a 3x3 nub at the top (ON) or bottom (OFF).
void drawSwitch(Scene& s, int x, int y, bool on) {
  for (int j = 0; j < 7; ++j)
    for (int i = 0; i < 5; ++i)
      if (i == 0 || i == 4 || j == 0 || j == 6) s.c.setPixel(x + i, y + j, false);
  s.c.fillRect(x + 1, y + (on ? 1 : 3), 3, 3, false);
  s.items.push_back({"switch", {x, y, 5, 7}, -1});
}

// Text knocked out of the lit strip.
void darkText(Scene& s, const char* label, int x, int y, const FontDesc& f, const char* str) {
  Canvas t;
  t.drawText(x, y, f, str);
  for (int j = 0; j < kH; ++j)
    for (int i = 0; i < kW; ++i)
      if (lit(t, i, j)) s.c.setPixel(i, j, false);
  s.items.push_back({label, {x, y, Canvas::textWidth(f, str), f.height}, -1});
}

// family 'D': small slot number in the strip. family 'E': big slot number under it.
// row[0] is the upper pot's name, row[1] the lower. `moving` is the row index or -1.
Scene drawn(const char* name, const char* caption, char family, int slot, const char* card,
            const char* row[2], const float p[2], int moving, const char* value, const char* unit,
            bool labelled, bool on) {
  Scene s{name, caption, {}, {}, {}, {}};
  s.strict = true;
  const FontDesc& big = font16x24();
  s.c.fillRect(0, 0, kW, 9, true);
  const std::string slotText = std::to_string(slot);
  int valueTop;
  int knobX, knobY[2], nameX, nameY[2];
  if (family == 'D') {
    darkText(s, "slot", 2, 1, font5x7(), slotText.c_str());
    darkText(s, "card", 14, 1, font5x7(), card);
    drawSwitch(s, 121, 1, on);
    knobX = 1; knobY[0] = 11; knobY[1] = 21;
    nameX = 13; nameY[0] = 12; nameY[1] = 22;
    valueTop = 34;
  } else {
    const int cw = Canvas::textWidth(font5x7(), card);
    darkText(s, "card", 127 - cw, 1, font5x7(), card);
    drawSwitch(s, 2, 1, on);
    s.text("slot", 1, 10, big, slotText.c_str());
    knobX = 20; knobY[0] = 12; knobY[1] = 23;
    nameX = 32; nameY[0] = 13; nameY[1] = 24;
    valueTop = 35;
  }
  for (int i = 0; i < 2; ++i) {
    drawKnob(s, "knob", knobX, knobY[i], p[i], on && i == moving);
    s.text(row[i], nameX, nameY[i], font5x7(), row[i]);
  }
  if (!on) {
    const int w = Canvas::textWidth(big, "OFF");
    s.text("value", (kW - w) / 2, valueTop, big, "OFF");
  } else if (labelled) {
    const int w = Canvas::textWidth(big, value);
    s.text("value", (kW - w) / 2, valueTop, big, value);
  } else {
    Example e = {slot, card, {row[0], row[1]}, moving, value, unit};
    valueField(s, e, valueTop);
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: card_proof out.raw out.tsv\n");
    return 1;
  }
  std::vector<Scene> scenes;
  if (argc > 3 && std::string(argv[3]) == "d") {
    // Rows follow the physical pot order. Slots 2 and 3 have pot B in the upper row.
    const char* oct[2] = {"MIX", "SEMITONES"};
    const float octP[2] = {0.65f, 0.0f};
    const char* gate[2] = {"THRESHOLD", "DECAY"};
    const float gateP[2] = {0.4f, 0.5f};
    const char* spring[2] = {"MIX", "TIME"};
    const float springP[2] = {0.65f, 1.0f};
    const char* tune[2] = {"RESPONSE", "KEY"};
    const float tuneP[2] = {0.3f, 0.6f};
    scenes.push_back(drawn("D", "small slot number in the strip, OCTAVE, SEMITONES moving", 'D', 3, "OCTAVE", oct, octP, 1, "-12", "ST", false, true));
    scenes.push_back(drawn("Db", "small slot number, INPUT GATE, DECAY moving", 'D', 1, "INPUT GATE", gate, gateP, 1, "160", "MS", false, true));
    scenes.push_back(drawn("Dc", "small slot number, SPRING REVERB, TIME moving", 'D', 8, "SPRING REVERB", spring, springP, 1, "1000", "MS", false, true));
    scenes.push_back(drawn("Dd", "small slot number, AUTOTUNE, KEY moving", 'D', 2, "AUTOTUNE", tune, tuneP, 1, "F# / D#M", "", true, true));
    scenes.push_back(drawn("DT", "small slot number, OCTAVE, toggle flipped off", 'D', 3, "OCTAVE", oct, octP, -1, "", "", false, false));
    scenes.push_back(drawn("E", "big slot number, OCTAVE, SEMITONES moving", 'E', 3, "OCTAVE", oct, octP, 1, "-12", "ST", false, true));
    scenes.push_back(drawn("Eb", "big slot number, SPRING REVERB, TIME moving", 'E', 8, "SPRING REVERB", spring, springP, 1, "1000", "MS", false, true));
    scenes.push_back(drawn("ET", "big slot number, OCTAVE, toggle flipped off", 'E', 3, "OCTAVE", oct, octP, -1, "", "", false, false));
  } else if (argc > 3 && std::string(argv[3]) == "c2") {
    const char* oct[2] = {"SEMITONES", "MIX"};
    const char* gate[2] = {"THRESHOLD", "DECAY"};
    const char* spring[2] = {"MIX", "TIME"};
    const char* tune[2] = {"KEY", "RESPONSE"};
    scenes.push_back(vstack("C2", "slot 3 OCTAVE, SEMITONES (A) moving, -12 ST, left-aligned", 3, "OCTAVE", oct, 0, "-12", "ST", false, false, false));
    scenes.push_back(vstack("C2b", "slot 2 INPUT GATE, DECAY (B) moving, 160 MS, left-aligned", 2, "INPUT GATE", gate, 1, "160", "MS", false, false, false));
    scenes.push_back(vstack("C2c", "slot 7 SPRING REVERB, TIME (B) moving, 1000 MS, left-aligned", 7, "SPRING REVERB", spring, 1, "1000", "MS", false, false, false));
    scenes.push_back(vstack("C2d", "slot 3 AUTOTUNE, KEY (A) moving, widest key value, left-aligned", 3, "AUTOTUNE", tune, 0, "F# / D#M", "", true, false, false));
    scenes.push_back(vstack("C2T", "slot 3 OCTAVE, toggle touched, card name in the bar", 3, "OCTAVE", oct, -1, nullptr, "", false, false, true));
    scenes.push_back(vstack("C2e", "slot 3 OCTAVE, SEMITONES moving, -12 ST, centred stack", 3, "OCTAVE", oct, 0, "-12", "ST", false, true, false));
    scenes.push_back(vstack("C2f", "slot 2 INPUT GATE, DECAY moving, 160 MS, centred stack", 2, "INPUT GATE", gate, 1, "160", "MS", false, true, false));
  } else if (argc > 3) {
    const char* oct[2] = {"SEMITONE", "MIX"};
    const char* gate[2] = {"THRESH", "DECAY"};
    const char* gateRev[2] = {"THRESH", "DECAY"};
    const char* spring[2] = {"MIX", "TIME"};
    const char* tune[2] = {"KEY", "RESPONSE"};
    const char* gateFull[2] = {"THRESHOLD", "DECAY"};
    scenes.push_back(loosened("C1", "slot 3 OCTAVE, SEMITONE moving, -12 ST", 3, "OCTAVE", oct, 0, "-12", "ST", false));
    scenes.push_back(loosened("C1b", "slot 2 INPUT GATE, THRESH moving, -24 DB", 2, "INPUT GATE", gate, 0, "-24", "DB", false));
    scenes.push_back(loosened("C1c", "slot 7 SPRING REVERB, TIME moving, 1000 MS", 7, "SPRING REVERB", spring, 1, "1000", "MS", false));
    scenes.push_back(loosened("C1d", "slot 3 AUTOTUNE, KEY moving, widest key value", 3, "AUTOTUNE", tune, 0, "F# / D#M", "", true));
    scenes.push_back(loosened("C1e", "slot 2 INPUT GATE, DECAY moving, 160 MS", 2, "INPUT GATE", gateRev, 1, "160", "MS", false));
    scenes.push_back(loosened("C1T", "slot 3 OCTAVE, toggle touched, no name marked", 3, "OCTAVE", oct, -1, nullptr, "", false));
    scenes.push_back(loosened("C1x", "slot 2 INPUT GATE, THRESHOLD untruncated, overruns its box", 2, "INPUT GATE", gateFull, 0, "-24", "DB", false));
  } else {
  scenes.push_back(stackedBar("A0", "stacked, moving name in a bar", kOctave, false));
  scenes.push_back(stackedBar("A0b", "stacked, moving name in a bar (THRESHOLD)", kGate, false));
  scenes.push_back(stackedSize("A1", "stacked, moving name in 9X12, other in 5x7", kOctave));
  scenes.push_back(stackedSize("A1b", "stacked, moving name in 9X12 (THRESHOLD, may overflow)", kGate));
  scenes.push_back(stackedDither("A2", "stacked, name not moving thinned to a checkerboard", kOctave));
  scenes.push_back(columns("B0", "two columns, name not moving thinned (owner sketch)", kOctave, true));
  scenes.push_back(columns("B0b", "two columns, thinned, SPRING REVERB with 1000 MS", kSpring, true));
  scenes.push_back(columns("B1", "two columns, moving name in a bar", kOctave, false));
  scenes.push_back(bigSlot("C0", "big slot number, moving name in a bar", kOctave, false));
  scenes.push_back(bigSlot("C0b", "big slot number, SPRING REVERB with 1000 MS", kSpring, false));
  scenes.push_back(stackedBar("A0T", "toggle touched, stacked layout, no name marked", kOctave, true));
  scenes.push_back(bigSlot("C0T", "toggle touched, big slot layout, no name marked", kOctave, true));
  }

  FILE* raw = std::fopen(argv[1], "wb");
  FILE* tsv = std::fopen(argv[2], "w");
  if (!raw || !tsv) {
    std::fprintf(stderr, "cannot open output\n");
    return 1;
  }
  for (const Scene& s : scenes) {
    check(s);
    uint8_t px[kW * kH];
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x) px[y * kW + x] = lit(s.c, x, y) ? 1 : 0;
    std::fwrite(px, 1, sizeof(px), raw);
    std::fprintf(tsv, "%s\t%s\n", s.name.c_str(), s.caption.c_str());
  }
  std::fclose(raw);
  std::fclose(tsv);
  return 0;
}
