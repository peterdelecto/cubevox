#pragma once

#include <array>
#include <cmath>
#include <cstring>

#include "engine/common.h"
#include "engine/deesser.h"
#include "engine/dsp.h"
#include "engine/smooth.h"

// VOX EQ voiced for Adam's voice, one stage in two places under one menu row with three settings.
// The front half runs ahead of slot 1 and holds a 40 Hz subsonic high-pass and a de-esser, plus a
// 70 Hz high-pass and a 6 kHz low-pass (the cuts) when `cuts` is set. The back half runs after
// POLISH COMP and holds a 300 Hz dip, a 3.5 kHz presence peak and the trim.
//
// The subsonic high-pass sits below any note Adam sings. Rumble and breath wind down there carry
// no loudness, but they count in RMS and POLISH COMP's makeup lifts them in the pauses, so without
// the filter the stage reads up to 1 dB high on a rumbly take.
//
// Each half crossfades with its input through its own 20 ms smoother on `on`, so a click fades both
// together. The cuts crossfade through a smoother of their own on `on` and `cuts`, so a change
// between ON and ON+CUTS fades too. Settled off, each half is a bit-exact copy and holds no filter
// state, and settled without the cuts the front half holds no filter state for them.
// RBJ biquads, TDF-II. The back half rebuilds its coefficients only when a setting changes.
// Specs docs/specs/2026-10-31-polish-eq-adam-voice-design.md and docs/specs/2026-11-05-vox-eq-design.md.

namespace cv {

// Front cutoffs. The three filters are 4th-order Butterworth, two biquads each. The subsonic
// high-pass runs whenever the stage is on, and the 70 Hz high-pass and the low-pass only with the cuts.
constexpr float kPolishSubsonicHz = 40.0f;
constexpr float kPolishHighPassHz = 70.0f;
constexpr float kPolishLowPassHz = 6000.0f;

// Section Qs of a 4th-order Butterworth, 1 / (2 cos(pi / 8)) and 1 / (2 cos(3 pi / 8)).
constexpr float kButterworth4Q1 = 0.54119610f;
constexpr float kButterworth4Q2 = 1.30656296f;

// The ranges of a back-half setting. The box and the emulator stay inside them, and cubevox-render
// refuses a value outside them.
constexpr float kPolishDipHzMin = 150.0f;
constexpr float kPolishDipHzMax = 600.0f;
constexpr float kPolishDipDbMin = -6.0f;
constexpr float kPolishDipDbMax = 0.0f;
constexpr float kPolishPresenceHzMin = 2000.0f;
constexpr float kPolishPresenceHzMax = 6000.0f;
constexpr float kPolishPresenceDbMin = 0.0f;
constexpr float kPolishPresenceDbMax = 6.0f;
constexpr float kPolishQMax = 10.0f;
constexpr float kPolishTrimMaxDb = 12.0f;

struct PolishTuning {
  float dipQ = 1.0f;
  float presenceQ = 0.7f;
  float trimDb = 1.3f;  // active back half only; level rule on program material
  DeEsserTuning deEsser;
};

struct PolishParams {
  bool on = true;
  bool cuts = true;  // the 70 Hz high-pass and 6 kHz low-pass ahead of the de-esser; read only while `on`
  float dipHz = 300.0f;
  float dipDb = -2.5f;
  float presenceHz = 3500.0f;
  float presenceDb = 1.5f;
  PolishTuning tuning;
};

// True when every back-half and de-esser setting sits inside its range.
inline bool polishParamsInRange(const PolishParams& p) {
  const auto within = [](float v, float lo, float hi) { return v >= lo && v <= hi; };
  const PolishTuning& t = p.tuning;
  return within(p.dipHz, kPolishDipHzMin, kPolishDipHzMax) && within(p.dipDb, kPolishDipDbMin, kPolishDipDbMax) &&
         within(p.presenceHz, kPolishPresenceHzMin, kPolishPresenceHzMax) &&
         within(p.presenceDb, kPolishPresenceDbMin, kPolishPresenceDbMax) && within(t.dipQ, kPeakMinQ, kPolishQMax) &&
         within(t.presenceQ, kPeakMinQ, kPolishQMax) && within(t.trimDb, -kPolishTrimMaxDb, kPolishTrimMaxDb) &&
         deEsserTuningInRange(t.deEsser);
}

class Polish {
 public:
  Polish() {
    for (int i = 0; i < 2; ++i) {
      const float q = i == 0 ? kButterworth4Q1 : kButterworth4Q2;
      hp_[static_cast<size_t>(i)].setHighPass(kPolishHighPassHz, q);
      lp_[static_cast<size_t>(i)].setLowPass(kPolishLowPassHz, q);
      sub_[static_cast<size_t>(i)].setHighPass(kPolishSubsonicHz, q);
    }
    reset();
  }

