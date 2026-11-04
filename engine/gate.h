#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Gate: a peak detector drives a gain computer with hysteresis, hold, and attack/release smoothing
// of the gain in dB. The Gate card keeps the default tuning (a detector behind a one-pole high-pass
// and a soft knee). The slot-1 Input gate runs inputGateTuning() (a plain peak detector and a hard
// range). Both cards give attack and release as travel times.
//
// The detector reads the larger of two running maxima over consecutive segments of kDetectorSegSmp
// samples, so a sung note's pitch peaks hold the level between them and the hysteresis keeps its
// width. Samples the detector cannot use (NaN, infinity, magnitude past kDetectorInputCap) read as 0.
//
// The sample loop keeps its state in locals and takes a flat path while the gain sits at 0 dB or at
// the range. The dB conversions run through fastmath, which fastmath_test pins to libm.
// process() needs `in` and `out` to be distinct buffers.

namespace cv {

// The Input gate's pots. THRESHOLD runs linear in dB, DECAY (the release time) log.
constexpr float kInputGateThresholdMinDb = -80.0f;
constexpr float kInputGateThresholdMaxDb = -30.0f;
constexpr float kInputGateDecayMinMs = 20.0f;
constexpr float kInputGateDecayMaxMs = 2000.0f;

// The Gate card's pots, same laws.
constexpr float kGateThresholdMinDb = -70.0f;
constexpr float kGateThresholdMaxDb = -10.0f;
constexpr float kGateDecayMinMs = 5.0f;
constexpr float kGateDecayMaxMs = 1000.0f;

// The longest hold the engine accepts; longer values clamp to it.
constexpr float kGateHoldMaxMs = 10000.0f;

struct GateTuning {
  // The Gate card's defaults sit at the middle of its pots (Decay is the geometric middle).
  float attackMs = 1.0f;  // closed range to within 1 dB of open
  float holdMs = 150.0f;
  float releaseMs = __builtin_sqrtf(kGateDecayMinMs * kGateDecayMaxMs);  // open to within 1 dB of closed
  float rangeDb = -40.0f;       // attenuation when closed
  float kneeDb = 12.0f;         // soft knee width below the threshold; 0 is a hard range
  float detectorHpHz = 120.0f;  // detector side-chain highpass; 0 leaves the detector unfiltered
  float hysteresisDb = 6.0f;    // close level = open threshold - hysteresis
  float detectorAttackMs = 0.0f;    // detector rise time constant; 0 follows a rise at once
  float detectorReleaseMs = 20.0f;  // detector fall time constant
};

// The Input gate's tuning before its DECAY pot sets the release (spec 2026-10-30-input-conditioning).
// The 30 ms hold pays for the 13 to 27 ms the detector window adds to the fall.
inline GateTuning inputGateTuning() {
  GateTuning t;
  t.attackMs = 0.5f;
  t.holdMs = 30.0f;
  t.releaseMs = 200.0f;
  t.rangeDb = -80.0f;
  t.kneeDb = 0.0f;
  t.detectorHpHz = 0.0f;
  t.hysteresisDb = 3.0f;
  t.detectorAttackMs = 0.1f;
  t.detectorReleaseMs = 5.0f;
  return t;
}

// The length of the gain's own on/off fade. It equals the chain's slot fade, so a gate that is
// switched off is a plain copy by the time the slot fade ends.
constexpr float kGateFadeMs = 8.0f;

struct GateParams {
  bool on = true;  // fades the gain's effect in or out over kGateFadeMs
  float thresholdDb = 0.5f * (kGateThresholdMinDb + kGateThresholdMaxDb);
  GateTuning tuning;
};

class Gate {
 public:
  void reset() { st_ = State{}; }

  void process(const float* in, float* out, int n, const GateParams& p) {
    if (!lawValid_ || !sameLaw(p)) setLaw(p);
    const int fadeGoal = p.on ? kFadeSmp : 0;
    if (law_.detectorHp) run<true>(in, out, n, fadeGoal);
    else run<false>(in, out, n, fadeGoal);
  }

