#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Octave: one voice a fixed number of semitones from the input, by an asynchronous granular
// shifter. Two taps ride a shared sawtooth over a fixed window and are Hann-crossfaded, so no pitch
// tracker is involved and the formants move with the pitch. PitchFx owns the ring and the mix, and
// its Octave fade is the only fade on the toggle. The voice's output passes a 5 Hz DC blocker
// before the mix, so the dry path is untouched.

namespace cv {

struct OctaveTuning {
  float glideMs = 15.0f;        // interval glide
  float grainWindowMs = 40.0f;  // 20..80, rounded to an even number of samples
  float trimDb = 1.1f;          // level rule at MIX 1 (wet only), on Adam's clips
};

// The MIX the card starts on. The registry, the render tool and the tests all take it from here.
constexpr float kOctaveDefaultMix = 0.10f;

struct OctaveParams {
  bool on = false;     // panel toggle, off at the box's start; off fades the stage out
  int semitones = 12;  // panel knob, -12..12, 0 = off
  float mix = kOctaveDefaultMix;  // panel knob
  OctaveTuning tuning;
};

// Autotune's correction reaches the voice through its own 2 ms one-pole. The correction steps every
// block, and the 15 ms glide of the SEMITONES step would hold the voice about half a vibrato cycle
// behind the tuned note; 2 ms follows it and smooths the 64-sample staircase it arrives in.
inline const float kOctaveCorrCoef = smooth::coef(0.002f);
inline const float kOctaveCorrDecay = powf(1.0f - kOctaveCorrCoef, static_cast<float>(kBlock));

class OctaveVoice {
 public:
  static constexpr int kMaxSemis = 12;
  static constexpr float kMinWindowMs = 20.0f;
  static constexpr float kMaxWindowMs = 80.0f;

  void reset() { *this = OctaveVoice{}; }

  // Called once per block before rendering. Returns true if the stage is on. corr is Autotune's
  // correction in semitones. libm runs only when the glide time moved.
  bool prepare(const OctaveParams& p, float corr) {
    const int s = p.semitones < -kMaxSemis ? -kMaxSemis : (p.semitones > kMaxSemis ? kMaxSemis : p.semitones);
    const OctaveTuning& t = p.tuning;
    semisT_ = static_cast<float>(s);
    corrT_ = corr;
    level_.set(t.trimDb);
    // A voice that is not running jumps to its new interval instead of gliding in from a stale one.
    if (idle_) {
      semis_ = semisT_;
      corr_ = corrT_;
    }
    // Digital silence leaves the blocker resting on a subnormal.
    if (fabsf(dc_.y) < kDcRest) dc_.y = 0.0f;
    if (t.glideMs != glideMs_) {
      glideMs_ = t.glideMs;
      aGlide_ = smooth::coef(t.glideMs * 0.001f);
      glideDecay_ = powf(1.0f - aGlide_, static_cast<float>(kBlock));
    }
    // The taps sit half a window apart, a whole number of samples, so they share one fraction.
    windowSmp_ = 2.0f * floorf(0.5f * clampf(t.grainWindowMs, kMinWindowMs, kMaxWindowMs) * kSmpPerMs + 0.5f);
    invWindow_ = 1.0f / windowSmp_;
    return p.on && s != 0;
  }

  // True from a reset or a park until the next render: the voice holds no state worth keeping.
  bool idle() const { return idle_; }

  // A block whose output nobody hears. The interval lands on its target and the blocker clears, so
  // the voice picks up in step when it is heard again.
  void park() {
    semis_ = semisT_;
    corr_ = corrT_;
    dc_.clear();
    idle_ = true;
  }

