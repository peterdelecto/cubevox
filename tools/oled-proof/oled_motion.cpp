// Animated OLED GUI proof. Ten scenes (Adam's voice, four knob turns, idle, mute, the motion
// rules, a toggle flip, the level bar beside the ribbon on a scripted drive) each run on their own
// clock from a clean screen, through the firmware's own Canvas and
// fonts at 25 frames a second, and come out as raw 1-bit frames with a two-line caption strip
// per frame plus a scene table. motion_gif.py cuts one GIF per scene from them.
//
//   clang++ -std=c++17 -O2 -I firmware/cubevox/src -I tools/font-editor/fonts \
//     tools/oled-proof/oled_motion.cpp firmware/cubevox/src/core/canvas.cpp \
//     firmware/cubevox/src/core/fonts.cpp -o build/oled_motion \
//   && build/oled_motion build/oled-motion.raw \
//   && python3 -I tools/oled-proof/motion_gif.py build/oled-motion.raw tools/oled-proof

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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

// Motion rules. These are the tunables the GUI spec will fix.
constexpr float kRestAfterS = 2.0f;     // knob still this long: back to the voice
constexpr int kDissolveFrames = 2;      // every screen change is a 2-frame Bayer dissolve (80 ms)
constexpr float kValueTauS = 0.02f;     // the readout eases toward the knob
constexpr int kScrollPxPerFrame = 2;    // voice history scrolls 50 px/s
constexpr float kSilence = 0.03f;       // env below this counts as silence

// The voice screen. The ribbon (the output) runs from column 4 to 127 about the horizon. The input
// bar stands in columns 1 and 2 over the ribbon's rows, 9 to 55, and fills up from row 55.
constexpr int kRibbonLeft = 4;
constexpr int kHalfMax = 22;
constexpr int kBarX = 1;
constexpr int kBarW = 2;
constexpr int kBarBottom = kH / 2 + kHalfMax + 1;   // 55
constexpr int kBarRows = 2 * (kHalfMax + 1) + 1;    // 47
constexpr float kBarFloorDb = -50.0f;               // empty here, full at 0 dBFS
constexpr float kBarReleaseS = 0.12f;
constexpr float kBarSilence = 0.01f;

// Scene clocks.
constexpr float kTurnS = 2.0f;          // an effect scene turns its knob this long
constexpr float kEffectSceneS = kTurnS + kRestAfterS;
constexpr float kVoiceSceneS = 6.0f;
constexpr float kIdleSceneS = 8.0f;
constexpr float kWalkDemoS = 5.0f;      // the walk step, shown early in the idle scene
constexpr float kMutedSceneS = 3.0f;
constexpr float kRulesSceneS = 8.5f;
constexpr float kRulesTouchS = 2.0f;    // in the rules scene the first knob is touched at this time
constexpr float kRulesSecondS = 4.0f;   // and a knob on another slot at this time
constexpr float kRulesTurnS = 1.0f;
constexpr float kToggleSceneS = 3.0f;
constexpr float kToggleOnS = 0.4f;      // in the toggle scene slot 6 flips on at this time
constexpr float kToggleOffS = 1.4f;     // and off, on the readout already up, at this time
constexpr int kToggleSlot = 5;          // Distortion
constexpr float kScriptSceneS = 4.0f;   // the scripted drive of scene 12

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float easeInOut(float p) {
  p = clamp01(p);
  return p < 0.5f ? 4.0f * p * p * p : 1.0f - std::pow(-2.0f * p + 2.0f, 3.0f) / 2.0f;
}
int roundInt(float v) { return static_cast<int>(std::lround(v)); }

constexpr uint8_t kBayer[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},   {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21},
};

// True for a share `density` (0..1) of the pixels, in the ordered-dither pattern.
bool bayer(int x, int y, float density) { return density * 64.0f > kBayer[y & 7][x & 7] + 0.5f; }

// ---------------------------------------------------------------------------------------------
// 1-bit frame in the SH1106 page layout, the same bytes the firmware canvas holds.

struct Frame {
  std::array<uint8_t, kW * kCanvasPages> b{};

