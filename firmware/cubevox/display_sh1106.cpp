// SH1106 driver. U8x8 writes each character cell immediately, so flush() has nothing to do.
// U8x8 hardware I2C uses the global Wire object, so the pins are set on Wire before begin().

#include <Arduino.h>
#include <U8x8lib.h>
#include <Wire.h>
#include <string.h>

#include "display_sh1106.h"
#include "pins.h"

namespace {

constexpr uint8_t kOledAddress = 0x3C;  // 7-bit
constexpr uint32_t kI2cHz = 400000;
constexpr int kRows = 8;
constexpr int kCols = 16;  // 8 px font across 128 px

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
  gU8x8.setFont(u8x8_font_chroma48medium8_r);
}

class Sh1106Display : public Display {
 public:
  void clear() override {
    memset(rows_, ' ', sizeof(rows_));
    for (int r = 0; r < kRows; ++r) rows_[r][kCols] = '\0';
    inverted_ = -1;
    gU8x8.clearDisplay();
  }

  void text(int row, const char* line) override {
    if (row < 0 || row >= kRows) return;
    int i = 0;
    for (; i < kCols && line[i] != '\0'; ++i) rows_[row][i] = line[i];
    for (; i < kCols; ++i) rows_[row][i] = ' ';
    drawRow(row, row == inverted_);
  }

  void invertRow(int row) override {
    if (row < 0 || row >= kRows) return;
    inverted_ = row;
    drawRow(row, true);
  }

  void flush() override {}

 private:
  void drawRow(int row, bool inverse) {
    gU8x8.setInverseFont(inverse ? 1 : 0);
    gU8x8.drawString(0, row, rows_[row]);
    gU8x8.setInverseFont(0);
  }

  char rows_[kRows][kCols + 1] = {};
  int inverted_ = -1;
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
