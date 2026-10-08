#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/shifters.h"
#include "engine/smooth.h"

// Harmony, modelled on the Zoom V3: up to three diatonic voices parallel to the
// sung pitch, a KEY that picks the scale, and MIX between dry and harmony.
// Chromatic mode swaps the key for fixed intervals. All voices run on one
// shifter engine: A grid PSOLA, B epoch PSOLA + formant, C granular.
// PitchFx owns the ring, the tracker, and the mix.

namespace cv {

enum class HarmonyVoice : uint8_t { Lower = 0, Low, Fixed, High, Higher };

struct HarmonySlot {
  HarmonyVoice voice = HarmonyVoice::High;
  int level = 0;                 // 0 off, 1 low, 2 medium, 3 high
  float formant = 0.0f;          // engine B only, -12..12 semitones
};

struct HarmonyTuning {
  float levelDb[3] = {-12.0f, -6.0f, 0.0f};   // low / medium / high
  float glideMs = 30.0f;                      // interval change glide
  float voicedThreshold = 0.15f;
  bool muteUnvoiced = false;                  // ignored on engine C
  bool snapToScale = false;
  // Semitones relative to the sung note, indexed [voice][scale degree 0..6].
  // Fixed has no row; it is computed.
  int8_t lower[7]  = {-7, -7, -7, -6, -7, -7, -7};
  int8_t low[7]    = {-3, -3, -4, -3, -3, -4, -4};
  int8_t high[7]   = { 4,  3,  3,  4,  4,  3,  3};
  int8_t higher[7] = { 7,  7,  7,  7,  7,  7,  6};
  // Chromatic intervals, indexed by voice; Fixed is unused.
  int8_t chromaticSemis[5] = {-7, -4, 0, 4, 7};
  // Per-engine trims on top of levelDb; level rule at mix 1 (wet only), one High voice.
  float trimDb[3] = {2.2f, 1.4f, 2.2f};
  ShifterTuning shifter;         // engines B and C
};

struct HarmonyParams {
  bool on = true;                // panel toggle; off fades the stage out
  int engine = 0;                // 0 = A (grid PSOLA), 1 = B (epoch PSOLA + formant), 2 = C (granular)
  bool chromatic = false;        // fixed intervals from tuning.chromaticSemis; key ignored
  int key = 0;                   // encoder index 0..11, see kKeyRoot / kKeyName
  float mix = 0.25f;             // 0 dry .. 1 harmony only
  std::array<HarmonySlot, 3> slots{};
  HarmonyTuning tuning;
};

// Encoder order follows the V3 knob: circle of fifths from C.
constexpr int kKeyRoot[12]        = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
constexpr const char* kKeyName[12] = {"C / Am", "G / Em", "D / Bm", "A / F#m",
  "E / C#m", "B / G#m", "F# / D#m", "Db / Bbm", "Ab / Fm", "Eb / Cm", "Bb / Gm",
  "F / Dm"};

// All harmony voices for one block at a time. The caller owns the ring, the
// tracker, and the dry/wet mix.
class HarmonyVoices {
 public:
  static constexpr int kSlots = 3;
  static constexpr float kMaxFormantSemis = 12.0f;

  HarmonyVoices() { reset(); }

  void reset() {
    resetA();
    for (EpochShifter& v : epoch_) v.reset();
    for (GrainShifter& v : grain_) v.reset();
    engine_ = 0;
  }

