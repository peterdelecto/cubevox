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

// A dissolve between two screens shows kDissolveSteps mixed frames. Step s lights
// s / (kDissolveSteps + 1) of the new screen's pixels, in the Bayer 8 x 8 pattern.
constexpr int kDissolveSteps = 2;

// Bayer 8 x 8 ordered dither. A pixel lights at `level` (0..64) when its threshold is below the
// level, so level 32 lights every other pixel.
bool bayerLit(int x, int y, int level);

class Canvas {
 public:
  // All pages start dirty so the first flush paints the whole panel.
  Canvas() { clear(); }

  void clear();
  void fillRect(int x, int y, int w, int h, bool on);
  void invertRect(int x, int y, int w, int h);

  // Sets or clears one pixel. Does nothing outside the canvas.
  void setPixel(int x, int y, bool on);

  // Draws set pixels only (blank glyph pixels leave the canvas alone). The 5x7 font keeps its fixed
  // cell, so x is the left edge of the first cell. The 9X12 and 16X24 fonts are spaced by ink, so x
  // is the left edge of the first glyph's ink and neighbouring inks sit the font's gap apart. A
  // blank glyph advances the pen by the font's space advance. Returns the pen x after the last
  // glyph, trailing gap included, so a second call continues the line.
  int drawText(int x, int y, const FontDesc& font, const char* text);

  // Width of the line drawText draws, without the trailing gap. For a font spaced by ink that is
  // the ink width, so centring on it centres the ink. 0 for "".
  static int textWidth(const FontDesc& font, const char* text);

  // Tabular text for numbers that change. Every character takes one fixed cell of width plus
  // spacing, and a glyph's ink is centred in the cell's width, so a digit swaps for another
  // without moving its neighbours. x is the left edge of the first cell. Returns the pen x after
  // the last cell, spacing included.
  int drawDigits(int x, int y, const FontDesc& font, const char* text);

  // Draws like drawDigits but keeps only the canvas rows from clipTop up to clipBottom. A digit
  // that rolls through a band stays inside it.
  int drawDigitsClipped(int x, int y, const FontDesc& font, const char* text, int clipTop, int clipBottom);

  // Width of the line drawDigits draws: the cell pitch times the count, minus the trailing
  // spacing. It depends on the length only, never on which digits. 0 for "".
  static int digitsWidth(const FontDesc& font, const char* text);

  // Copies `src` and marks dirty only the pages whose bytes changed.
  void copyFrom(const Canvas& src);

  // Sets this canvas to `from` dissolved into `to`. Step 0 is `from`, step kDissolveSteps + 1 is
  // `to`, and the steps between mix the two. Each byte depends on the same byte of the inputs only,
  // so `from` or `to` may be this canvas. Marks dirty only the pages that changed.
  void dissolve(const Canvas& from, const Canvas& to, int step);

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
  void drawGlyph(int x, int y, const FontDesc& font, const uint8_t* glyph, int clipTop = 0,
                 int clipBottom = kCanvasHeight);
  void put(int page, int x, uint8_t value);

  std::array<std::array<uint8_t, kCanvasWidth>, kCanvasPages> pages_;
  uint8_t dirty_ = 0;
};
