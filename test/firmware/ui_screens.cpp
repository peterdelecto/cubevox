// Checks the runtime screens on the host. Covers the readout against the proof renderer byte for
// byte (settled, rolling and muted), its rows and edges, the voice ribbon and the muted word. The
// proof PGMs go to build/oled-*.pgm.

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>

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

// Digits sit in fixed cells, so the unit never shifts while the digits change.
void checkReadoutTabular() {
  const FontDesc& big = font16x24();
  const int cell = big.width + big.spacing;
  const int unitX = (kCanvasWidth - (3 * cell + Canvas::textWidth(big, " MS"))) / 2 + 3 * cell;
  const Canvas base = readoutOf(4, 1, 100.0f);
  bool still = true;
  for (int v = 100; v <= 250; ++v) {
    const Canvas next = readoutOf(4, 1, static_cast<float>(v));
    for (int y = 34; y < 58 && still; ++y) {
      for (int x = unitX; x < kCanvasWidth; ++x) still = still && lit(base, x, y) == lit(next, x, y);
    }
  }
  check(still, "readout: the unit stays in place while the digits run from 100 to 250");

  const Canvas one = readoutOf(4, 1, 111.0f);
  const Canvas eight = readoutOf(4, 1, 188.0f);
  check(!sameRows(one, eight, 34, 58) && sameRows(one, eight, 0, 34), "readout: only the value rows change between two numbers");
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

void checkReadoutText() {
  // The autotune key is a name. It draws centred in 16X24 and does not roll.
  const Canvas key = readoutOf(1, 0, 11.0f);
  Canvas want;
  want.drawText(1, 1, font5x7(), "2 AUTOTUNE");
  want.drawText((kCanvasWidth - Canvas::textWidth(font9x12(), "KEY")) / 2, 15, font9x12(), "KEY");
  want.drawText((kCanvasWidth - Canvas::textWidth(font16x24(), "F / DM")) / 2, 34, font16x24(), "F / DM");
  check(sameRows(key, want, 0, kCanvasHeight), "readout: the key shows its name in 16X24 as on the approved sheet");
  writePgm(key, "oled-readout-key.pgm");

  check(edgesDark(readoutOf(1, 0, 6.0f)), "readout: the widest key name, F# / D#m, keeps the edges dark");
  ReadoutView stray = viewOf(1, 0, 3.5f);
  stray.target = 99;
  Canvas clamped;
  renderReadout(clamped, stray);
  Canvas last;
  last.drawText((kCanvasWidth - Canvas::textWidth(font16x24(), "F / DM")) / 2, 34, font16x24(), "F / DM");
  check(sameRows(clamped, last, 34, 58), "readout: a target past the last key clamps to the last name");
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

}  // namespace

void checkScreens() {
  checkReadoutSettled();
  checkReadoutRolling();
  checkReadoutRows();
  checkReadoutTabular();
  checkReadoutRollClip();
  checkReadoutEdges();
  checkReadoutText();
  checkReadoutEdgeCases();
  checkRibbonHeight();
  checkRibbonHistory();
  checkVoiceSilence();
  checkVoiceShape();
  checkVoiceWholeFrame();
  checkVoiceScroll();
  checkVoiceWalk();
  checkMuted();
}