  // Called once per block before ticking. Sets the selected engine's targets;
  // returns true if any slot is on. Switching engines resets the ones left
  // behind so they start clean next time. corrOffset is Autotune's correction
  // in semitones; the voices build on the corrected note.
  bool prepare(const PitchResult& pr, const HarmonyParams& p, float corrOffset = 0.0f) {
    const int engine = p.engine == 1 || p.engine == 2 ? p.engine : 0;
    if (engine != engine_) {
      if (engine != 0) resetA();
      if (engine != 1)
        for (EpochShifter& v : epoch_) v.reset();
      if (engine != 2)
        for (GrainShifter& v : grain_) v.reset();
      engine_ = engine;
    }
    const HarmonyTuning& t = p.tuning;
    const ShifterTuning& st = t.shifter;
    // C needs no pitch, so unvoiced input never mutes it.
    const bool voiced = pr.voiced || engine_ == 2;
    const float hz = pr.hz * exp2f(corrOffset / 12.0f);
    bool any = false;
    // Summed voices scale by 1/sqrt(active count) so stacking voices does not add level.
    int active = 0;
    for (int v = 0; v < kSlots; ++v)
      if (p.on && targetGain(p.slots[v].level, voiced, 0.0f, t) > 0.0f) ++active;
    const float norm = active > 1 ? 1.0f / sqrtf(static_cast<float>(active)) : 1.0f;
    for (int v = 0; v < kSlots; ++v) {
      const HarmonySlot& slot = p.slots[v];
      semisT_[v] = (p.chromatic ? chromaticSemis(slot.voice, t)
                                : targetSemis(slot.voice, hz, p.key, t)) +
                   corrOffset;
      gainT_[v] = p.on ? norm * targetGain(slot.level, voiced, t.trimDb[engine_], t) : 0.0f;
      any = any || (p.on && slot.level > 0);
      if (engine_ == 1) {
        const float fm = slot.formant < -kMaxFormantSemis
                             ? -kMaxFormantSemis
                             : (slot.formant > kMaxFormantSemis ? kMaxFormantSemis : slot.formant);
        epoch_[v].prepare(semisT_[v], fm, gainT_[v], t.glideMs, st.grainPeriods, st.epochSearch);
      } else if (engine_ == 2) {
        grain_[v].prepare(semisT_[v], gainT_[v], t.glideMs, st.grainWindowMs, st.grainCount);
      } else {
        // Smoothers start on target after reset; silent voices jump to their
        // new interval instead of gliding in from a stale one.
        if (fresh_ || gain_[v] == 0.0f) semis_[v] = semisT_[v];
        if (fresh_) gain_[v] = gainT_[v];
      }
    }
    if (engine_ == 0) fresh_ = false;
    aGlide_ = smooth::coef(t.glideMs * 0.001f);
    return any;
  }

  // One sample of all voices summed, gains and glide applied. lp is the ring
  // low-passed for engine B's peak search.
  float tick(const VoiceRing& ring, const VoiceRing& lp, long writeCount, float period) {
    if (engine_ == 1) {
      float wet = 0.0f;
      for (EpochShifter& v : epoch_) wet += v.tick(ring, lp, writeCount, period);
      return wet;
    }
    if (engine_ == 2) {
      float wet = 0.0f;
      for (GrainShifter& v : grain_) wet += v.tick(ring, writeCount);
      return wet;
    }
    float wet = 0.0f;
    for (int v = 0; v < kSlots; ++v) {
      gain_[v] = smooth::step(gain_[v], gainT_[v], smooth::kSmoothCoef);
      semis_[v] = smooth::step(semis_[v], semisT_[v], aGlide_);
      if (gain_[v] == 0.0f) continue;
      const float ratio = exp2f(semis_[v] / 12.0f);
      wet += gain_[v] * voices_[v].tick(ring, writeCount, period, ratio);
    }
    return wet;
  }

  struct Degree {
    int index;
    float dev;  // sung pitch minus the scale note, semitones
  };

  // Nearest major-scale degree by circular distance; ties snap down.
  // Autotune shares it.
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

 private:
  static constexpr int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
  static constexpr float kTieEps = 1e-4f;

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

  static float chromaticSemis(HarmonyVoice voice, const HarmonyTuning& t) {
    const int v = static_cast<int>(voice);
    return v < 5 ? static_cast<float>(t.chromaticSemis[v]) : 0.0f;
  }

  static float targetGain(int level, bool voiced, float trimDb, const HarmonyTuning& t) {
    if (level <= 0 || (t.muteUnvoiced && !voiced)) return 0.0f;
    const int idx = level > 3 ? 2 : level - 1;
    return powf(10.0f, (t.levelDb[idx] + trimDb) / 20.0f);
  }

  // Engine A's voices and smoothers.
  void resetA() {
    for (PsolaVoice& v : voices_) v.reset();
    semis_.fill(0.0f);
    gain_.fill(0.0f);
    semisT_.fill(0.0f);
    gainT_.fill(0.0f);
    aGlide_ = 1.0f;
    fresh_ = true;
  }

  std::array<PsolaVoice, kSlots> voices_{};
  std::array<EpochShifter, kSlots> epoch_{};
  std::array<GrainShifter, kSlots> grain_{};
  std::array<float, kSlots> semis_{};
  std::array<float, kSlots> gain_{};
  std::array<float, kSlots> semisT_{};
  std::array<float, kSlots> gainT_{};
  float aGlide_ = 1.0f;
  int engine_ = 0;
  bool fresh_ = true;
};

}  // namespace cv
