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
#include "engine/spring.h"
#include "engine/spring_c.h"
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

// SPRING
constexpr Anchor kSprHfMixDbLo{-40.0f, -22.0f, -10.0f};
constexpr Anchor kSprHfMixDbHi{-30.0f, -14.0f, -4.0f};
constexpr Anchor kSprRipple{0.03f, 0.10f, 0.30f};
constexpr Anchor kSprDiffuse{0.0f, 0.0f, 0.7f};
constexpr Anchor kSprHfSections{0.0f, 0.0f, 120.0f, I::Round};
constexpr Anchor kSprModDepth{2.0f, 8.0f, 24.0f};
constexpr Anchor kSprModRate{1.5f, 3.0f, 5.0f, I::Log};
constexpr Anchor kSprSprings{2.0f, 2.0f, 3.0f, I::Step};
constexpr Anchor kSprHp{600.0f, 300.0f, 120.0f, I::Log};
constexpr Anchor kSprBoingDb{0.0f, 0.0f, 6.0f};

// CHASM
constexpr Anchor kChmWobble{0.0f, 0.15f, 1.0f};                     // 100 % = full engine range, as the old WOBBLE knob
constexpr Anchor kChmTrebleLoss{1500.0f, 3000.0f, 7000.0f, I::Log};
constexpr Anchor kChmInputTrebleCut{0.85f, 0.95f, 1.0f};
constexpr Anchor kChmBassCut{400.0f, 200.0f, 80.0f, I::Log};
constexpr Anchor kChmBassCutTop{200.0f, 100.0f, 40.0f, I::Log};

// PARKER SPRING
constexpr Anchor kPrkHfMixDb{-40.0f, -22.0f, -8.0f};
constexpr Anchor kPrkEcho{0.05f, 0.2f, 0.3f};
constexpr Anchor kPrkRipple{0.05f, 0.2f, 0.3f};
constexpr Anchor kPrkPresenceDb{1.0f, 5.0f, 8.0f};
constexpr Anchor kPrkALf{0.55f, 0.70f, 0.82f};
constexpr Anchor kPrkMLow{60.0f, 100.0f, 100.0f, I::Round};
constexpr Anchor kPrkSpread{0.3f, 1.0f, 1.6f};                        // k in tdFactor
constexpr Anchor kPrkModDepth{2.0f, 8.0f, 16.0f};
constexpr Anchor kPrkLp{5000.0f, 9000.0f, 14000.0f, I::Log};
constexpr Anchor kPrkPresenceHz{2500.0f, 3000.0f, 4000.0f, I::Log};

// The middle anchors are today's defaults; a drift fails the build.
static_assert(kOctSlide.mid == OctaveTuning{}.glideMs, "octave slide");
static_assert(kHarGlide.mid == HarmonyTuning{}.glideMs, "harmony glide");
static_assert(kUniDetune.mid == UnisonTuning{}.detuneCents[0], "unison detune");
static_assert(kDisInputHp.mid == DistortionTuning{}.inputHpHz, "dist input hp");
static_assert(kDisS1BassDb.mid == DistortionTuning{}.s1BassDb, "dist s1 bass");
static_assert(kDisStackBassDb.mid == DistortionTuning{}.stackBassDb, "dist stack bass");
static_assert(kDisBassPeakDb.mid == DistortionTuning{}.bassPeakDb, "dist bass peak");
static_assert(kDisS1Lp.mid == DistortionTuning{}.s1LpHz, "dist s1 lp");
static_assert(kDisStackTrebleDb.mid == DistortionTuning{}.stackTrebleDb, "dist stack treble");
static_assert(kDisS2Lp.mid == DistortionTuning{}.s2LpHz, "dist s2 lp");
static_assert(kDisTrebleCutDb.mid == DistortionTuning{}.trebleCutDb, "dist treble cut");
static_assert(kDisRailAsym.mid == DistortionTuning{}.railAsym, "dist rail asym");
static_assert(kDisRailSoft.mid == DistortionTuning{}.railSoft, "dist rail soft");
static_assert(kSprHfMixDbLo.mid == SpringTuning{}.hfMixDbLo, "spring hf lo");
static_assert(kSprHfMixDbHi.mid == SpringTuning{}.hfMixDbHi, "spring hf hi");
static_assert(kSprRipple.mid == SpringTuning{}.rippleGain, "spring ripple");
static_assert(kSprDiffuse.mid == SpringTuning{}.splashDiffuse, "spring diffuse");
static_assert(kSprHfSections.mid == static_cast<float>(SpringTuning{}.hfSections), "spring sections");
static_assert(kSprModDepth.mid == SpringTuning{}.modDepth, "spring mod depth");
static_assert(kSprModRate.mid == SpringTuning{}.modRateHz, "spring mod rate");
static_assert(kSprSprings.mid == static_cast<float>(SpringTuning{}.springs), "spring springs");
static_assert(kSprHp.mid == SpringTuning{}.hpHz, "spring hp");
static_assert(kSprBoingDb.mid == SpringTuning{}.boingDb, "spring boing");
static_assert(kChmWobble.mid == ChasmParams{}.wobble, "chasm wobble");
static_assert(kChmTrebleLoss.mid == ChasmTuning{}.trebleLossHz, "chasm treble loss");
static_assert(kChmInputTrebleCut.mid == ChasmTuning{}.inputTrebleCut, "chasm input cut");
static_assert(kChmBassCut.mid == ChasmTuning{}.bassCutHz, "chasm bass cut");
static_assert(kChmBassCutTop.mid == ChasmTuning{}.bassCutHzTop, "chasm bass cut top");
static_assert(kPrkHfMixDb.mid == SpringCTuning{}.hfMixDb, "parker hf mix");
static_assert(kPrkEcho.mid == SpringCTuning{}.echoGain, "parker echo");
static_assert(kPrkRipple.mid == SpringCTuning{}.rippleGain, "parker ripple");
static_assert(kPrkPresenceDb.mid == SpringCTuning{}.presenceDb, "parker presence db");
static_assert(kPrkALf.mid == SpringCTuning{}.aLf, "parker aLf");
static_assert(kPrkMLow.mid == static_cast<float>(SpringCTuning{}.mLow), "parker mLow");
static_assert(kPrkModDepth.mid == SpringCTuning{}.modDepth, "parker mod depth");
static_assert(kPrkLp.mid == SpringCTuning{}.lpHz, "parker lp");
static_assert(kPrkPresenceHz.mid == SpringCTuning{}.presenceHz, "parker presence hz");

