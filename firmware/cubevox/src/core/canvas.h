// 128 x 64 monochrome frame buffer in SH1106 page layout: 8 pages of 128 bytes, bit 0 is the top
// pixel of a page. The UI draws here and a display backend pushes dirty pages to the panel.
// Everything clips at the edges.

#pragma once

#include <stdint.h>

#include <array>

#include "core/fonts.h"

constexpr int kCanvasWidth = 128;
constexpr int kCanvasHeight = 64;
constexpr int kCanvasPages = kCanvasHeight / 8;

class Canvas {
 public:
  // All pages start dirty so the first flush paints the whole panel.
  Canvas() { clear(); }

  void clear();
  void fillRect(int x, int y, int w, int h, bool on);
  void invertRect(int x, int y, int w, int h);

  // Draws set pixels only (blank glyph pixels leave the canvas alone). Returns the pen x after
  // the last glyph, trailing spacing included, so a second call continues the line.
  int drawText(int x, int y, const FontDesc& font, const char* text);

  // Ink width: advance per glyph times the count, minus the trailing spacing. 0 for "".
  static int textWidth(const FontDesc& font, const char* text);

  // Bit p set means page p changed since clearDirty(p).
  uint8_t dirtyPages() const { return dirty_; }

  // Does nothing for a page outside 0..kCanvasPages - 1.
  void clearDirty(int page);

  // The lowest dirty page, or -1 when none is.
  int firstDirtyPage() const;

  // 128 bytes, left to right; nullptr for a page outside 0..kCanvasPages - 1.
  const uint8_t* page(int p) const;

 private:
  enum class Op : uint8_t { Set, Clear, Toggle };

  void apply(int x, int y, int w, int h, Op op);
  void orColumn(int x, int y, uint8_t bits);
  void drawGlyph(int x, int y, const FontDesc& font, const uint8_t* glyph);

  std::array<std::array<uint8_t, kCanvasWidth>, kCanvasPages> pages_;
  uint8_t dirty_ = 0;
};
