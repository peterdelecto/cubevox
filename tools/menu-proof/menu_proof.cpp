// Menu proof. A seven-row menu in the 5x7 font, opened, walked, and clicked row by row, drawn
// through the firmware's own Canvas and 5x7 font at 25 frames a second. Every screen change is
// the GUI's 2-frame dissolve. Nine scenes, each on its own clock from a clean
// screen; motion_gif.py cuts one GIF per scene.
//
//   clang++ -std=c++17 -O2 -I firmware/cubevox/src -I tools/font-editor/fonts \
//     tools/menu-proof/menu_proof.cpp firmware/cubevox/src/core/canvas.cpp \
//     firmware/cubevox/src/core/fonts.cpp -o build/menu_proof \
//   && build/menu_proof build/menu-proof.raw \
//   && python3 -I tools/oled-proof/motion_gif.py build/menu-proof.raw tools/menu-proof

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/fonts.h"

namespace {

constexpr int kW = kCanvasWidth;
constexpr int kH = kCanvasHeight;
constexpr int kFps = 25;
constexpr float kDt = 1.0f / kFps;
constexpr int kPad = 1;
constexpr int kCapH = 16;
constexpr int kCapPages = kCapH / 8;
constexpr int kDissolveFrames = 2;  // every screen change, as in the GUI proof

// Menu grid: seven rows of 5x7 at an 8 px pitch from y 1, so the top keeps its 1 px pad.
constexpr int kMenuRows = 7;
constexpr int kRowPitch = 8;
constexpr int kRowTop = 1;
constexpr int kCursorX = kPad;                        // the `>` in character column 0
constexpr int kTextX = kPad + 2 * kFont5x7Advance;    // row text from character column 2
constexpr const char* kVersion = "V2.00.20";

constexpr uint8_t kBayer[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},   {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
};

bool bayer(int x, int y, float density) { return density * 64.0f > kBayer[y & 7][x & 7] + 0.5f; }

// 1-bit frame in the SH1106 page layout, the same bytes the firmware canvas holds.
struct Frame {
  std::array<uint8_t, kW * kCanvasPages> b{};

  bool get(int x, int y) const {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return false;
    return (b[(y >> 3) * kW + x] >> (y & 7)) & 1;
  }
  void set(int x, int y) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    b[(y >> 3) * kW + x] |= static_cast<uint8_t>(1u << (y & 7));
  }
  void orCanvas(const Canvas& c) {
    for (int p = 0; p < kCanvasPages; ++p)
      for (int x = 0; x < kW; ++x) b[p * kW + x] |= c.page(p)[x];
  }
};

Frame dissolve(const Frame& from, const Frame& to, float p) {
  Frame r;
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      if (bayer(x, y, p) ? to.get(x, y) : from.get(x, y)) r.set(x, y);
  return r;
}

// Progress of a change that began `since` seconds ago: 1/3, 2/3, then complete.
float dissolveProgress(float since) {
  const int k = static_cast<int>(std::floor(since / kDt + 0.5f));
  return std::min(1.0f, static_cast<float>(k + 1) / (kDissolveFrames + 1));
}

void text(Frame& f, int x, int y, const char* s) {
  Canvas c;
  c.drawText(x, y, font5x7(), s);
  f.orCanvas(c);
}

// ---------------------------------------------------------------------------------------------
// Screens.

struct Settings {
  bool eq = true;
  int out = 0;  // LINE, INSTRUMENT
  int in = 0;   // MIC, PEDAL
  int top = 0;  // MUTE, BYPASS
};

std::string rowText(int row, const Settings& s) {
  switch (row) {
    case 0: return std::string("CLEANUP EQ ") + (s.eq ? "LIGHT" : "OFF");
    case 1: return std::string("OUT ") + (s.out ? "INSTRUMENT" : "LINE");
    case 2: return std::string("INPUT ") + (s.in ? "PEDAL" : "MIC");
    case 3: return std::string("TOP SWITCH ") + (s.top ? "BYPASS" : "MUTE");
    case 4: return "CHAIN ORDER";
    case 5: return "UPDATE FIRMWARE";
    default: return "EXIT";
  }
}