  // The voice for one chunk of n <= kBlock samples into out[0..n). The chunk is already in the ring
  // from ring index head0, and the voice reads only older samples. Two taps share one 32-bit
  // sawtooth counter and one Hann rotor, with delays in Q18 samples. A tap at ratio r drifts
  // (1 - r) samples of delay per sample, and the sawtooth is that drift normalised to the window.
  void render(const VoiceRing& ring, int head0, int n, float* out) {
    idle_ = false;
    advanceInterval(n);
    const float step = (1.0f - rateMean_) * invWindow_;  // turns of sawtooth a sample
    const int32_t dp = static_cast<int32_t>(step * kTurn + (step >= 0.0f ? 0.5f : -0.5f));
    HannRotor rot;
    rot.init(static_cast<float>(ph_) * (kTwoPi / kTurn), kTwoPi * step);
    // A chunk that stays clear of the ring's seam needs no wrap tests.
    if (head0 >= static_cast<int>(windowSmp_) + 8 && head0 + n <= kVoiceRingLen) {
      renderLoop<false>(ring, head0, n, out, dp, rot);
    } else {
      renderLoop<true>(ring, head0, n, out, dp, rot);
    }
  }

 private:
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kMinDelay = 4.0f;
  static constexpr float kTurn = 4294967296.0f;  // one turn of the sawtooth counter
  static constexpr float kDcRest = 1e-30f;       // a blocker state under this rests at 0

  // Delays and ring positions are Q18, 18 bits of fraction. The ring must fit 32 bits that way, and
  // readRing's four taps cost 3 samples of reach.
  static constexpr uint32_t kFracBits = 18;
  static constexpr uint32_t kFracMask = (1u << kFracBits) - 1;
  static constexpr uint32_t kLenQ = static_cast<uint32_t>(kVoiceRingLen) << kFracBits;
  static constexpr float kFracScale = 1.0f / static_cast<float>(1u << kFracBits);
  static constexpr float kReadReach = 3.0f;
  static_assert((static_cast<uint64_t>(kVoiceRingLen) << kFracBits) < (uint64_t{1} << 32), "the ring overflows Q18");
  static_assert(kMaxWindowMs * kSmpPerMs + kMinDelay + kReadReach < kVoiceRingLen, "grains would outrun the ring");

  // NaN compares unequal to every value, so a cleared input recomputes on the next prepare().
  static constexpr float kUnset = std::numeric_limits<float>::quiet_NaN();

  // exp2 of the interval over 12, recomputed only while the interval glides. The interval is the
  // glided SEMITONES step and Autotune's smoothed correction.
  void updateRatio() {
    const float interval = semis_ + corr_;
    if (interval == ratioSemis_) return;
    ratio_ = exp2Fast(interval / 12.0f);
    ratioSemis_ = interval;
  }

  // x moved n samples toward target by a one-pole with coefficient a, whose block of kBlock samples
  // has the closed form decay. It lands exactly on the target once within kSnap.
  static float glide(float x, float target, float a, float decay, int n) {
    if (x == target) return x;
    if (n != kBlock) return smooth::advance(x, target, a, n);
    x = target + (x - target) * decay;
    return fabsf(target - x) < smooth::kSnap ? target : x;
  }

  // Moves the interval n samples along its two glides and sets rateMean_ to the mean of the ratio at
  // the two ends of the chunk, which is exact for a ratio that moves linearly.
  void advanceInterval(int n) {
    updateRatio();
    rateMean_ = ratio_;
    if (semis_ == semisT_ && corr_ == corrT_) return;
    const float r0 = ratio_;
    semis_ = glide(semis_, semisT_, aGlide_, glideDecay_, n);
    corr_ = glide(corr_, corrT_, kOctaveCorrCoef, kOctaveCorrDecay, n);
    updateRatio();
    rateMean_ = 0.5f * (r0 + ratio_);
  }

