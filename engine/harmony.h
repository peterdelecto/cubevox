#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/pitch.h"
#include "engine/psola.h"

// Harmony, modelled on the Zoom V3: up to two diatonic voices parallel to the
// sung pitch, a KEY that picks the scale, and MIX between dry and harmony.

namespace cv {

enum class HarmonyVoice : uint8_t { Lower = 0, Low, Fixed, High, Higher };

struct HarmonySlot {
  HarmonyVoice voice = HarmonyVoice::High;
  int level = 0;                 // 0 off, 1 low, 2 medium, 3 high
};

struct HarmonyTuning {
  float levelDb[3] = {-12.0f, -6.0f, 0.0f};   // low / medium / high
  float glideMs = 15.0f;                      // interval change glide
  float voicedThreshold = 0.15f;
  bool muteUnvoiced = false;
  bool snapToScale = false;
  // Semitones relative to the sung note, indexed [voice][scale degree 0..6].
  // Fixed has no row; it is computed.
  int8_t lower[7]  = {-7, -7, -7, -6, -7, -7, -7};
  int8_t low[7]    = {-3, -3, -4, -3, -3, -4, -4};
  int8_t high[7]   = { 4,  3,  3,  4,  4,  3,  3};
  int8_t higher[7] = { 7,  7,  7,  7,  7,  7,  6};
};

struct HarmonyParams {
  int key = 0;                   // encoder index 0..11, see kKeyRoot / kKeyName
  float mix = 0.5f;              // 0 dry .. 1 harmony only
  std::array<HarmonySlot, 2> slots{};
  HarmonyTuning tuning;
};

// Encoder order follows the V3 knob: circle of fifths from C.
constexpr int kKeyRoot[12]        = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
constexpr const char* kKeyName[12] = {"C / Am", "G / Em", "D / Bm", "A / F#m",
  "E / C#m", "B / G#m", "F# / D#m", "Db / Bbm", "Ab / Fm", "Eb / Cm", "Bb / Gm",
  "F / Dm"};

class Harmony {
 public:
  Harmony() { reset(); }

  void reset() {
    ring_.fill(0.0f);
    writeCount_ = 0;
    tracker_.reset();
    for (PsolaVoice& v : voices_) v.reset();
    semis_.fill(0.0f);
    gain_.fill(0.0f);
    mix_ = 0.0f;
    active_ = 0.0f;
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const HarmonyParams& p) {
    const HarmonyTuning& t = p.tuning;
    tracker_.push(in, n, t.voicedThreshold);
    const PitchResult& pr = tracker_.result();

    std::array<float, 2> semisT{};
    std::array<float, 2> gainT{};
    bool any = false;
    for (int v = 0; v < 2; ++v) {
      semisT[v] = targetSemis(p.slots[v].voice, pr.hz, p.key, t);
      gainT[v] = targetGain(p.slots[v].level, pr.voiced, t);
      any = any || p.slots[v].level > 0;
    }
    const float mixT = clamp01(p.mix);
    const float activeT = any ? 1.0f : 0.0f;
    settle(semisT, gainT, mixT, activeT);

    const float aSmooth = coef(kSmoothSec);
    const float aGlide = coef(t.glideMs * 0.001f);
    for (int i = 0; i < n; ++i) {
      ring_[writeCount_] = in[i];
      float wet = 0.0f;
      for (int v = 0; v < 2; ++v) {
        gain_[v] = step(gain_[v], gainT[v], aSmooth);
        semis_[v] = step(semis_[v], semisT[v], aGlide);
        if (gain_[v] == 0.0f) continue;
        const float ratio = exp2f(semis_[v] / 12.0f);
        wet += gain_[v] * voices_[v].tick(ring_, writeCount_, pr.period, ratio);
      }
      mix_ = step(mix_, mixT, aSmooth);
      active_ = step(active_, activeT, aSmooth);
      const float dry = 1.0f + (cosf(mix_ * kHalfPi) - 1.0f) * active_;
      const float wetGain = sinf(mix_ * kHalfPi) * active_;
      out[i] = dry * in[i] + wetGain * wet;
      // Voices only use writeCount modulo the ring, so wrapping here is exact.
      writeCount_ = writeCount_ + 1 == kVoiceRingLen ? 0 : writeCount_ + 1;
    }
  }

