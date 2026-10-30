// OLED GUI proof: draws candidate screens with the firmware's own Canvas and fonts and writes
// one sheet, 2 columns of screens at 3x, as a PGM. Exploration only; the GUI spec decides the real screens.
//
//   clang++ -std=c++17 -O1 -I . -I firmware/cubevox/src -I tools/font-editor/fonts \
//     tools/oled-proof/oled_proof.cpp firmware/cubevox/src/core/canvas.cpp \
//     firmware/cubevox/src/core/fonts.cpp firmware/cubevox/src/ui/screens.cpp \
//     firmware/cubevox/src/core/registry.cpp firmware/cubevox/src/core/chain_params.cpp \
//     firmware/cubevox/src/core/unison_rate.cpp -o build/oled_proof && build/oled_proof build/oled-proof.pgm

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/canvas.h"
#include "core/fonts.h"
#include "core/registry.h"
#include "ui/screens.h"

namespace {

constexpr int kSlots = 8;
constexpr int kCell = kCanvasWidth / kSlots;  // 16 px per slot column

struct Slot {
  const char* code;  // two letters for the home map
  const char* name;
  float knobA;
  float knobB;
  bool on;
};

const Slot kChain[kSlots] = {
    {"IG", "INPUT GATE", 0.35f, 0.50f, true}, {"AT", "AUTOTUNE", 0.40f, 0.70f, true},
    {"OC", "OCTAVE", 0.25f, 0.50f, false},    {"UN", "UNISON", 0.60f, 0.30f, true},
    {"SL", "SLAPBACK", 0.45f, 0.20f, true},   {"DS", "DISTORTION", 0.15f, 0.55f, false},
    {"GT", "GATE", 0.30f, 0.40f, true},       {"RV", "SPRING REVERB", 0.50f, 0.65f, true},
};

// Text keeps 1 px clear of the screen edge and of every other element on all sides.
constexpr int kPad = 1;

void centered(Canvas& c, int y, const FontDesc& f, const char* s) {
  c.drawText((kCanvasWidth - Canvas::textWidth(f, s)) / 2, y, f, s);
}

void rightAligned(Canvas& c, int y, const FontDesc& f, const char* s) {
  c.drawText(kCanvasWidth - kPad - 1 - Canvas::textWidth(f, s), y, f, s);
}

void frameRect(Canvas& c, int x, int y, int w, int h) {
  c.fillRect(x, y, w, 1, true);
  c.fillRect(x, y + h - 1, w, 1, true);
  c.fillRect(x, y, 1, h, true);
  c.fillRect(x + w - 1, y, 1, h, true);
}

// Eight columns, one per slot, left to right as on the panel. The two bars are the slot's two
// knobs; a filled foot marks a slot that is on. `focus` highlights one column (-1 for none).
void slotMap(Canvas& c, int top, int barH, int focus) {
  for (int s = 0; s < kSlots; ++s) {
    const int x = s * kCell;
    c.drawText(x + 2, top, font5x7(), kChain[s].code);
    const int barTop = top + 10;
    const float knobs[2] = {kChain[s].knobA, kChain[s].knobB};
    for (int k = 0; k < 2; ++k) {
      const int bx = x + 3 + k * 6;
      frameRect(c, bx, barTop, 4, barH);
      const int fill = static_cast<int>(knobs[k] * (barH - 2) + 0.5f);
      c.fillRect(bx + 1, barTop + barH - 1 - fill, 2, fill, true);
    }
    const int footY = barTop + barH + 2;
    if (kChain[s].on) {
      c.fillRect(x + 3, footY, 10, 3, true);
    } else {
      frameRect(c, x + 3, footY, 10, 3);
    }
    if (s == focus) c.invertRect(x, top - 1, kCell, footY + 4 - (top - 1));
  }
}

void home(Canvas& c) {
  c.drawText(kPad, kPad, font5x7(), "CUBEVOX");
  rightAligned(c, kPad, font5x7(), "MIC  LINE");
  slotMap(c, 12, 34, -1);
}

void muted(Canvas& c) {
  home(c);
  c.fillRect(10, 20, 108, 30, false);
  c.fillRect(12, 22, 104, 26, true);
  Canvas label;
  label.clear();
  const int w = Canvas::textWidth(font16x24(), "MUTED");
  label.drawText(0, 0, font16x24(), "MUTED");
  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < w; ++x) {
      if (label.page(y / 8)[x] >> (y % 8) & 1) c.setPixel((kCanvasWidth - w) / 2 + x, 23 + y, false);
    }
  }
}

// The readout's title bar: a lit bar across the whole width, rows 0 to 13, with the slot number and
// card name in black 7X12, centred, top at row 1. The text is drawn lit and the bar inverted over it.
void titleBar(Canvas& c, const char* head) {
  c.drawText((kCanvasWidth - Canvas::textWidth(font7x12(), head)) / 2, kPad, font7x12(), head);
  c.invertRect(0, 0, kCanvasWidth, 12 + 2 * kPad);
}

// The knob readout: the title bar, the parameter name with its top at row 16, the value at row 34.
void knobScreen(Canvas& c, int slot, const char* label, const char* value) {
  char head[24];
  std::snprintf(head, sizeof(head), "%d %s", slot + 1, kChain[slot].name);
  titleBar(c, head);
  centered(c, 16, font9x12(), label);
  centered(c, 34, font16x24(), value);
}