// ---- Macro positions ---------------------------------------------------------

enum Id : int {
  OctaveSlide,
  HarmonyTracking,
  UnisonBlend,
  UnisonMotion,
  DistBody,
  DistBite,
  DistGrit,
  SpringSplash,
  SpringFlutter,
  SpringLowEnd,
  ChasmWobble,
  ChasmBrightness,
  ChasmBass,
  ParkerSplash,
  ParkerDrip,
  ParkerFlutter,
  ParkerBrightness,
  kCount
};

// Names for the Print tuning line; reverb names carry the engine where two share one.
// nullptr = a panel knob, printed on the Knobs line instead.
constexpr const char* kPrintName[kCount] = {
    "Slide",          "Tracking speed", "Chorus-Double", nullptr,       "Body",
    "Bite",           "Grit",           "SPRING Splash",    "SPRING Flutter", "SPRING Low end",
    "Wobble",         "CHASM Brightness", "Bass",           "PARKER Splash",  "Drip",
    "PARKER Flutter", "PARKER Brightness"};

// Keys for the saved-state file; one word each, stable across builds.
constexpr const char* kStateKey[kCount] = {
    "OctaveSlide",    "HarmonyTracking", "UnisonBlend",   "UnisonMotion",  "DistBody",
    "DistBite",       "DistGrit",        "SpringSplash",  "SpringFlutter", "SpringLowEnd",
    "ChasmWobble",    "ChasmBrightness", "ChasmBass",     "ParkerSplash",  "ParkerDrip",
    "ParkerFlutter",  "ParkerBrightness"};

struct State {
  std::array<float, kCount> pos;
  State() { pos.fill(kCenter); }
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
inline void unison(UnisonTuning& t, float blendPos, float ratePos) {
  const UnisonTuning base;
  const float d = at(kUniDetune, blendPos);
  const float m = at(kUniLfoScale, ratePos);
  const float swing = at(kUniSwingScale, blendPos) * (m > kUniMaxSwingGrowth ? kUniMaxSwingGrowth / m : 1.0f);
  t.detuneCents[0] = d;
  t.detuneCents[1] = -d;
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

inline void springSplash(SpringTuning& t, float pos) {
  t.hfMixDbLo = at(kSprHfMixDbLo, pos);
  t.hfMixDbHi = at(kSprHfMixDbHi, pos);
  t.rippleGain = at(kSprRipple, pos);
  t.splashDiffuse = at(kSprDiffuse, pos);
  t.hfSections = atInt(kSprHfSections, pos);
}

inline void springFlutter(SpringTuning& t, float pos) {
  t.modDepth = at(kSprModDepth, pos);
  t.modRateHz = at(kSprModRate, pos);
  t.springs = atInt(kSprSprings, pos);
}

inline void springLowEnd(SpringTuning& t, float pos) {
  t.hpHz = at(kSprHp, pos);
  t.boingDb = at(kSprBoingDb, pos);
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

inline void parkerSplash(SpringCTuning& t, float pos) {
  t.hfMixDb = at(kPrkHfMixDb, pos);
  t.echoGain = at(kPrkEcho, pos);
  t.rippleGain = at(kPrkRipple, pos);
  t.presenceDb = at(kPrkPresenceDb, pos);
}

inline void parkerDrip(SpringCTuning& t, float pos) {
  t.aLf = at(kPrkALf, pos);
  t.mLow = atInt(kPrkMLow, pos);
}

// Spread k widens the three springs' delay factors around 1.
inline void parkerFlutter(SpringCTuning& t, float pos) {
  const float k = at(kPrkSpread, pos);
  t.tdFactor = {1.0f, 1.0f + 0.15f * k, 1.0f - 0.12f * k};
  t.modDepth = at(kPrkModDepth, pos);
}

inline void parkerBrightness(SpringCTuning& t, float pos) {
  t.lpHz = at(kPrkLp, pos);
  t.presenceHz = at(kPrkPresenceHz, pos);
}

}  // namespace cv::macros
