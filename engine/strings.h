#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

namespace cv {

// The Sympath card (engine id Strings). Eight Karplus-Strong strings ring sympathetically with the voice.
// Each string is a delay line with a one-pole damping filter and a first-order allpass in its loop. The
// damping filter and the allpass delays are worked out at the string's frequency, so the loop sums to the
// note's period. The voice enters each loop at a low send.
// The card follows the held note and reads no Autotune key. Its own tracker lowpasses the voice at 1.2 kHz,
// decimates it to 8 kHz and runs a normalised autocorrelation over 24 ms every 24 ms. A note counts as held
// when the tracked pitch stays within 0.5 semitone of one semitone for 150 ms. The strings then sit on that
// note's root and fifth over four octaves, starting at the root at or above 110 Hz. A new held semitone
// builds the new strings in a second bank, one string a call. When the last is done that bank takes the
// voice, and the old bank loses its send and fades out over 200 ms. The same semitone held again changes
// nothing. The strings are silent until the first held note, and unvoiced frames and silence keep the last
// tuning.
// DECAY sets the ring time of every string's fundamental (T60). MIX is an equal-power crossfade, and the
// wet alone is trimmed to unity on Adam's clips at the defaults.

constexpr int kStringCount = 8;
constexpr int kStringMaxLen = 440;  // samples; 48000 / 110 Hz is 436.4
constexpr float kStringsMinHz = 110.0f;
constexpr float kStringsDecayMinMs = 500.0f;
constexpr float kStringsDecayMaxMs = 8000.0f;

struct StringsTuning {
  float trimDb = 16.8f;     // the wet path's gain, set so the wet alone lands at unity on the clips
  float dampingShare = 0.25f; // the part of the fundamental's loop loss at 8 s that the damping filter takes
  float retuneMs = 200.0f;    // the old bank's fade-out when the held note changes
};

struct StringsParams {
  bool on = false;
  float decayMs = 2000.0f;  // knob A (DECAY), the ring time T60
  float mix = 0.4f;       // knob B (MIX), 0..1
  StringsTuning tuning;
};

namespace strings_detail {

constexpr int kUntuned = -2;
constexpr int kFirstMidi = 45;  // A2, 110 Hz

// The MIDI note of string `i` for the held pitch class `pc` (0 is C): the root and its fifth, then the same
// pair an octave up, over four octaves from the root at or above 110 Hz.
inline int stringMidi(int pc, int i) {
  const int base = kFirstMidi + ((pc - kFirstMidi % 12 + 12) % 12);
  return base + 12 * (i / 2) + 7 * (i % 2);
}

inline float midiHz(int midi) { return 440.0f * exp2f(static_cast<float>(midi - 69) / 12.0f); }

constexpr float kLog2Ten = 3.32192809f;
constexpr float kMaxDamping = 0.5f;
constexpr float kMaxLoopGain = 0.99995f;
constexpr float kLn1000 = 6.907755f;
constexpr float kTiny = 1e-20f;  // keeps a decayed loop off the subnormals

// One string. tune() runs libm and only runs when the held note changes. run() is the per-sample loop.
struct String {
  std::array<float, kStringMaxLen> line;
  int len = 2;
  int pos = 0;
  float a = 0.0f;       // the allpass coefficient
  float b = 0.0f;       // the damping pole
  float omb = 1.0f;     // 1 - b
  float lossK = 0.0f;   // log2 of the fundamental's loop gain at T60 = 1 s
  float dampInv = 1.0f; // 1 over the damping filter's gain at the fundamental
  float g = 0.0f;       // the loop gain ahead of the damping filter's own loss
  float send = 0.0f;
  float apx = 0.0f;
  float apy = 0.0f;
  float dm = 0.0f;
  float hz = 0.0f;
  bool ready = false;   // tuned since the last reset; a string that is not takes no send and holds no loop

