#pragma once

// Musician macros for the emulator face. Each macro is a 0..100 % slider whose
// 50 % lands on today's tuning. A macro maps its position onto its fields
// piecewise between three anchors (0 / 50 / 100 %). Hz and ms fields
// interpolate in log space, other fields linearly. Integer fields round or
// step. Emulator only; the firmware never includes this file.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "engine/chasm.h"
#include "engine/distortion.h"
#include "engine/harmony.h"
#include "engine/octave.h"
#include "engine/spring_b.h"
#include "engine/unison.h"

namespace cv::macros {

enum class Interp : uint8_t {
  Linear,
  Log,    // Hz and ms fields; falls back to linear if an anchor is not positive
  Round,  // linear, then nearest integer
  Step,   // integer; the high anchor applies from kStepAt up
};

struct Anchor {
  float lo, mid, hi;
  Interp mode = Interp::Linear;
};

constexpr float kCenter = 50.0f;
constexpr float kStepAt = 75.0f;

// Value at position pos (0..100 %). The segments meet at the middle anchor.
inline float at(const Anchor& a, float pos) {
  if (pos < 0.0f) pos = 0.0f;
  if (pos > 100.0f) pos = 100.0f;
  if (a.mode == Interp::Step) return pos >= kStepAt ? a.hi : a.mid;
  const bool upper = pos > kCenter;
  const float from = upper ? a.mid : a.lo;
  const float to = upper ? a.hi : a.mid;
  const float t = upper ? (pos - kCenter) / kCenter : pos / kCenter;
  float v = from + (to - from) * t;
  if (a.mode == Interp::Log && from > 0.0f && to > 0.0f)
    v = from * std::pow(to / from, t);
  if (t <= 0.0f) v = from;
  if (t >= 1.0f) v = to;
  return a.mode == Interp::Round ? std::round(v) : v;
}

inline int atInt(const Anchor& a, float pos) { return static_cast<int>(at(a, pos)); }

// ---- Anchor table: lo / mid (today) / hi ------------------------------------

using I = Interp;

// Octave
constexpr Anchor kOctSlide{2.0f, 15.0f, 120.0f, I::Log};              // glideMs

// Harmony
constexpr Anchor kHarGlide{120.0f, 30.0f, 5.0f, I::Log};              // glideMs

// Unison
// Ends are wide so each slider is clearly audible (owner 2026-10-02); 50 % is the
// owner's tuning. Chorus end: 6 ms swing, no fixed detune. Double end: 15 cents, near-still.
constexpr Anchor kUniDetune{0.0f, 2.0f, 15.0f};                       // |detuneCents|
constexpr Anchor kUniSwingScale{2.0f, 1.0f, 0.1f};                    // x swingMin/MaxMs
constexpr Anchor kUniLfoScale{0.33f, 1.0f, 7.0f, I::Log};             // x lfoHz both: ~0.2..6 Hz
// Above today's rate the sweep narrows so the pitch swing stays within about twice
// today's (RATE 100 %: ~60 cents peak instead of ~200).
constexpr float kUniMaxSwingGrowth = 2.0f;

// Distortion
constexpr Anchor kDisInputHp{160.0f, 90.0f, 40.0f, I::Log};
constexpr Anchor kDisS1BassDb{-18.0f, -12.0f, -6.0f};
constexpr Anchor kDisStackBassDb{6.0f, 10.0f, 14.0f};
constexpr Anchor kDisBassPeakDb{-2.0f, 2.0f, 6.0f};
constexpr Anchor kDisS1Lp{3000.0f, 6000.0f, 12000.0f, I::Log};
constexpr Anchor kDisStackTrebleDb{-10.0f, -5.0f, 0.0f};
constexpr Anchor kDisS2Lp{4000.0f, 6000.0f, 10000.0f, I::Log};
constexpr Anchor kDisTrebleCutDb{-9.0f, -6.0f, -3.0f};
constexpr Anchor kDisRailAsym{0.0f, 0.05f, 0.2f};
constexpr Anchor kDisRailSoft{0.3f, 0.1f, 0.02f};

// CHASM
constexpr Anchor kChmWobble{0.0f, 0.15f, 0.83f};                    // 100 % = the old 90 % (owner 2026-10-02)
constexpr Anchor kChmTrebleLoss{1500.0f, 3000.0f, 7000.0f, I::Log};
constexpr Anchor kChmInputTrebleCut{0.85f, 0.95f, 1.0f};
constexpr Anchor kChmBassCut{400.0f, 200.0f, 80.0f, I::Log};
constexpr Anchor kChmBassCutTop{200.0f, 100.0f, 40.0f, I::Log};

// SPRING B
constexpr Anchor kSbChirpA{0.55f, 0.70f, 0.85f};
constexpr Anchor kSbChirpSections{12.0f, 24.0f, 32.0f, I::Round};
constexpr Anchor kSbWobbleDepth{2.0f, 8.0f, 24.0f};
constexpr Anchor kSbWobbleRate{1.5f, 3.0f, 5.0f, I::Log};
constexpr Anchor kSbDiffuse{0.45f, 0.30f, 0.15f};                    // less diffusion = crisper repeats
constexpr Anchor kSbTrebleLoss{3000.0f, 6000.0f, 10000.0f, I::Log};

// The default positions reproduce the engine tuning defaults; the --layout
// probe fails when they drift apart.

// ---- Macro positions ---------------------------------------------------------

enum Id : int {
  OctaveSlide,
  HarmonyTracking,
  UnisonBlend,
  UnisonMotion,
  DistBody,
  DistBite,
  DistGrit,
  ChasmWobble,
  ChasmBrightness,
  ChasmBass,
  SpringBDrip,
  SpringBFlutter,
  SpringBBrightness,
  kCount
};

// Names for the Print tuning line; reverb names carry the engine where two share one.
// nullptr = a panel knob, printed on the Knobs line instead.
constexpr const char* kPrintName[kCount] = {
    "Slide",          nullptr,"Chorus-Double", nullptr,       "Body",
    "Bite",           "Grit",           "Wobble",           "CHASM Brightness", "Bass",
    "SPRING B Drip",  "SPRING B Flutter", "SPRING B Brightness"};

// Keys for the saved-state file; one word each, stable across builds.
constexpr const char* kStateKey[kCount] = {
    "OctaveSlide",    "HarmonyTracking", "UnisonBlend",   "UnisonMotion",  "DistBody",
    "DistBite",       "DistGrit",        "ChasmWobble",   "ChasmBrightness", "ChasmBass",
    "SpringBDrip",    "SpringBFlutter",  "SpringBBrightness"};

// Adam's starting positions (owner 2026-10-02), in Id order.
constexpr std::array<float, kCount> kDefaultPos = {
    50.0f, 50.0f, 0.0f,  0.0f,  50.0f, 60.0f, 50.0f, 85.0f,
    85.0f, 50.0f, 50.0f, 50.0f, 50.0f};

struct State {
  std::array<float, kCount> pos;
  State() : pos(kDefaultPos) {}
};

// "Macros: Slide 50 %, Tracking speed 50 %, ..." with every macro, moved or not.
inline void formatAll(const State& s, char* buf, size_t size) {
  size_t used = static_cast<size_t>(std::snprintf(buf, size, "Macros:"));
  bool first = true;
  for (int i = 0; i < kCount && used < size; ++i) {
    if (kPrintName[i] == nullptr) continue;
    used += static_cast<size_t>(std::snprintf(buf + used, size - used, "%s %s %.0f %%",
                                              first ? "" : ",", kPrintName[i],
                                              static_cast<double>(s.pos[static_cast<size_t>(i)])));
    first = false;
  }
}

// ---- Apply: one function per macro -------------------------------------------

inline void octaveSlide(OctaveTuning& t, float pos) { t.glideMs = at(kOctSlide, pos); }

inline void harmonyTracking(HarmonyTuning& t, float pos) { t.glideMs = at(kHarGlide, pos); }

// Chorus <-> Double (blend) and RATE (rate) both shape the sweep, so one function
// writes every field they touch. Blend: detune grows and the swing shrinks.
// Rate: the LFOs speed up; past today's rate the swing narrows to cap the pitch swing.
// Unison at Chorus-Double 50 % and RATE 50 %; the macro scales from here.
struct UnisonBase {
  float lfoHz[2] = {0.60f, 0.90f};
  float swingMinMs = 0.5f, swingMaxMs = 3.0f;
};

inline void unison(UnisonTuning& t, float blendPos, float ratePos) {
  const UnisonBase base;
  const float d = at(kUniDetune, blendPos);
  const float m = at(kUniLfoScale, ratePos);
  const float swing = at(kUniSwingScale, blendPos) * (m > kUniMaxSwingGrowth ? kUniMaxSwingGrowth / m : 1.0f);
  t.detuneCents[0] = d;
  t.detuneCents[1] = 0.0f - d;  // +0, not -0, at no detune
  t.swingMinMs = base.swingMinMs * swing;
  t.swingMaxMs = base.swingMaxMs * swing;
  t.lfoHz[0] = base.lfoHz[0] * m;
  t.lfoHz[1] = base.lfoHz[1] * m;
}

inline void distBody(DistortionTuning& t, float pos) {
  t.inputHpHz = at(kDisInputHp, pos);
  t.s1BassDb = at(kDisS1BassDb, pos);
  t.stackBassDb = at(kDisStackBassDb, pos);
  t.bassPeakDb = at(kDisBassPeakDb, pos);
}

inline void distBite(DistortionTuning& t, float pos) {
  t.s1LpHz = at(kDisS1Lp, pos);
  t.stackTrebleDb = at(kDisStackTrebleDb, pos);
  t.s2LpHz = at(kDisS2Lp, pos);
  t.trebleCutDb = at(kDisTrebleCutDb, pos);
}

inline void distGrit(DistortionTuning& t, float pos) {
  t.railAsym = at(kDisRailAsym, pos);
  t.railSoft = at(kDisRailSoft, pos);
}

inline void chasmWobble(ChasmParams& p, float pos) { p.wobble = at(kChmWobble, pos); }

inline void chasmBrightness(ChasmTuning& t, float pos) {
  t.trebleLossHz = at(kChmTrebleLoss, pos);
  t.inputTrebleCut = at(kChmInputTrebleCut, pos);
}

inline void chasmBass(ChasmTuning& t, float pos) {
  t.bassCutHz = at(kChmBassCut, pos);
  t.bassCutHzTop = at(kChmBassCutTop, pos);
}

inline void springBDrip(SpringBTuning& t, float pos) {
  t.chirpA = at(kSbChirpA, pos);
  t.chirpSections = atInt(kSbChirpSections, pos);
}

inline void springBFlutter(SpringBTuning& t, float pos) {
  t.wobbleDepth = at(kSbWobbleDepth, pos);
  t.wobbleRateHz = at(kSbWobbleRate, pos);
  t.diffuse = at(kSbDiffuse, pos);
}

inline void springBBrightness(SpringBTuning& t, float pos) { t.trebleLossHz = at(kSbTrebleLoss, pos); }

}  // namespace cv::macros