  // Applied gain in dB, 0 while off.
  float gainDb() const { return st_.gain * mixOf(st_.fade); }

  // The detector's state after the last sample: above the threshold, with the hysteresis. It runs
  // while the gate is off, so a later stage can use it as a key.
  bool open() const { return st_.open; }

  // The highest detector level (linear, full scale 1) since the previous call, and the window
  // restarts. No division, so the audio interrupt can call it.
  float takeDetectorPeak() {
    const float peak = fromBits(st_.peakBits);
    st_.peakBits = 0;
    return peak;
  }

 private:
  static constexpr float kFloor = 1e-9f;
  static constexpr float kKneeMinDb = 0.1f;

  // Detector window: one period of 75 Hz, so Adam's lowest note (78 Hz) keeps a peak in every window.
  static constexpr int kDetectorSegSmp = 640;
  static constexpr float kDetectorInputCap = 1e4f;  // +80 dBFS

  // Travel times. A one-pole covers a fraction f of a step in -ln(1 - f) time constants. A move ends
  // kSettleDb short of its goal, so its time constant is the travel time over ln(range / kSettleDb).
  static constexpr float kSettleDb = 1.0f;

  // The on/off fade is a linear ramp of the dB gain's weight.
  static constexpr int kFadeSmp = static_cast<int>(kGateFadeMs * kSmpPerMs);
  static constexpr float kFadeStep = 1.0f / static_cast<float>(kFadeSmp);

  // Inside this distance of a threshold the sample loop decides by dbFast. Outside it the linear
  // compare is certain, because dbFast sits within 2e-4 dB of 20 log10 (fastmath_test).
  static constexpr float kEdgeDb = 0.01f;

  // The gain computer's settings, worked out when a parameter changes.
  struct Law {
    float range, knee, openAt, closeAt, aAtt, aRel, aDetAtt, aDetRel, hpA;
    uint32_t openLo, openHi, closeLo, closeHi, kneeLo;  // linear level edges as bit patterns
    int holdSmp;
    bool detectorHp, detectorInstant;
  };

  // The sample loop's state.
  struct State {
    float env = 0.0f;
    float gain = 0.0f;  // dB, before the fade's weight
    float hpY = 0.0f;
    float g = 1.0f;     // dbToGainFast(gain * mixOf(fade)) as the last sample applied it
    float segMax = 0.0f;
    float segPrev = 0.0f;
    uint32_t peakBits = 0;
    int segPos = 0;
    int hold = 0;
    int fade = 0;  // 0 off .. kFadeSmp on
    bool open = false;
  };

  // The gain at 0 dB (Open) or at the range (Closed) stays there until the hold runs out or the level
  // crosses into the knee or past the threshold. A flat run skips the gain computer.
  enum class Flat { No, Open, Closed };

  // How the fade stands for a whole block. On multiplies the input by the gain, Off copies the input
  // and still runs the gain computer, and Fading moves the fade's weight a sample at a time.
  enum class Mode { On, Fading, Off };

  static CV_INLINE uint32_t bitsOf(float x) {
    uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    return u;
  }

  static float fromBits(uint32_t u) {
    float x;
    std::memcpy(&x, &u, sizeof x);
    return x;
  }

  static CV_INLINE float mixOf(int fade) { return fade == kFadeSmp ? 1.0f : static_cast<float>(fade) * kFadeStep; }

  // Bit pattern of the linear level db dB from full scale. The fast form is exact to 7.7e-7, far inside
  // kEdgeDb, and a Threshold that moves pays for five of these each block.
  static uint32_t levelBits(float db) { return bitsOf(dbToGainFast(db)); }