  // Silences the string: the line and the filter states. The tuning stays.
  void clear() {
    line.fill(0.0f);
    pos = 0;
    apx = apy = dm = 0.0f;
  }

  // Builds the string for `hzTarget`: clears the line, and sets the loop's phase delay to the note's
  // period at that frequency. The damping pole is the largest that keeps its loss at the fundamental
  // inside `share` of the loss that T60 = 8 s asks of the loop, so DECAY never needs a pole of its own.
  void tune(float hzTarget, float share) {
    hz = hzTarget;
    const float w = kTwoPi * hzTarget / kSampleRate;
    const float period = kSampleRate / hzTarget;
    const float allowed = share * kLn1000 / (hzTarget * kStringsDecayMaxMs * 0.001f);
    const float x = 2.0f * allowed / (w * w);
    float pole = kMaxDamping;
    if (x < 2.0f) pole = x < 1e-4f ? x : ((2.0f * x + 1.0f) - sqrtf(4.0f * x + 1.0f)) / (2.0f * x);
    if (pole > kMaxDamping) pole = kMaxDamping;
    const float dampDelay = atan2f(pole * sinf(w), 1.0f - pole * cosf(w)) / w;
    int whole = static_cast<int>(floorf(period - dampDelay - 0.5f));
    if (whole < 2) whole = 2;
    if (whole > kStringMaxLen) whole = kStringMaxLen;
    const float frac = period - dampDelay - static_cast<float>(whole);  // the allpass's delay at w, in [0.5, 1.5)
    const float s = sinf(0.5f * w);
    clear();
    len = whole;
    a = sinf((1.0f - frac) * 0.5f * w) / sinf((1.0f + frac) * 0.5f * w);
    b = pole;
    omb = 1.0f - pole;
    dampInv = sqrtf(omb * omb + 4.0f * pole * s * s) / omb;
    lossK = -3.0f * kLog2Ten / hzTarget;
    g = 0.0f;
    send = 0.0f;
    ready = true;
  }

  // Sets the loop gain for the ring time whose reciprocal is `invT60`, and the send that gives a sine
  // on the note the same peak whatever the ring time. Returns nothing; run() reads both.
  void setDecay(float invT60, float sendScale) {
    if (!ready) {
      g = 0.0f;
      send = 0.0f;
      return;
    }
    const float target = exp2Fast(lossK * invT60);  // the fundamental's loop gain, damping included
    float loop = target * dampInv;
    if (loop > kMaxLoopGain) loop = kMaxLoopGain;
    g = loop;
    send = (1.0f - target) * sendScale;
  }

  // Adds `m` samples of the string's output to acc, with `x` entering the loop at this string's send.
  void run(const float* x, float* acc, int m) {
    int p = pos;
    float apx1 = apx;
    float apy1 = apy;
    float d = dm;
    for (int i = 0; i < m; ++i) {
      const float s = line[static_cast<size_t>(p)];
      const float ap = a * (s - apy1) + apx1;
      apx1 = s;
      apy1 = ap;
      d = omb * ap + b * d;
      line[static_cast<size_t>(p)] = g * d + x[i] * send + kTiny;
      acc[i] += s;
      if (++p == len) p = 0;
    }
    pos = p;
    apx = apx1;
    apy = apy1;
    dm = d;
  }
};

struct Bank {
  std::array<String, kStringCount> s;
};

// The card's own pitch tracker. It needs the pitch class only, so an octave error costs nothing. The input
// drops to 8 kHz through a 6-sample average and a 1.2 kHz lowpass. Every 192 decimated samples (24 ms) a
// snapshot of the last 229 is taken, and a normalised autocorrelation over 128 of them is worked out for lags
// 11 to 101 (80 to 667 Hz as candidates), a few lags per call so no block pays for the whole sum. A quiet
// snapshot skips the sums. The smallest lag whose peak reaches 90 % of the best one, refined by a parabola,
// gives the pitch. Counting the loops, the sums are 0.49M multiply-adds a second, about 1.1M cycles on the
// M7 at two cycles each, and the average, lowpass, snapshots and normalising add 0.5M. That is 1.6M cycles a
// second, 0.33 % of the 480 MHz processor, and less in silence.
class HeldNote {
 public:
  static constexpr int kDecim = 6;
  static constexpr float kRate = static_cast<float>(kSampleRate) / static_cast<float>(kDecim);
  static constexpr int kWin = 128;                  // decimated samples in the correlation window
  static constexpr int kLagLo = 11;                 // lags kLagLo + 1 to kLagHi - 1 are candidates
  static constexpr int kLagHi = 101;
  static constexpr int kSpan = kWin + kLagHi;       // decimated samples in a snapshot
  static constexpr int kHop = 192;                  // decimated samples between snapshots
  static constexpr uint32_t kHoldSamples = 150u * (kSampleRate / 1000);
  static constexpr float kHoldSemis = 0.5f;
  static constexpr float kMinPower = 1e-5f;         // -50 dBFS RMS
  static constexpr float kVoicedMin = 0.6f;
  static constexpr float kPeakShare = 0.9f;
  // Lags worked per input sample: 91 lags in 1152 samples with a third to spare, so a sum ends well before
  // the next snapshot.
  static constexpr float kLagsPerSample = 0.105f;