Frame menuScreen(int cursor, const Settings& s) {
  Frame f;
  for (int row = 0; row < kMenuRows; ++row) {
    const int y = kRowTop + row * kRowPitch;
    if (row == cursor) text(f, kCursorX, y, ">");
    text(f, kTextX, y, rowText(row, s).c_str());
  }
  return f;
}

// Read-only chain view: the eight slots in order, same grid, no cursor.
Frame chainScreen() {
  const char* slots[8] = {"1 INPUT GATE", "2 AUTOTUNE", "3 OCTAVE", "4 UNISON",
                          "5 SLAPBACK", "6 DISTORTION", "7 GATE", "8 SPRING REVERB"};
  Frame f;
  for (int row = 0; row < 8; ++row) text(f, kPad, kRowTop + row * kRowPitch, slots[row]);
  return f;
}

// The approved confirm screen, 5x7 prose on rows 0, 2, 4, 6.
Frame confirmScreen() {
  char have[24];
  std::snprintf(have, sizeof have, "YOU HAVE %s.", kVersion);
  Frame f;
  text(f, kPad, 1, "UPDATE THE FIRMWARE?");
  text(f, kPad, 17, have);
  text(f, kPad, 33, "CLICK TO START.");
  text(f, kPad, 49, "TURN TO CANCEL.");
  return f;
}

// The voice screen in silence: the line.
Frame voiceScreen() {
  Frame f;
  for (int x = 0; x < kW; ++x) f.set(x, kH / 2);
  return f;
}

// ---------------------------------------------------------------------------------------------
// Scenes as keyframes: at each time a new screen and caption take over through the dissolve.

struct Caption {
  char line1[22];
  char line2[22];
};

struct Key {
  float t;
  Frame frame;
  const char* caption;
};

struct Scene {
  const char* number;
  const char* slug;
  const char* title;
  float seconds;
  std::vector<Key> keys;
};

Frame atTime(const std::vector<Key>& keys, float t, const char*& caption) {
  size_t k = 0;
  while (k + 1 < keys.size() && keys[k + 1].t <= t) ++k;
  caption = keys[k].caption;
  if (k == 0) return keys[0].frame;
  return dissolve(keys[k - 1].frame, keys[k].frame, dissolveProgress(t - keys[k].t));
}

// Rows 1 to 4: click flips the value, click flips it back.
Scene toggleScene(const char* number, const char* slug, const char* title, int row,
                  Settings first, Settings second, const char* to, const char* back) {
  Settings start;
  return {number, slug, title, 4.0f,
          {{0.0f, menuScreen(row, start), "CLICK FLIPS THE VALUE"},
           {1.0f, menuScreen(row, first), to},
           {2.5f, menuScreen(row, second), back}}};
}