  void reset() {
    clearFront();
    clearBack();
    frontActive_ = 0.0f;
    backActive_ = 0.0f;
  }

  // Front half. `in` and `out` may be the same buffer. Takes any `n`.
  void processFront(const float* in, float* out, int n, const PolishParams& p) {
    const float target = p.on ? 1.0f : 0.0f;
    const float cutsTarget = p.on && p.cuts ? 1.0f : 0.0f;
    if (frontActive_ == 0.0f && target == 0.0f) {
      if (frontDirty_) clearFront();
      copy(in, out, n);
      return;
    }
    frontDirty_ = true;
    for (int i = 0; i < n; i += kBlock) {
      frontChunk(in + i, out + i, n - i < kBlock ? n - i : kBlock, target, cutsTarget, p.tuning.deEsser);
    }
  }

  // Back half. `in` and `out` may be the same buffer. Takes any `n`.
  void processBack(const float* in, float* out, int n, const PolishParams& p) {
    const float target = p.on ? 1.0f : 0.0f;
    if (backActive_ == 0.0f && target == 0.0f) {
      if (backDirty_) clearBack();
      copy(in, out, n);
      return;
    }
    backDirty_ = true;
    if (!shapeValid_ || !sameShape(p, applied_)) applyShape(p);

    // The sections and the smoother live in locals for the call, because `out` may alias the
    // members and would force a reload every sample.
    Biquad dip = dip_;
    Biquad presence = presence_;
    const float trim = trim_;
    float active = backActive_;
    if (active == target) {
      for (int i = 0; i < n; ++i) {
        const float x = presence.run(dip.run(in[i])) * trim;
        out[i] = in[i] + active * (x - in[i]);
      }
    } else {
      for (int i = 0; i < n; ++i) {
        active = smooth::step(active, target, smooth::kSmoothCoef);
        const float x = presence.run(dip.run(in[i])) * trim;
        out[i] = in[i] + active * (x - in[i]);
      }
    }
    dip_ = dip;
    presence_ = presence;
    backActive_ = active;
  }

 private:
  // The settings the back half's coefficients come from.
  struct BackShape {
    float dipHz, dipDb, presenceHz, presenceDb, dipQ, presenceQ, trimDb;
  };

  static BackShape shapeOf(const PolishParams& p) {
    return {p.dipHz, p.dipDb, p.presenceHz, p.presenceDb, p.tuning.dipQ, p.tuning.presenceQ, p.tuning.trimDb};
  }

  static bool sameShape(const PolishParams& p, const BackShape& s) {
    const BackShape a = shapeOf(p);
    return a.dipHz == s.dipHz && a.dipDb == s.dipDb && a.presenceHz == s.presenceHz &&
           a.presenceDb == s.presenceDb && a.dipQ == s.dipQ && a.presenceQ == s.presenceQ &&
           a.trimDb == s.trimDb;
  }

  static void copy(const float* in, float* out, int n) {
    std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
  }