void keyScreen(Canvas& c) {
  titleBar(c, "2 AUTOTUNE");
  centered(c, 16, font9x12(), "KEY");
  centered(c, 34, font16x24(), "F / DM");
}

// The runtime toggle readout, drawn by the firmware's renderer: slot 6 flipped on.
void toggleScreen(Canvas& c) {
  ReadoutView view;
  view.slot = 5;
  view.card = cardDesc(Card::Distortion).name;
  view.toggle = true;
  view.on = true;
  renderReadout(c, view);
}

// The runtime voice screen, drawn by the firmware's renderer. The input bar stands 25 rows tall in
// columns 1 and 2, and the output ribbon runs from column 4 on a swell that fades.
void voiceScreen(Canvas& c) {
  Ribbon ribbon;
  for (int i = 0; i < kRibbonLevels; ++i) {
    const float age = static_cast<float>(kRibbonLevels - 1 - i) / static_cast<float>(kRibbonLevels);
    const float swell = 0.5f + 0.5f * std::sin(static_cast<float>(i) * 0.45f);
    ribbon.push((1.0f - 0.6f * age) * swell);
  }
  renderVoice(c, ribbon, 25, Walk{0, 0});
}

void menu(Canvas& c) {
  struct Row {
    const char* label;
    const char* value;
  };
  const Row rows[] = {{"1/4 INPUT", "MIC"},     {"OUTPUT", "LINE"},     {"EQ", ">"},
                      {"CHAIN", ">"},           {"UPDATE FIRMWARE", ">"}, {"ABOUT", "V0.3"},
                      {"EXIT", ""}};
  c.drawText(kPad, kPad, font5x7(), "MENU");
  c.fillRect(0, 9, kCanvasWidth, 1, true);

  // Row pitch 9: 7 px of text with 1 px above and below, so the inverted row pads the text too.
  for (int i = 0; i < 6; ++i) {
    const int y = 11 + i * 9;
    c.drawText(2, y, font5x7(), rows[i].label);
    c.drawText(kCanvasWidth - 3 - Canvas::textWidth(font5x7(), rows[i].value), y, font5x7(), rows[i].value);
    if (i == 0) c.invertRect(0, y - kPad, kCanvasWidth, 7 + 2 * kPad);
  }
}

using Draw = void (*)(Canvas&);

// Proof number in the margin, 5x7 at 2x.
void drawNumber(std::vector<unsigned char>& img, int W, int ox, int oy, int n) {
  char s[4];
  std::snprintf(s, sizeof(s), "%d", n);
  Canvas c;
  c.clear();
  const int w = c.drawText(0, 0, font5x7(), s);
  for (int y = 0; y < 14; ++y) {
    for (int x = 0; x < 2 * w; ++x) {
      if (c.page(0)[x / 2] >> (y / 2) & 1) img[static_cast<size_t>(oy + y) * W + ox + x] = 235;
    }
  }
}

void writeSheet(const char* path, const std::vector<Draw>& screens) {
  constexpr int kScale = 3;
  constexpr int kGap = 12;
  constexpr int kLabel = 22;  // band above each screen for its proof number
  constexpr int kCols = 2;
  const int rows = (static_cast<int>(screens.size()) + kCols - 1) / kCols;
  const int sw = kCanvasWidth * kScale;
  const int sh = kCanvasHeight * kScale;
  const int W = kCols * sw + (kCols + 1) * kGap;
  const int H = rows * (sh + kLabel) + (rows + 1) * kGap;
  std::vector<unsigned char> img(static_cast<size_t>(W) * H, 70);
  for (size_t i = 0; i < screens.size(); ++i) {
    Canvas c;
    c.clear();
    screens[i](c);
    const int ox = kGap + static_cast<int>(i % kCols) * (sw + kGap);
    const int oy = kGap + kLabel + static_cast<int>(i / kCols) * (sh + kLabel + kGap);
    drawNumber(img, W, ox, oy - kLabel + 2, static_cast<int>(i) + 1);
    for (int y = 0; y < sh; ++y) {
      for (int x = 0; x < sw; ++x) {
        const int px = x / kScale;
        const int py = y / kScale;
        const bool on = c.page(py / 8)[px] >> (py % 8) & 1;
        img[static_cast<size_t>(oy + y) * W + ox + x] = on ? 235 : 8;
      }
    }
  }
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    std::perror(path);
    return;
  }
  std::fprintf(f, "P5\n%d %d\n255\n", W, H);
  std::fwrite(img.data(), 1, img.size(), f);
  std::fclose(f);
}

void knobMix(Canvas& c) { knobScreen(c, 7, "MIX", "50 %"); }
void knobSemis(Canvas& c) { knobScreen(c, 2, "SEMITONES", "-12 ST"); }
void knobTime(Canvas& c) { knobScreen(c, 4, "TIME", "120 MS"); }

}  // namespace

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "oled-proof.pgm";
  writeSheet(out, {home, knobMix, knobSemis, keyScreen, knobTime, toggleScreen, muted, menu, voiceScreen});
  std::printf("wrote %s\n", out);
  return 0;
}
