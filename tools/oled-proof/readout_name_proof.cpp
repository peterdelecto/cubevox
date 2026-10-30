// Readout name-row proof. Draws two sets of eight candidate readout screens and nine specimen lines
// through the firmware's own Canvas, fonts and renderReadout, and writes them as 1x PGMs plus
// manifests. The name set (A0..B2) compares name rows. The title set (C0..C2) inverts the chosen
// name row into a lit bar. readout_name_sheet.py composes each set into one PNG; the name sheet
// also carries the Myrtle 7X12 glyph grid. Exploration only; the GUI spec decides the real screen.
//
//   clang++ -std=c++17 -O1 -I . -I firmware/cubevox/src -I tools/font-editor/fonts \
//     tools/oled-proof/readout_name_proof.cpp firmware/cubevox/src/core/canvas.cpp \
//     firmware/cubevox/src/core/fonts.cpp firmware/cubevox/src/core/registry.cpp \
//     firmware/cubevox/src/core/chain_params.cpp firmware/cubevox/src/core/unison_rate.cpp \
//     firmware/cubevox/src/core/layout.cpp firmware/cubevox/src/ui/screens.cpp \
//     -o build/readout_name_proof && build/readout_name_proof build \
//   && python3 -I tools/oled-proof/readout_name_sheet.py build tools/oled-proof/readout-name-proof.png name \
//   && python3 -I tools/oled-proof/readout_name_sheet.py build tools/oled-proof/readout-title-proof.png title

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
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

constexpr FontDesc k7x12Gap2 = CUBEVOX_DESCRIBE(Myrtle7X12, 2);
constexpr FontDesc k7x12Gap3 = CUBEVOX_DESCRIBE(Myrtle7X12, 3);

// Candidate rows. The name row keeps its top at the pad, the parameter name sits 3 px below it, and
// the value keeps the firmware's kReadoutValueY.
constexpr int kNameRowY = kScreenPad;
constexpr int kParamY = 16;
constexpr int kOnY = kReadoutValueY;

// A name row clips when its ink passes the last column. It breaks the pad when it passes column 126.
constexpr int kLastInkColumn = kCanvasWidth - 1 - kScreenPad;

struct Candidate {
  std::string id;
  std::string caption;
  Canvas canvas;
  int nameWidth = 0;
  int barWidth = -1;  // -1 when the name row has no bar
  const char* status = "fits";
};

int slotOf(Card card) {
  const Layout layout = defaultLayout();
  for (int s = 0; s < kSlotCount; ++s) {
    if (layout.card[static_cast<size_t>(s)] == card) return s;
  }
  return -1;
}

bool litAt(const Canvas& c, int x, int y) { return (c.page(y / 8)[x] >> (y % 8)) & 1; }

// What renderReadout draws for the knob, with the card line left in place for the A0 reference.
struct Reading {
  ReadoutView view;
  char card[32];
};

Reading readingFor(Card card, int knob, float position) {
  Reading r{};
  const CardDesc& d = cardDesc(card);
  std::snprintf(r.card, sizeof(r.card), "%s", d.name);
  r.view.slot = slotOf(card);
  r.view.card = r.card;
  r.view.knob = knobDesc(card, knob);
  r.view.shown = static_cast<float>(knobShown(*r.view.knob, position));
  r.view.target = knobShown(*r.view.knob, position);
  return r;
}

// Copies the firmware's value band (rows kReadoutValueY and below) onto `to`, so the value is drawn
// by drawNumber itself.
void copyValueBand(const Canvas& from, Canvas& to) {
  for (int y = kReadoutValueY; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (litAt(from, x, y)) to.setPixel(x, y, true);
    }
  }
}

void drawCentred(Canvas& c, int y, const FontDesc& font, const char* text) {
  c.drawText((kCanvasWidth - Canvas::textWidth(font, text)) / 2, y, font, text);
}

// Draws the name row at the pad and returns the ink width Canvas measures for it.
int drawNameRow(Candidate& s, const FontDesc& font, const char* text) {
  s.nameWidth = Canvas::textWidth(font, text);
  const int lastInk = kScreenPad + s.nameWidth - 1;
  if (lastInk >= kCanvasWidth) {
    s.status = "clips";
  } else if (lastInk > kLastInkColumn) {
    s.status = "breaks pad";
  }
  s.canvas.drawText(kScreenPad, kNameRowY, font, text);
  return s.nameWidth;
}

// The parameter name in 9X12 at kParamY and the firmware's value, under whatever name row came first.
void drawParamAndValue(Candidate& s, const Reading& r) {
  char name[32];
  upperText(r.view.knob->name, name, sizeof(name));
  drawCentred(s.canvas, kParamY, font9x12(), name);
  Canvas ref;
  renderReadout(ref, r.view);
  copyValueBand(ref, s.canvas);
}

