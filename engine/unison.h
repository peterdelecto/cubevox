#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Unison: two copies of the voice, each with a slow LFO wobble on its delay (chorus). DEPTH scales the
// swing and the wet level together. Over the top of the knob, from topFrom to DEPTH 1, the swing grows by
// up to topSwingGain and the wet level by up to topWetDb. The wet level follows DEPTH on the 20 ms
// smoother. The swing and both LFO speeds glide together on 150 ms, so a fast knob move bends the pitch
// of the copies by a bounded amount and the taps never jump. The dry voice passes at unity at every DEPTH,
// and the trim scales the wet copies alone. After a reset the wet stays at 0 until the line holds live
// audio past the farthest tap, and then fades in.

namespace cv {

struct UnisonTuning {
  // Plain defaults for the tests and the render tool. On the box the Rate law (core/unison_rate.cpp)
  // sets both speeds and both swings.
  std::array<float, 2> baseDelayMs = {20.0f, 15.0f};
  std::array<float, 2> lfoHz = {0.20f, 0.30f};
  float swingMinMs = 1.0f;  // modulation swing (peak) at DEPTH 0
  float swingMaxMs = 6.0f;  // modulation swing (peak) at DEPTH 1
  float wetMaxDb = -4.0f;   // per-voice level at DEPTH 1, before the trim
  // Gain on the wet copies alone, so the dry stays at unity; the level row at RATE 50 % and DEPTH 50 % sets it.
  float trimDb = -2.0f;
  // Top of the knob (owner 2026-10-30). Both terms are exactly neutral up to topFrom.
  float topFrom = 0.6f;         // DEPTH where the extra reach starts
  float topSwingGain = 1.333f;  // swing multiplier at DEPTH 1, ramped linearly from topFrom
  float topWetDb = 2.5f;        // extra per-voice level at DEPTH 1, ramped in dB from topFrom
};

struct UnisonParams {
  bool on = false;
  float depth = 0.0f;  // 0..1
  UnisonTuning tuning;
};

class Unison {
 public:
  // The ranges the render tool holds a tuning to.
  static constexpr float kLfoMaxHz = 20.0f;
  static constexpr float kLevelLimitDb = 24.0f;  // wetMaxDb and trimDb
  static constexpr float kTopSwingGainMax = 2.0f;

  // The swing and the LFO speeds glide on kGlideSec. The keep factor over kBlock is a literal so the block
  // path reads an immediate; unison_test pins it to smooth::keep.
  static constexpr float kGlideSec = 0.150f;
  static constexpr float kGlideKeepBlock = 0.991150498f;

  // True when both copies' taps stay inside the line at the widest swing the tuning gives.
  static bool tapsFit(const UnisonTuning& t) {
    const float widest = fmaxf(swingAt(t, 0.0f), swingAt(t, 1.0f));
    for (size_t v = 0; v < 2; ++v) {
      const float base = t.baseDelayMs[v] * kSmpPerMs;
      if (!(base - widest >= kLo && base + widest <= kHi)) return false;
    }
    return true;
  }

  void reset() {
    line_.fill(0.0f);
    lfoPhase_ = {0u, kHalfTurn};
    depth_ = 0.0f;
    swingSmp_ = 0.0f;
    writePos_ = 0;
    written_ = 0;
  }

  // Test hooks: an LFO's phase in turns of 2^32, and the swing at the end of the last block in samples.
  void setLfoPhase(int v, uint32_t phase) { lfoPhase_[static_cast<size_t>(v)] = phase; }
  uint32_t lfoPhase(int v) const { return lfoPhase_[static_cast<size_t>(v)]; }
  float swingSamples() const { return swingSmp_; }

