// SH1106 driver. U8x8 handles init and I2C; the canvas goes out one page per flushStep() with
// drawTile. U8x8 hardware I2C uses the global Wire object, so the pins are set on Wire before begin().

#include "board/display_sh1106.h"

#include <Arduino.h>
#include <U8x8lib.h>
#include <Wire.h>

#include <array>
#include <string.h>

#include "board/pins.h"
#include "board/serial_link.h"

namespace {

constexpr uint32_t kI2cHz = 400000;

U8X8_SH1106_128X64_NONAME_HW_I2C gU8x8;
bool gInitialised = false;
bool gPresent = false;
bool gWriteFailing = false;  // the last page write failed

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

// One warning per run of failed writes. The page stays dirty, so the next pass retries it.
void noteWrite(bool ok) {
  if (ok == !gWriteFailing) return;
  gWriteFailing = !ok;
  serialOut().println(ok ? "[boot] OLED writes recovered" : "[WARN] OLED page write failed, retrying");
}

class Sh1106Display : public Display {
 public:
  void flushStep(Canvas& canvas) override {
    if (!gPresent) return;
    const int page = canvas.firstDirtyPage();
    if (page < 0) return;
    // A page is 16 tiles of 8 bytes, which is the canvas page layout as is. The C call takes a
    // writable pointer, so it gets a copy. The U8x8 member wrapper would drop the result.
    std::array<uint8_t, kCanvasWidth> tiles;
    memcpy(tiles.data(), canvas.page(page), tiles.size());
    const bool ok = u8x8_DrawTile(gU8x8.getU8x8(), 0, static_cast<uint8_t>(page), kCanvasWidth / 8, tiles.data()) != 0;
    noteWrite(ok);
    if (ok) canvas.clearDirty(page);
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
