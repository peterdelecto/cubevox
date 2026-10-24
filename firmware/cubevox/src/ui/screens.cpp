#include "ui/screens.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/fonts.h"

namespace {

// Raises the level before it becomes a height, so a quiet voice still shows.
constexpr float kRibbonCurve = 0.8f;

// Bayer level of the rim above and below the ribbon, which lights half of its pixels.
constexpr int kRibbonRimLevel = 32;

// Values past this never occur. The limit keeps the cast to int defined.
constexpr float kShownLimit = 1.0e6f;

// Longest printed number, sign and digits, with its NUL.
constexpr int kNumberChars = 12;

int centredX(const FontDesc& font, const char* text) { return (kCanvasWidth - Canvas::textWidth(font, text)) / 2; }

void drawCentred(Canvas& canvas, int y, const FontDesc& font, const char* text) {
  canvas.drawText(centredX(font, text), y, font, text);
}

// Right-aligns `text` in `cells` characters. `out` holds at least cells + 1.
void padLeft(char* out, const char* text, int cells) {
  const int length = static_cast<int>(strlen(text));
  for (int i = 0; i < cells - length; ++i) out[i] = ' ';
  memcpy(out + (cells - length), text, static_cast<size_t>(length) + 1);
}

// The number just below the value and the one just above it, as strings of equal width.
struct RollPair {
  char from[kNumberChars + 1];
  char to[kNumberChars + 1];
  int cells;
};

RollPair rollPair(const KnobDesc& knob, int base) {
  char low[kNumberChars];
  char high[kNumberChars];
  formatShown(knob, base, low, sizeof(low));
  formatShown(knob, base + 1, high, sizeof(high));
  const int lowLength = static_cast<int>(strlen(low));
  const int highLength = static_cast<int>(strlen(high));
  RollPair pair;
  pair.cells = lowLength > highLength ? lowLength : highLength;
  padLeft(pair.from, low, pair.cells);
  padLeft(pair.to, high, pair.cells);
  return pair;
}

// Draws the digits of the value, each in a fixed cell. A digit that differs between the two numbers
// rolls up out of its cell and the next one rolls in from below, both clipped to the value's band.
int drawRolled(Canvas& canvas, int x, const RollPair& pair, int rolled) {
  const FontDesc& big = font16x24();
  const int top = kReadoutValueY;
  const int bottom = top + big.height;
  for (int i = 0; i < pair.cells; ++i) {
    const char from[2] = {pair.from[i], '\0'};
    const char to[2] = {pair.to[i], '\0'};
    if (pair.from[i] == pair.to[i]) {
      x = canvas.drawDigits(x, top, big, from);
    } else {
      canvas.drawDigitsClipped(x, top - rolled, big, from, top, bottom);
      x = canvas.drawDigitsClipped(x, bottom - rolled, big, to, top, bottom);
    }
  }
  return x;
}

void drawNumber(Canvas& canvas, const ReadoutView& view) {
  const FontDesc& big = font16x24();
  const KnobDesc& knob = *view.knob;
  float shown = view.shown;
  if (!(shown > -kShownLimit)) shown = -kShownLimit;  // also catches NaN
  if (shown > kShownLimit) shown = kShownLimit;
  const float base = floorf(shown);
  const RollPair pair = rollPair(knob, static_cast<int>(base));

  char unit[16] = "";
  if (knob.unit[0] != '\0') snprintf(unit, sizeof(unit), " %s", knob.unit);
  const int unitWidth = unit[0] != '\0' ? big.spacing + Canvas::textWidth(big, unit) : 0;
  const int x = (kCanvasWidth - (Canvas::digitsWidth(big, pair.from) + unitWidth)) / 2;
  const int rolled = static_cast<int>(lroundf((shown - base) * static_cast<float>(big.height)));
  const int pen = drawRolled(canvas, x, pair, rolled);
  canvas.drawText(pen, kReadoutValueY, big, unit);
}

}  // namespace

int ribbonHalfHeight(float level) {
  if (!(level > 0.0f)) return 0;
  const float shaped = powf(level > 1.0f ? 1.0f : level, kRibbonCurve);
  return static_cast<int>(lroundf(shaped * static_cast<float>(kRibbonHalfMax)));
}

void Ribbon::push(float level) {
  half_[static_cast<size_t>(head_)] = static_cast<uint8_t>(ribbonHalfHeight(level));
  head_ = (head_ + 1) % kRibbonLevels;
}

int Ribbon::half(int age) const {
  if (age < 0 || age >= kRibbonLevels) return 0;
  return half_[static_cast<size_t>((head_ - 1 - age + 2 * kRibbonLevels) % kRibbonLevels)];
}

void renderVoice(Canvas& canvas, const Ribbon& ribbon, Walk walk) {
  canvas.clear();
  const int centre = kRibbonHorizonY + walk.y;
  for (int x = 0; x < kCanvasWidth; ++x) {
    const int half = ribbon.half((kCanvasWidth - 1 - x) / kRibbonColumnsPerLevel);
    const int column = x + walk.x;
    if (half <= 0) {
      canvas.setPixel(column, centre, true);
      continue;
    }
    canvas.fillRect(column, centre - half, 1, 2 * half + 1, true);
    if (bayerLit(x, kRibbonHorizonY - half - 1, kRibbonRimLevel)) canvas.setPixel(column, centre - half - 1, true);
    if (bayerLit(x, kRibbonHorizonY + half + 1, kRibbonRimLevel)) canvas.setPixel(column, centre + half + 1, true);
  }
}

void renderReadout(Canvas& canvas, const ReadoutView& view) {
  canvas.clear();
  char head[32];
  snprintf(head, sizeof(head), "%d %s", view.slot + 1, view.card);
  canvas.drawText(kScreenPad, kScreenPad, font5x7(), head);
  if (!view.knob) return;
  drawCentred(canvas, kReadoutNameY, font9x12(), view.knob->name);
  if (view.knob->labels) {
    drawCentred(canvas, kReadoutValueY, font16x24(), knobLabel(*view.knob, view.target));
  } else {
    drawNumber(canvas, view);
  }
}

const char* topSwitchWord(TopSwitch role) { return role == TopSwitch::Bypass ? "BYPASS" : "MUTED"; }

void renderMuted(Canvas& canvas, const char* word) {
  canvas.clear();
  drawCentred(canvas, (kCanvasHeight - font16x24().height) / 2, font16x24(), word);
}
