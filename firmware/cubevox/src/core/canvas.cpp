#include "core/canvas.h"

#include <string.h>

namespace {

// Page of row y, rounding toward negative infinity so rows above the canvas land on pages
// below zero.
int pageOf(int y) { return y >= 0 ? y / 8 : -((-y + 7) / 8); }

int cellPitch(const FontDesc& font) { return font.width + font.spacing; }

bool spacedByInk(const FontDesc& font) { return font.inkGap > 0 && font.inkLeft && font.inkWidth; }

// Where one character sits on the pen. The glyph cell starts `shift` columns left of the pen, so
// the ink starts at the pen. The pen then moves `advance` columns, and the last `trail` of them are
// blank, which textWidth leaves out. A null glyph draws nothing but still moves the pen.
struct Step {
  const uint8_t* glyph;
  int shift;
  int advance;
  int trail;
};

Step stepFor(const FontDesc& font, char c) {
  const int i = glyphIndex(font, c);
  const uint8_t* glyph = i < 0 ? nullptr : glyphAt(font, i);
  if (!spacedByInk(font)) return {glyph, 0, cellPitch(font), font.spacing};
  const int ink = i < 0 ? 0 : font.inkWidth[i];
  if (ink == 0) return {nullptr, 0, font.spaceAdvance, 0};
  return {glyph, font.inkLeft[i], ink + font.inkGap, font.inkGap};
}

// Offset from a cell's left edge at which to draw glyph i so its ink sits mid-cell. An odd slack
// puts the extra pixel on the right.
int centreShift(const FontDesc& font, int i) {
  if (!font.inkLeft || !font.inkWidth || font.inkWidth[i] == 0) return 0;
  return (font.width - font.inkWidth[i]) / 2 - font.inkLeft[i];
}

}  // namespace

void Canvas::clear() {
  for (auto& p : pages_) p.fill(0);
  dirty_ = 0xFF;
}

void Canvas::apply(int x, int y, int w, int h, Op op) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > kCanvasWidth) w = kCanvasWidth - x;
  if (y + h > kCanvasHeight) h = kCanvasHeight - y;
  if (w <= 0 || h <= 0) return;

  for (int p = y / 8; p <= (y + h - 1) / 8; ++p) {
    const int top = y > p * 8 ? y - p * 8 : 0;
    const int bottom = y + h - 1 < p * 8 + 7 ? y + h - 1 - p * 8 : 7;
    const uint8_t mask = static_cast<uint8_t>((0xFFu << top) & (0xFFu >> (7 - bottom)));
    for (int c = x; c < x + w; ++c) {
      const uint8_t before = pages_[p][c];
      uint8_t after = before;
      if (op == Op::Set) after |= mask;
      else if (op == Op::Clear) after &= static_cast<uint8_t>(~mask);
      else after ^= mask;
      if (after != before) {
        pages_[p][c] = after;
        dirty_ |= static_cast<uint8_t>(1u << p);
      }
    }
  }
}

void Canvas::clearDirty(int page) {
  if (page < 0 || page >= kCanvasPages) return;
  dirty_ &= static_cast<uint8_t>(~(1u << page));
}

int Canvas::firstDirtyPage() const {
  for (int p = 0; p < kCanvasPages; ++p) {
    if (dirty_ & (1u << p)) return p;
  }
  return -1;
}

const uint8_t* Canvas::page(int p) const { return p >= 0 && p < kCanvasPages ? pages_[p].data() : nullptr; }

void Canvas::fillRect(int x, int y, int w, int h, bool on) { apply(x, y, w, h, on ? Op::Set : Op::Clear); }

void Canvas::invertRect(int x, int y, int w, int h) { apply(x, y, w, h, Op::Toggle); }

// ORs an 8-pixel column byte whose top pixel sits at row y into the one or two pages it spans.
void Canvas::orColumn(int x, int y, uint8_t bits) {
  const int p = pageOf(y);
  const int shift = y - p * 8;
  const uint8_t low = static_cast<uint8_t>(bits << shift);
  const uint8_t high = static_cast<uint8_t>(bits >> (8 - shift));
  const struct { int page; uint8_t bits; } parts[2] = {{p, low}, {p + 1, shift ? high : uint8_t{0}}};
  for (const auto& part : parts) {
    if (part.page < 0 || part.page >= kCanvasPages || (pages_[part.page][x] | part.bits) == pages_[part.page][x]) continue;
    pages_[part.page][x] |= part.bits;
    dirty_ |= static_cast<uint8_t>(1u << part.page);
  }
}

void Canvas::drawGlyph(int x, int y, const FontDesc& font, const uint8_t* glyph) {
  const int bands = (font.height + 7) / 8;
  for (int col = 0; col < font.width; ++col) {
    if (x + col < 0 || x + col >= kCanvasWidth) continue;
    for (int band = 0; band < bands; ++band) {
      const int rows = font.height - band * 8;
      const uint8_t keep = rows >= 8 ? 0xFF : static_cast<uint8_t>((1u << rows) - 1);
      const uint8_t bits = glyph[band * font.width + col] & keep;
      if (bits) orColumn(x + col, y + band * 8, bits);
    }
  }
}

int Canvas::drawText(int x, int y, const FontDesc& font, const char* text) {
  for (; *text; ++text) {
    const Step step = stepFor(font, *text);
    if (step.glyph) drawGlyph(x - step.shift, y, font, step.glyph);
    x += step.advance;
  }
  return x;
}

int Canvas::textWidth(const FontDesc& font, const char* text) {
  int width = 0;
  int trail = 0;
  for (; *text; ++text) {
    const Step step = stepFor(font, *text);
    width += step.advance;
    trail = step.trail;
  }
  return width - trail;
}

int Canvas::drawDigits(int x, int y, const FontDesc& font, const char* text) {
  for (; *text; ++text) {
    const int i = glyphIndex(font, *text);
    if (i >= 0) drawGlyph(x + centreShift(font, i), y, font, glyphAt(font, i));
    x += cellPitch(font);
  }
  return x;
}

int Canvas::digitsWidth(const FontDesc& font, const char* text) {
  const int n = static_cast<int>(strlen(text));
  return n == 0 ? 0 : n * cellPitch(font) - font.spacing;
}
