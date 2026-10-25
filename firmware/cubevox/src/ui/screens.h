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

// Readout rows. The card line is 5x7 at the pad, the parameter name is 9X12 centred with its top at
// kReadoutNameY, and the value is 16X24 in a field centred once with its top at kReadoutValueY.
constexpr int kReadoutNameY = 15;
constexpr int kReadoutValueY = 34;

// The voice ribbon is mirrored about the horizon, and each level it holds spans two columns, so it
// scrolls 50 px/s at 25 frames a second.
constexpr int kRibbonHorizonY = kCanvasHeight / 2;
constexpr int kRibbonHalfMax = 22;
constexpr int kRibbonColumnsPerLevel = 2;
constexpr int kRibbonLevels = kCanvasWidth / kRibbonColumnsPerLevel;

// Half-height of the ribbon, in pixels, for a level of 0..1.
int ribbonHalfHeight(float level);

// The recent input levels, newest first.
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

// The ribbon mirrored about the horizon, shifted by `walk`. In silence it is the horizon line alone.
void renderVoice(Canvas& canvas, const Ribbon& ribbon, Walk walk);

// What the readout needs, taken from the card table and the gui.
struct ReadoutView {
  int slot = 0;                    // 0-based; the screen shows slot + 1
  const char* card = "";           // card name
  const KnobDesc* knob = nullptr;  // null draws the card line only
  float shown = 0.0f;              // the eased value, in the whole steps knobShown counts
  int target = 0;                  // where it settles; picks the name of a labelled knob
};

// Slot and card, the parameter name, and the value with its unit. The value sits in a field fixed
// by the parameter, so the row never moves sideways. Each digit that changes rolls vertically from
// the old glyph to the new one.
void renderReadout(Canvas& canvas, const ReadoutView& view);

// The value row's unit with its leading space, in capitals, or "" for a knob without one. The value
// font's lowercase cells hold the flat sign and the minor-key glyph for key names, so unit text
// never passes through them. Returns the length written.
int unitText(const KnobDesc& knob, char* out, int size);

// Copies text in capitals. The 9X12 and 16X24 draw a lowercase b and m as the flat and the
// minor-key glyph, so prose that may hold them goes up before it is drawn.
int upperText(const char* in, char* out, int size);

// The word the top switch shows while it is engaged.
const char* topSwitchWord(TopSwitch role);

// The word centred on the panel, nothing else.
void renderMuted(Canvas& canvas, const char* word);
