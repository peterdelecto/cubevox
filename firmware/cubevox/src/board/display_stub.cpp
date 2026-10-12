#include "board/display_stub.h"

#include <Arduino.h>

namespace {

// Prints one dirty page per step as four text lines. Each character covers two canvas rows of
// one column, so the page reads as a half-height picture: ' ' blank, '\'' top lit, ',' bottom
// lit, '#' both.
class StubDisplay : public Display {
 public:
  void flushStep(Canvas& canvas) override {
    const uint8_t dirty = canvas.dirtyPages();
    if (dirty == 0) return;
    int page = 0;
    while (!(dirty & (1u << page))) ++page;

    Serial.print("[oled] page ");
    Serial.println(page);
    const uint8_t* bytes = canvas.page(page);
    for (int pair = 0; pair < 4; ++pair) {
      char line[kCanvasWidth + 1];
      for (int x = 0; x < kCanvasWidth; ++x) {
        const int top = (bytes[x] >> (pair * 2)) & 1;
        const int bottom = (bytes[x] >> (pair * 2 + 1)) & 1;
        line[x] = top && bottom ? '#' : top ? '\'' : bottom ? ',' : ' ';
      }
      line[kCanvasWidth] = '\0';
      Serial.print("[oled] ");
      Serial.println(line);
    }
    canvas.clearDirty(page);
  }
};

}  // namespace

Display& stubDisplay() {
  static StubDisplay display;
  return display;
}
