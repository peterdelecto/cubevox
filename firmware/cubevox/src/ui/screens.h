// The runtime screens are the voice ribbon, the knob readout and the muted word. Each draws a whole
// frame onto a canvas it clears first. The code is pure. Spec: docs/specs/2026-10-24-oled-gui-design.md.

#pragma once

#include <stdint.h>

#include <array>

#include "core/canvas.h"
#include "core/registry.h"
#include "core/settings.h"
#include "ui/gui.h"

// Text keeps this many pixels clear of the panel edge.
constexpr int kScreenPad = 1;

// Readout rows. A lit title bar spans the full width over rows 0 to kReadoutBarHeight - 1. The slot
// number and card name sit on it in black 7X12, centred, with their top at kReadoutTitleY and 1 px
// of bar above and below the 12 px glyphs. The parameter name is 9X12 centred with its top at
// kReadoutNameY, and the value is 16X24, centred on what is drawn, with its top at kReadoutValueY.
constexpr int kReadoutTitleY = kScreenPad;
constexpr int kReadoutBarHeight = 12 + 2 * kScreenPad;
constexpr int kReadoutNameY = 16;
constexpr int kReadoutValueY = 34;

// The voice ribbon is mirrored about the horizon, and each level it holds spans two columns, so it
// scrolls 50 px/s at 25 frames a second. It runs from kRibbonLeftX to kRibbonRightX, and the last
// column of the panel stays blank.
constexpr int kRibbonHorizonY = kCanvasHeight / 2;
constexpr int kRibbonHalfMax = 22;
constexpr int kRibbonColumnsPerLevel = 2;
constexpr int kRibbonLeftX = 13;
constexpr int kRibbonRightX = kCanvasWidth - 2;
constexpr int kRibbonLevels = (kRibbonRightX - kRibbonLeftX + 1) / kRibbonColumnsPerLevel;

// The input level meter is a 1 px outline box at the left edge with 16 segments inside, like a
// thermometer without the bulb. The box stands on the voice screen in silence too. Its sides are
// columns kMeterBoxLeftX and kMeterBoxRightX and its top and bottom are rows kMeterBoxTopY and
// kMeterBoxBottomY. The interior is columns 2 to 8, rows 9 to 55. Each segment is 2 rows tall with
// 1 row of gap above it, and the lowest one sits on the interior's last row. Columns 10 to 12,
// between the box and the ribbon, stay blank.
constexpr int kMeterBoxLeftX = 1;
constexpr int kMeterBoxRightX = 9;
constexpr int kMeterBoxTopY = 8;
constexpr int kMeterBoxBottomY = 56;
constexpr int kMeterX = kMeterBoxLeftX + 1;
constexpr int kMeterWidth = kMeterBoxRightX - kMeterBoxLeftX - 1;
constexpr int kMeterTopY = kMeterBoxTopY + 1;
constexpr int kMeterRows = kMeterBoxBottomY - kMeterBoxTopY - 1;
constexpr int kMeterSegments = 16;
constexpr int kMeterSegmentRows = 2;
constexpr int kMeterSegmentGap = 1;
static_assert(kMeterRows == kMeterSegments * kMeterSegmentRows + (kMeterSegments - 1) * kMeterSegmentGap,
              "the segments and their gaps fill the box exactly");
static_assert(kRibbonLeftX - kMeterBoxRightX - 1 == 3, "three blank columns lie between the box and the ribbon");
static_assert(kMeterWidth == 7, "the interior is 7 columns wide");

// Half-height of the ribbon, in pixels, for a level of 0..1.
int ribbonHalfHeight(float level);

// Lit segments of the level meter, 0 to kMeterSegments, for a level of 0..1.
int meterSegments(float level);

// The recent output levels, newest first.
class Ribbon {
 public:
  // Adds the newest level, which enters at the right edge and scrolls left.
  void push(float level);

  // Half-height in pixels of the level pushed `age` ticks ago. 0 beyond the history.
  int half(int age) const;

 private:
  std::array<uint8_t, kRibbonLevels> half_{};
  int head_ = 0;
};

// The voice screen. The ribbon is mirrored about the horizon and shifted by `walk`; in silence it
// is the horizon line alone. The meter box stands at the left edge with `segments` segments lit,
// held to 0..kMeterSegments. The box, the segments and the ribbon walk together.
void renderVoice(Canvas& canvas, const Ribbon& ribbon, int segments, Walk walk);

// What the readout needs, taken from the card table and the gui.
struct ReadoutView {
  int slot = 0;                    // 0-based; the screen shows slot + 1
  const char* card = "";           // card name
  const KnobDesc* knob = nullptr;  // null draws the title bar only
  float shown = 0.0f;              // the eased value, in the whole steps knobShown counts
  int target = 0;                  // where it settles; picks the name of a labelled knob
  bool toggle = false;             // the slot's toggle: no parameter row, ON or OFF as the value
  bool on = false;                 // the toggle's state
};

// The title bar with slot and card, the parameter name, and the value with its unit. Each digit
// that changes rolls vertically from the old glyph to the new one. The value row is centred on its
// field, which holds the cells of the wider of the two numbers the roll sits between, a space and
// the unit. A settled value is one number, so its field is that number's own width. Cells and unit
// stay put while numbers of one digit count roll, and the row re-centres by half a cell when a digit
// or a sign appears or vanishes. A labelled knob centres its name on the name's own width. A toggle
// keeps the bar, leaves the parameter row empty and centres ON or OFF on the value row.
void renderReadout(Canvas& canvas, const ReadoutView& view);

// The value row's unit with its leading space, as written (st, ms, dB, %), or "" for a knob without
// one. Returns the length written.
int unitText(const KnobDesc& knob, char* out, int size);

// Copies text in capitals. The 7X12, 9X12 and 16X24 fonts ink lowercase b, d, m, s and t as glyphs
// of their own, so prose that may hold them goes up before it is drawn.
int upperText(const char* in, char* out, int size);

// The word the top switch shows while it is engaged.
const char* topSwitchWord(TopSwitch role);

// The word centred on the panel, nothing else.
void renderMuted(Canvas& canvas, const char* word);

// The DEBUG load figure, for example 42%, in 5x7 at the bottom right. Its last column is 126 and its
// top row is 56, so it keeps kScreenPad from both edges. It draws over the canvas without clearing it,
// and blanks the box behind the text first. `percent` is held to 0 to kMaxLoadPercent.
void drawLoad(Canvas& canvas, int percent);
