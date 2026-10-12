// Text display the menu draws on: 8 rows of 16 characters. The OLED driver and the
// serial stub both implement it.

#pragma once

constexpr int kDisplayRows = 8;
constexpr int kDisplayCols = 16;  // 8 px font across 128 px

class Display {
 public:
  virtual ~Display() {}
  virtual void clear() = 0;
  virtual void text(int row, const char* line) = 0;  // row 0..7
  virtual void invertRow(int row) = 0;
  virtual void flush() = 0;
};
