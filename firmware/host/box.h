// The box on the Mac: the firmware's main loop (cubevox.ino) with host stand-ins for what lives in
// src/board. Everything under src/core and src/ui runs unchanged. One tick() is one pass of loop().

#pragma once

#include <stdint.h>

#include <array>

#include "audio_block.h"
#include "core/canvas.h"
#include "core/compose.h"
#include "core/brightness.h"
#include "core/debounce.h"
#include "core/display.h"
#include "core/knob_filter.h"
#include "core/layout.h"
#include "core/load_led.h"
#include "core/loop_policy.h"
#include "core/mute_sequence.h"
#include "core/panel.h"
#include "core/settings.h"
#include "core/slots.h"
#include "ui/face.h"
#include "ui/menu.h"
#include "ui/screen_dim.h"
#include "ui/splash.h"

namespace host {

// The layout this box holds is the default one, under this name.
constexpr const char* kLayoutName = "default";

// Every knob starts at mid travel.
constexpr float kMidTravel = 0.5f;

// Where the panel's controls sit this pass. Knobs run 0 to 1 in slot-major order, the upper pot of
// a slot first. A slot toggle is on when it engages its effect; mute is on when MUTE engages.
struct PanelInput {
  std::array<float, kPotCount> knob;
  std::array<bool, kSlotCount> toggle;
  bool mute = false;
  int32_t detents = 0;  // encoder turns since the last tick
  bool click = false;   // encoder released after a short press since the last tick
  bool hold = false;    // encoder press reached kMenuHoldMs since the last tick

  PanelInput();
};

// The OLED, one byte per pixel (0 dark, 1 lit), row by row from the top left.
constexpr int kPixelCount = kCanvasWidth * kCanvasHeight;
using Pixels = std::array<uint8_t, kPixelCount>;

// The panel stand-in. flushStep copies the lowest dirty page into the pixel buffer, as the SH1106
// backend sends it to the glass, and clears that page's dirty bit. The window draws the pixels at
// the contrast set here.
class HostDisplay : public Display {
 public:
  void flushStep(Canvas& canvas) override;
  void setContrast(uint8_t contrast) override { contrast_ = contrast; }
  const Pixels& pixels() const { return pixels_; }
  uint8_t contrast() const { return contrast_; }

 private:
  Pixels pixels_ = {};
  uint8_t contrast_ = brightnessLevel(Brightness::High).contrast;
};

class Box {
 public:
  // `nowMs` is the host clock at power-on; the splash and the boot timers count from it.
  Box(AudioBlock& audio, uint32_t nowMs);

  // The audio clocks run, so the mute sequence may release the output.
  void audioStarted();

  // One pass of loop().
  void tick(const PanelInput& in, uint32_t nowMs);

  // The menu's SLOT 3, 4, 6 and 8 rows. A layout the box would not load stays as it is.
  void swapOctaveFormant();
  void swapUnisonMob();
  void swapDistortionGrowl();
  void swapReverb();

  // The card switches under slots 3, 4, 6 and 7. Each applies its menu action until the slot holds
  // `card` (Octave, Formant or the Formant follower; Unison or Mob; Distortion or Growl; Gate,
  // Lofi or Chatter). When no step of the cycle reaches it, because the box would not load that
  // layout, the layout stays as it was.
  void pickSlot3Card(Card card);
  void pickSlot4Card(Card card);
  void pickSlot6Card(Card card);
  void pickSlot7Card(Card card);
  void pickSlot8Card(Card card);  // Spring or Chasm

  const Pixels& pixels() const { return display_.pixels(); }
  uint8_t contrast() const { return display_.contrast(); }
  const Layout& layout() const { return layout_; }
  const char* layoutName() const { return kLayoutName; }
  LayoutCost cost() const { return layoutCost(layout_); }
  // The user LED's level, 0 to 1, from the cost table's mean. The host never marks a block late, so
  // the trouble flash never fires here.
  float ledLevel() const {
    return loadLedLevel(cost().avg, kLoadLedLimitPercent, audioRunning_, kLoadLedNoTrouble);
  }
  bool menuOpen() const { return menu_.open(); }
  bool splashEnded() const { return splashEnded_; }
  const Settings& settings() const { return settings_; }

 private:
  void pickCycledCard(Card card, UiAction action, int (*slotOf)(const Layout&));
  void readControls(const PanelInput& in, uint32_t nowMs);
  void runMute(const PanelInput& in, uint32_t nowMs);
  bool applyAction(UiAction action);
  void publishSnapshot();
  PanelReading reading() const;
  void drawFace(const UiContext& ctx, uint32_t nowMs);

  AudioBlock& audio_;
  HostDisplay display_;
  Canvas canvas_;
  Layout layout_ = defaultLayout();
  Settings settings_;
  Menu menu_;
  Face face_;
  Splash splash_;
  ScreenDim dim_;
  PublishGate publish_;
  KnobBank knobs_;
  MuteSequence mute_;
  DebouncedInput bypassSwitch_;

  std::array<bool, kSlotCount> toggle_ = {};
  bool toggleSeen_ = false;
  bool toggleChanged_ = false;
  ToggleFlip flip_;
  bool bypassOn_ = false;
  bool audioRunning_ = false;
  bool splashEnded_ = false;
  uint32_t nowMs_ = 0;
};

}  // namespace host