  bool get(int x, int y) const {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return false;
    return (b[(y >> 3) * kW + x] >> (y & 7)) & 1;
  }
  void set(int x, int y, bool on = true) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    uint8_t& byte = b[(y >> 3) * kW + x];
    const uint8_t bit = static_cast<uint8_t>(1u << (y & 7));
    if (on) byte |= bit; else byte &= static_cast<uint8_t>(~bit);
  }
  void fill(int x, int y, int w, int h, bool on = true) {
    for (int yy = y; yy < y + h; ++yy)
      for (int xx = x; xx < x + w; ++xx) set(xx, yy, on);
  }
  void dither(int x, int y, int w, int h, float density) {
    for (int yy = y; yy < y + h; ++yy)
      for (int xx = x; xx < x + w; ++xx)
        if (bayer(xx, yy, density)) set(xx, yy);
  }
  void hline(int x0, int x1, int y) { fill(std::min(x0, x1), y, std::abs(x1 - x0) + 1, 1); }

  // Bresenham; density < 1 draws a dotted line in the dither pattern.
  void line(int x0, int y0, int x1, int y1, float density = 1.0f) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      if (density >= 1.0f || bayer(x0, y0, density)) set(x0, y0);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  void orCanvas(const Canvas& c) {
    for (int p = 0; p < kCanvasPages; ++p)
      for (int x = 0; x < kW; ++x) b[p * kW + x] |= c.page(p)[x];
  }
};

Frame shifted(const Frame& f, int dx, int dy) {
  Frame r;
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      if (f.get(x - dx, y - dy)) r.set(x, y);
  return r;
}

// Ordered dissolve from `from` to `to`, p 0..1.
Frame dissolve(const Frame& from, const Frame& to, float p) {
  Frame r;
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x)
      if (bayer(x, y, p) ? to.get(x, y) : from.get(x, y)) r.set(x, y);
  return r;
}

// Progress of a screen change that began `since` seconds ago: the in-between frames of the
// dissolve sit at 1/3 and 2/3, then the new screen is complete.
float dissolveProgress(float since) {
  const int k = static_cast<int>(std::floor(since / kDt + 0.5f));
  return std::min(1.0f, static_cast<float>(k + 1) / (kDissolveFrames + 1));
}

// ---------------------------------------------------------------------------------------------
// Text goes through the firmware canvas so the glyphs are exactly what the box draws.

int textW(const FontDesc& font, const char* s) { return Canvas::textWidth(font, s); }

void text(Frame& f, int x, int y, const FontDesc& font, const char* s) {
  Canvas c;
  c.drawText(x, y, font, s);
  f.orCanvas(c);
}

void textCentered(Frame& f, int y, const FontDesc& font, const char* s) {
  text(f, (kW - textW(font, s)) / 2, y, font, s);
}

// Tabular digits: one fixed cell per character. Returns the pen after the last cell.
int digits(Frame& f, int x, int y, const FontDesc& font, const char* s) {
  Canvas c;
  const int pen = c.drawDigits(x, y, font, s);
  f.orCanvas(c);
  return pen;
}

// Rows outside [top, bottom) are dropped. The odometer roll clips each digit to its band.
int digitsClipped(Frame& f, int x, int y, const FontDesc& font, const char* s, int top, int bottom) {
  Canvas c;
  const int pen = c.drawDigits(x, y, font, s);
  Frame g;
  g.orCanvas(c);
  for (int yy = std::max(0, top); yy < std::min(kH, bottom); ++yy)
    for (int xx = 0; xx < kW; ++xx)
      if (g.get(xx, yy)) f.set(xx, yy);
  return pen;
}

// ---------------------------------------------------------------------------------------------
// Adam's voice as a per-frame level, read from the sample WAVs. Each scene starts it from its own
// frame 0, after `delay` frames of silence, or keeps it silent.

struct Voice {
  std::vector<float> perf;  // the ribbon's level per frame, 0..1, scaled to the performance's own peak
  std::vector<float> bar;   // the bar's level per frame, 0..1, absolute and linear in dB from -50 to 0 dBFS
  int delay = 0;
  bool silent = false;

  float at(int frame) const {
    const int i = frame - delay;
    if (silent || i < 0 || perf.empty()) return 0.0f;
    return perf[i % perf.size()];
  }
  float barAt(int frame) const {
    const int i = frame - delay;
    if (silent || i < 0 || bar.empty()) return 0.0f;
    return bar[i % bar.size()];
  }
  float atTime(float t) const { return at(static_cast<int>(std::floor(t * kFps))); }
};

