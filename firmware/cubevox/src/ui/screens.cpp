#include "ui/screens.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>

#include "core/fonts.h"
#include "core/load_window.h"

namespace {

// Raises the level before it becomes a height, so a quiet voice still shows.
constexpr float kRibbonCurve = 0.8f;

// Bayer level of the rim above and below the ribbon, which lights half of its pixels.
constexpr int kRibbonRimLevel = 32;

// Values past this never occur. The limit keeps the cast to int defined.
constexpr float kShownLimit = 1.0e6f;

int centredX(const FontDesc& font, const char* text) { return (kCanvasWidth - Canvas::textWidth(font, text)) / 2; }

void drawCentred(Canvas& canvas, int y, const FontDesc& font, const char* text) {
  canvas.drawText(centredX(font, text), y, font, text);
}

// The number just below the value and the one just above it, each in all the cells of the knob's
// field, so the two strings and every other value of the knob have the same length.
struct RollPair {
  char from[kMaxKnobCells + 1];
  char to[kMaxKnobCells + 1];
  int cells;
};

RollPair rollPair(const KnobDesc& knob, int base) {
  RollPair pair;
  pair.cells = knobCells(knob);
  formatCells(knob, base, pair.from, sizeof(pair.from));
  formatCells(knob, base + 1, pair.to, sizeof(pair.to));
  return pair;
}

// The widest name of a labelled knob. Its field is that wide for every name.
int widestLabel(const FontDesc& font, const KnobDesc& knob) {
  int widest = 0;
  for (int i = 0; i < knob.positions; ++i) {
    const int width = Canvas::textWidth(font, knobLabel(knob, i));
    widest = width > widest ? width : widest;
  }
  return widest;
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

// The field holds the knob's cells, a space and the unit, and its width never changes. It is
// centred once, so the unit stays put and each cell keeps its place while the value changes.
void drawNumber(Canvas& canvas, const ReadoutView& view) {
  const FontDesc& big = font16x24();
  const KnobDesc& knob = *view.knob;
  float shown = view.shown;
  if (!(shown > -kShownLimit)) shown = -kShownLimit;  // also catches NaN
  if (shown > kShownLimit) shown = kShownLimit;
  const float base = floorf(shown);
  const RollPair pair = rollPair(knob, static_cast<int>(base));

  char unit[16];
  unitText(knob, unit, sizeof(unit));
  const int unitWidth = unit[0] != '\0' ? big.spacing + Canvas::textWidth(big, unit) : 0;
  const int x = (kCanvasWidth - (Canvas::digitsWidth(big, pair.from) + unitWidth)) / 2;
  const int rolled = static_cast<int>(lroundf((shown - base) * static_cast<float>(big.height)));
  const int pen = drawRolled(canvas, x, pair, rolled);
  canvas.drawText(pen, kReadoutValueY, big, unit);
}

// The slot number, a space and the card name in capitals, black on a lit bar across the full
// width. The text is drawn lit and the bar inverted over it, as the menu does for its cursor row.
void drawTitleBar(Canvas& canvas, const ReadoutView& view) {
  char card[32];
  upperText(view.card, card, sizeof(card));
  char title[48];
  snprintf(title, sizeof(title), "%d %s", view.slot + 1, card);
  drawCentred(canvas, kReadoutTitleY, font7x12(), title);
  canvas.invertRect(0, 0, kCanvasWidth, kReadoutBarHeight);
}

}  // namespace

int upperText(const char* in, char* out, int size) {
  if (size <= 0) return 0;
  int n = 0;
  for (; in[n] != '\0' && n < size - 1; ++n) out[n] = static_cast<char>(toupper(static_cast<unsigned char>(in[n])));
  out[n] = '\0';
  return n;
}

int unitText(const KnobDesc& knob, char* out, int size) {
  if (size <= 0) return 0;
  out[0] = '\0';
  if (knob.unit[0] == '\0') return 0;
  snprintf(out, static_cast<size_t>(size), " %s", knob.unit);
  return upperText(out, out, size);
}

int ribbonHalfHeight(float level) {
  if (!(level > 0.0f)) return 0;
  const float shaped = powf(level > 1.0f ? 1.0f : level, kRibbonCurve);
  return static_cast<int>(lroundf(shaped * static_cast<float>(kRibbonHalfMax)));
}

int meterRows(float level) {
  if (!(level > 0.0f)) return 0;
  return static_cast<int>(lroundf((level > 1.0f ? 1.0f : level) * static_cast<float>(kMeterRows)));
}

void Ribbon::push(float level) {
  half_[static_cast<size_t>(head_)] = static_cast<uint8_t>(ribbonHalfHeight(level));
  head_ = (head_ + 1) % kRibbonLevels;
}

int Ribbon::half(int age) const {
  if (age < 0 || age >= kRibbonLevels) return 0;
  return half_[static_cast<size_t>((head_ - 1 - age + 2 * kRibbonLevels) % kRibbonLevels)];
}

void renderVoice(Canvas& canvas, const Ribbon& ribbon, int rows, Walk walk) {
  canvas.clear();
  if (rows > 0) {
    const int shown = rows < kMeterRows ? rows : kMeterRows;
    canvas.fillRect(kMeterX + walk.x, kMeterTopY + kMeterRows - shown + walk.y, kMeterWidth, shown, true);
  }
  const int centre = kRibbonHorizonY + walk.y;
  for (int x = kRibbonLeftX; x < kCanvasWidth; ++x) {
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
  drawTitleBar(canvas, view);
  if (view.toggle) {
    drawCentred(canvas, kReadoutValueY, font16x24(), view.on ? "ON" : "OFF");
    return;
  }
  if (!view.knob) return;
  char name[32];
  upperText(view.knob->name, name, sizeof(name));
  drawCentred(canvas, kReadoutNameY, font9x12(), name);
  if (view.knob->labels) {
    // The field is as wide as the widest name. The name starts at its left edge.
    const FontDesc& big = font16x24();
    const int x = (kCanvasWidth - widestLabel(big, *view.knob)) / 2;
    canvas.drawText(x, kReadoutValueY, big, knobLabel(*view.knob, view.target));
  } else {
    drawNumber(canvas, view);
  }
}

const char* topSwitchWord(TopSwitch role) { return role == TopSwitch::Bypass ? "BYPASS" : "MUTED"; }

void renderMuted(Canvas& canvas, const char* word) {
  canvas.clear();
  drawCentred(canvas, (kCanvasHeight - font16x24().height) / 2, font16x24(), word);
}

void drawLoad(Canvas& canvas, int percent) {
  const FontDesc& font = font5x7();
  const int shown = percent < 0 ? 0 : (percent > kMaxLoadPercent ? kMaxLoadPercent : percent);
  char text[16];
  snprintf(text, sizeof(text), "%d%%", shown);
  const int width = Canvas::textWidth(font, text);
  const int x = kCanvasWidth - kScreenPad - width;
  const int y = kCanvasHeight - kScreenPad - font.height;
  canvas.fillRect(x, y, width, font.height, false);
  canvas.drawText(x, y, font, text);
}