  HeldNote() {
    lp_.setLowPass(1200.0f * static_cast<float>(kDecim), kButterworthQ);  // a corner at 8 kHz, set at 48 kHz
    reset();
  }

  void reset() {
    lp_.clear();
    ring_.fill(0.0f);
    boxSum_ = 0.0f;
    w_ = 0;
    filled_ = 0;
    phase_ = 0;
    sinceHop_ = 0;
    pending_ = false;
    credit_ = 0.0f;
    clock_ = 0;
    candidate_ = kNone;
    pc_ = -1;
  }

  // Takes `m` samples of the voice. The held pitch class updates when an analysis ends.
  void push(const float* x, int m) {
    for (int i = 0; i < m; ++i) {
      boxSum_ += x[i];
      if (++phase_ < kDecim) continue;
      phase_ = 0;
      const float y = lp_.run(boxSum_ * (1.0f / static_cast<float>(kDecim)));
      boxSum_ = 0.0f;
      ring_[static_cast<size_t>(w_)] = y;
      if (++w_ == kSpan) w_ = 0;
      if (filled_ < kSpan) ++filled_;
      if (++sinceHop_ >= kHop && filled_ == kSpan && !pending_) snapshot(clock_ + static_cast<uint32_t>(i) + 1u);
    }
    clock_ += static_cast<uint32_t>(m);
    if (!pending_) return;
    credit_ += static_cast<float>(m) * kLagsPerSample;
    while (pending_ && credit_ >= 1.0f) {
      credit_ -= 1.0f;
      lagStep();
    }
    if (!pending_) credit_ = 0.0f;
  }

  // The pitch class (0 is C) of the last held note, or -1 before the first.
  int pitchClass() const { return pc_; }

 private:
  static constexpr int kNone = -1000;

  // Copies the newest kSpan samples in time order and starts the lag sums.
  void snapshot(uint32_t at) {
    sinceHop_ = 0;
    snapClock_ = at;
    int r = w_;
    float sum = 0.0f;
    cs_[0] = 0.0f;
    for (int k = 0; k < kSpan; ++k) {
      const float v = ring_[static_cast<size_t>(r)];
      if (++r == kSpan) r = 0;
      snap_[static_cast<size_t>(k)] = v;
      sum += v * v;
      cs_[static_cast<size_t>(k) + 1] = sum;
    }
    if (!(cs_[kWin] >= kMinPower * static_cast<float>(kWin))) {
      candidate_ = kNone;
      return;
    }
    lag_ = kLagLo;
    credit_ = 0.0f;
    pending_ = true;
  }