std::string slotText(int slot, const char* name) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%d %s", slot + 1, name);
  return buf;
}

std::string upper(const char* in) {
  char buf[48];
  upperText(in, buf, sizeof(buf));
  return buf;
}

std::vector<Candidate> makeScreens() {
  std::vector<Candidate> out;
  const Reading mix = readingFor(Card::ReverbSpring, 0, 0.5f);
  const int slot = mix.view.slot;
  const std::string full = upper(mix.card);
  const std::string numbered = slotText(slot, full.c_str());

  {
    Candidate s{"A0", "A0  today, 5x7 name row"};
    renderReadout(s.canvas, mix.view);
    const std::string head = slotText(slot, mix.card);
    s.nameWidth = Canvas::textWidth(font5x7(), head.c_str());
    out.push_back(std::move(s));
  }
  {
    Candidate s{"A1", "A1  9X12 SPRING, no slot"};
    drawNameRow(s, font9x12(), "SPRING");
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"A2", "A2  9X12 SPRING REVERB, no slot"};
    drawNameRow(s, font9x12(), full.c_str());
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"B0", "B0  7X12 gap 2, slot + name"};
    drawNameRow(s, k7x12Gap2, numbered.c_str());
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"B1", "B1  7X12 gap 3, slot + name"};
    drawNameRow(s, k7x12Gap3, numbered.c_str());
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"B2", "B2  7X12 gap 3, name only"};
    drawNameRow(s, k7x12Gap3, full.c_str());
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"B0toggle", "B0 toggle screen  7X12 gap 2"};
    drawNameRow(s, k7x12Gap2, numbered.c_str());
    drawCentred(s.canvas, kOnY, font16x24(), "ON");
    out.push_back(std::move(s));
  }
  {
    const Reading octave = readingFor(Card::Octave, 0, 0.0f);
    const std::string name = slotText(octave.view.slot, upper(octave.card).c_str());
    Candidate s{"B0octave", "B0 octave screen  7X12 gap 2"};
    drawNameRow(s, k7x12Gap2, name.c_str());
    drawParamAndValue(s, octave);
    out.push_back(std::move(s));
  }
  return out;
}

// An inverted name row is the text height plus 1 px above and below.
constexpr int kBarHeight = 14;

enum class Bar { Full, Tag, FullCentred };

// Draws the name lit, then inverts the bar rectangle over it, so the glyphs come out dark on a lit
// bar. Full and Tag put the text at the pad. Tag cuts the bar to the name plus 1 px each side.
void drawBarRow(Candidate& s, const FontDesc& font, const char* text, Bar bar) {
  s.nameWidth = Canvas::textWidth(font, text);
  const int x = bar == Bar::FullCentred ? (kCanvasWidth - s.nameWidth) / 2 : kScreenPad;
  s.barWidth = bar == Bar::Tag ? s.nameWidth + 2 * kScreenPad : kCanvasWidth;
  const int lastInk = x + s.nameWidth - 1;
  if (lastInk >= kCanvasWidth) {
    s.status = "clips";
  } else if (lastInk > kLastInkColumn || x < kScreenPad) {
    s.status = "breaks pad";
  }
  s.canvas.drawText(x, kNameRowY, font, text);
  s.canvas.invertRect(0, 0, s.barWidth, kBarHeight);
}

std::vector<Candidate> makeTitleScreens() {
  std::vector<Candidate> out;
  const Reading mix = readingFor(Card::ReverbSpring, 0, 0.5f);
  const std::string numbered = slotText(mix.view.slot, upper(mix.card).c_str());

  {
    Candidate s{"C0", "C0  full-width bar, name at the pad"};
    drawBarRow(s, k7x12Gap2, numbered.c_str(), Bar::Full);
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"C1", "C1  tag bar, name + 1 px each side"};
    drawBarRow(s, k7x12Gap2, numbered.c_str(), Bar::Tag);
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"C2", "C2  full-width bar, name centred"};
    drawBarRow(s, k7x12Gap2, numbered.c_str(), Bar::FullCentred);
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"B0", "B0  no bar, as rendered before"};
    drawNameRow(s, k7x12Gap2, numbered.c_str());
    drawParamAndValue(s, mix);
    out.push_back(std::move(s));
  }
  {
    const Reading gate = readingFor(Card::InputGate, 0, 0.5f);
    const std::string name = slotText(gate.view.slot, upper(gate.card).c_str());
    Candidate s{"C0input", "C0 input gate screen, THRESHOLD"};
    drawBarRow(s, k7x12Gap2, name.c_str(), Bar::Full);
    drawParamAndValue(s, gate);
    std::printf("input gate shown %d %s\n", knobShown(*gate.view.knob, 0.5f), gate.view.knob->unit);
    out.push_back(std::move(s));
  }
  {
    Candidate s{"C0toggle", "C0 toggle screen, ON"};
    drawBarRow(s, k7x12Gap2, numbered.c_str(), Bar::Full);
    drawCentred(s.canvas, kOnY, font16x24(), "ON");
    out.push_back(std::move(s));
  }
  {
    const Reading octave = readingFor(Card::Octave, 0, 0.0f);
    const std::string name = slotText(octave.view.slot, upper(octave.card).c_str());
    Candidate s{"C0octave", "C0 octave screen, SEMITONES"};
    drawBarRow(s, k7x12Gap2, name.c_str(), Bar::Full);
    drawParamAndValue(s, octave);
    out.push_back(std::move(s));
  }
  {
    const Reading key = readingFor(Card::Autotune, 0, 0.0f);
    const std::string name = slotText(key.view.slot, upper(key.card).c_str());
    Candidate s{"C0autotune", "C0 autotune screen, KEY"};
    drawBarRow(s, k7x12Gap2, name.c_str(), Bar::Full);
    drawParamAndValue(s, key);
    std::printf("autotune first key label \"%s\"\n", knobLabel(*key.view.knob, key.view.target));
    out.push_back(std::move(s));
  }
  return out;
}

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

