// Unit glyph proof. Draws the readout unit strings and four readout screens through the firmware's
// own Canvas and fonts, with the lowercase units the box will show once unitText stops upper-casing
// them, and writes 1x PGMs plus manifests. unit_glyph_sheet.py composes them, with the s, t and d
// glyph rows read from the font library, into one PNG. Exploration only; the GUI spec decides the
// real screens.
//
//   clang++ -std=c++17 -O1 -I . -I firmware/cubevox/src -I tools/font-editor/fonts \
//     tools/oled-proof/unit_glyph_proof.cpp firmware/cubevox/src/core/canvas.cpp \
//     firmware/cubevox/src/core/fonts.cpp firmware/cubevox/src/core/registry.cpp \
//     firmware/cubevox/src/core/chain_params.cpp firmware/cubevox/src/core/unison_rate.cpp \
//     firmware/cubevox/src/core/layout.cpp firmware/cubevox/src/ui/screens.cpp \
//     -o build/unit_glyph_proof && build/unit_glyph_proof build \
//   && python3 -I tools/oled-proof/unit_glyph_sheet.py build tools/oled-proof/unit-glyph-proof.png

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/fonts.h"
#include "core/layout.h"
#include "core/registry.h"
#include "cubevox_fonts.h"
#include "ui/screens.h"

namespace {

namespace f = cubevox::fonts;

// The same descriptor macro fonts.cpp uses, so a font gets the same ink metrics here as on the box.
#define CUBEVOX_DESCRIBE(ns, gap)                                                                  \
  FontDesc {                                                                                       \
    f::ns::kWidth, f::ns::kHeight, f::ns::kSpacing, static_cast<uint8_t>(f::ns::kBytesPerGlyph),    \
        &f::ns::kCodes[0], f::ns::kGlyphCount, &f::ns::kGlyphs[0][0], &f::ns::kInkLeft[0],          \
        &f::ns::kInkWidth[0], f::ns::kSpaceAdvance, gap                                             \
  }

constexpr FontDesc k7x12 = CUBEVOX_DESCRIBE(Myrtle7X12, 2);

// The parameter name sits with its top at row 16 and the title bar covers rows 0 to 13.
constexpr int kParamY = 16;
constexpr int kBarHeight = 14;

bool litAt(const Canvas& c, int x, int y) { return (c.page(y / 8)[x] >> (y % 8)) & 1; }

bool writePgm(const std::string& path, int w, int h, const std::vector<uint8_t>& px) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    std::perror(path.c_str());
    return false;
  }
  std::fprintf(file, "P5\n%d %d\n255\n", w, h);
  std::fwrite(px.data(), 1, px.size(), file);
  std::fclose(file);
  return true;
}

// A specimen line as wide as its text. Each 128 px strip is a Canvas drawn with the pen moved left,
// so the glyphs come out of Canvas::drawText exactly as they would on the box.
bool writeLine(const std::string& path, const FontDesc& font, const char* text) {
  const int band = font.height + 2 * kScreenPad;
  const int width = Canvas::textWidth(font, text) + 2 * kScreenPad;
  const int strips = (width + kCanvasWidth - 1) / kCanvasWidth;
  std::vector<uint8_t> px(static_cast<size_t>(width) * band, 0);
  for (int k = 0; k < strips; ++k) {
    Canvas c;
    c.drawText(kScreenPad - k * kCanvasWidth, kScreenPad, font, text);
    for (int y = 0; y < band; ++y) {
      for (int x = 0; x < kCanvasWidth && k * kCanvasWidth + x < width; ++x) {
        if (litAt(c, x, y)) px[static_cast<size_t>(y) * width + k * kCanvasWidth + x] = 255;
      }
    }
  }
  return writePgm(path, width, band, px);
}

struct Shot {
  std::string title;
  std::string param;
  std::string digits;
  std::string unit;
  Canvas canvas;
};

int slotOf(Card card) {
  const Layout layout = defaultLayout();
  for (int s = 0; s < kSlotCount; ++s) {
    if (layout.card[static_cast<size_t>(s)] == card) return s;
  }
  return -1;
}

std::string upper(const char* in) {
  char buf[48];
  upperText(in, buf, sizeof(buf));
  return buf;
}

void drawCentred(Canvas& c, int y, const FontDesc& font, const char* text) {
  c.drawText((kCanvasWidth - Canvas::textWidth(font, text)) / 2, y, font, text);
}