  void process(const float* in, float* out, int n, const UnisonParams& p) {
    if (n <= 0) return;
    const UnisonTuning& t = p.tuning;
    float target = p.on ? smooth::clamp01(p.depth) : 0.0f;
    if (!lineWarm(t)) target = 0.0f;
    const std::array<float, 2> hzTarget = {clampf(t.lfoHz[0], 0.0f, kLfoMaxHz), clampf(t.lfoHz[1], 0.0f, kLfoMaxHz)};

    // Off and settled: only the line and the LFO phases advance, and the speeds and the swing wait on
    // their DEPTH 0 values so the next engage starts there.
    if (depth_ == 0.0f && target == 0.0f) {
      hz_ = hzTarget;
      useSpeeds();
      swingSmp_ = swingAt(t, 0.0f);
      passThrough(in, out, n);
      return;
    }

    Ramp r = glide(t, target, hzTarget, n);
    r.wetTop0 = topGain_.of(t.topWetDb * topOver(t, r.depth0));
    r.wetTop1 = r.depth1 == r.depth0 ? r.wetTop0 : topGain_.of(t.topWetDb * topOver(t, r.depth1));
    render(in, out, n, t, r);
    depth_ = r.depth1;
    swingSmp_ = r.swing1;
  }

 private:
  static constexpr int kDelayMs = 50;
  static constexpr int kLen = static_cast<int>(kDelayMs * kSmpPerMs);
  static constexpr int kGuard = 3;         // line[kLen..kLen + 2] repeat line[0..2]
  static constexpr float kTapSpan = 3.0f;  // samples a read touches beyond its delay

  // A tap's delay stays in [2, kLen - 3] (readAt clamps to it), and a tap whose centre stays inside
  // [kLo, kHi] with a rounding margin needs no clamp.
  static constexpr float kClampMargin = 0.01f;
  static constexpr float kLo = 2.0f + kClampMargin;
  static constexpr float kHi = kLen - 3.0f - kClampMargin;

  static constexpr float kMinTopSpan = 1e-4f;
  static constexpr float kNoHz = -1.0f;       // no speed has made a step yet
  static constexpr float kGlideLand = 2e-3f;  // samples or Hz
  static constexpr uint32_t kQuarterTurn = 0x40000000u;
  static constexpr uint32_t kHalfTurn = 0x80000000u;

  // A sine and cosine pair that turns by a fixed angle a sample.
  struct Rotor {
    float sn = 0.0f, cs = 1.0f;
    static Rotor at(uint32_t phase) { return {sinTurns(phase), sinTurns(phase + kQuarterTurn)}; }
    CV_INLINE void turn(const Rotor& by) {
      const float s = sn * by.cs + cs * by.sn;
      cs = cs * by.cs - sn * by.sn;
      sn = s;
    }
  };

  // One dB value and its gain, kept for as long as the value holds.
  struct GainOf {
    float db = 0.0f;
    float gain = 1.0f;
    float of(float x) {
      if (x != db) {
        db = x;
        gain = dbToGain(x);
      }
      return gain;
    }
  };

  // Values at the two edges of a call, which the sample loops interpolate between.
  struct Ramp {
    float depth0 = 0.0f, depth1 = 0.0f;
    float swing0 = 0.0f, swing1 = 0.0f;
    float wetTop0 = 1.0f, wetTop1 = 1.0f;
    bool settled = false;
  };

  // Everything the sample loops share for one call.
  struct Call {
    Ramp r;
    float inv;  // 1 / n
    float base0, base1;
    float wetMax;
  };

  // How far DEPTH has climbed from topFrom to 1, 0..1. Exactly 0.0f at or below topFrom, so every top
  // term built on it is an exact 0 (or an exact 1 as a factor) there.
  static float topOver(const UnisonTuning& t, float depth) {
    const float from = clampf(t.topFrom, 0.0f, 1.0f);
    const float room = 1.0f - from;
    return depth > from ? (depth - from) / (room < kMinTopSpan ? kMinTopSpan : room) : 0.0f;
  }

  // LFO swing (peak) in samples at a given depth, within the line.
  static float swingAt(const UnisonTuning& t, float depth) {
    const float top = 1.0f + (t.topSwingGain - 1.0f) * topOver(t, depth);
    return clampf((t.swingMinMs + (t.swingMaxMs - t.swingMinMs) * depth) * kSmpPerMs * top, 0.0f,
                  static_cast<float>(kLen));
  }

