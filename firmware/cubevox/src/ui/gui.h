// Decides which runtime screen the OLED shows and when it changes. Knob touches and the switch
// states go in, and a description of the next frame comes out. The code is pure. ui/face turns the
// description into pixels. Spec: docs/specs/2026-10-24-oled-gui-design.md.

#pragma once

#include <stdint.h>

#include "core/canvas.h"
#include "core/settings.h"

// The UI ticks at 25 frames a second. The panel needs about 25 ms to take a full frame, so a
// faster tick would outrun it.
constexpr int kUiFps = 25;
constexpr uint32_t kUiTickMs = 1000 / kUiFps;

// A readout stays up this long after the last knob move.
constexpr uint32_t kReadoutHoldMs = 2000;

// The voice screen moves 1 px this often, to spread the wear on the panel.
constexpr uint32_t kWalkStepMs = 60000;

enum class Screen : uint8_t {
  Voice,    // the input level as a scrolling ribbon
  Readout,  // the knob that moved last
  Muted,    // the word of the top switch, MUTED or BYPASS
  Menu,
};

// Where the voice screen sits against its nominal place.
struct Walk {
  int8_t x;
  int8_t y;
};

// The shift after `step` periods of kWalkStepMs. It starts at the centre of a 3 by 3 block and then
// circles the block's 8 outer cells, moving 1 px along one axis per step.
Walk walkAt(uint32_t step);

// True while the top switch sits in its engaged position. Under MUTE that is the mute sequence
// being active, and under BYPASS it is the debounced switch.
constexpr bool topSwitchEngaged(TopSwitch role, bool muteActive, bool bypassOn) {
  return role == TopSwitch::Bypass ? bypassOn : muteActive;
}

// What one tick shows.
struct GuiFrame {
  Screen screen = Screen::Voice;

  // 0 once the screen is settled, else the dissolve step (1 to kDissolveSteps) that blends the frame
  // on the panel toward this screen.
  int dissolve = 0;

  // Readout only. `target` is the reading the knob holds and `shown` is the value easing toward it,
  // both in the whole steps that knobShown counts.
  int slot = 0;
  int knob = 0;
  float shown = 0.0f;
  float target = 0.0f;

  // Voice only.
  Walk walk = {0, 0};
};

class Gui {
 public:
  Gui();

  // A knob moved. `target` is its reading in the whole steps that knobShown counts. A different
  // knob, or one whose readout had gone, starts at the reading. The same knob eases to it.
  void knobMoved(int slot, int knob, float target, uint32_t nowMs);

  // Ends the readout now, as when the card it describes has left its slot.
  void dropReadout() { readoutLive_ = false; }

  // Starts a dissolve on the coming tick even when the wanted screen is the one shown. Call it when
  // something else drew on the panel.
  void forceChange() { force_ = true; }

  // One UI tick. The open menu wins, then a live readout, then the top switch, then the voice.
  GuiFrame tick(uint32_t nowMs, bool switchOn, bool menuOpen);

 private:
  struct Id {
    Screen screen;
    uint8_t slot;
    uint8_t knob;
  };

  Id wanted(bool switchOn, bool menuOpen) const;
  void advanceDissolve(Id want);

  float ease_;  // share of the gap the value closes each tick

  Id shown_ = {Screen::Voice, 0, 0};
  int dissolve_ = 0;
  bool force_ = false;

  bool readoutLive_ = false;
  uint8_t slot_ = 0;
  uint8_t knob_ = 0;
  float target_ = 0.0f;
  float value_ = 0.0f;
  uint32_t touchedMs_ = 0;

  bool started_ = false;
  uint32_t startMs_ = 0;
};