  // Safe adds the wrap tests for a chunk near the ring's seam.
  template <bool Safe>
  void renderLoop(const VoiceRing& ring, int head0, int n, float* out, int32_t dp, HannRotor rot) {
    const float* r = ring.data();
    const uint32_t wq = static_cast<uint32_t>(windowSmp_ * static_cast<float>(1u << kFracBits));
    const uint32_t halfQ = wq >> 1;
    constexpr uint32_t kMinQ = static_cast<uint32_t>(kMinDelay) << kFracBits;
    uint32_t ph = ph_;
    uint32_t hq = static_cast<uint32_t>(head0) << kFracBits;
    float c = rot.c, s = rot.s;
    const float dc = rot.dc, ds = rot.ds;
    float dcY = dc_.y;
    const float dcA = dc_.a;
    const float level = level_.gain();
    for (int j = 0; j < n; ++j) {
      const float g0 = 0.5f - 0.5f * c;
      const float c2 = c * dc - s * ds;
      s = s * dc + c * ds;
      c = c2;
      const uint32_t delay0 = kMinQ + mulHi(ph, wq);
      uint32_t pos0 = hq - delay0;
      if (Safe && hq < delay0) pos0 += kLenQ;
      const bool upper = (ph & 0x80000000u) != 0;  // the first tap's phase is past half a turn
      uint32_t pos1 = upper ? pos0 + halfQ : pos0 - halfQ;
      if (Safe) {
        if (!upper && pos0 < halfQ) pos1 += kLenQ;
        if (upper && pos1 >= kLenQ) pos1 -= kLenQ;
      }
      float x0, x1;
      readTaps<Safe>(r, pos0, pos1, x0, x1);
      const float y = level * (x1 + g0 * (x0 - x1));
      dcY += dcA * (y - dcY);
      out[j] = y - dcY;
      ph += static_cast<uint32_t>(dp);
      hq += 1u << kFracBits;
      if (Safe && hq >= kLenQ) hq -= kLenQ;
    }
    ph_ = ph;
    dc_.y = dcY;
  }

  // The two taps' Catmull-Rom reads at Q18 ring positions pos0 and pos1, which share pos0's fraction.
  // Safe wraps a read that crosses the ring's seam.
  template <bool Safe>
  static CV_INLINE void readTaps(const float* r, uint32_t pos0, uint32_t pos1, float& x0, float& x1) {
    constexpr uint32_t kReach = static_cast<uint32_t>(kVoiceRingLen - 3);
    const uint32_t i0 = pos0 >> kFracBits;
    const uint32_t i1 = pos1 >> kFracBits;
    const CatmullRomWeights w = catmullRomWeights(static_cast<float>(pos0 & kFracMask) * kFracScale);
    if (!Safe || (i0 - 1u < kReach && i1 - 1u < kReach)) {
      const float* q0 = r + i0;
      const float* q1 = r + i1;
      x0 = w.m * q0[-1] + w.z * q0[0] + w.p * q0[1] + w.q * q0[2];
      x1 = w.m * q1[-1] + w.z * q1[0] + w.p * q1[1] + w.q * q1[2];
    } else {
      x0 = dotWrapped(r, i0, w);
      x1 = dotWrapped(r, i1, w);
    }
  }

  // The dot product of four taps at i0 - 1 .. i0 + 2 with the ring wrapping, for a read across its seam.
  static __attribute__((noinline)) float dotWrapped(const float* r, uint32_t i0, const CatmullRomWeights& w) {
    const uint32_t len = static_cast<uint32_t>(kVoiceRingLen);
    const uint32_t im = i0 == 0 ? len - 1 : i0 - 1;
    const uint32_t i1 = i0 + 1 >= len ? i0 + 1 - len : i0 + 1;
    const uint32_t i2 = i0 + 2 >= len ? i0 + 2 - len : i0 + 2;
    return w.m * r[im] + w.z * r[i0] + w.p * r[i1] + w.q * r[i2];
  }

  // The high 32 bits of the 64-bit product.
  static CV_INLINE uint32_t mulHi(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>((static_cast<uint64_t>(a) * b) >> 32);
  }

  uint32_t ph_ = 0;      // the sawtooth, 2^32 a turn
  float semis_ = 0.0f;   // the SEMITONES step after its glide
  float semisT_ = 0.0f;
  float corr_ = 0.0f;    // Autotune's correction after its 2 ms one-pole
  float corrT_ = 0.0f;
  float aGlide_ = 1.0f;
  float glideDecay_ = 0.0f;  // (1 - aGlide_)^kBlock, the glide over one block
  float glideMs_ = kUnset;   // the glide time aGlide_ came from
  DbGain level_;
  float ratio_ = 1.0f;
  float ratioSemis_ = 0.0f;
  float rateMean_ = 1.0f;  // the chunk's mean ratio
  float windowSmp_ = 40.0f * kSmpPerMs;
  float invWindow_ = 1.0f / (40.0f * kSmpPerMs);
  DcBlocker dc_;
  bool idle_ = true;
};

}  // namespace cv