bool writeScreen(const std::string& dir, const std::string& set, const Candidate& s) {
  std::vector<uint8_t> px(static_cast<size_t>(kCanvasWidth) * kCanvasHeight);
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) px[static_cast<size_t>(y) * kCanvasWidth + x] = litAt(s.canvas, x, y) ? 255 : 0;
  }
  return writePgm(dir + "/readout-" + set + "-" + s.id + ".pgm", kCanvasWidth, kCanvasHeight, px);
}

// One PGM per screen and a manifest of id|caption|name width|status|bar width, bar width 0 for none.
bool writeSet(const std::string& dir, const std::string& set, const std::vector<Candidate>& screens) {
  FILE* manifest = std::fopen((dir + "/readout-" + set + "-screens.txt").c_str(), "w");
  if (!manifest) {
    std::perror("manifest");
    return false;
  }
  bool ok = true;
  for (const Candidate& s : screens) {
    ok = writeScreen(dir, set, s) && ok;
    const int bar = s.barWidth < 0 ? 0 : s.barWidth;
    std::fprintf(manifest, "%s|%s|%d|%s|%d\n", s.id.c_str(), s.caption.c_str(), s.nameWidth, s.status, bar);
    std::printf("%-6s %-11s bar %3d px  name row %3d px  %s\n", set.c_str(), s.id.c_str(), bar, s.nameWidth, s.status);
  }
  std::fclose(manifest);
  return ok;
}

// A specimen line wider than the panel. Each 128 px strip is a Canvas drawn with the pen moved left,
// so the glyphs come out of Canvas::drawText exactly as they would on the box.
bool writeLine(const std::string& dir, int index, const FontDesc& font, const char* text) {
  constexpr int kBand = 14;  // 12 px glyphs with 1 px above and below
  const int width = Canvas::textWidth(font, text) + 2 * kScreenPad;
  const int strips = (width + kCanvasWidth - 1) / kCanvasWidth;
  std::vector<uint8_t> px(static_cast<size_t>(width) * kBand, 0);
  for (int k = 0; k < strips; ++k) {
    Canvas c;
    c.drawText(kScreenPad - k * kCanvasWidth, kScreenPad, font, text);
    for (int y = 0; y < kBand; ++y) {
      for (int x = 0; x < kCanvasWidth && k * kCanvasWidth + x < width; ++x) {
        if (litAt(c, x, y)) px[static_cast<size_t>(y) * width + k * kCanvasWidth + x] = 255;
      }
    }
  }
  char name[32];
  std::snprintf(name, sizeof(name), "/readout-name-line%d.pgm", index);
  return writePgm(dir + name, width, kBand, px);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "build";
  bool ok = writeSet(dir, "name", makeScreens());
  ok = writeSet(dir, "title", makeTitleScreens()) && ok;

  // Specimen lines in Myrtle 7X12 at the ink gap of B1 and B2.
  const char* const lines[] = {"8 SPRING REVERB", "8 CHASM REVERB", "1 INPUT GATE", "6 DISTORTION",
                               "3 OCTAVE  SEMITONES", "0123456789", "-12 st  250 ms  100 %  -40 dB",
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "bm !\"#&'(),-./:"};
  FILE* lineList = std::fopen((dir + "/readout-name-lines.txt").c_str(), "w");
  if (!lineList) {
    std::perror("line list");
    return 1;
  }
  int index = 0;
  for (const char* text : lines) {
    ok = writeLine(dir, index, k7x12Gap3, text) && ok;
    std::fprintf(lineList, "%s\n", text);
    ++index;
  }
  std::fclose(lineList);
  return ok ? 0 : 1;
}
