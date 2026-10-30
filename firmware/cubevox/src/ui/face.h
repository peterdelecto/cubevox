// The runtime face of the box. Each UI tick it asks the gui which screen to show, draws that screen
// and dissolves from the frame already on the panel. The menu draws itself through it, so its text
// and cursor stay as the menu spec has them. The code is pure.
// Spec: docs/specs/2026-10-24-oled-gui-design.md.

#pragma once

#include <stdint.h>

#include "core/canvas.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/panel.h"
#include "core/timing.h"
#include "ui/gui.h"
#include "ui/menu.h"
#include "ui/screens.h"

class Face {
 public:
  // A knob moved. Takes its card from `layout` and its reading from `panel`. A slot with no card,
  // and so no knob, raises no readout. A readout ends at the next tick once its card leaves the slot.
  void knobMoved(const Layout& layout, const PanelReading& panel, LastMoved moved, uint32_t nowMs);

  // A slot's toggle flipped. Takes its card from `layout`. A slot with no card raises no readout.
  // The readout ends at the next tick once its card leaves the slot, as for a knob.
  void toggleFlipped(const Layout& layout, ToggleFlip flip, uint32_t nowMs);

  // Something else drew on the panel. The next frame dissolves from whatever it left.
  void invalidate() { gui_.forceChange(); }

  // True when a tick is due, which takes kUiTickMs since the last one and every page of the
  // previous frame already sent. A tick that outran the panel would tear the picture. Call tick()
  // whenever this returns true.
  bool due(uint32_t nowMs, const Canvas& live);

  // One UI tick. `input` and `output` are what the audio interrupt heard going into the chain and
  // coming out of it since the last tick. The level bar shows the input and the ribbon shows the
  // output. From the first tick on, the face is on the panel, so the menu listens to the encoder.
  void tick(uint32_t nowMs, const UiContext& ctx, const level::InputEnergy& input,
            const level::OutputEnergy& output, Menu& menu, Canvas& live);

 private:
  void render(const GuiFrame& frame, const UiContext& ctx, Menu& menu);

  Gui gui_;

  // The slot of the last knob moved or toggle flipped, -1 for none, and the card that slot held then.
  int readoutSlot_ = -1;
  Card readoutCard_ = Card::Empty;

  level::Meter inputMeter_;
  level::OutputMeter outputMeter_;
  int meterSegments_ = 0;
  Ribbon ribbon_;
  Canvas target_;                  // the screen the gui picked, before it reaches the panel
  bool targetHoldsMenu_ = false;   // whether the menu's last frame is still in target_
  Every tick_{kUiTickMs};
  bool ticked_ = false;
  uint32_t lastTickMs_ = 0;        // the level meters fall by real time, so a late tick falls further
};