  bool sameLaw(const GateParams& p) const {
    const GateTuning& a = p.tuning;
    const GateTuning& b = lawKey_.tuning;
    return p.thresholdDb == lawKey_.thresholdDb && a.attackMs == b.attackMs && a.holdMs == b.holdMs &&
           a.releaseMs == b.releaseMs && a.rangeDb == b.rangeDb && a.kneeDb == b.kneeDb &&
           a.detectorHpHz == b.detectorHpHz && a.hysteresisDb == b.hysteresisDb &&
           a.detectorAttackMs == b.detectorAttackMs && a.detectorReleaseMs == b.detectorReleaseMs;
  }

  void setLaw(const GateParams& p) {
    const GateTuning& t = p.tuning;
    Law& law = law_;
    law.range = t.rangeDb < 0.0f ? t.rangeDb : 0.0f;
    law.knee = t.kneeDb <= 0.0f ? 0.0f : (t.kneeDb > kKneeMinDb ? t.kneeDb : kKneeMinDb);
    law.openAt = p.thresholdDb;
    law.closeAt = p.thresholdDb - (t.hysteresisDb > 0.0f ? t.hysteresisDb : 0.0f);

    // A range inside the settle margin has no travel to scale.
    const float travelCovered = -law.range / kSettleDb;
    const float travelConstants = travelCovered > 1.0f ? logf(travelCovered) : 1.0f;
    law.aAtt = smooth::coef(t.attackMs / 1000.0f / travelConstants);
    law.aRel = smooth::coef(t.releaseMs / 1000.0f / travelConstants);
    law.aDetAtt = smooth::coef(t.detectorAttackMs / 1000.0f);
    law.aDetRel = smooth::coef(t.detectorReleaseMs / 1000.0f);
    law.detectorInstant = law.aDetAtt >= 1.0f;
    law.detectorHp = t.detectorHpHz > 0.0f;
    law.hpA = onePoleCoef(t.detectorHpHz);
    law.holdSmp = static_cast<int>(clampf(t.holdMs, 0.0f, kGateHoldMaxMs) * kSmpPerMs);
    law.openLo = levelBits(law.openAt - kEdgeDb);
    law.openHi = levelBits(law.openAt + kEdgeDb);
    law.closeLo = levelBits(law.closeAt - kEdgeDb);
    law.closeHi = levelBits(law.closeAt + kEdgeDb);
    law.kneeLo = law.knee > 0.0f ? levelBits(law.openAt - law.knee - kEdgeDb) : 0xffffffffu;
    lawKey_ = p;
    lawValid_ = true;
  }

  // The detector for one sample, then the hysteresis. Returns the level's bit pattern.
  template <bool kHp>
  static CV_INLINE uint32_t detect(float in, State& s, const Law& law) {
    const float x = fabsf(in) < kDetectorInputCap ? in : 0.0f;
    float mag;
    if (kHp) {
      s.hpY += law.hpA * (x - s.hpY);
      mag = fabsf(x - s.hpY);
    } else {
      mag = fabsf(x);
    }
    s.segMax = fmaxf(s.segMax, mag);
    if (++s.segPos == kDetectorSegSmp) {
      s.segPrev = s.segMax;
      s.segMax = 0.0f;
      s.segPos = 0;
    }
    const float held = fmaxf(s.segMax, s.segPrev);
    if (bitsOf(held) <= bitsOf(s.env)) s.env += law.aDetRel * (held - s.env);
    else if (law.detectorInstant) s.env = held;
    else s.env += law.aDetAtt * (held - s.env);
    const uint32_t envBits = bitsOf(s.env);
    if (envBits > s.peakBits) s.peakBits = envBits;

    const float level = s.env + kFloor;
    const uint32_t lb = bitsOf(level);
    if (lb >= law.openHi || (lb >= law.openLo && dbFast(level) >= law.openAt)) s.open = true;
    else if (lb < law.closeLo || (lb < law.closeHi && dbFast(level) < law.closeAt)) s.open = false;
    return lb;
  }

  static CV_INLINE Flat flatAt(const State& s, const Law& law) {
    if (s.gain == 0.0f) return Flat::Open;
    return s.gain == law.range ? Flat::Closed : Flat::No;
  }