// Draws the screen the way renderReadout and drawNumber will once the unit keeps its case. The title
// is drawn lit and inverted over the bar. The digits take fixed cells, then the unit follows after a
// space, and the field is centred as one.
void drawShot(Shot& s) {
  Canvas& c = s.canvas;
  c.drawText((kCanvasWidth - Canvas::textWidth(k7x12, s.title.c_str())) / 2, kScreenPad, k7x12, s.title.c_str());
  c.invertRect(0, 0, kCanvasWidth, kBarHeight);
  drawCentred(c, kParamY, font9x12(), s.param.c_str());

  const FontDesc& big = font16x24();
  const std::string unit = " " + s.unit;
  const int unitWidth = big.spacing + Canvas::textWidth(big, unit.c_str());
  const int x = (kCanvasWidth - (Canvas::digitsWidth(big, s.digits.c_str()) + unitWidth)) / 2;
  const int pen = c.drawDigits(x, kReadoutValueY, big, s.digits.c_str());
  c.drawText(pen, kReadoutValueY, big, unit.c_str());
}

// The screen's title, parameter name and unit come from the registry, so they are what the box holds.
Shot makeShot(Card card, int knob, const char* digits) {
  const CardDesc& d = cardDesc(card);
  const KnobDesc* k = knobDesc(card, knob);
  Shot s;
  s.title = std::to_string(slotOf(card) + 1) + " " + upper(d.name);
  s.param = upper(k->name);
  s.digits = digits;
  s.unit = k->unit;
  drawShot(s);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "build";
  bool ok = true;

  // The unit strings. Each is drawn in the three fonts that carry them, with drawText's ink spacing.
  const char* const strings[] = {"-12 st", "250 ms", "-40 dB", "50 %", "C / Am", "Bb / Gm"};
  struct Face {
    const char* tag;
    const FontDesc* font;
  };
  const Face faces[] = {{"16", &font16x24()}, {"9", &font9x12()}, {"7", &k7x12}};
  FILE* list = std::fopen((dir + "/unit-lines.txt").c_str(), "w");
  if (!list) {
    std::perror("unit-lines.txt");
    return 1;
  }
  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i) {
    std::fprintf(list, "%zu|%s", i, strings[i]);
    for (const Face& face : faces) {
      ok = writeLine(dir + "/unit-line-" + face.tag + "-" + std::to_string(i) + ".pgm", *face.font, strings[i]) && ok;
      std::fprintf(list, "|%s %d", face.tag, Canvas::textWidth(*face.font, strings[i]));
    }
    std::fprintf(list, "\n");
  }
  std::fclose(list);

  // The four readout screens.
  std::vector<Shot> screens;
  screens.push_back(makeShot(Card::Octave, 0, "-12"));
  screens.push_back(makeShot(Card::InputGate, 0, "-40"));
  screens.push_back(makeShot(Card::Slapback, 0, "250"));
  screens.push_back(makeShot(Card::ReverbSpring, 1, "50"));

  FILE* manifest = std::fopen((dir + "/unit-screens.txt").c_str(), "w");
  if (!manifest) {
    std::perror("unit-screens.txt");
    return 1;
  }
  int n = 0;
  for (const Shot& s : screens) {
    std::vector<uint8_t> px(static_cast<size_t>(kCanvasWidth) * kCanvasHeight);
    int left = kCanvasWidth;
    int right = -1;
    for (int y = kReadoutValueY; y < kCanvasHeight; ++y) {
      for (int x = 0; x < kCanvasWidth; ++x) {
        if (litAt(s.canvas, x, y)) {
          if (x < left) left = x;
          if (x > right) right = x;
        }
      }
    }
    for (int y = 0; y < kCanvasHeight; ++y) {
      for (int x = 0; x < kCanvasWidth; ++x) px[static_cast<size_t>(y) * kCanvasWidth + x] = litAt(s.canvas, x, y) ? 255 : 0;
    }
    ok = writePgm(dir + "/unit-screen-" + std::to_string(n) + ".pgm", kCanvasWidth, kCanvasHeight, px) && ok;
    std::fprintf(manifest, "%d|%s|%s|%s %s|%d|%d\n", n, s.title.c_str(), s.param.c_str(), s.digits.c_str(),
                 s.unit.c_str(), left, kCanvasWidth - 1 - right);
    std::printf("screen %d  %-16s %-10s %s %s  value ink %d px left, %d px right\n", n, s.title.c_str(),
                s.param.c_str(), s.digits.c_str(), s.unit.c_str(), left, kCanvasWidth - 1 - right);
    ++n;
  }
  std::fclose(manifest);
  return ok ? 0 : 1;
}