  const PitchResult& pitch() const { return tracker_.result(); }

 private:
  struct Degree {
    int index;
    float dev;  // sung pitch minus the scale note, semitones
  };

  static constexpr int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
  static constexpr float kHalfPi = 1.57079632679489661923f;
  static constexpr float kSmoothSec = 0.020f;
  static constexpr float kSnap = 1e-6f;
  static constexpr float kTieEps = 1e-4f;

  static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

  static float coef(float sec) {
    return sec <= 0.0f ? 1.0f : 1.0f - expf(-1.0f / (sec * kSampleRate));
  }

  // One-pole that lands exactly on its target, so settled bypass is bit-exact.
  static float step(float x, float target, float a) {
    x += a * (target - x);
    return fabsf(target - x) < kSnap ? target : x;
  }

  // Smoothers start on target after reset; silent voices jump to their new
  // interval instead of gliding in from a stale one.
  void settle(const std::array<float, 2>& semisT, const std::array<float, 2>& gainT,
              float mixT, float activeT) {
    for (int v = 0; v < 2; ++v) {
      if (fresh_ || gain_[v] == 0.0f) semis_[v] = semisT[v];
      if (fresh_) gain_[v] = gainT[v];
    }
    if (fresh_) {
      mix_ = mixT;
      active_ = activeT;
      fresh_ = false;
    }
  }

  // Nearest major-scale degree by circular distance; ties snap down.
  static Degree degreeOf(float hz, int key) {
    const int k = ((key % 12) + 12) % 12;
    const float midi = 69.0f + 12.0f * log2f(hz / 440.0f);
    float rel = fmodf(midi - kKeyRoot[k], 12.0f);
    if (rel < 0.0f) rel += 12.0f;
    Degree best{0, 0.0f};
    float bestDist = 99.0f;
    for (int d = 0; d < 7; ++d) {
      float s = rel - kMajor[d];
      if (s >= 6.0f) s -= 12.0f;
      if (s < -6.0f) s += 12.0f;
      const float dist = fabsf(s);
      const bool tie = fabsf(dist - bestDist) <= kTieEps;
      if ((dist < bestDist && !tie) || (tie && s > 0.0f)) {
        best = {d, s};
        bestDist = dist;
      }
    }
    return best;
  }

  static float targetSemis(HarmonyVoice voice, float hz, int key, const HarmonyTuning& t) {
    if (hz <= 0.0f) return 0.0f;
    const Degree deg = degreeOf(hz, key);
    int interval = 0;
    switch (voice) {
      case HarmonyVoice::Lower: interval = t.lower[deg.index]; break;
      case HarmonyVoice::Low: interval = t.low[deg.index]; break;
      case HarmonyVoice::Fixed: interval = deg.index == 0 ? -12 : -kMajor[deg.index]; break;
      case HarmonyVoice::High: interval = t.high[deg.index]; break;
      case HarmonyVoice::Higher: interval = t.higher[deg.index]; break;
    }
    return t.snapToScale ? static_cast<float>(interval) - deg.dev : static_cast<float>(interval);
  }

  static float targetGain(int level, bool voiced, const HarmonyTuning& t) {
    if (level <= 0 || (t.muteUnvoiced && !voiced)) return 0.0f;
    const int idx = level > 3 ? 2 : level - 1;
    return powf(10.0f, t.levelDb[idx] / 20.0f);
  }

  VoiceRing ring_{};
  PitchTracker tracker_;
  std::array<PsolaVoice, 2> voices_{};
  std::array<float, 2> semis_{};
  std::array<float, 2> gain_{};
  float mix_ = 0.0f;
  float active_ = 0.0f;
  long writeCount_ = 0;
  bool fresh_ = true;
};

}  // namespace cv