  // One flat sample. False ends the flat run, and the sample goes through stepSample.
  static CV_INLINE bool flatSample(Flat& flat, uint32_t lb, State& s, const Law& law) {
    if (flat == Flat::Open) {
      if (s.open) {
        s.hold = law.holdSmp;
        return true;
      }
      if (s.hold > 0) {
        --s.hold;
        return true;
      }
    } else if (!s.open && lb < law.kneeLo) {
      s.hold = law.holdSmp;
      return true;
    }
    flat = Flat::No;
    return false;
  }

  // The gain computer's target: 0 dB open, the range closed, the soft knee in between.
  static CV_INLINE float targetOf(uint32_t lb, const State& s, const Law& law) {
    if (s.open) return 0.0f;
    if (lb < law.kneeLo) return law.range;
    float levelDb = dbFast(fromBits(lb));

    // Keeps clang's AArch64 build from fusing the subtraction below with the dbFast multiply. The
    // Cortex-M7 build compiles this line out.
#if defined(__aarch64__)
    asm("" : "+w"(levelDb));
#endif
    const float below = law.openAt - levelDb;
    return below >= law.knee || levelDb >= law.openAt ? law.range : law.range * below / law.knee;
  }

  // Attack, hold and release on the gain in dB, then the fade and the applied gain.
  template <Mode kMode>
  static CV_INLINE void stepSample(uint32_t lb, State& s, const Law& law, int fadeGoal) {
    const float target = targetOf(lb, s, law);
    bool moved = true;
    if (target >= s.gain) {
      s.hold = law.holdSmp;
      s.gain = smooth::step(s.gain, target, law.aAtt);
    } else if (s.hold > 0) {
      --s.hold;
      moved = false;
    } else {
      s.gain = smooth::step(s.gain, target, law.aRel);
    }
    if (kMode == Mode::Off) return;  // a fade weight of 0 leaves g at 1
    if (kMode == Mode::Fading) {
      if (s.fade != fadeGoal) s.fade += s.fade < fadeGoal ? 1 : -1;
      moved = true;
    }
    if (moved) s.g = dbToGainFast(s.gain * mixOf(s.fade));
  }

  template <bool kHp, Mode kMode>
  static CV_INLINE void sampleRun(const float* in, float* CV_RESTRICT out, int n, State& s, const Law& law,
                                  int fadeGoal) {
    Flat flat = kMode == Mode::Fading ? Flat::No : flatAt(s, law);
    for (int i = 0; i < n; ++i) {
      const float x = in[i];
      const uint32_t lb = detect<kHp>(x, s, law);
      if (flat == Flat::No || !flatSample(flat, lb, s, law)) stepSample<kMode>(lb, s, law, fadeGoal);
      out[i] = kMode == Mode::Off ? x : x * s.g;
    }
  }

  // Out of line, one call a block, so each mode's loop gets its own registers.
  template <bool kHp, Mode kMode>
  __attribute__((noinline)) static void sampleLoop(const float* in, float* CV_RESTRICT out, int n, State& state,
                                                   const Law& lawIn, int fadeGoal) {
    State s = state;
    const Law law = lawIn;
    sampleRun<kHp, kMode>(in, out, n, s, law, fadeGoal);
    state = s;
  }

  template <bool kHp>
  void run(const float* in, float* out, int n, int fadeGoal) {
    if (st_.fade != fadeGoal) sampleLoop<kHp, Mode::Fading>(in, out, n, st_, law_, fadeGoal);
    else if (fadeGoal == 0) sampleLoop<kHp, Mode::Off>(in, out, n, st_, law_, fadeGoal);
    else sampleLoop<kHp, Mode::On>(in, out, n, st_, law_, fadeGoal);
  }

  Law law_{};
  GateParams lawKey_;  // the parameters law_ was worked out from
  bool lawValid_ = false;
  State st_;
};

}  // namespace cv
