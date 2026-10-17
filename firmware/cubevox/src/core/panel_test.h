// Day-one panel self-test, pure. Tracks per pot the range seen and per toggle the states seen, so
// a dead, reversed or miswired control shows up before anyone debugs effects.

#pragma once

#include <stdint.h>

#include <array>

#include "core/slots.h"

class Canvas;

constexpr int kPanelTestPots = kPotCount;
constexpr int kPanelTestToggles = kSlotCount;

constexpr float kPanelTestLow = 0.05f;        // a pot has reached its low end below this
constexpr float kPanelTestHigh = 0.95f;       // and its high end above this
constexpr float kPanelTestPrintStep = 0.03f;  // movement since the last print that reports again
constexpr uint32_t kPanelTestPrintGapMs = 40;  // per-pot minimum between prints

class PanelTest {
 public:
  PanelTest() { reset(); }

  void reset();

  // Feeds one raw pot value (0..1). Returns true when the value is worth printing: it moved by
  // more than kPanelTestPrintStep since the last print (at least kPanelTestPrintGapMs apart), or
  // it newly reached an end. An out-of-range pot is ignored and returns false.
  bool updatePot(int pot, float value, uint32_t nowMs);

  // Feeds one debounced toggle level. Returns true on an edge. The first call per toggle only
  // records the starting state. An out-of-range slot is ignored and returns false.
  bool updateToggle(int slot, bool on);

  // False for an out-of-range index.
  bool potDone(int pot) const;
  bool toggleDone(int slot) const;

  int potsDone() const;
  int togglesDone() const;

 private:
  std::array<float, kPanelTestPots> min_;
  std::array<float, kPanelTestPots> max_;
  std::array<float, kPanelTestPots> printed_;
  std::array<uint32_t, kPanelTestPots> printedAtMs_;
  std::array<bool, kPanelTestPots> potSeen_;
  std::array<bool, kPanelTestToggles> seenOn_;
  std::array<bool, kPanelTestToggles> seenOff_;
  std::array<bool, kPanelTestToggles> toggleLevel_;
  std::array<bool, kPanelTestToggles> toggleSeen_;
};

// Draws the OLED test pattern: 1 px border, a checkerboard block, and one line each in
// font5x7, font9x12 and font16x24.
void drawPanelTestPattern(Canvas& canvas);
