// Day-one panel self-test, pure. Tracks per pot the range seen and per toggle the states seen, so
// a dead, reversed or miswired control shows up before anyone debugs effects.

#pragma once

#include <stdint.h>

class Canvas;

constexpr int kPanelTestPots = 16;
constexpr int kPanelTestToggles = 8;

constexpr float kPanelTestLow = 0.05f;        // a pot has reached its low end below this
constexpr float kPanelTestHigh = 0.95f;       // and its high end above this
constexpr float kPanelTestPrintStep = 0.03f;  // movement since the last print that reports again
constexpr uint32_t kPanelTestPrintGapMs = 40;  // per-pot minimum between prints

class PanelTest {
 public:
  void reset();

  // Feeds one raw pot value (0..1). Returns true when the value is worth printing: it moved by
  // more than kPanelTestPrintStep since the last print (at least kPanelTestPrintGapMs apart), or
  // it newly reached an end.
  bool updatePot(int pot, float value, uint32_t nowMs);

  // Feeds one debounced toggle level. Returns true on an edge. The first call per toggle only
  // records the starting state.
  bool updateToggle(int slot, bool on);

  float potMin(int pot) const { return min_[pot]; }
  float potMax(int pot) const { return max_[pot]; }
  bool potDone(int pot) const;
  bool toggleDone(int slot) const { return seenOn_[slot] && seenOff_[slot]; }

  int potsDone() const;
  int togglesDone() const;

  // First pot (mux channel) or toggle (slot) not yet fully exercised, or -1.
  int nextPot() const;
  int nextToggle() const;

 private:
  float min_[kPanelTestPots];
  float max_[kPanelTestPots];
  float printed_[kPanelTestPots];
  uint32_t printedAtMs_[kPanelTestPots];
  bool potSeen_[kPanelTestPots];
  bool seenOn_[kPanelTestToggles];
  bool seenOff_[kPanelTestToggles];
  bool toggleLevel_[kPanelTestToggles];
  bool toggleSeen_[kPanelTestToggles];
};

// Draws the OLED test pattern: 1 px border, a checkerboard block, and one line each in
// font5x7, font9x12 and font16x24.
void drawPanelTestPattern(Canvas& canvas);