uint32_t u32At(const std::vector<uint8_t>& d, size_t i) {
  return d[i] | d[i + 1] << 8 | d[i + 2] << 16 | d[i + 3] << 24;
}

bool readWav16(const char* path, std::vector<float>& mono, uint32_t& rate) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  if (d.size() < 12 || std::memcmp(&d[0], "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0) return false;
  uint16_t channels = 0, bits = 0;
  size_t i = 12;
  while (i + 8 <= d.size()) {
    const uint32_t size = u32At(d, i + 4);
    const size_t body = i + 8;
    if (std::memcmp(&d[i], "fmt ", 4) == 0 && body + 16 <= d.size()) {
      channels = static_cast<uint16_t>(d[body + 2] | d[body + 3] << 8);
      rate = u32At(d, body + 4);
      bits = static_cast<uint16_t>(d[body + 14] | d[body + 15] << 8);
    } else if (std::memcmp(&d[i], "data", 4) == 0) {
      if (bits != 16 || channels == 0) return false;
      const size_t end = std::min(d.size(), body + size);
      for (size_t s = body; s + 2 * channels <= end; s += 2 * channels) {
        float sum = 0;
        for (int c = 0; c < channels; ++c)
          sum += static_cast<int16_t>(d[s + 2 * c] | d[s + 2 * c + 1] << 8) / 32768.0f;
        mono.push_back(sum / channels);
      }
      return !mono.empty();
    }
    i = body + size + (size & 1);
  }
  return false;
}

// RMS per frame, the three takes with breaths between, smoothed for the display.
bool buildVoice(Voice& voice) {
  const char* takes[] = {"samples/adam_vox_sample_3s.wav", "samples/adam_vox_sample2_4s.wav",
                         "samples/adam_vox_sample3_5s.wav"};
  const int breaths[] = {10, 10, 15};  // frames of rest after each take
  std::vector<float> raw;
  for (int k = 0; k < 3; ++k) {
    std::vector<float> mono;
    uint32_t rate = 0;
    if (!readWav16(takes[k], mono, rate)) {
      std::fprintf(stderr, "cannot read %s (run from the repo root)\n", takes[k]);
      return false;
    }
    const size_t window = rate / kFps;
    for (size_t s = 0; s + window <= mono.size(); s += window) {
      double acc = 0;
      for (size_t j = s; j < s + window; ++j) acc += mono[j] * mono[j];
      raw.push_back(static_cast<float>(std::sqrt(acc / window)));
    }
    raw.insert(raw.end(), breaths[k], 0.0f);
  }
  std::vector<float> sorted = raw;
  std::sort(sorted.begin(), sorted.end());
  const float ref = std::max(1e-6f, sorted[sorted.size() * 98 / 100]);

  float smooth = 0;
  float barSmooth = 0;
  const float release = std::exp(-kDt / 0.12f);
  const float barRelease = std::exp(-kDt / kBarReleaseS);
  for (float r : raw) {
    const float level = std::pow(clamp01(r / ref), 0.6f);
    smooth = level > smooth ? level : level + (smooth - level) * release;
    voice.perf.push_back(smooth < 0.01f ? 0.0f : smooth);

    const float db = r > 0.0f ? 20.0f * std::log10(r) : -200.0f;
    const float pos = clamp01((db - kBarFloorDb) / -kBarFloorDb);
    barSmooth = pos >= barSmooth ? pos : pos + (barSmooth - pos) * barRelease;
    if (barSmooth < kBarSilence) barSmooth = 0.0f;
    voice.bar.push_back(barSmooth);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Cards.

// What a screen needs from the world: the voice and the scene's frame counter.
struct Live {
  const Voice* voice;
  int frame;
};

struct Param {
  const char* label;
  const char* unit;
  float lo, hi;
  bool stepped;
  float fallback;  // the other knob's setting while one is turned
};

struct Card {
  const char* name;
  Param a, b;
};

// Slots 1..8 of the default layout.
const Card kCards[8] = {
    {"INPUT GATE", {"THRESHOLD", "DB", -60, -10, false, -35}, {"DECAY", "MS", 20, 500, false, 150}},
    {"AUTOTUNE", {"KEY", "", 0, 11, true, 5}, {"RESPONSE", "%", 0, 100, false, 50}},
    {"OCTAVE", {"SEMITONES", "ST", -12, 12, true, 0}, {"MIX", "%", 0, 100, false, 60}},
    {"UNISON", {"DEPTH", "%", 0, 100, false, 20}, {"RATE", "HZ", 0.1f, 5, false, 1.5f}},
    {"SLAPBACK", {"MIX", "%", 0, 100, false, 40}, {"TIME", "MS", 60, 250, false, 120}},
    {"DISTORTION", {"DRIVE", "%", 0, 100, false, 20}, {"TONE", "%", 0, 100, false, 50}},
    {"GATE", {"THRESHOLD", "DB", -60, -10, false, -40}, {"DECAY", "MS", 20, 500, false, 150}},
    {"SPRING REVERB", {"MIX", "%", 0, 100, false, 30}, {"TIME", "%", 0, 100, false, 50}},
};

// The value's field is fixed per parameter, so the row never moves sideways. It holds a sign cell
// when the range is signed, one cell per digit of the widest number, a space and the unit, and it is
// centred once. A number is right-aligned in the digit cells with its sign in the sign cell, and a
// shorter number leaves its leading cells blank.
int fieldDigits(const Param& p) {
  const long widest = std::max(std::lround(std::fabs(p.lo)), std::lround(std::fabs(p.hi)));
  return static_cast<int>(std::to_string(widest).size());
}

// One character per cell of the field for the whole number v.
std::string fieldCells(const Param& p, int v) {
  std::string cells(static_cast<size_t>(fieldDigits(p)), ' ');
  const std::string magnitude = std::to_string(std::abs(v));
  for (size_t i = 0; i < magnitude.size() && i < cells.size(); ++i)
    cells[cells.size() - 1 - i] = magnitude[magnitude.size() - 1 - i];
  if (p.lo < 0) cells.insert(cells.begin(), v < 0 ? '-' : (v > 0 ? '+' : ' '));
  return cells;
}

// ---------------------------------------------------------------------------------------------
// Screens.

// 16X24 readout. Each digit that changes rolls vertically from the old glyph to the new one. The
// digits sit in the fixed cells of the parameter's field, so the unit after them never moves while
// they roll, and a number that gains a digit lights its leading cell in place.
void rollValue(Frame& f, int y, const Param& p, float shown) {
  const FontDesc& big = font16x24();
  const float fl = std::floor(shown);
  const float frac = shown - fl;
  const std::string A = fieldCells(p, static_cast<int>(fl));
  const std::string B = fieldCells(p, static_cast<int>(fl) + 1);
  const int n = static_cast<int>(A.size());
  char unit[8];
  std::snprintf(unit, sizeof unit, " %s", p.unit);
  const int x0 = (kW - (Canvas::digitsWidth(big, A.c_str()) + big.spacing + textW(big, unit))) / 2;
  const int dy = roundInt(frac * big.height);
  int pen = x0;
  for (int i = 0; i < n; ++i) {
    const char ca[2] = {A[i], 0}, cb[2] = {B[i], 0};
    if (A[i] == B[i]) {
      pen = digits(f, pen, y, big, ca);
    } else {
      digitsClipped(f, pen, y - dy, big, ca, y, y + big.height);
      pen = digitsClipped(f, pen, y + big.height - dy, big, cb, y, y + big.height);
    }
  }
  text(f, pen, y, big, unit);
}

void title(Frame& f, int slot) {
  char head[32];
  std::snprintf(head, sizeof head, "%d %s", slot + 1, kCards[slot].name);
  text(f, kPad, kPad, font5x7(), head);
}

// The knob screen: slot and card on top, the parameter, the rolling value. The three rows share
// the free height evenly (7 px, 7 px, 6 px gaps).
Frame knobScreen(int slot, int knob, float shown) {
  const Param& p = knob ? kCards[slot].b : kCards[slot].a;
  Frame f;
  title(f, slot);
  textCentered(f, 15, font9x12(), p.label);
  rollValue(f, 34, p, shown);
  return f;
}

// The toggle screen: slot and card on top, the parameter row empty, ON or OFF in 16X24 centred on
// the value row.
Frame toggleScreen(int slot, bool on) {
  Frame f;
  title(f, slot);
  textCentered(f, 34, font16x24(), on ? "ON" : "OFF");
  return f;
}

// The voice screen. The output scrolls by as a ribbon around a horizon line, from column 4, and
// the input stands as a 2 px bar at the left edge. `ribbonAt(age)` is the ribbon's level `age`
// frames back and `barRows` the bar's height. In silence the ribbon is the line and the bar is empty.
template <typename LevelAt>
Frame voiceScreen(LevelAt ribbonAt, int barRows) {
  Frame f;
  const int cy = kH / 2;
  f.fill(kBarX, kBarBottom - barRows + 1, kBarW, barRows);
  for (int x = kRibbonLeft; x < kW; ++x) {
    const float e = ribbonAt((kW - 1 - x) / kScrollPxPerFrame);
    const int h = roundInt(std::pow(e, 0.8f) * kHalfMax);
    if (h <= 0) {
      f.set(x, cy);
      continue;
    }
    f.fill(x, cy - h, 1, 2 * h + 1);
    if (bayer(x, cy - h - 1, 0.5f)) f.set(x, cy - h - 1);
    if (bayer(x, cy + h + 1, 0.5f)) f.set(x, cy + h + 1);
  }
  return f;
}

// The rest screen shows Adam's voice. The ribbon is the performance scaled to its own peak, the way
// the self-scaling output window shows it, and the bar is the same take at its absolute level.
Frame restScreen(const Live& L) {
  return voiceScreen([&L](int age) { return L.voice->at(L.frame - age); },
                     roundInt(L.voice->barAt(L.frame) * kBarRows));
}

// ---------------------------------------------------------------------------------------------
// The scripted drive of scene 12, which test/firmware/ui_face.cpp repeats tick for tick. The input
// holds kScriptInDb and the output cycles through twelve levels, one per frame. The ribbon level
// follows the box's rule. The window top jumps to a louder tick and falls 10 dB a second, never
// below -30 dBFS. The level is the tick's place in the 44 dB under the top, raised to 0.6, with an
// instant attack and a 120 ms release.
constexpr float kScriptInDb = -23.0f;
constexpr float kScriptOutDb[12] = {-30, -24, -19, -12, -8, -14, -20, -26, -32, -40, -52, -60};

std::vector<float> scriptedRibbon(int frames) {
  std::vector<float> levels;
  float top = -30.0f;
  float level = 0.0f;
  const float release = std::exp(-kDt / 0.12f);
  for (int k = 0; k < frames; ++k) {
    const float d = kScriptOutDb[k % 12];
    if (d > top) {
      top = d;
    } else {
      top = std::max(-30.0f, top - 10.0f * kDt);
    }
    const float position = clamp01((d - (top - 44.0f)) / 44.0f);
    const float now = std::pow(position, 0.6f);
    level = now >= level ? now : now + (level - now) * release;
    if (level < 0.01f) level = 0.0f;
    levels.push_back(level);
  }
  return levels;
}

// The word may become BYPASS, depending on what the switch ends up doing.
constexpr const char* kMuteText = "MUTED";

Frame mutedScreen() {
  Frame f;
  textCentered(f, (kH - font16x24().height) / 2, font16x24(), kMuteText);
  return f;
}

// ---------------------------------------------------------------------------------------------
// Knob turns.

struct Turn {
  int slot, knob;
  float from, to;
};

const Param& turnParam(const Turn& turn) {
  return turn.knob ? kCards[turn.slot].b : kCards[turn.slot].a;
}

// Where the knob is `age` seconds into a turn lasting `turnS`.
float knobTarget(const Turn& turn, float age, float turnS) {
  const float v = lerp(turn.from, turn.to, easeInOut(age / turnS));
  return turnParam(turn).stepped ? std::round(v) : v;
}

// The readout eases toward the knob; reset at the first frame of a turn.
float shownValue(float& shown, float target, bool first) {
  if (first) shown = target;
  shown += (target - shown) * (1.0f - std::exp(-kDt / kValueTauS));
  return shown;
}

// ---------------------------------------------------------------------------------------------
// The scenes. Each starts from a clean screen on its own clock, so a GIF shows one thing.

struct Caption {
  char line1[22];
  char line2[22];
};

struct Scene {
  const char* number;
  const char* slug;
  float seconds;
  std::function<Frame(int i, float t, Caption& cap)> draw;
};

void setCaption(Caption& cap, const char* line1, const char* line2) {
  std::snprintf(cap.line1, sizeof cap.line1, "%s", line1);
  std::snprintf(cap.line2, sizeof cap.line2, "%s", line2);
}

struct Effect {
  const char* number;
  const char* slug;
  const char* name;
  Turn turn;
};

// Scenes 6 and 7 (Distortion, Unison) are gone: without pictures they would repeat 2 and 3.
const Effect kEffects[] = {
    {"2", "slapback-time", "SLAPBACK  TIME", {4, 1, 120, 210}},
    {"3", "reverb-mix", "REVERB  MIX", {7, 0, 30, 65}},
    {"4", "octave-semitones", "OCTAVE  SEMITONES", {2, 0, 0, -12}},
    {"5", "gate-threshold", "GATE  THRESHOLD", {6, 0, -40, -24}},
};

// Scene 1: the rest screen while Adam sings, from an empty line. The bar reads his level in
// absolute dB and the ribbon the same take scaled to its own peak, so the two differ.
Scene voiceScene(Voice& voice) {
  return {"1", "voice-line", kVoiceSceneS, [&voice](int i, float, Caption& cap) {
            voice.delay = kFps / 2;
            voice.silent = false;
            setCaption(cap, "1  VOICE LINE", voice.at(i) > kSilence ? "SINGING" : "SILENCE");
            return restScreen(Live{&voice, i});
          }};
}

// Scenes 2-5: the knob turns for 2 s on the readout, then rests 2 s. What follows, the fade back
// to the voice, is scene 10's subject.
Scene effectScene(const Effect& e, float& shown) {
  return {e.number, e.slug, kEffectSceneS, [&e, &shown](int i, float t, Caption& cap) {
            const float target = knobTarget(e.turn, std::min(t, kTurnS), kTurnS);
            const float value = shownValue(shown, target, i == 0);
            char head[22];
            std::snprintf(head, sizeof head, "%s  %s", e.number, e.name);
            setCaption(cap, head, t < kTurnS ? "KNOB TURNING" : "KNOB STILL");
            return knobScreen(e.turn.slot, e.turn.knob, value);
          }};
}

// Scene 8: silence. The line, and one 1 px burn-in step shown early.
Scene idleScene(Voice& voice) {
  return {"8", "idle", kIdleSceneS, [&voice](int i, float t, Caption& cap) {
            voice.silent = true;
            const Live L{&voice, i};
            const Frame rest = restScreen(L);
            if (t < kWalkDemoS) {
              setCaption(cap, "8  IDLE", "SILENT, JUST THE LINE");
              return rest;
            }
            setCaption(cap, "8  IDLE", "1 PX WALK EVERY 60 S");
            return shifted(rest, 0, 1);
          }};
}

// Scene 9: mute on. Plain text, nothing behind it.
Scene mutedScene(Voice& voice) {
  return {"9", "muted", kMutedSceneS, [&voice](int i, float t, Caption& cap) {
            voice.silent = true;
            const Live L{&voice, i};
            setCaption(cap, "9  MUTED", "MUTE ON");
            const Frame muted = mutedScreen();
            return dissolve(restScreen(L), muted, dissolveProgress(t));
          }};
}

// Scene 10: the screen changes on their own. The voice screen, a knob touched, a knob on another
// slot touched while that readout is up, 2 s of stillness, and back to the voice. Every change
// is the same 2-frame dissolve.
Scene rulesScene(Voice& voice, float& shown) {
  static const Turn first{4, 1, 120, 160};
  static const Turn second{2, 0, 0, -3};
  return {"10", "transitions", kRulesSceneS, [&voice, &shown](int i, float t, Caption& cap) {
            voice.delay = 0;
            voice.silent = false;
            const Frame rest = restScreen(Live{&voice, i});
            if (t < kRulesTouchS) {
              setCaption(cap, "10  TRANSITIONS", "VOICE, NO KNOB TOUCHED");
              return rest;
            }
            const bool onSecond = t >= kRulesSecondS;
            const Turn& turn = onSecond ? second : first;
            const float age = t - (onSecond ? kRulesSecondS : kRulesTouchS);
            const float target = knobTarget(turn, std::min(age, kRulesTurnS), kRulesTurnS);
            const float value = shownValue(shown, target, age < kDt);
            const Frame readout = knobScreen(turn.slot, turn.knob, value);
            if (!onSecond) {
              setCaption(cap, "10  TRANSITIONS", age < kRulesTurnS ? "KNOB 5 TOUCHED" : "KNOB STILL");
              return dissolve(rest, readout, dissolveProgress(age));
            }
            const float still = age - kRulesTurnS;
            if (still < kRestAfterS) {
              setCaption(cap, "10  TRANSITIONS", age < kRulesTurnS ? "KNOB 3 TOUCHED" : "KNOB STILL FOR 2 S");
              const Frame previous = knobScreen(first.slot, first.knob, first.to);
              return dissolve(previous, readout, dissolveProgress(age));
            }
            setCaption(cap, "10  TRANSITIONS", "BACK TO THE VOICE");
            return dissolve(readout, rest, dissolveProgress(still - kRestAfterS));
          }};
}

// Scene 11: a slot toggle flips on from the silent voice, then off. The first flip dissolves the
// readout in. The second changes the word in place, since the slot is already shown.
Scene toggleScene(Voice& voice) {
  return {"11", "toggle", kToggleSceneS, [&voice](int i, float t, Caption& cap) {
            voice.silent = true;
            const Frame rest = restScreen(Live{&voice, i});
            if (t < kToggleOnS) {
              setCaption(cap, "11  TOGGLE", "VOICE, NO FLIP");
              return rest;
            }
            if (t < kToggleOffS) {
              setCaption(cap, "11  TOGGLE", "SLOT 6 FLIPPED ON");
              return dissolve(rest, toggleScreen(kToggleSlot, true), dissolveProgress(t - kToggleOnS));
            }
            setCaption(cap, "11  TOGGLE", "SLOT 6 FLIPPED OFF");
            return toggleScreen(kToggleSlot, false);
          }};
}

// Scene 12: the voice screen on the scripted drive above. The bar holds still at 25 rows while the
// ribbon cycles, so the two read apart. The host test repeats the drive and matches frame 79.
Scene scriptScene() {
  return {"12", "bar-ribbon", kScriptSceneS, [](int i, float, Caption& cap) {
            static const std::vector<float> ribbon = scriptedRibbon(static_cast<int>(kScriptSceneS * kFps));
            setCaption(cap, "12  BAR AND RIBBON", "IN STEADY, OUT CYCLES");
            const int rows = roundInt(clamp01((kScriptInDb - kBarFloorDb) / -kBarFloorDb) * kBarRows);
            return voiceScreen([i](int age) { return i - age >= 0 ? ribbon[i - age] : 0.0f; }, rows);
          }};
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

// <stem>.scenes beside the raw stream: number, slug, first frame, frame after the last.
std::string scenesPath(const char* rawPath) {
  std::string path(rawPath);
  const size_t dot = path.rfind('.');
  if (dot != std::string::npos && path.find('/', dot) == std::string::npos) path.erase(dot);
  return path + ".scenes";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: oled_motion <out.raw>\n");
    return 2;
  }
  Voice voice;
  if (!buildVoice(voice)) return 1;
  float shown = 0;

  std::vector<Scene> scenes;
  scenes.push_back(voiceScene(voice));
  for (const Effect& e : kEffects) scenes.push_back(effectScene(e, shown));
  scenes.push_back(idleScene(voice));
  scenes.push_back(mutedScene(voice));
  scenes.push_back(rulesScene(voice, shown));
  scenes.push_back(toggleScene(voice));
  scenes.push_back(scriptScene());

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
    double sung = 0;
    for (int i = 0; i < frames; ++i) {
      Caption cap{};
      const Frame screen = s.draw(i, i * kDt, cap);
      Frame capFrame;
      writeCaption(capFrame, cap);
      std::fwrite(screen.b.data(), 1, screen.b.size(), out);
      std::fwrite(capFrame.b.data(), 1, kW * kCapPages, out);
      sung += voice.at(i);
    }
    std::fprintf(table, "%s\t%s\t%d\t%d\n", s.number, s.slug, written, written + frames);
    std::printf("scene %-2s %-18s %4.1f s  mean voice level %.2f\n", s.number, s.slug, s.seconds, sung / frames);
    written += frames;
  }
  std::fclose(table);
  std::fclose(out);
  std::printf("%d frames at %d fps, scene table %s\n", written, kFps, tablePath.c_str());
  return 0;
}
