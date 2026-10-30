#include "ui/screens.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

// Two numbers a roll sits between, each in the same `cells` characters. The digits are right-aligned
// and a sign cell comes first when either number has a sign, so a digit or sign that only one number
// has is a blank in the other string. A settled value is the pair of one number with itself.
struct RollPair {
  char from[kMaxKnobCells + 1];
  char to[kMaxKnobCells + 1];
  int cells;
};

int leadingBlanks(const char* text) {
  int n = 0;
  while (text[n] == ' ') ++n;
  return n;
}

// formatCells lays a number out in the knob's widest field. This drops the cells neither number
// uses, which are the sign cell when both are unsigned and the leading blank digit cells both leave.
RollPair rollPair(const KnobDesc& knob, int from, int to) {
  RollPair pair = {"", "", 0};
  const int total = knobCells(knob);
  if (total <= 0) return pair;

  char full[2][kMaxKnobCells + 1];
  formatCells(knob, from, full[0], sizeof(full[0]));
  formatCells(knob, to, full[1], sizeof(full[1]));

  const int signCells = knob.min < 0.0f ? 1 : 0;
  const bool keepSign = signCells > 0 && (full[0][0] != ' ' || full[1][0] != ' ');
  const int blankFrom = leadingBlanks(full[0] + signCells);
  const int blankTo = leadingBlanks(full[1] + signCells);
  const int blank = blankFrom < blankTo ? blankFrom : blankTo;
  const int digitCells = total - signCells - blank;
  pair.cells = (keepSign ? 1 : 0) + digitCells;

  char* const out[2] = {pair.from, pair.to};
  for (int n = 0; n < 2; ++n) {
    int at = 0;
    if (keepSign) out[n][at++] = full[n][0];
    memcpy(out[n] + at, full[n] + signCells + blank, static_cast<size_t>(digitCells));
    out[n][pair.cells] = '\0';
  }
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

// The field holds the cells of the number, a space and the unit, and it is centred on the panel.
// A roll holds the wider of its two numbers, so the unit and every cell stay put while numbers of
// one digit count roll, and the row re-centres once when a digit or the sign appears or vanishes.
// A roll of 0 px or of the full height draws one number alone, so the field is that number's own.
void drawNumber(Canvas& canvas, const ReadoutView& view) {
  const FontDesc& big = font16x24();
  const KnobDesc& knob = *view.knob;
  float shown = view.shown;
  if (!(shown > -kShownLimit)) shown = -kShownLimit;  // also catches NaN
  if (shown > kShownLimit) shown = kShownLimit;
  const float base = floorf(shown);
  const int below = static_cast<int>(base);
  const int rolled = static_cast<int>(lroundf((shown - base) * static_cast<float>(big.height)));

  const bool settledBelow = rolled <= 0;
  const bool settledAbove = rolled >= big.height;
  const int from = settledAbove ? below + 1 : below;
  const int to = settledBelow || settledAbove ? from : below + 1;
  const RollPair pair = rollPair(knob, from, to);

  char unit[16];
  unitText(knob, unit, sizeof(unit));
  const int unitWidth = unit[0] != '\0' ? big.spacing + Canvas::textWidth(big, unit) : 0;
  const int x = (kCanvasWidth - (Canvas::digitsWidth(big, pair.from) + unitWidth)) / 2;
  const int pen = drawRolled(canvas, x, pair, settledBelow || settledAbove ? 0 : rolled);
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
  const int length = snprintf(out, static_cast<size_t>(size), " %s", knob.unit);
  return length < size ? length : size - 1;
}

int ribbonHalfHeight(float level) {
  if (!(level > 0.0f)) return 0;
  const float shaped = powf(level > 1.0f ? 1.0f : level, kRibbonCurve);
  return static_cast<int>(lroundf(shaped * static_cast<float>(kRibbonHalfMax)));
}

int meterSegments(float level) {
  const float held = level > 0.0f ? (level < 1.0f ? level : 1.0f) : 0.0f;  // also catches NaN
  return static_cast<int>(lroundf(held * static_cast<float>(kMeterSegments)));
}

void Ribbon::push(float level) {
  half_[static_cast<size_t>(head_)] = static_cast<uint8_t>(ribbonHalfHeight(level));
  head_ = (head_ + 1) % kRibbonLevels;
}

int Ribbon::half(int age) const {
  if (age < 0 || age >= kRibbonLevels) return 0;
  return half_[static_cast<size_t>((head_ - 1 - age + 2 * kRibbonLevels) % kRibbonLevels)];
}

namespace {

// The outline of the meter, 1 px wide, shifted by the walk.
void drawMeterBox(Canvas& canvas, Walk walk) {
  const int left = kMeterBoxLeftX + walk.x;
  const int top = kMeterBoxTopY + walk.y;
  const int width = kMeterBoxRightX - kMeterBoxLeftX + 1;
  const int height = kMeterBoxBottomY - kMeterBoxTopY + 1;
  canvas.fillRect(left, top, width, 1, true);
  canvas.fillRect(left, top + height - 1, width, 1, true);
  canvas.fillRect(left, top, 1, height, true);
  canvas.fillRect(left + width - 1, top, 1, height, true);
}

// The lowest `lit` segments, each kMeterSegmentRows tall with kMeterSegmentGap rows between them.
void drawMeterSegments(Canvas& canvas, int lit, Walk walk) {
  const int bottom = kMeterTopY + kMeterRows;
  for (int i = 0; i < lit; ++i) {
    const int top = bottom - (i + 1) * kMeterSegmentRows - i * kMeterSegmentGap;
    canvas.fillRect(kMeterX + walk.x, top + walk.y, kMeterWidth, kMeterSegmentRows, true);
  }
}

}  // namespace

void renderVoice(Canvas& canvas, const Ribbon& ribbon, int segments, Walk walk) {
  canvas.clear();
  drawMeterBox(canvas, walk);
  drawMeterSegments(canvas, segments < kMeterSegments ? segments : kMeterSegments, walk);
  const int centre = kRibbonHorizonY + walk.y;
  for (int x = kRibbonLeftX; x <= kRibbonRightX; ++x) {
    const int half = ribbon.half((kRibbonRightX - x) / kRibbonColumnsPerLevel);
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