  // Normalised correlation of the window with itself `lag_` samples later.
  void lagStep() {
    const float* a = snap_.data();
    const float* b = a + lag_;
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (int i = 0; i < kWin; i += 4) {
      s0 += a[i] * b[i];
      s1 += a[i + 1] * b[i + 1];
      s2 += a[i + 2] * b[i + 2];
      s3 += a[i + 3] * b[i + 3];
    }
    const float s = (s0 + s1) + (s2 + s3);
    const float den = cs_[kWin] * (cs_[static_cast<size_t>(lag_ + kWin)] - cs_[static_cast<size_t>(lag_)]);
    ncc_[static_cast<size_t>(lag_)] = (s > 0.0f && den > 1e-20f) ? s / sqrtf(den) : 0.0f;
    if (++lag_ > kLagHi) {
      pending_ = false;
      evaluate();
    }
  }

  void evaluate() {
    float best = 0.0f;
    for (int l = kLagLo + 1; l < kLagHi; ++l) best = fmaxf(best, ncc_[static_cast<size_t>(l)]);
    int pick = 0;
    if (best >= kVoicedMin) {
      for (int l = kLagLo + 1; l < kLagHi; ++l) {
        const float v = ncc_[static_cast<size_t>(l)];
        if (v >= kPeakShare * best && v >= ncc_[static_cast<size_t>(l - 1)] && v >= ncc_[static_cast<size_t>(l + 1)]) {
          pick = l;
          break;
        }
      }
    }
    if (pick == 0) {
      candidate_ = kNone;
      return;
    }
    const float a = ncc_[static_cast<size_t>(pick - 1)];
    const float b = ncc_[static_cast<size_t>(pick)];
    const float c = ncc_[static_cast<size_t>(pick + 1)];
    const float curve = a - 2.0f * b + c;
    const float shift = curve < -1e-9f ? clampf(0.5f * (a - c) / curve, -0.5f, 0.5f) : 0.0f;
    const float hz = kRate / (static_cast<float>(pick) + shift);
    follow(69.0f + 12.0f * log2Fast(hz * (1.0f / 440.0f)));
  }

  // Judges one pitch, in semitones from MIDI 0, against the semitone being held.
  void follow(float midi) {
    if (candidate_ == kNone || fabsf(midi - static_cast<float>(candidate_)) > kHoldSemis) {
      candidate_ = static_cast<int>(floorf(midi + 0.5f));
      candStart_ = snapClock_;
    }
    if (snapClock_ - candStart_ >= kHoldSamples) pc_ = (candidate_ % 12 + 12) % 12;
  }

  Biquad lp_;
  float boxSum_ = 0.0f;
  std::array<float, kSpan> ring_{};
  std::array<float, kSpan> snap_{};
  std::array<float, kSpan + 1> cs_{};  // running sum of squares of the snapshot
  std::array<float, kLagHi + 1> ncc_{};
  int w_ = 0;
  int filled_ = 0;
  int phase_ = 0;
  int sinceHop_ = 0;
  int lag_ = kLagLo;
  bool pending_ = false;
  float credit_ = 0.0f;
  uint32_t clock_ = 0;      // input samples seen
  uint32_t snapClock_ = 0;  // the clock at the newest sample of the snapshot
  uint32_t candStart_ = 0;
  int candidate_ = kNone;   // the semitone being held, or kNone
  int pc_ = -1;
};

}  // namespace strings_detail

class Strings {
 public:
  Strings() { reset(); }

  void reset() {
    for (auto& bank : banks_) {
      for (auto& st : bank.s) {
        st.clear();
        st.ready = false;
      }
    }
    active_ = 0;
    building_ = false;
    retiring_ = false;
    retireGain_ = 0.0f;
    tunedKey_ = strings_detail::kUntuned;
    retunes_ = 0;
    tracker_.reset();
    dcX_ = dcY_ = 0.0f;
    decay_ = 0.0f;
    mix_ = 0.0f;
    wet_ = 0.0f;
    fresh_ = true;
  }

