// SH1106 driver. U8x8 handles init and I2C; the canvas goes out one page per flushStep() with
// drawTile. U8x8 hardware I2C uses the global Wire object, so the pins are set on Wire before begin().

#include <Arduino.h>
#include <U8x8lib.h>
#include <Wire.h>

#include "board/display_sh1106.h"
#include "board/pins.h"

namespace {

constexpr uint8_t kOledAddress = 0x3C;  // 7-bit
constexpr uint32_t kI2cHz = 400000;

U8X8_SH1106_128X64_NONAME_HW_I2C gU8x8;
bool gInitialised = false;
bool gPresent = false;

// Probes the address and starts the panel. The panel is only started when it answers.
void initOnce() {
  if (gInitialised) return;
  gInitialised = true;

  Wire.setSDA(pins::kOledSda);
  Wire.setSCL(pins::kOledScl);
  Wire.begin();
  Wire.beginTransmission(kOledAddress);
  gPresent = Wire.endTransmission() == 0;
  if (!gPresent) return;

  gU8x8.setBusClock(kI2cHz);
  gU8x8.begin();
}

class Sh1106Display : public Display {
 public:
  void flushStep(Canvas& canvas) override {
    const uint8_t dirty = canvas.dirtyPages();
    if (dirty == 0) return;
    int page = 0;
    while (!(dirty & (1u << page))) ++page;
    // A page is 16 tiles of 8 bytes, which is the canvas page layout as is.
    gU8x8.drawTile(0, page, kCanvasWidth / 8, const_cast<uint8_t*>(canvas.page(page)));
    canvas.clearDirty(page);
  }
};

}  // namespace

bool sh1106Present() {
  initOnce();
  return gPresent;
}

Display& sh1106Display() {
  static Sh1106Display display;
  initOnce();
  return display;
}
