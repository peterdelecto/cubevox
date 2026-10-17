#include "board/display_stub.h"

#include <Arduino.h>

#include <array>

#include "board/serial_link.h"

namespace {

// Prints one dirty page per step as four text lines. Each character covers two canvas rows of
// one column, so the page reads as a half-height picture: ' ' blank, '\'' top lit, ',' bottom
// lit, '#' both.
class StubDisplay : public Display {
 public:
  void flushStep(Canvas& canvas) override {
    const int page = canvas.firstDirtyPage();
    if (page < 0) return;

    serialOut().print("[oled] page ");
    serialOut().println(page);
    const uint8_t* bytes = canvas.page(page);
    for (int pair = 0; pair < 4; ++pair) {
      std::array<char, kCanvasWidth + 1> line;
      for (int x = 0; x < kCanvasWidth; ++x) {
        const int top = (bytes[x] >> (pair * 2)) & 1;
        const int bottom = (bytes[x] >> (pair * 2 + 1)) & 1;
        line[x] = top && bottom ? '#' : top ? '\'' : bottom ? ',' : ' ';
      }
      line[kCanvasWidth] = '\0';
      serialOut().print("[oled] ");
      serialOut().println(line.data());
    }
    canvas.clearDirty(page);
  }
};

}  // namespace

Display& stubDisplay() {
  static StubDisplay display;
  return display;
}