  // Off and settled, out equals in bit for bit. in and out may be the same buffer.
  void process(const float* in, float* out, int n, const StringsParams& p) {
    if (n <= 0) return;
    const float wetTarget = p.on ? 1.0f : 0.0f;
    if (wet_ == 0.0f && wetTarget == 0.0f) {
      if (!fresh_) reset();  // a stopped card holds no ringing strings
      if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    if (fresh_) {
      fresh_ = false;
      decay_ = clampf(p.decayMs, kStringsDecayMinMs, kStringsDecayMaxMs);
      mix_ = smooth::clamp01(p.mix);
    }
    retune(tracker_.pitchClass(), p.tuning);

    // DECAY and MIX glide once per call; the wet fade scales MIX, so a switch-on starts at the dry input.
    const float a = smooth::blockCoef(smooth::kSmoothSec, n);
    decay_ = smooth::step(decay_, clampf(p.decayMs, kStringsDecayMinMs, kStringsDecayMaxMs), a);
    const float invT60 = 1000.0f / decay_;  // per second
    auto& live = banks_[static_cast<size_t>(active_)];
    for (int i = 0; i < kStringCount; ++i) live.s[static_cast<size_t>(i)].setDecay(invT60, solo_ < 0 || solo_ == i ? 1.0f : 0.0f);
    const float e0 = mix_ * wet_;
    mix_ = smooth::step(mix_, smooth::clamp01(p.mix), a);
    wet_ = toward(wet_, wetTarget, kFadeStep * static_cast<float>(n));
    const float e1 = mix_ * wet_;
    const float trim = dbToGainFast(clampf(p.tuning.trimDb, -36.0f, 24.0f));
    float dry = dryGain(e0);
    float wet = wetGain(e0) * trim;
    const float inv = 1.0f / static_cast<float>(n);
    const float dDry = (dryGain(e1) - dry) * inv;
    const float dWet = (wetGain(e1) * trim - wet) * inv;
    const float retireStep = 1.0f / (clampf(p.tuning.retuneMs, 1.0f, 500.0f) * kSmpPerMs);

    for (int at = 0; at < n; at += kBlock) {
      const int m = n - at < kBlock ? n - at : kBlock;
      blockIn(in + at, m);
      tracker_.push(x_.data(), m);
      acc_.fill(0.0f);
      for (auto& st : live.s) st.run(x_.data(), acc_.data(), m);
      if (retiring_) addRetired(m, retireStep);
      for (int i = 0; i < m; ++i) {
        dry += dDry;
        wet += dWet;
        out[at + i] = in[at + i] * dry + acc_[static_cast<size_t>(i)] * wet;
      }
    }
  }

  // Test hooks: the active bank's tuning, the pitch class it sits on, the builds started, the one string that
  // takes the voice (-1 for all), and an at-once tuning to a pitch class that skips the tracker.
  float noteHz(int i) const { return banks_[static_cast<size_t>(active_)].s[static_cast<size_t>(i)].hz; }
  int tunedKey() const { return tunedKey_; }
  int retuneCount() const { return retunes_; }
  bool retiring() const { return retiring_; }
  void soloForTest(int i) { solo_ = i; }
  void tuneForTest(int pc, float share = StringsTuning{}.dampingShare) {
    for (int i = 0; i < kStringCount; ++i) {
      banks_[static_cast<size_t>(active_)].s[static_cast<size_t>(i)].tune(
          strings_detail::midiHz(strings_detail::stringMidi(pc, i)), share);
    }
    tunedKey_ = pc;
    building_ = false;
  }

 private:
  static constexpr float kFadeStep = 1.0f / (20.0f * kSmpPerMs);
  static constexpr uint32_t kQuarterTurn = 1u << 30;
  static constexpr float kDcPole = 0.9974f;  // 20 Hz

  static float toward(float x, float target, float step) {
    if (x < target) return target - x <= step ? target : x + step;
    return x - target <= step ? target : x - step;
  }
  static uint32_t phaseOf(float e) { return static_cast<uint32_t>(smooth::clamp01(e) * static_cast<float>(kQuarterTurn)); }
  static float dryGain(float e) { return sinTurns(kQuarterTurn - phaseOf(e)); }
  static float wetGain(float e) { return sinTurns(phaseOf(e)); }

  // Takes the voice through a DC blocker into x_, so an offset cannot build in the loops.
  void blockIn(const float* in, int m) {
    float x1 = dcX_;
    float y1 = dcY_;
    for (int i = 0; i < m; ++i) {
      const float y = in[i] - x1 + kDcPole * y1;
      x1 = in[i];
      y1 = y;
      x_[static_cast<size_t>(i)] = y;
    }
    dcX_ = x1;
    dcY_ = y1;
  }

  // Builds the strings for the held pitch class `want` (negative before the first held note) when it differs
  // from the one tuned, one string a call so no block pays for all eight. A first build tunes the active
  // bank, whose strings stay silent until tuned. A change builds the idle bank, and the banks swap when the
  // last string is done. While the old bank is still fading, a second change waits for it. A new note asked
  // for during a build starts the build over.
  void retune(int want, const StringsTuning& t) {
    if (want < 0) return;
    if (building_ && want != buildKey_) building_ = false;
    if (!building_) {
      if (want == tunedKey_) return;
      if (tunedKey_ != strings_detail::kUntuned && retiring_) return;
      building_ = true;
      ++retunes_;
      buildKey_ = want;
      built_ = 0;
      buildBank_ = tunedKey_ == strings_detail::kUntuned ? active_ : active_ ^ 1;
    }
    banks_[static_cast<size_t>(buildBank_)].s[static_cast<size_t>(built_)].tune(
        strings_detail::midiHz(strings_detail::stringMidi(buildKey_, built_)), t.dampingShare);
    if (++built_ < kStringCount) return;
    building_ = false;
    if (buildBank_ != active_) {
      active_ = buildBank_;
      retiring_ = true;
      retireGain_ = 1.0f;
    }
    tunedKey_ = buildKey_;
  }

  // Runs the old bank without a send and adds it to acc_ under its fade-out. The bank goes idle once
  // the fade ends.
  void addRetired(int m, float step) {
    zero_.fill(0.0f);
    old_.fill(0.0f);
    for (auto& st : banks_[static_cast<size_t>(active_ ^ 1)].s) st.run(zero_.data(), old_.data(), m);
    float gain = retireGain_;
    for (int i = 0; i < m; ++i) {
      acc_[static_cast<size_t>(i)] += old_[static_cast<size_t>(i)] * gain;
      gain = gain > step ? gain - step : 0.0f;
    }
    retireGain_ = gain;
    if (gain == 0.0f) retiring_ = false;
  }

  std::array<strings_detail::Bank, 2> banks_;
  std::array<float, kBlock> x_{};
  std::array<float, kBlock> acc_{};
  std::array<float, kBlock> old_{};
  std::array<float, kBlock> zero_{};
  int active_ = 0;
  strings_detail::HeldNote tracker_;
  int tunedKey_ = strings_detail::kUntuned;  // the pitch class the active bank sits on
  int buildKey_ = strings_detail::kUntuned;  // the pitch class being built, valid while building_
  int retunes_ = 0;
  int buildBank_ = 0;
  int built_ = 0;                            // strings of the build done
  bool building_ = false;
  int solo_ = -1;
  bool retiring_ = false;
  float retireGain_ = 0.0f;
  float dcX_ = 0.0f;
  float dcY_ = 0.0f;
  float decay_ = 0.0f;
  float mix_ = 0.0f;
  float wet_ = 0.0f;
  bool fresh_ = true;
};

}  // namespace cv