  // x after one call of the glide, whose one-pole keeps `decay` of its distance. A swing of 400 samples
  // moves in steps of 3e-5, so the glide would stall up to 0.1 sample short and then snap; landing inside
  // kGlideLand keeps that last step far below the sample.
  static float glideTo(float x, float target, float decay) {
    const float next = target + (x - target) * decay;
    return next == x || fabsf(target - next) < kGlideLand ? target : next;
  }

  // True once the line holds live audio past the farthest tap either copy can reach.
  bool lineWarm(const UnisonTuning& t) const {
    if (written_ >= kLen) return true;
    const float widest = fmaxf(swingAt(t, 0.0f), swingAt(t, 1.0f));
    const float reach = fmaxf(t.baseDelayMs[0], t.baseDelayMs[1]) * kSmpPerMs + widest + kTapSpan;
    return static_cast<float>(written_) >= reach;
  }

  // Steps the depth on the 20 ms smoother and the swing and the LFO speeds on the 150 ms glide.
  Ramp glide(const UnisonTuning& t, float target, const std::array<float, 2>& hzTarget, int n) {
    Ramp r;
    r.depth0 = depth_;
    r.swing0 = swingSmp_;
    const float swingTarget = swingAt(t, target);
    r.settled = depth_ == target && swingSmp_ == swingTarget && hz_ == hzTarget;
    r.depth1 = depth_;
    r.swing1 = swingSmp_;
    if (r.settled) return r;
    float decay = kGlideKeepBlock;
    if (n == kBlock) {
      r.depth1 = smooth::advanceBlock(depth_, target);
    } else {
      r.depth1 = smooth::advance(depth_, target, smooth::kSmoothCoef, n);
      decay = smooth::keep(kGlideSec, n);
    }
    r.swing1 = glideTo(swingSmp_, swingTarget, decay);
    hz_ = {glideTo(hz_[0], hzTarget[0], decay), glideTo(hz_[1], hzTarget[1], decay)};
    useSpeeds();
    return r;
  }

  // Phase steps for the current LFO speeds, recomputed only when a speed changes.
  void useSpeeds() {
    for (size_t v = 0; v < 2; ++v) {
      if (hz_[v] == incHz_[v]) continue;
      incHz_[v] = hz_[v];
      lfoInc_[v] = static_cast<uint32_t>(hz_[v] / kSampleRate * kTurn32 + 0.5f);
      incStep_[v] = Rotor::at(lfoInc_[v]);
    }
  }

  // Stores a sample, and its copy past the end of the line when it lands in the first kGuard slots.
  static void write(float* line, int wp, float x) {
    line[wp] = x;
    line[wp + (((wp - kGuard) >> 31) & kLen)] = x;
  }

  void advanceLine(int n) {
    written_ = std::min(written_ + n, kLen);
  }

  void passThrough(const float* in, float* out, int n) {
    float* line = line_.data();
    int wp = writePos_;
    for (int i = 0; i < n; ++i) {
      write(line, wp, in[i]);
      out[i] = in[i];
      wp = wp + 1 == kLen ? 0 : wp + 1;
    }
    writePos_ = wp;
    advanceLine(n);
    for (size_t v = 0; v < 2; ++v) lfoPhase_[v] += lfoInc_[v] * static_cast<uint32_t>(n);
  }

  // Both taps' centres stay within the swing of their base (|sin| <= 1 up to rounding). When that band,
  // with a rounding margin, sits inside the clamp range, the clamp is an identity and the call skips it.
  void render(const float* in, float* out, int n, const UnisonTuning& t, const Ramp& r) {
    Call k;
    k.r = r;
    k.inv = 1.0f / static_cast<float>(n);
    k.base0 = t.baseDelayMs[0] * kSmpPerMs;
    k.base1 = t.baseDelayMs[1] * kSmpPerMs;
    k.wetMax = wetMaxGain_.of(t.wetMaxDb + t.trimDb);
    const float swingMax = fmaxf(r.swing0, r.swing1);
    const bool inside = k.base0 - swingMax >= kLo && k.base0 + swingMax <= kHi && k.base1 - swingMax >= kLo &&
                        k.base1 + swingMax <= kHi;
    if (r.settled) {
      renderCall<false>(in, out, n, k, inside);
    } else {
      renderCall<true>(in, out, n, k, inside);
    }
    advanceLine(n);
  }

