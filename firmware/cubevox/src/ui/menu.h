// MENU state machine and the OLED screens. Pure: input events in, actions out, text rows drawn
// onto a Canvas that a Display backend pushes to the panel.

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
enum class UiAction : uint8_t { None, ToggleOutput, ToggleInputJack, EnterDfu };

// Read-only state the screens draw. The idle screen shows the last-moved knob from `panel`;
// the chain view lists `layout`.
struct UiContext {
  const Layout& layout;
  const PanelReading& panel;
  const Settings& settings;
  bool muted;
};

// The text of one screen: kDisplayRows rows of up to kDisplayCols characters, each NUL-terminated.
using MenuFrame = std::array<std::array<char, kDisplayCols + 1>, kDisplayRows>;

class Menu {
 public:
  UiAction update(const UiInput& in, uint32_t nowMs);

  // Redraws the canvas when the frame differs from the last one drawn.
  void draw(const UiContext& ctx, Canvas& canvas);

  // Forgets the last frame, so the next draw repaints even if the content is unchanged. Call it
  // after something else drew on the canvas.
  void invalidate() { shownValid_ = false; }

 private:
  enum class Mode : uint8_t { Idle, List, ChainView, ConfirmDfu };

  UiAction onClick();
  void onTurn(int32_t detents);

  Mode mode_ = Mode::Idle;
  uint8_t cursor_ = 0;
  uint32_t lastActivityMs_ = 0;
  MenuFrame shown_ = {};
  bool shownValid_ = false;
};