std::vector<Scene> buildScenes() {
  Settings s;
  std::vector<Scene> scenes;

  scenes.push_back({"1", "open", "1  OPEN", 3.0f,
                    {{0.0f, voiceScreen(), "VOICE SCREEN"},
                     {1.0f, menuScreen(0, s), "CLICK OPENS THE MENU"}}});

  Scene walk{"2", "navigate", "2  NAVIGATE", 6.0f, {}};
  walk.keys.push_back({0.0f, menuScreen(0, s), "TURN MOVES THE CURSOR"});
  for (int row = 1; row < kMenuRows; ++row)
    walk.keys.push_back({0.5f * row, menuScreen(row, s), row == kMenuRows - 1 ? "ROW 7, NO WRAP" : "TURN, DOWN ONE ROW"});
  walk.keys.push_back({4.3f, menuScreen(5, s), "TURN, UP ONE ROW"});
  walk.keys.push_back({4.8f, menuScreen(4, s), "TURN, UP ONE ROW"});
  scenes.push_back(walk);

  Settings a = s, b = s;
  a.eq = false;
  scenes.push_back(toggleScene("R1", "cleanup-eq", "R1  CLEANUP EQ", 0, a, b, "CLICK, LIGHT TO OFF", "CLICK, OFF TO LIGHT"));
  a = s; a.out = 1;
  scenes.push_back(toggleScene("R2", "out", "R2  OUT", 1, a, b, "CLICK, TO INSTRUMENT", "CLICK, TO LINE"));
  a = s; a.in = 1;
  scenes.push_back(toggleScene("R3", "input", "R3  INPUT", 2, a, b, "CLICK, TO PEDAL", "CLICK, TO MIC"));
  a = s; a.top = 1;
  scenes.push_back(toggleScene("R4", "top-switch", "R4  TOP SWITCH", 3, a, b, "CLICK, TO BYPASS", "CLICK, TO MUTE"));

  scenes.push_back({"R5", "chain-order", "R5  CHAIN ORDER", 5.0f,
                    {{0.0f, menuScreen(4, s), "CLICK OPENS THE VIEW"},
                     {1.0f, chainScreen(), "READ-ONLY, EIGHT SLOTS"},
                     {3.5f, menuScreen(4, s), "CLICK, BACK TO MENU"}}});
  scenes.push_back({"R6", "update-firmware", "R6  UPDATE FIRMWARE", 5.0f,
                    {{0.0f, menuScreen(5, s), "CLICK OPENS CONFIRM"},
                     {1.0f, confirmScreen(), "CONFIRM SCREEN"},
                     {3.5f, menuScreen(5, s), "TURN CANCELS"}}});
  scenes.push_back({"R7", "exit", "R7  EXIT", 2.5f,
                    {{0.0f, menuScreen(6, s), "CLICK CLOSES THE MENU"},
                     {1.0f, voiceScreen(), "BACK TO THE VOICE"}}});
  return scenes;
}

void writeCaption(Frame& cap, const Caption& c) {
  Canvas canvas;
  canvas.drawText(kPad, kPad, font5x7(), c.line1);
  canvas.drawText(kPad, 9, font5x7(), c.line2);
  cap.orCanvas(canvas);
}

void putU32(FILE* f, uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

std::string scenesPath(const char* rawPath) {
  std::string path(rawPath);
  const size_t dot = path.rfind('.');
  if (dot != std::string::npos && path.find('/', dot) == std::string::npos) path.erase(dot);
  return path + ".scenes";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: menu_proof <out.raw>\n");
    return 2;
  }
  const std::vector<Scene> scenes = buildScenes();
  int total = 0;
  for (const Scene& s : scenes) total += static_cast<int>(s.seconds * kFps);

  FILE* out = std::fopen(argv[1], "wb");
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", argv[1]);
    return 1;
  }
  std::fwrite("CVOM", 1, 4, out);
  for (uint32_t v : {1u, static_cast<uint32_t>(kW), static_cast<uint32_t>(kH), static_cast<uint32_t>(kW),
                     static_cast<uint32_t>(kCapH), static_cast<uint32_t>(kFps), static_cast<uint32_t>(total)})
    putU32(out, v);

  const std::string tablePath = scenesPath(argv[1]);
  FILE* table = std::fopen(tablePath.c_str(), "w");
  if (!table) {
    std::fprintf(stderr, "cannot write %s\n", tablePath.c_str());
    return 1;
  }
  int written = 0;
  for (const Scene& s : scenes) {
    const int frames = static_cast<int>(s.seconds * kFps);
    for (int i = 0; i < frames; ++i) {
      const char* state = "";
      const Frame screen = atTime(s.keys, i * kDt, state);
      Caption cap{};
      std::snprintf(cap.line1, sizeof cap.line1, "%s", s.title);
      std::snprintf(cap.line2, sizeof cap.line2, "%s", state);
      Frame capFrame;
      writeCaption(capFrame, cap);
      std::fwrite(screen.b.data(), 1, screen.b.size(), out);
      std::fwrite(capFrame.b.data(), 1, kW * kCapPages, out);
    }
    std::fprintf(table, "%s\t%s\t%d\t%d\n", s.number, s.slug, written, written + frames);
    std::printf("scene %-3s %-16s %4.1f s\n", s.number, s.slug, s.seconds);
    written += frames;
  }
  std::fclose(table);
  std::fclose(out);
  std::printf("%d frames at %d fps, scene table %s\n", written, kFps, tablePath.c_str());
  return 0;
}