  template <bool kMoving>
  void renderCall(const float* in, float* out, int n, const Call& k, bool inside) {
    for (int c = 0; c < n; c += kBlock) {
      const int m = std::min(n - c, kBlock);
      lfoPass<kMoving>(k, c, m);
      if (inside) {
        readPass<kMoving, false>(in, out, k, c, m);
      } else {
        readPass<kMoving, true>(in, out, k, c, m);
      }
    }
  }

  // The delays of both copies for samples c..c+m of the call. Each LFO is a rotating sine and cosine
  // pair that restarts from the integer phase every kBlock samples.
  template <bool kMoving>
  void lfoPass(const Call& k, int c, int m) {
    const Ramp& r = k.r;
    Rotor a = Rotor::at(lfoPhase_[0]);
    Rotor b = Rotor::at(lfoPhase_[1]);
    for (int j = 0; j < m; ++j) {
      float swing = r.swing0;
      if (kMoving) swing = r.swing0 + (r.swing1 - r.swing0) * (static_cast<float>(c + j + 1) * k.inv);
      delay_[0][static_cast<size_t>(j)] = k.base0 + swing * a.sn;
      delay_[1][static_cast<size_t>(j)] = k.base1 + swing * b.sn;
      a.turn(incStep_[0]);
      b.turn(incStep_[1]);
    }
    lfoPhase_[0] += lfoInc_[0] * static_cast<uint32_t>(m);
    lfoPhase_[1] += lfoInc_[1] * static_cast<uint32_t>(m);
  }

  // Writes the input into the line and sums both copies onto it, with the wet gain ramped across the call.
  template <bool kMoving, bool kClamp>
  void readPass(const float* in, float* out, const Call& k, int c, int m) {
    const Ramp& r = k.r;
    const float gainC = r.depth0 * k.wetMax * r.wetTop0;
    float* line = line_.data();
    int wp = writePos_;
    for (int j = 0; j < m; ++j) {
      const int i = c + j;
      float wetGain = gainC;
      if (kMoving) {
        const float f = static_cast<float>(i + 1) * k.inv;
        const float wetTop = r.wetTop0 + (r.wetTop1 - r.wetTop0) * f;
        wetGain = (r.depth0 + (r.depth1 - r.depth0) * f) * k.wetMax * wetTop;
      }
      const float x = in[i];
      write(line, wp, x);
      const float s0 = readAt<kClamp>(line, wp, delay_[0][static_cast<size_t>(j)]);
      const float s1 = readAt<kClamp>(line, wp, delay_[1][static_cast<size_t>(j)]);
      out[i] = x + wetGain * (s0 + s1);
      wp = wp + 1 == kLen ? 0 : wp + 1;
    }
    writePos_ = wp;
  }

  // Catmull-Rom read `delay` samples behind the head. The guard keeps the four taps side by side, so the
  // read wraps its start index once and no tap wraps on its own.
  template <bool kClamp>
  static float readAt(const float* line, int head, float delay) {
    if (kClamp) delay = clampf(delay, 2.0f, kLen - 3.0f);
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    int i = static_cast<int>(fl) - 1;
    i += i < 0 ? kLen : 0;
    const float* q = line + i;
    return catmullRom(q[0], q[1], q[2], q[3], pos - fl);
  }

  // Both copies read the same input, so one delay line serves both.
  std::array<float, kLen + kGuard> line_{};
  std::array<std::array<float, kBlock>, 2> delay_{};
  std::array<uint32_t, 2> lfoPhase_{{0u, kHalfTurn}};
  std::array<uint32_t, 2> lfoInc_{{0u, 0u}};
  std::array<float, 2> hz_{{0.0f, 0.0f}};
  std::array<float, 2> incHz_{{kNoHz, kNoHz}};
  std::array<Rotor, 2> incStep_{};
  GainOf wetMaxGain_;
  GainOf topGain_;
  float depth_ = 0.0f;
  float swingSmp_ = 0.0f;
  int writePos_ = 0;
  int written_ = 0;
};

}  // namespace cv
