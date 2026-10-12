// MENU state machine and the OLED screens. Pure: input events in, actions out, text rows drawn
// onto a Canvas that a Display backend pushes to the panel.

#pragma once

#include <stdint.h>

#include "core/canvas.h"
#include "core/display.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/settings.h"

// What the encoder did since the last pass.
struct UiInput {
  int detents = 0;  // signed turn steps
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

class Menu {
 public:
  UiAction update(const UiInput& in, uint32_t nowMs);

  // Redraws the canvas when the frame differs from the last one drawn.
  void draw(const UiContext& ctx, Canvas& canvas);

 private:
  enum class Mode : uint8_t { Idle, List, ChainView, ConfirmDfu };
  struct Frame {
    char row[kDisplayRows][kDisplayCols + 1];
  };

  UiAction onClick();
  void onTurn(int detents);

  Mode mode_ = Mode::Idle;
  int cursor_ = 0;
  uint32_t lastActivityMs_ = 0;
  Frame shown_ = {};
};
