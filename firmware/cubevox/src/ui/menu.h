// MENU state machine and its OLED screens. Input events go in, actions come out, and text rows are
// drawn onto a Canvas that a Display backend pushes to the panel. The code is pure. The screens
// shown while the menu is closed (voice, readout, muted) live in ui/screens, and ui/face picks
// between them.

#pragma once

#include <stdint.h>

#include <array>

#include "core/canvas.h"
#include "core/display.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/settings.h"

// What the encoder did since the last pass.
struct UiInput {
  int32_t detents = 0;  // signed turn steps
  bool click = false;
};

// What the sketch must do after an update. The menu never edits settings itself.
enum class UiAction : uint8_t {
  None,
  TogglePolishComp,
  TogglePolishEq,
  ToggleOutput,
  ToggleInputJack,
  CycleTopSwitch,
  EnterDfu,
};

// Read-only state the screens draw. `layout` names the cards, `panel` holds the knob readings and
// `settings` the menu values. `topSwitchOn` says the top switch sits in its engaged position,
// whether it mutes or bypasses.
struct UiContext {
  const Layout& layout;
  const PanelReading& panel;
  const Settings& settings;
  bool topSwitchOn;
};

// The text of one screen: kDisplayRows rows of up to kDisplayCols characters, each NUL-terminated.
using MenuFrame = std::array<std::array<char, kDisplayCols + 1>, kDisplayRows>;

class Menu {
 public:
  // Takes the encoder's input. Until listen() it drops every click and turn.
  UiAction update(const UiInput& in, uint32_t nowMs);

  // Lets input through. The face calls it on every tick, so a click or a turn during the boot splash,
  // when nobody could see the menu it opened, does nothing.
  void listen() { listening_ = true; }

  // True while a menu screen is up, which is until Exit or the timeout closes it, and from the
  // update's confirm click until the reset.
  bool open() const { return mode_ != Mode::Idle; }

  // Redraws the canvas when the frame differs from the last one drawn. Does nothing while the menu
  // is closed.
  void draw(const UiContext& ctx, Canvas& canvas);

  // Forgets the last frame, so the next draw repaints even if the content is unchanged. Call it
  // after something else drew on the canvas.
  void invalidate() { shownValid_ = false; }

  // Opens the read-only chain view. No list row opens it, so only the proof writer calls this.
  void showChainView() { mode_ = Mode::ChainView; }

 private:
  enum class Mode : uint8_t { Idle, List, ChainView, ConfirmDfu, Starting };

  UiAction onClick();
  void onTurn(int32_t detents);

  Mode mode_ = Mode::Idle;
  bool listening_ = false;
  uint8_t cursor_ = 0;
  uint32_t lastActivityMs_ = 0;
  MenuFrame shown_ = {};
  bool shownValid_ = false;
};
