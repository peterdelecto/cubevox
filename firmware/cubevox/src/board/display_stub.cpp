#include "board/display_stub.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

namespace {

class StubDisplay : public Display {
 public:
  void clear() override { memset(rows_, 0, sizeof(rows_)); inverted_ = -1; }
  void text(int r, const char* line) override { snprintf(rows_[r], sizeof(rows_[r]), "%.*s", kDisplayCols, line); }
  void invertRow(int r) override { inverted_ = r; }
  void flush() override {
    Serial.println("[oled] ----------------------");
    for (int r = 0; r < kDisplayRows; ++r) {
      Serial.print(r == inverted_ ? "[oled] *" : "[oled]  ");
      Serial.println(rows_[r]);
    }
  }

 private:
  char rows_[kDisplayRows][kDisplayCols + 1] = {};
  int inverted_ = -1;
};

}  // namespace

Display& stubDisplay() {
  static StubDisplay display;
  return display;
}