  // Writes up to kBlock samples of the input through the cuts, mixed with the input by the cuts
  // smoother. Settled without the cuts it copies the input and clears the filters. The filters and the
  // smoother live in locals for the call.
  void cutsChunk(const float* in, float* y, int n, float cutsTarget) {
    float cuts = cutsActive_;
    if (cuts == 0.0f && cutsTarget == 0.0f) {
      if (cutsDirty_) clearCuts();
      copy(in, y, n);
      return;
    }
    cutsDirty_ = true;
    Biquad h0 = hp_[0];
    Biquad h1 = hp_[1];
    Biquad l0 = lp_[0];
    Biquad l1 = lp_[1];
    if (cuts == cutsTarget) {
      for (int i = 0; i < n; ++i) y[i] = l1.run(l0.run(h1.run(h0.run(in[i]))));
    } else {
      for (int i = 0; i < n; ++i) {
        cuts = smooth::step(cuts, cutsTarget, smooth::kSmoothCoef);
        y[i] = in[i] + cuts * (l1.run(l0.run(h1.run(h0.run(in[i])))) - in[i]);
      }
    }
    hp_[0] = h0;
    hp_[1] = h1;
    lp_[0] = l0;
    lp_[1] = l1;
    cutsActive_ = cuts;
  }

  // Writes up to kBlock samples of the input through the subsonic high-pass.
  void subsonicChunk(const float* in, float* y, int n) {
    Biquad s0 = sub_[0];
    Biquad s1 = sub_[1];
    for (int i = 0; i < n; ++i) y[i] = s1.run(s0.run(in[i]));
    sub_[0] = s0;
    sub_[1] = s1;
  }

  // Filters and de-esses up to kBlock samples, then mixes them with the input by the smoother. The
  // smoother lives in a local, and the de-esser's notch feeds the mix directly.
  void frontChunk(const float* in, float* out, int n, float target, float cutsTarget, const DeEsserTuning& t) {
    std::array<float, kBlock> y;
    subsonicChunk(in, y.data(), n);
    cutsChunk(y.data(), y.data(), n, cutsTarget);

    float active = frontActive_;
    if (active == target) {
      deEsser_.process(y.data(), n, t, [&](int i, float v) { out[i] = in[i] + active * (v - in[i]); });
      return;
    }
    deEsser_.process(y.data(), n, t, [&](int i, float v) {
      active = smooth::step(active, target, smooth::kSmoothCoef);
      out[i] = in[i] + active * (v - in[i]);
    });
    frontActive_ = active;
  }

  // Rebuilds the back coefficients and keeps each section's state.
  void applyShape(const PolishParams& p) {
    const PolishTuning& t = p.tuning;
    dip_.setPeak(p.dipHz, p.dipDb, t.dipQ);
    presence_.setPeak(p.presenceHz, p.presenceDb, t.presenceQ);
    trim_ = dbToGain(t.trimDb);
    applied_ = shapeOf(p);
    shapeValid_ = true;
  }

  void clearFront() {
    for (Biquad& b : sub_) b.clear();
    clearCuts();
    deEsser_.reset();
    frontDirty_ = false;
  }

  void clearCuts() {
    for (Biquad& b : hp_) b.clear();
    for (Biquad& b : lp_) b.clear();
    cutsActive_ = 0.0f;
    cutsDirty_ = false;
  }

  void clearBack() {
    dip_.clear();
    presence_.clear();
    backDirty_ = false;
  }

  std::array<Biquad, 2> sub_, hp_, lp_;
  DeEsser deEsser_;
  Biquad dip_, presence_;
  float trim_ = 1.0f;
  float frontActive_ = 0.0f;
  float cutsActive_ = 0.0f;
  float backActive_ = 0.0f;
  bool frontDirty_ = false;
  bool cutsDirty_ = false;
  bool backDirty_ = false;
  BackShape applied_{};  // the settings the back coefficients were built from
  bool shapeValid_ = false;
};

}  // namespace cv
