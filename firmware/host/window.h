// The emulator window: one ImGui screen that mirrors the box's panel. Top band, the MUTE toggle, the
// OLED and the MENU knob with the LED and the CPU line. Middle band, the eight slot columns. Bottom
// band, the source switch and the version.

#pragma once

#include <stdint.h>

#include <array>

#include "box.h"

struct GLFWwindow;

namespace host {

// What the user has set on the panel and on the source switch.
struct Panel {
  PanelInput input;
  bool useMic = false;
};

class Window {
 public:
  // A hidden window is for the proof: it renders into an offscreen target and never shows.
  explicit Window(bool visible);
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  bool ok() const { return window_ != nullptr; }
  bool shouldClose() const;

  // One frame is begin(), draw(), the box tick, then end().
  void begin();

  // Draws the face and reads the user's gestures into `panel`. `micAvailable` undims the MIC position.
  void draw(Box& box, Panel& panel, bool micAvailable);

  // Uploads the OLED and presents (or renders offscreen when hidden).
  void end(const Box& box);

  // Draws a few frames of the face offscreen and writes the last as a P6 PPM.
  bool saveProof(Box& box, Panel& panel, bool micAvailable, const char* path);

  // The MENU knob's gesture state between frames.
  struct Encoder {
    float drag = 0;    // drag and scroll travel not yet turned into a detent, in detents
    float travel = 0;  // pointer travel since the press, to tell a click from a turn
    float angle = 0;   // how far the knob has turned, for the indicator
    double flashUntil = 0;
  };

 private:
  void ensureProofTarget();

  GLFWwindow* window_ = nullptr;
  bool offscreen_ = false;
  bool imguiUp_ = false;
  unsigned oledTexture_ = 0;
  unsigned proofFbo_ = 0;
  unsigned proofTexture_ = 0;
  Encoder encoder_;
  std::array<uint32_t, kPixelCount> oledRgba_ = {};
};

}  // namespace host
